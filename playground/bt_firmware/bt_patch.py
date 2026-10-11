#!/usr/bin/env python3
"""One-off lower-BT patch transaction. No implicit reset, retry, or repair.

Only the explicitly reviewed source image is accepted. The production TOP
flasher is imported for checked SBL framing/reads, never its mutation methods.
"""
import argparse
import binascii
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import time

DRIVER_PATH = Path(__file__).resolve().parents[2] / 'scripts' / 'jbl_bl.py'
DRIVER_SHA = '228f6e64994b234a76557afc25db4a65e9f27cdfa54aee45916c44eab241d7ed'
SOURCE_SHA = 'a3e2192ba8c1d3ef39c3049a9648f35466f9d95aa9b66ae438eac2e861862968'
BT_SHA = 'e95e20967e694928df11cec42d36416662a79eb3f8afa2a35f027ef63bee518c'
FLASH_ID = bytes.fromhex('85601600ffffffff4250305539313311')
FLASH_SIZE, SECTOR = 0x400000, 0x1000
BT_START, BT_LENGTH, BT_END = 0x50000, 0xDD000, 0x150000
META_ORDER = (0x1000, 0)
RECORD = 0x60

def sha(data):
    return hashlib.sha256(data).hexdigest()

def need(ok, message):
    if not ok:
        raise ValueError(message)

need(sha(DRIVER_PATH.read_bytes()) == DRIVER_SHA, 'Pinned SBL protocol source changed')
_spec = importlib.util.spec_from_file_location('bt_patch_sbl', DRIVER_PATH)
bl = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = bl
_spec.loader.exec_module(bl)

def number(value):
    need(type(value) in (int, str), 'Address must be integer or explicit integer string')
    return int(value, 0) if isinstance(value, str) else value

