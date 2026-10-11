#!/usr/bin/env python3
"""Audit the exact tested BT patch against an external verified flash backup.

Native reset reconstructs initialized RAM from that backup. The 30-case audit
executes original and patched instructions offline; it cannot establish physical
PLL lock or USB electrical behavior. No firmware images are written.
"""

import argparse
import gc
import json
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from capstone.arm import ARM_OP_IMM, ARM_OP_MEM, ARM_REG_PC
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn.arm_const import *

sys.dont_write_bytecode = True
from build import external_path, load_manifest, sha256, verify_build

BASE, LENGTH, HOOK, CAVE = 0x50000, 0xDD000, 0x7C4A4, 0xD8000
RECORD = bytes.fromhex(
    "00b0710b0404040000b0710b80ba8c013307800000504140000f10120200000000000400"
)
RETURN = 0x23000000
INITIALIZED_RAM_SHA256 = (
    "b12ac7a7b265265ab76b5d446ad20012192222d1b0a607c15b8fd11d0ea060e5"
)
REGS = {f"r{i}": globals()[f"UC_ARM_REG_R{i}"] for i in range(13)}
REGS.update(
    sp=UC_ARM_REG_SP,
    lr=UC_ARM_REG_LR,
    pc=UC_ARM_REG_PC,
    ip=UC_ARM_REG_R12,
    fp=UC_ARM_REG_R11,
    sl=UC_ARM_REG_R10,
    sb=UC_ARM_REG_R9,
)
cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
cs.detail = True
cs.skipdata = True


def word(u, address):
    return int.from_bytes(u.mem_read(address, 4), "little")


def put(u, address, value):
    u.mem_write(address, struct.pack("<I", value))


def environment(original, initialized_ram=None):
    u = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    for address, length in (
        (0, 0x100000),
        (0x400000, 0x10000),
        (0x20000000, 0x50000),
        (0x21000000, 0x2000),
        (RETURN, 0x1000),
        (0x40000000, 0x80000),
        (0x42000000, 0x1000000),
        (0xE000E000, 0x2000),
    ):
        u.mem_map(address, length)
    u.mem_write(0, original)
    u.mem_write(0x400000, original[0xC7EA0:0xD7EA0])
    if initialized_ram is not None:
        u.mem_write(0x20020000, initialized_ram)
    return u


def restore_native_ram(original):
    """Execute the unmodified reset/scatter dispatcher, stopping before main."""
    u = environment(original)
    u.mem_write(0x20020000, b"\xa5" * 0x30000)
    u.reg_write(UC_ARM_REG_SP, word(u, 0))
    u.emu_start(word(u, 4), 0x138, count=2000000)
    assert u.reg_read(UC_ARM_REG_PC) == 0x138, "Native scatter did not finish"
    assert bytes(u.mem_read(0x20021DF4, 0x2AED4)) == b"\0" * 0x2AED4
    assert bytes(u.mem_read(0x20029628, 1)) == b"\0"
    initialized_ram = bytes(u.mem_read(0x20020000, 0x1DF4))
    assert sha256(initialized_ram) == INITIALIZED_RAM_SHA256
    assert bytes(u.mem_read(0, len(original))) == original
    del u
    gc.collect()
    return initialized_ram


def audit_padding(original, stub):
    refs = []
    for data, base in [(original[:0xC7EA0], 0), (original[0xC7EA0:0xD50E8], 0x400000)]:
        for ins in cs.disasm(data, base):
            if not ins.id:
                continue
            if ins.mnemonic.startswith(("b", "cb")):
                for op in ins.operands:
                    if op.type == ARM_OP_IMM and CAVE <= (op.imm & ~1) < CAVE + len(
                        stub
                    ):
                        refs.append(
                            dict(
                                kind="direct_branch",
                                pc=hex(ins.address),
                                target=hex(op.imm),
                            )
                        )
            for op in ins.operands:
                if op.type == ARM_OP_MEM and op.mem.base == ARM_REG_PC:
                    a = ((ins.address + 4) & ~3) + op.mem.disp
                    if CAVE <= a < CAVE + len(stub):
                        refs.append(
                            dict(
                                kind="pc_relative_reference",
                                pc=hex(ins.address),
                                target=hex(a),
                            )
                        )
    rawrefs = []
    for a in range(len(original) - 3):
        v = struct.unpack_from("<I", original, a)[0]
        if CAVE <= (v & ~1) < CAVE + len(stub):
            rawrefs.append(dict(offset=hex(a), value=hex(v)))
    assert not refs and not rawrefs
    return dict(
        begin="0xd50e8",
        end="0xdcffc",
        stub=[hex(CAVE), hex(CAVE + len(stub))],
        all_ff=True,
        secondary_source=[hex(0xC7EA0), hex(0xD7EA0)],
        direct_references=refs,
        raw_pointer_references=rawrefs,
        limitation="Static no-reference evidence and outside restored secondary code source; cannot exclude arbitrary computed references or independently prove hardware XIP permissions",
    )


