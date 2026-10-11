#!/usr/bin/env python3
"""Offline transaction tests; never imports serial or opens hardware.

Supply the verified original full-flash backup outside this repository using
--backup PATH or CWBEAT_BT_ORIGINAL_BACKUP. No firmware image is bundled.
"""
import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from contextlib import contextmanager

spec = importlib.util.spec_from_file_location('bt_patch', Path(__file__).with_name('bt_patch.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
BACKUP_ENV = 'CWBEAT_BT_ORIGINAL_BACKUP'

def external_backup():
    value = os.environ.get(BACKUP_ENV)
    if not value:
        raise ValueError(f'Provide --backup PATH or set {BACKUP_ENV} to the verified original backup')
    backup = Path(value).expanduser().resolve()
    repository = Path(__file__).resolve().parents[2]
    if backup.is_relative_to(repository):
        raise ValueError('Keep the full firmware backup outside the repository')
    return backup

class FakeSession:
    def __init__(self, data, fail_write=None, corrupt_readback=False):
        self.data = bytearray(data)
        self.failed = False
        self.ops = []
        self.fail_write = fail_write
        self.write_count = 0
        self.corrupt_readback = corrupt_readback
        self.closed = False
    def mutation_check(self):
        if self.failed:
            raise RuntimeError('Poisoned session')
    def boot_version(self): return m.bl.SBL_VERSION
    def read_word(self, address): return m.bl.SBL_VECTOR_BASE
    def get_flash_selection(self): return 2
    def flash_id(self): return m.FLASH_ID
    def read_flash(self, address, length):
        self.ops.append(('read', address, length))
        result = bytes(self.data[address:address + length])
        if self.corrupt_readback and self.write_count and address >= m.BT_START:
            return bytes([result[0] ^ 1]) + result[1:]
        return result
    def check_flash(self, address, length, expected):
        self.ops.append(('crc', address, length))
        return m.bl.crc32(self.data[address:address + length])
    def request(self, payload, expected):
        self.mutation_check()
        command = payload[0]
        if command == m.bl.COMMAND_ERASE:
            sector, count = struct.unpack('<II', payload[1:])
            assert count == 1 and expected == 0
            address = sector * m.SECTOR
            self.ops.append(('erase', address))
            self.data[address:address + m.SECTOR] = b'\xff' * m.SECTOR
        elif command == m.bl.COMMAND_WRITE:
            address = struct.unpack_from('<I', payload, 1)[0]
            self.write_count += 1
            self.ops.append(('write', address))
            if self.write_count == self.fail_write:
                # Simulate an uncertain partial hardware write followed by failed reply.
                self.data[address:address + 7] = payload[5:12]
                self.failed = True
                raise RuntimeError('Injected partial write with lost ACK')
            self.data[address:address + len(payload) - 5] = payload[5:]
        else:
            raise AssertionError(f'Unexpected mutation opcode {command}')
        return b''

class PatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.backup = external_backup()
        cls.source = cls.backup.read_bytes()
        address = 0xCC4A4
        cls.manifest = {
            'source_flash_sha256': m.SOURCE_SHA, 'source_bt_sha256': m.BT_SHA,
            'patches': [{'flash_offset': address,
                         'old_hex': cls.source[address:address + 4].hex(),
                         'new_hex': '00bf00bf', 'purpose': 'Offline test fixture only'},
                        {'flash_offset': 0x128000, 'old_hex': 'ffffffff',
                         'new_hex': '00bf7047', 'purpose': 'Offline test fixture only'}]}
        cls.target, cls.sectors, _ = m.build_candidate(cls.source, cls.manifest)

    def test_metadata_and_protected_bytes(self):
        self.assertEqual(self.target[:0x1000], self.target[0x1000:0x2000])
        self.assertEqual(self.target[:0x40], self.source[:0x40])
        self.assertEqual(self.target[0x270000:0x370000], self.source[0x270000:0x370000])
        self.assertEqual(self.target[0x2000:0xCC4A4], self.source[0x2000:0xCC4A4])
        m.executable_checks(self.target)
        self.assertEqual(self.sectors, [0xCC000, 0x128000])

    def test_stale_hash(self):
        manifest = copy.deepcopy(self.manifest)
        manifest['source_flash_sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'Manifest source flash'):
            m.build_candidate(self.source, manifest)

    def test_tampered_source(self):
        source = bytearray(self.source)
        source[0x300000] ^= 1
        with self.assertRaisesRegex(ValueError, 'Wrong or stale'):
            m.build_candidate(bytes(source), self.manifest)

    def test_outside_bt_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest['patches'][0]['flash_offset'] = 0x270000
        with self.assertRaisesRegex(ValueError, 'escapes declared lower BT'):
            m.build_candidate(self.source, manifest)

    def test_wrong_expected_bytes_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest['patches'][0]['old_hex'] = 'ffffffff'
        with self.assertRaisesRegex(ValueError, 'old bytes mismatch'):
            m.build_candidate(self.source, manifest)

    def test_overlap_rejected(self):
        manifest = copy.deepcopy(self.manifest)
        manifest['patches'].append(manifest['patches'][0])
        with self.assertRaisesRegex(ValueError, 'Overlapping'):
            m.build_candidate(self.source, manifest)

    def test_reviewed_plan_tampering(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            manifest_path = tmp / 'input.json'
            manifest_path.write_text(json.dumps(self.manifest))
            out = tmp / 'plan'
            m.make_plan(self.backup, manifest_path, out)
            m.load_plan(out / 'plan.json')
            changed = json.loads((out / 'manifest.json').read_text())
            changed['patches'][0]['new_hex'] = '70477047'
            (out / 'manifest.json').write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, 'Plan modified/stale'):
                m.load_plan(out / 'plan.json')

    def test_transaction_order_and_full_verify(self):
        session = FakeSession(self.source)
        events = []
        m.transact(session, self.source, self.target, self.sectors,
                   lambda event, **values: events.append((event, values)))
        self.assertEqual(bytes(session.data), self.target)
        self.assertEqual([op[1] for op in session.ops if op[0] == 'erase'],
                         self.sectors + [0x1000, 0])
        verified = next(i for i, x in enumerate(events) if x[0] == 'bt_verified_before_metadata')
        first_meta = next(i for i, x in enumerate(events)
                          if x[0] == 'erase_intent' and x[1]['address'] == 0x1000)
        self.assertLess(verified, first_meta)
        self.assertEqual(events[-1][0], 'whole_flash_verified')

    def test_partial_payload_failure_never_advances(self):
        session = FakeSession(self.source, fail_write=1)
        with self.assertRaisesRegex(RuntimeError, 'partial write'):
            m.transact(session, self.source, self.target, self.sectors, lambda *a, **kw: None)
        self.assertTrue(session.failed)
        self.assertEqual([op for op in session.ops if op[0] == 'erase'], [('erase', self.sectors[0])])
        self.assertEqual(session.ops[-1], ('write', self.sectors[0]))

    def test_partial_mirror_failure_never_touches_primary(self):
        session = FakeSession(self.source, fail_write=17)
        with self.assertRaisesRegex(RuntimeError, 'partial write'):
            m.transact(session, self.source, self.target, self.sectors, lambda *a, **kw: None)
        self.assertTrue(session.failed)
        self.assertNotIn(('erase', 0), session.ops)
        self.assertEqual(session.ops[-1], ('write', 0x1000))

    def test_readback_failure_never_commits_metadata(self):
        session = FakeSession(self.source, corrupt_readback=True)
        with self.assertRaisesRegex(ValueError, 'readback mismatch'):
            m.transact(session, self.source, self.target, self.sectors, lambda *a, **kw: None)
        self.assertNotIn(('erase', 0x1000), session.ops)
        self.assertNotIn(('erase', 0), session.ops)

    def test_journal_failure_stops_mutations(self):
        session = FakeSession(self.source)
        def fail(event, **values):
            if event == 'erase_ack':
                raise OSError('Disk full')
        with self.assertRaisesRegex(OSError, 'Disk full'):
            m.transact(session, self.source, self.target, self.sectors, fail)
        self.assertEqual([op for op in session.ops if op[0] == 'write'], [])
        self.assertTrue(session.failed)

    def test_restore_preserves_unrelated_current_changes(self):
        current = bytearray(self.target)
        current[0x1B0100] ^= 1  # Shared ATS setting, not executable partition.
        current[0x3FF100] ^= 1  # Tail configuration, also preserved.
        restored = m.restore_target(bytes(current), self.source, self.target, self.sectors)
        expected = bytearray(self.source)
        expected[0x1B0100] ^= 1
        expected[0x3FF100] ^= 1
        self.assertEqual(restored, bytes(expected))

    def test_restore_rejects_unexpected_bt_and_metadata(self):
        for address in (0x50010, 0x14F000, 0x10):
            current = bytearray(self.target)
            current[address] ^= 1
            with self.subTest(address=address), self.assertRaisesRegex(ValueError, 'Unexpected'):
                m.restore_target(bytes(current), self.source, self.target, self.sectors)

    def test_illegal_transaction_sector_rejected_before_io(self):
        session = FakeSession(self.source)
        with self.assertRaisesRegex(ValueError, 'Invalid lower BT sector'):
            m.transact(session, self.source, self.target, [0x270000], lambda *a, **kw: None)
        self.assertEqual(session.ops, [])

    def test_artifact_failure_closes_session_before_exit(self):
        session = FakeSession(self.source)
        @contextmanager
        def fake_open(port):
            try:
                yield session
            finally:
                session.closed = True
        with tempfile.TemporaryDirectory() as tmp:
            plan = {'payload_sectors': self.sectors}
            planpath = Path(tmp) / 'plan.json'
            planpath.write_text('{}')
            with patch.object(m, 'load_plan', return_value=(plan, self.source, self.target)), \
                 patch.object(m.bl, 'open_session', fake_open), \
                 patch.object(m, 'save_new', side_effect=OSError('Disk full')):
                with self.assertRaisesRegex(OSError, 'Disk full'):
                    m.execute('apply', planpath, 'FAKE-NOT-A-PORT', Path(tmp) / 'out')
            self.assertTrue(session.closed)
            self.assertFalse(any(op[0] in ('erase', 'write') for op in session.ops))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument('--backup', type=Path, help=f'External original full-flash backup (or {BACKUP_ENV})')
    options, unittest_args = parser.parse_known_args()
    if options.backup is not None:
        os.environ[BACKUP_ENV] = str(options.backup)
    unittest.main(argv=[sys.argv[0], *unittest_args], verbosity=2)
