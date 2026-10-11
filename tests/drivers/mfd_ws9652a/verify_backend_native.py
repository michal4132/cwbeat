#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the built ARM byte-register bridge without hardware.

Usage: python verify_backend_native.py /path/to/zephyr.elf
Requires capstone, pyelftools and unicorn. Peripheral responses are scripted;
this verifies instructions, bounded reads and ABI, not bus timing. Domain lock
entry points are modeled; real mutex/admission behavior is tested by the
register_bus_ws9652a and pmu_bridge_ws9652a QEMU suites.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

import capstone
import unicorn
from capstone import arm as ca
from elftools.elf.elffile import ELFFile
from unicorn import arm_const as ua

REGISTERS = {
    name: getattr(ua, "UC_ARM_REG_" + name.upper())
    for name in (
        "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11",
        "r12", "sp", "lr", "pc",
    )
}
REGISTERS.update(sb=ua.UC_ARM_REG_R9, sl=ua.UC_ARM_REG_R10,
                 fp=ua.UC_ARM_REG_R11, ip=ua.UC_ARM_REG_R12)
BUS, GATE = 0x40038000, 0x424001EC
BRIDGE_DEVICE, BRIDGE_CONFIG = 0x20012000, 0x20012100
DOMAIN_DEVICE, DOMAIN_CONFIG = 0x20012200, 0x20012300
STATE, OUTPUT = 0x20012400, 0x20012600
STACK, RETURN = 0x20018000, 0xF0000


def member_offset(dwarf, structure, member):
    """Locate the provider device without assuming Zephyr's device layout."""
    for unit in dwarf.iter_CUs():
        for entry in unit.iter_DIEs():
            name = entry.attributes.get("DW_AT_name")
            if entry.tag != "DW_TAG_structure_type" or not name or name.value != structure:
                continue
            for child in entry.iter_children():
                name = child.attributes.get("DW_AT_name")
                if child.tag == "DW_TAG_member" and name and name.value == member:
                    return child.attributes["DW_AT_data_member_location"].value
    raise AssertionError(f"missing DWARF member {structure!r}.{member!r}")