DEFAULT = {
    0x40000040: 0x80000610,
    0x40000050: 5,
    0x40000010: 0x101,
    0x400000A4: 0,
    0x40030000: 0x14,
    0x40028000: 0xFDE20628,
    0x4002802C: 0x800733,
    0x4002803C: 0x40415000,
    0x40028040: 0x12100F00,
    0x40028048: 0xEC4E0800,
}
STATE = 0x20029614
REC = 0x200295F0
CALLEE = [
    UC_ARM_REG_R4,
    UC_ARM_REG_R5,
    UC_ARM_REG_R6,
    UC_ARM_REG_R7,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
]


def run(original, patched, initialized_ram, variant, case):
    u = environment(original, initialized_ram)
    if variant:
        u.mem_write(0, bytes(patched))
    record = bytearray(RECORD)
    for a, v in case.get("record", {}).items():
        record[a] = v
    u.mem_write(REC, bytes(record))
    u.mem_write(STATE, b"\x00" * 24)
    u.mem_write(STATE + 20, bytes([case.get("initialized", 0)]))
    seeds = {**DEFAULT, **case.get("registers", {})}
    for a, v in seeds.items():
        put(u, a, v)
    u.reg_write(UC_ARM_REG_SP, 0x21002000)
    u.reg_write(UC_ARM_REG_LR, RETURN | 1)
    u.reg_write(UC_ARM_REG_R0, case.get("alternate", 0))
    u.reg_write(UC_ARM_REG_R1, REC)
    for i, r in enumerate(CALLEE):
        u.reg_write(r, 0xA1100000 + i)
    u.reg_write(UC_ARM_REG_PRIMASK, case.get("primask", 0))
    writes = []
    sram_writes = []
    paths = set()
    count = 0
    guard_boundaries = {}

    def on_code(uc, address, size, _):
        nonlocal count
        count += 1
        if address in [HOOK, 0x7C4A8, 0x7C4B8, 0x7C310, 0x7C542, 0xD80A6, 0xD80AA]:
            paths.add(address)
        if address in [HOOK, 0x7C4A8, 0x7C4B8]:
            guard_boundaries[hex(address)] = dict(
                regs=[uc.reg_read(r) for r in CALLEE],
                sp=uc.reg_read(UC_ARM_REG_SP),
                primask=uc.reg_read(UC_ARM_REG_PRIMASK),
            )
        if variant and address == 0xD808C and case.get("change_source_during_guard"):
            put(uc, 0x40000040, word(uc, 0x40000040) & ~0x10)
        if variant and address == 0xD809A and case.get("change_select_during_guard"):
            put(uc, 0x40000050, 1)

    def on_read(uc, access, address, size, value, _):
        if 0x42000000 <= address < 0x43000000:
            delta = address - 0x42000000
            byte = 0x40000000 + delta // 32
            bit = (delta % 32) // 4
            actual = (int.from_bytes(uc.mem_read(byte, 1), "little") >> bit) & 1
            put(uc, address, actual)

    def on_write(uc, access, address, size, value, _):
        if 0x40000000 <= address < 0x40080000 or 0x42000000 <= address < 0x43000000:
            writes.append(
                dict(
                    pc=hex(uc.reg_read(UC_ARM_REG_PC)),
                    address=hex(address),
                    value=hex(value),
                    size=size,
                )
            )
            if 0x42000000 <= address < 0x43000000:
                delta = address - 0x42000000
                byte = 0x40000000 + delta // 32
                bit = (delta % 32) // 4
                old = int.from_bytes(uc.mem_read(byte, 1), "little")
                new = (old & ~(1 << bit)) | ((value & 1) << bit)
                uc.mem_write(byte, bytes([new]))
        elif 0x20000000 <= address < 0x20050000:
            sram_writes.append(
                dict(
                    pc=hex(uc.reg_read(UC_ARM_REG_PC)),
                    address=hex(address),
                    value=hex(value),
                    size=size,
                )
            )

    # Single-step avoids macOS libffi callback allocation failures in Unicorn hooks.
    pc = 0x7C484
    for steps in range(600000):
        ins = next(cs.disasm(bytes(u.mem_read(pc, 4)), pc, count=1))
        on_code(u, pc, ins.size, None)
        if ins.mnemonic.startswith(("ldr", "str")):
            for op in ins.operands:
                if op.type != ARM_OP_MEM:
                    continue
                base = (
                    ((pc + 4) & ~3)
                    if op.mem.base == ARM_REG_PC
                    else u.reg_read(REGS[ins.reg_name(op.mem.base)])
                )
                a = base + op.mem.disp
                if op.mem.index:
                    a += u.reg_read(REGS[ins.reg_name(op.mem.index)]) << op.shift.value
                width = (
                    1
                    if ins.mnemonic.startswith(("ldrb", "strb"))
                    else 2
                    if ins.mnemonic.startswith(("ldrh", "strh"))
                    else 8
                    if ins.mnemonic.startswith(("ldrd", "strd"))
                    else 4
                )
                if ins.mnemonic.startswith("ldr"):
                    on_read(u, 0, a, width, 0, None)
                else:
                    value = u.reg_read(REGS[ins.reg_name(ins.operands[0].reg)])
                    if width == 8:
                        value |= (
                            u.reg_read(REGS[ins.reg_name(ins.operands[1].reg)]) << 32
                        )
                    on_write(u, 0, a, width, value, None)
        u.emu_start(pc | 1, RETURN, count=1)
        pc = u.reg_read(UC_ARM_REG_PC)
        if pc == RETURN:
            break
    else:
        raise AssertionError("bounded instruction limit exceeded")
    assert u.reg_read(UC_ARM_REG_PC) == RETURN, (
        case["name"],
        hex(u.reg_read(UC_ARM_REG_PC)),
    )
    assert bytes(u.mem_read(0, len(original))) == (
        bytes(patched) if variant else original
    )
    restored = [u.reg_read(r) for r in CALLEE]
    assert restored == [0xA1100000 + i for i in range(8)]
    assert u.reg_read(UC_ARM_REG_SP) == 0x21002000
    assert u.reg_read(UC_ARM_REG_PRIMASK) == case.get("primask", 0)
    adopted = 0xD80A6 in paths
    if adopted:
        assert guard_boundaries[hex(HOOK)] == guard_boundaries[hex(0x7C4B8)]
    elif variant:
        assert guard_boundaries[hex(HOOK)] == guard_boundaries[hex(0x7C4A8)]
    result = dict(
        instructions=count,
        adopted=adopted,
        paths=[hex(x) for x in sorted(paths)],
        writes=writes,
        sram_writes=sram_writes,
        state_words=[word(u, STATE + i) for i in range(0, 24, 4)],
        final_mmio={hex(a): hex(word(u, a)) for a in seeds},
        primask=u.reg_read(UC_ARM_REG_PRIMASK),
        callee_preserved=True,
    )
    del u
    gc.collect()
    return result