def canonical(value):
    return (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()

def save_new(path, data):
    """Refuse overwrite, sync contents; a failed write prevents hardware mutation."""
    path = Path(path)
    with path.open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    need(path.read_bytes() == data, f'Disk readback failed: {path}')

def executable_checks(image, *, original_boot=None):
    need(len(image) == FLASH_SIZE, 'Expected full 4 MiB image')
    prefix = bytearray(image[:bl.PREFIX_SIZE])
    # Restore preserves current unrelated boot-record changes. Do not turn
    # original lower-bank eligibility into a requirement on those changes.
    if original_boot is not None:
        start = bl.BOOT_RECORD_OFFSET
        prefix[start:start + bl.BOOT_RECORD.size] = original_boot
    layout = bl.validate_layout(bytes(prefix))
    bt = next(part for part in layout.components if part.index == 1)
    need((bt.address, bt.length, bt.capacity, bt.status) ==
         (BT_START, BT_LENGTH, BT_END - BT_START, 0), 'Unexpected lower BT descriptor')
    for part in layout.components:
        need(bl.crc32(image[part.address:part.address + part.length]) == part.crc,
             f'Executable partition {part.index} CRC mismatch')
    return layout

def build_candidate(source, manifest):
    need(len(source) == FLASH_SIZE and sha(source) == SOURCE_SHA, 'Wrong or stale full backup')
    need(manifest.get('source_flash_sha256') == SOURCE_SHA, 'Manifest source flash hash mismatch')
    need(manifest.get('source_bt_sha256') == BT_SHA, 'Manifest source BT hash mismatch')
    need(sha(source[BT_START:BT_START + BT_LENGTH]) == BT_SHA, 'Backup lower BT hash mismatch')
    executable_checks(source)
    patches = manifest.get('patches')
    need(isinstance(patches, list) and 0 < len(patches) <= 16, 'Expected 1..16 explicit patches')
    result = bytearray(source)
    covered, sectors, normalized = set(), set(), []
    for patch in patches:
        address = number(patch['flash_offset'])
        old, new = bytes.fromhex(patch['old_hex']), bytes.fromhex(patch['new_hex'])
        need(0 < len(old) == len(new) <= SECTOR, 'Patch must preserve length and fit one sector size')
        need(BT_START <= address and address + len(old) <= BT_START + BT_LENGTH,
             'Patch escapes declared lower BT payload')
        need(address % 2 == 0 and len(old) % 2 == 0, 'Thumb patch must be halfword aligned')
        need(old != new, 'No-op patch rejected')
        positions = set(range(address, address + len(old)))
        need(not covered.intersection(positions), 'Overlapping patches rejected')
        need(source[address:address + len(old)] == old, 'Patch old bytes mismatch')
        result[address:address + len(old)] = new
        covered.update(positions)
        sectors.update(pos // SECTOR * SECTOR for pos in positions)
        normalized.append({'flash_offset': address, 'old_hex': old.hex(), 'new_hex': new.hex(),
                           'purpose': patch.get('purpose', '')})
    new_bt_crc = bl.crc32(result[BT_START:BT_START + BT_LENGTH])
    for base in META_ORDER:
        struct.pack_into('<I', result, base + RECORD + bl.PARTITION_CRC_OFFSET, new_bt_crc)
        crc = binascii.crc_hqx(result[base + RECORD:base + RECORD + 30], 0)
        struct.pack_into('<H', result, base + RECORD + 30, crc)
    metadata_bytes = {base + RECORD + offset for base in META_ORDER
                      for offset in (*range(22, 26), 30, 31)}
    allowed = covered | metadata_bytes
    need(all(a == b or index in allowed for index, (a, b) in enumerate(zip(source, result))),
         'Candidate has an unauthorized byte change')
    executable_checks(bytes(result))
    return bytes(result), sorted(sectors), normalized

def describe(source, target, sectors, patches):
    return {
        'schema': 1, 'helper_sha256': sha(Path(__file__).read_bytes()),
        'driver_sha256': DRIVER_SHA, 'source_sha256': sha(source),
        'target_sha256': sha(target), 'source_native_crc': bl.crc32(source),
        'target_native_crc': bl.crc32(target), 'source_bt_sha256': BT_SHA,
        'target_bt_sha256': sha(target[BT_START:BT_START + BT_LENGTH]),
        'source_bt_crc': bl.crc32(source[BT_START:BT_START + BT_LENGTH]),
        'target_bt_crc': bl.crc32(target[BT_START:BT_START + BT_LENGTH]),
        'payload_sectors': sectors, 'metadata_sectors': list(META_ORDER),
        'write_order': sectors + list(META_ORDER), 'patches': patches,
        'changed_byte_count': sum(a != b for a, b in zip(source, target)),
        'metadata_header_policy': 'Preserve all header bytes, including existing stale aggregate CRC',
        'sector_hashes': [{'address': address,
                           'before_sha256': sha(source[address:address + SECTOR]),
                           'after_sha256': sha(target[address:address + SECTOR])}
                          for address in sectors + list(META_ORDER)],
        'hardware_executed': False,
    }

def make_plan(backup, manifest_path, output):
    source, manifest_bytes = Path(backup).read_bytes(), Path(manifest_path).read_bytes()
    target, sectors, patches = build_candidate(source, json.loads(manifest_bytes))
    plan = describe(source, target, sectors, patches)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    save_new(output / 'source.bin', source)
    save_new(output / 'manifest.json', manifest_bytes)
    save_new(output / 'candidate.bin', target)
    plan['manifest_sha256'] = sha(manifest_bytes)
    save_new(output / 'plan.json', canonical(plan))
    return plan

def load_plan(path):
    path = Path(path).resolve()
    plan = json.loads(path.read_bytes())
    source = (path.parent / 'source.bin').read_bytes()
    manifest_bytes = (path.parent / 'manifest.json').read_bytes()
    target, sectors, patches = build_candidate(source, json.loads(manifest_bytes))
    expected = describe(source, target, sectors, patches)
    expected['manifest_sha256'] = sha(manifest_bytes)
    need(plan == expected, 'Plan modified/stale or helper/source/manifest changed')
    need((path.parent / 'candidate.bin').read_bytes() == target, 'Saved candidate modified')
    return plan, source, target

def restore_target(current, source, candidate, sectors):
    need(len(current) == FLASH_SIZE, 'Incomplete live flash for restore')
    need(current[BT_START:BT_END] == candidate[BT_START:BT_END], 'Unexpected BT bytes; restore refused')
    need(current[:SECTOR * 2] == candidate[:SECTOR * 2], 'Unexpected metadata; restore refused')
    target = bytearray(current)
    for address in sectors + list(META_ORDER):
        target[address:address + SECTOR] = source[address:address + SECTOR]
    boot = source[bl.BOOT_RECORD_OFFSET:bl.BOOT_RECORD_OFFSET + bl.BOOT_RECORD.size]
    executable_checks(bytes(target), original_boot=boot)
    return bytes(target)

def hardware_guard(session):
    session.mutation_check()
    need(session.boot_version() == bl.SBL_VERSION, 'Wrong SBL version')
    need(session.read_word(bl.SCB_VTOR) == bl.SBL_VECTOR_BASE, 'SBL vector table not active')
    need(session.get_flash_selection() == bl.FLASH_SELECTOR, 'Wrong flash selector; will not change it')
    need(session.flash_id() == FLASH_ID, 'Flash identity differs from verified backup')

def stable_full_read(session, expected=None):
    session.mutation_check()
    before = session.check_flash(0, FLASH_SIZE, 0)
    data = session.read_flash(0, FLASH_SIZE)
    actual = bl.crc32(data)
    session.mutation_check()
    after = session.check_flash(0, FLASH_SIZE, actual)
    need(before == actual == after, 'Full flash changed or CRC disagrees during read')
    if expected is not None:
        need(data == expected, 'Live flash differs bytewise from the required source')
    return data

def transact(session, before, target, sectors, emit):
    """Only called with a validated exact full-image source and target."""
    allowed = set(sectors) | set(META_ORDER)
    need(all(BT_START <= address < BT_START + BT_LENGTH and address % SECTOR == 0
             for address in sectors), 'Invalid lower BT sector allowlist')
    need(len(sectors) == len(set(sectors)) and sectors == sorted(sectors), 'Bad sector ordering')
    need(len(before) == len(target) == FLASH_SIZE, 'Transaction images must be complete')
    need(all(a == b or index // SECTOR * SECTOR in allowed
             for index, (a, b) in enumerate(zip(before, target))), 'Target escapes write allowlist')
    expected_bt = target[BT_START:BT_START + BT_LENGTH]
    bt_crc = bl.crc32(expected_bt)

    def replace_sector(address):
        need(address in allowed, 'Mutation outside sector allowlist')
        previous, replacement = before[address:address + SECTOR], target[address:address + SECTOR]
        need(session.read_flash(address, SECTOR) == previous, f'Sector {address:#x} changed before erase')
        emit('erase_intent', address=address)
        session.mutation_check()
        session.request(bytes([bl.COMMAND_ERASE]) + bl.PAIR.pack(address // SECTOR, 1), 0)
        emit('erase_ack', address=address)
        for offset in range(0, SECTOR, bl.CHUNK_SIZE):
            chunk = replacement[offset:offset + bl.CHUNK_SIZE]
            emit('write_intent', address=address + offset, length=len(chunk))
            session.mutation_check()
            session.request(bytes([bl.COMMAND_WRITE]) + bl.WORD.pack(address + offset) + chunk, 0)
            emit('write_ack', address=address + offset)
        need(session.read_flash(address, SECTOR) == replacement, f'Sector {address:#x} readback mismatch')
        emit('sector_verified', address=address, sha256=sha(replacement))

    try:
        # Recheck immediately before any mutation, after target is safely on disk.
        need(session.check_flash(0, FLASH_SIZE, bl.crc32(before)) == bl.crc32(before),
             'Whole flash changed immediately before first erase')
        for address in sectors:
            replace_sector(address)
        need(session.read_flash(BT_START, BT_LENGTH) == expected_bt, 'BT payload bytewise verify failed')
        need(session.check_flash(BT_START, BT_LENGTH, bt_crc) == bt_crc, 'BT native CRC failed')
        emit('bt_verified_before_metadata', crc=bt_crc)
        need(session.read_flash(0, 2 * SECTOR) == before[:2 * SECTOR], 'Metadata changed before commit')
        for address in META_ORDER:
            replace_sector(address)
        stable_full_read(session, target)
        emit('whole_flash_verified', sha256=sha(target), native_crc=bl.crc32(target))
    except BaseException:
        session.failed = True
        raise

def execute(action, plan_path, port, output):
    plan, source, candidate = load_plan(plan_path)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    # Journal exists, is writable and synced before the serial port is opened.
    with (output / 'journal.jsonl').open('x') as journal:
        def emit(event, **values):
            entry = {'event': event, 'wall_ns': time.time_ns(), 'monotonic_ns': time.monotonic_ns(), **values}
            journal.write(json.dumps(entry, sort_keys=True) + '\n')
            journal.flush()
            os.fsync(journal.fileno())
        emit('started', action=action, port=port, plan_sha256=sha(Path(plan_path).read_bytes()))
        try:
            with bl.open_session(port) as session:
                hardware_guard(session)
                current = stable_full_read(session, source if action == 'apply' else None)
                target = candidate if action == 'apply' else restore_target(
                    current, source, candidate, plan['payload_sectors'])
                save_new(output / 'before.bin', current)
                save_new(output / 'target.bin', target)
                emit('preflight_verified', source_sha256=sha(current), target_sha256=sha(target),
                     payload_sectors=plan['payload_sectors'], metadata_order=list(META_ORDER))
                transact(session, current, target, plan['payload_sectors'], emit)
            # Connection already closed before final report writes.
            report = {'action': action, 'success': True, 'source_sha256': sha(current),
                      'target_sha256': sha(target), 'native_crc': bl.crc32(target),
                      'unrelated_current_changes_preserved': action == 'restore', 'reset_sent': False}
            save_new(output / 'result.json', canonical(report))
            return report
        except BaseException as error:
            # open_session's finally closes serial before this handler runs.
            try:
                emit('failed_no_retry_no_reset', error=repr(error))
            except BaseException:
                pass
            raise

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('plan', help='Offline only: prepare reviewed candidate and manifest')
    p.add_argument('--backup', required=True)
    p.add_argument('--manifest', required=True)
    p.add_argument('--output', required=True)
    for action in ('apply', 'restore'):
        p = sub.add_parser(action, help='Explicit hardware transaction; never resets')
        p.add_argument('--plan', required=True)
        p.add_argument('--port', required=True)
        p.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.command == 'plan':
        report = make_plan(args.backup, args.manifest, args.output)
    else:
        report = execute(args.command, args.plan, args.port, args.output)
    print(json.dumps(report, indent=2, sort_keys=True))

if __name__ == '__main__':
    main()