def verify_image(path):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        assert elf["e_machine"] == "EM_ARM"
        symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
        entry = symbols["ws9652a_pmu_bridge_read"]
        read_entry = entry & ~1
        write_entry = symbols["ws9652a_pmu_bridge_write"] & ~1
        lock_entry = symbols["ws9652a_bus_lock"] & ~1
        unlock_entry = symbols["ws9652a_bus_unlock"] & ~1
        segments = [(s["p_vaddr"], s.data()) for s in elf.iter_segments()
                    if s["p_type"] == "PT_LOAD" and s["p_filesz"]]
        dwarf = elf.get_dwarf_info()
        api_offset = member_offset(dwarf, b"device", b"api")
        config_offset = member_offset(dwarf, b"device", b"config")
        bridge_base_offset = member_offset(dwarf, b"ws9652a_pmu_bridge_config", b"base")
        bridge_bus_offset = member_offset(dwarf, b"ws9652a_pmu_bridge_config", b"bus")
        domain_gate_offset = member_offset(dwarf, b"ws9652a_bus_config", b"gate")
    decoder = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    decoder.detail = True
    decoded = {}

    def run(name, expected, c7=4, response="ok", control=0x401, null=False,
            basepri=0, primask=0, write=False, lock_error=0):
        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_THUMB | unicorn.UC_MODE_MCLASS)
        for address, size in ((0x40000, 0x40000), (RETURN, 0x1000),
                              (0x20000000, 0x20000), (0x40000000, 0x60000),
                              (0x42400000, 0x1000), (0xE0000000, 0x100000)):
            cpu.mem_map(address, size)
        for address, contents in segments:
            cpu.mem_write(address, contents)

        def put(address, value):
            cpu.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))

        def get(address):
            return struct.unpack("<I", cpu.mem_read(address, 4))[0]

        put(BRIDGE_DEVICE + config_offset, BRIDGE_CONFIG)
        put(BRIDGE_CONFIG + bridge_base_offset, BUS)
        put(BRIDGE_CONFIG + bridge_bus_offset, DOMAIN_DEVICE)
        put(DOMAIN_DEVICE + config_offset, DOMAIN_CONFIG)
        put(DOMAIN_CONFIG + domain_gate_offset, GATE)
        # Unrelated RAM must remain untouched by the byte engine.
        state = bytes((i * 17 + 3) & 255 for i in range(256))
        cpu.mem_write(STATE, state)
        put(OUTPUT, 0xA5A5A5A5)
        put(BUS, control)
        saved = {number: 0x76540000 + number * 0x1111 for number in range(4, 12)}
        for number, value in saved.items():
            cpu.reg_write(REGISTERS[f"r{number}"], value)
        for register, value in ((ua.UC_ARM_REG_R0, BRIDGE_DEVICE),
                                (ua.UC_ARM_REG_R1, 0x05 if write else 0xC7),
                                (ua.UC_ARM_REG_R2, 0x5A if write else 0 if null else OUTPUT),
                                (ua.UC_ARM_REG_SP, STACK), (ua.UC_ARM_REG_LR, RETURN | 1),
                                (ua.UC_ARM_REG_BASEPRI, basepri), (ua.UC_ARM_REG_PRIMASK, primask),
                                (ua.UC_ARM_REG_PC, (write_entry if write else entry) | 1)):
            cpu.reg_write(register, value)
        original_ram = bytes(cpu.mem_read(0x20000000, 0x20000))
        reads, writes, status_reads, calls, min_sp = [], [], 0, 0, STACK
        held, lock_calls, unlock_calls = False, 0, 0
        for steps in range(100000):
            pc = cpu.reg_read(ua.UC_ARM_REG_PC)
            if pc == RETURN:
                break
            min_sp = min(min_sp, cpu.reg_read(ua.UC_ARM_REG_SP))
            calls += pc == (write_entry if write else read_entry)
            # Model only the kernel mutex boundary. Gate access remains the
            # actual provider instructions, sharing the synthetic domain config.
            if pc in (lock_entry, unlock_entry):
                assert cpu.reg_read(ua.UC_ARM_REG_R0) == DOMAIN_DEVICE
                if pc == lock_entry:
                    assert not held and cpu.reg_read(ua.UC_ARM_REG_R1) == 0
                    lock_calls += 1
                    held = not lock_error
                    cpu.reg_write(ua.UC_ARM_REG_R0, lock_error & 0xFFFFFFFF)
                else:
                    assert held
                    unlock_calls += 1
                    held = False
                cpu.reg_write(ua.UC_ARM_REG_PC, cpu.reg_read(ua.UC_ARM_REG_LR))
                continue
            instruction = decoded.get(pc)
            if instruction is None:
                instruction = decoded[pc] = next(decoder.disasm(bytes(cpu.mem_read(pc, 4)), pc, count=1))
            address = None
            if instruction.mnemonic.startswith(("ldr", "str")) and instruction.operands[-1].type == ca.ARM_OP_MEM:
                operand = instruction.operands[-1]
                mem = operand.mem
                base = ((pc + 4) & ~3 if mem.base == ca.ARM_REG_PC
                        else cpu.reg_read(REGISTERS[instruction.reg_name(mem.base)]))
                index = cpu.reg_read(REGISTERS[instruction.reg_name(mem.index)]) if mem.index else 0
                if operand.shift.type == ca.ARM_SFT_LSL:
                    index <<= operand.shift.value
                elif operand.shift.type:
                    raise AssertionError((name, "unsupported address shift"))
                address = (base + mem.disp + index) & 0xFFFFFFFF
            mmio = address is not None and address >= 0x40000000
            if mmio:
                assert held, (name, "MMIO outside domain lock")
            mask = cpu.reg_read(ua.UC_ARM_REG_PRIMASK)
            control_before = get(BUS)
            if mmio and instruction.mnemonic.startswith("ldr"):
                assert address in (BUS, BUS + 8, BUS + 12), (name, "unexpected MMIO read", hex(address))
                if address == BUS + 12:
                    assert cpu.reg_read(ua.UC_ARM_REG_BASEPRI) == basepri
                    # Stock write checks the error bit after restoring PRIMASK.
                    assert mask == (primask if write and not control_before & 4 else 1)
                    if response == "ok":
                        value = 1 if status_reads == 0 else 0
                    elif response == "error":
                        value = 1 if status_reads == 0 else 2
                    else:
                        value = {"never_assert": 0, "never_clear": 1, "assert_timeout_error": 2}[response]
                    put(address, value)
                    status_reads += 1
                elif address == BUS + 8:
                    assert not write and mask == 1
                    put(address, c7)
                reads.append(address)
            cpu.emu_start(pc | 1, 0xFFFFFFFF, count=1)
            if mmio and instruction.mnemonic.startswith("str"):
                value = get(address)
                allowed = (BUS, BUS + 16, BUS + 20, GATE) + ((BUS + 4,) if write else ())
                assert address in allowed, (name, "unexpected MMIO write", hex(address))
                if address in (BUS + 4, BUS + 20):
                    assert mask == 1, (name, "unprotected data/index")
                elif address == BUS:
                    assert not value & (2 if write else 4), (name, "wrong request")
                    if (value ^ control_before) & 6:
                        assert mask == 1, (name, "unprotected request change")
                    else:
                        assert mask == primask, (name, "setup/cleanup mask")
                else:
                    assert mask == primask, (name, "setup/cleanup mask")
                writes.append((address, value))
        else:
            raise AssertionError((name, "unbounded execution"))
        assert cpu.reg_read(ua.UC_ARM_REG_R0) == expected, (name, "result")
        assert cpu.reg_read(ua.UC_ARM_REG_SP) == STACK, (name, "SP")
        assert cpu.reg_read(ua.UC_ARM_REG_BASEPRI) == basepri, (name, "BASEPRI restore")
        assert cpu.reg_read(ua.UC_ARM_REG_PRIMASK) == primask, (name, "PRIMASK restore")
        for number, value in saved.items():
            assert cpu.reg_read(REGISTERS[f"r{number}"]) == value, (name, "callee-saved", number)
        assert bytes(cpu.mem_read(STATE, 256)) == state, (name, "diagnostics altered")
        final_ram = bytes(cpu.mem_read(0x20000000, 0x20000))
        for offset, (before, after) in enumerate(zip(original_ram, final_ram)):
            address = 0x20000000 + offset
            assert before == after or STACK - 256 <= address < STACK or address == OUTPUT, (name, "unexpected RAM write", hex(address))
        assert calls == 1, (name, "must use one bridge transfer", calls)
        assert lock_calls == (0 if null else 1), (name, "lock calls")
        assert unlock_calls == (0 if null or lock_error else 1), (name, "unlock calls")
        assert not held, (name, "domain left locked")
        refused = null or bool(control & 6) or bool(lock_error)
        index = 0x05 if write else 0xC7
        assert [value for address, value in writes if address == BUS + 20] == ([] if refused else [index])
        assert [value for address, value in writes if address == BUS + 4] == ([0x5A] if write and not refused else [])
        if refused:
            assert not writes
            assert get(OUTPUT) == 0xA5A5A5A5
            if null or lock_error:
                assert not reads
        else:
            assert get(BUS) == control | 1
            assert get(GATE) == 0
            if write:
                assert get(OUTPUT) == 0xA5A5A5A5
            elif expected == 0:
                assert cpu.mem_read(OUTPUT, 1)[0] == c7
            else:
                assert get(OUTPUT) == 0xA5A5A5A5, (name, "failed read changed output")
            assert status_reads <= 6001
        return {"name": name, "pass": True, "result": expected, "instructions": steps,
                "status_reads": status_reads, "direct_calls": calls,
                "stack_bytes": STACK - min_sp, "lock_calls": lock_calls,
                "unlock_calls": unlock_calls, "mmio_writes": writes}

    def verify_clock_snapshot():
        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_THUMB | unicorn.UC_MODE_MCLASS)
        for address, size in ((0x40000, 0x40000), (RETURN, 0x1000),
                              (0x20000000, 0x20000), (0x40000000, 0x1000)):
            cpu.mem_map(address, size)
        for address, contents in segments:
            cpu.mem_write(address, contents)
        clock_devices = [address for symbol, address in symbols.items()
                         if symbol.startswith("__device_dts_ord_")
                         and struct.unpack("<I", cpu.mem_read(address + api_offset, 4))[0]
                         == symbols["ws9652a_clock_api"]]
        assert len(clock_devices) == 1, "expected one WS9652A clock provider in image"
        # Preserve every raw bit, including source bit 31 and reserved fields.
        samples = [(0x40000040, 0x80000010), (0x40000050, 0xABCDEF09),
                   (0x40000010, 0x76543210)]
        for address, value in samples:
            cpu.mem_write(address, struct.pack("<I", value))
        reads, writes = [], []

        for register, value in ((ua.UC_ARM_REG_R0, clock_devices[0]),
                                (ua.UC_ARM_REG_R1, OUTPUT),
                                (ua.UC_ARM_REG_SP, STACK), (ua.UC_ARM_REG_LR, RETURN | 1),
                                (ua.UC_ARM_REG_PC,
                                 symbols["ws9652a_clock_control_get_registers"] | 1)):
            cpu.reg_write(register, value)
        # Step instructions without Unicorn callbacks: libffi callback trampolines
        # are unavailable in some macOS/Nix environments used for this oracle.
        for steps in range(1000):
            pc = cpu.reg_read(ua.UC_ARM_REG_PC)
            if pc == RETURN:
                break
            instruction = next(decoder.disasm(bytes(cpu.mem_read(pc, 4)), pc, count=1))
            if instruction.mnemonic.startswith(("ldr", "str")) and instruction.operands[-1].type == ca.ARM_OP_MEM:
                operand = instruction.operands[-1]
                mem = operand.mem
                base = ((pc + 4) & ~3 if mem.base == ca.ARM_REG_PC
                        else cpu.reg_read(REGISTERS[instruction.reg_name(mem.base)]))
                index = cpu.reg_read(REGISTERS[instruction.reg_name(mem.index)]) if mem.index else 0
                if operand.shift.type == ca.ARM_SFT_LSL:
                    index <<= operand.shift.value
                elif operand.shift.type:
                    raise AssertionError(("clock_snapshot", "unsupported address shift"))
                address = (base + mem.disp + index) & 0xFFFFFFFF
                if address >= 0x40000000:
                    assert instruction.mnemonic in ("ldr", "ldr.w", "str", "str.w"), (
                        "clock_snapshot", "access width", instruction.mnemonic)
                    (reads if instruction.mnemonic.startswith("ldr") else writes).append(address)
            elif instruction.mnemonic.startswith(("ldm", "stm")):
                # A compiler may copy the three-word result with a RAM-only
                # multiple transfer. Peripheral snapshots must remain ordered
                # individual 32-bit accesses and are checked above.
                assert instruction.operands[0].type == ca.ARM_OP_REG
                base = cpu.reg_read(REGISTERS[instruction.reg_name(instruction.operands[0].reg)])
                assert base < 0x40000000, (
                    "clock_snapshot", "unexpected MMIO multiple access", instruction.mnemonic)
            cpu.emu_start(pc | 1, 0xFFFFFFFF, count=1)
        else:
            raise AssertionError(("clock_snapshot", "unbounded execution"))
        assert reads == [address for address, _ in samples], ("clock_snapshot", "read order", reads)
        assert not writes, ("clock_snapshot", "unexpected MMIO writes", writes)
        assert cpu.reg_read(ua.UC_ARM_REG_R0) == 0, ("clock_snapshot", "result")
        assert struct.unpack("<III", cpu.mem_read(OUTPUT, 12)) == tuple(value for _, value in samples)
        assert cpu.reg_read(ua.UC_ARM_REG_SP) == STACK, ("clock_snapshot", "SP")
        return {"name": "clock_raw_snapshot", "pass": True, "instructions": steps,
                "mmio_reads": reads}

    cases = [
        ("byte_04", 0, {}), ("byte_e7", 0, {"c7": 0xE7}),
        ("byte_00", 0, {"c7": 0}), ("byte_08", 0, {"c7": 8}),
        ("byte_0c", 0, {"c7": 12}), ("null_output", 0xB01, {"null": True}),
        ("lock_busy", 0xB02, {"lock_error": -16}),
        ("lock_before_kernel", 0xB02, {"lock_error": -11}),
        ("lock_isr", 0xB02, {"lock_error": -11}),
        ("write_lock_busy", 0xB02, {"write": True, "lock_error": -16}),
        ("read_busy", 0xB02, {"control": 0x403}), ("write_busy", 0xB02, {"control": 0x405}),
        ("status_error", 0xB03, {"response": "error"}),
        ("assert_timeout", 0xB00, {"response": "never_assert"}),
        ("clear_timeout", 0xB00, {"response": "never_clear"}),
        ("error_after_assert_timeout", 0xB03, {"response": "assert_timeout_error"}),
        ("nested_mask", 0, {"basepri": 0x10}), ("weaker_mask", 0, {"basepri": 0x40}),
        ("primask", 0, {"primask": 1}),
        ("write_ok", 0, {"write": True}),
        ("write_status_error", 0xB03, {"write": True, "response": "error"}),
        ("write_assert_timeout", 0xB00, {"write": True, "response": "never_assert"}),
        ("write_clear_timeout", 0xB00, {"write": True, "response": "never_clear"}),
        ("write_read_busy", 0xB02, {"write": True, "control": 0x403}),
        ("write_write_busy", 0xB02, {"write": True, "control": 0x405}),
        ("write_nested_mask", 0, {"write": True, "basepri": 0x10}),
        ("write_primask", 0, {"write": True, "primask": 1}),
    ]
    results = []
    for name, expected, kwargs in cases:
        result = run(name, expected, **kwargs)
        results.append(result)
        print(f"PASS {name}: result=0x{expected:x}, direct calls={result['direct_calls']}")
    results.append(verify_clock_snapshot())
    print("PASS clock_raw_snapshot: raw fields, ordered reads, no MMIO writes")
    return {"elf": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "helper_address": hex(entry),
            "method": "Actual ARM bridge and gate instructions; modeled domain mutex boundary; scripted MMIO; no timing/electrical model",
            "cases": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    result = verify_image(args.elf)
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n")