cases = [
    dict(name="cold_match", adopt=True),
    dict(name="cold_match_primask1", primask=1, adopt=True),
    dict(name="alternate_match", alternate=1, adopt=True),
    dict(
        name="dynamic_unrelated_pll48_bits",
        registers={0x40028048: 0xABCD0800},
        adopt=True,
    ),
    dict(name="warm_initialized", initialized=1, adopt=False),
    dict(name="source_pll_off", registers={0x40000040: 0x80000600}, adopt=False),
    dict(name="source_ready_off_timeout", registers={0x40000040: 0x610}, adopt=False),
    dict(name="source_bit2", registers={0x40000040: 0x80000614}, adopt=False),
    dict(name="source_bit3", registers={0x40000040: 0x80000618}, adopt=False),
    dict(name="select_already192", registers={0x40000050: 0x201}, adopt=False),
    dict(name="divider_mismatch", registers={0x40000010: 0x100}, adopt=False),
    dict(name="a4_bit0", registers={0x400000A4: 1}, adopt=False),
    dict(name="a4_bit1", registers={0x400000A4: 2}, adopt=False),
    dict(
        name="pll_bit9_off",
        registers={0x40028000: DEFAULT[0x40028000] & ~0x200},
        adopt=False,
    ),
    dict(
        name="pll_bit10_off",
        registers={0x40028000: DEFAULT[0x40028000] & ~0x400},
        adopt=False,
    ),
]
for a in [0x4002802C, 0x4002803C, 0x40028040, 0x40028048]:
    cases.append(
        dict(
            name=f"analog_mismatch_{a:x}",
            registers={a: DEFAULT[a] ^ 0x400},
            adopt=False,
        )
    )
for offset in range(0, 36, 4):
    cases.append(
        dict(
            name=f"record_word_{offset:x}_mismatch",
            record={offset: RECORD[offset] ^ 1},
            adopt=False,
        )
    )
cases += [
    dict(
        name="source_changes_during_guard", change_source_during_guard=True, adopt=False
    ),
    dict(
        name="select_changes_during_guard", change_select_during_guard=True, adopt=False
    ),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--backup",
        type=Path,
        required=True,
        help="Original verified 4 MiB flash backup, outside repository",
    )
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument(
        "--output", type=Path, required=True, help="External JSON proof path"
    )
    args = parser.parse_args()
    if not __debug__:
        parser.error("Run without -O: instruction-proof assertions must remain enabled")
    backup = external_path(args.backup, "Firmware backup")
    build_dir = external_path(args.build_dir, "Build output")
    output = external_path(args.output, "Proof output")
    if output == backup or output in (build_dir / "stub.elf", build_dir / "stub.bin"):
        parser.error("Proof output must not overwrite an input artifact")
    manifest = load_manifest()
    blob = backup.read_bytes()
    assert len(blob) == 0x400000, "Expected full 4 MiB backup"
    assert sha256(blob) == manifest["source_flash_sha256"], (
        "Original flash SHA mismatch"
    )
    original = blob[BASE : BASE + LENGTH]
    assert sha256(original) == manifest["source_bt_sha256"], "Original BT SHA mismatch"
    hook, stub = verify_build(build_dir / "stub.elf", manifest)
    patched = bytearray(original)
    for patch in manifest["patches"]:
        offset = patch["cpu_offset"]
        old, new = bytes.fromhex(patch["old_hex"]), bytes.fromhex(patch["new_hex"])
        assert original[offset : offset + len(old)] == old, "Patch preimage mismatch"
        patched[offset : offset + len(new)] = new
    assert sha256(patched) == manifest["patched_bt_sha256_before_metadata"]
    assert blob[0x1B5462 : 0x1B5462 + 36] == RECORD
    assert blob[0x208462 : 0x208462 + 36] == RECORD
    padding = audit_padding(original, stub)
    initialized_ram = restore_native_ram(original)
    results = []
    for case in cases:
        print(case["name"], flush=True)
        before = run(original, patched, initialized_ram, False, case)
        after = run(original, patched, initialized_ram, True, case)
        assert after["adopted"] == case["adopt"], case["name"]
        assert before["state_words"] == after["state_words"], case["name"]
        assert before["primask"] == after["primask"]
        if not case["adopt"]:
            assert before["writes"] == after["writes"], case["name"]
            assert before["sram_writes"] == after["sram_writes"], case["name"]
        else:
            assert after["state_words"] == [
                192000000,
                48000000,
                48000000,
                1000000,
                48000000,
                1,
            ]
            assert not any(
                0x40028000 <= int(w["address"], 16) < 0x40028060
                or 0x42500000 <= int(w["address"], 16) < 0x42501000
                for w in after["writes"]
            )
            assert [
                w["value"] for w in after["writes"] if w["address"] == "0x42000810"
            ] == ["0x1"]
            assert [
                w["value"] for w in after["writes"] if w["address"] == "0x40000050"
            ] == ["0x201"]
            assert [
                w["value"] for w in after["writes"] if w["address"] == "0x40000010"
            ] == ["0x101"]
            # Native original has exactly the preamble, two source-record writes,
            # fresh analog initializer, then final requested-rate path. Compare every
            # retained MMIO write (address, value, PC, size), including flash timing.
            old = before["writes"]
            assert len(old) == 22
            assert [(w["address"], w["value"]) for w in old[1:5]] == [
                ("0x42001480", "0x0"),
                ("0x42001484", "0x0"),
                ("0x42000810", "0x0"),
                ("0x40000010", "0x100"),
            ]
            assert all(0x7C310 <= int(w["pc"], 16) < 0x7C364 for w in old[7:16])
            assert after["writes"] == old[:1] + old[5:7] + old[16:]

        results.append(dict(case=case, original=before, patched=after))
    report = dict(
        all_assertions_passed=True,
        cases=len(results),
        padding=padding,
        source_flash_sha256=sha256(blob),
        source_bt_sha256=sha256(original),
        patched_bt_sha256=sha256(patched),
        initialized_ram_sha256=sha256(initialized_ram),
        initialized_ram_source="Native reset/scatter instructions from verified backup",
        hook_hex=hook.hex(),
        stub_bytes=len(stub),
        emulator_limitations="Models bit-band backing and native instructions. PLL readiness is synthetic; does not establish electrical lock or live SELECT5-to201 glitchlessness. No hardware access.",
        results=results,
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(dict(proof=str(output), cases=len(results), passed=True)))


if __name__ == "__main__":
    main()
