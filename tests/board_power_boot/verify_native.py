#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Execute the built Go 4 boot gate with scripted GPIO/counter responses.

Usage: python verify_native.py /path/to/zephyr.elf [--json results.json]
Requires capstone, pyelftools and unicorn. Uses the actual board, GPIO, pinctrl,
peer and watchdog ARM instructions; checks sequencing, not physical behavior.
"""
import argparse
import errno
import hashlib
import itertools
import json
import struct
from pathlib import Path

import capstone
from capstone import arm as decoded_arm
from elftools.elf.elffile import ELFFile
import unicorn
from unicorn import arm_const as arm

GPIO = 0x40014000
GPIO_INPUT = GPIO + 0x10
GPIO_PULL_DOWN = GPIO + 0x0C
POWER_ALIAS = GPIO + 0x100
COUNTER = 0xE0001004
BT = 0x40000080
DSP = 0x4004C000
WATCHDOG = 0x40018000
CONTROLLER = 0x40020008
POLICY = 0x4002D03C
STACK, RETURN = 0x20020000, 0xF0000
REGISTERS = {name: getattr(arm, "UC_ARM_REG_" + name.upper())
             for name in ("r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8",
                          "r9", "r10", "r11", "r12", "sp", "lr", "pc")}
REGISTERS.update(sb=arm.UC_ARM_REG_R9, sl=arm.UC_ARM_REG_R10,
                 fp=arm.UC_ARM_REG_R11, ip=arm.UC_ARM_REG_R12)


def member_offset(dwarf, structure, member):
    """Read the built image's layout rather than hardcoding Zephyr's device ABI."""
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
        segments = [(s["p_vaddr"], s.data()) for s in elf.iter_segments()
                    if s["p_type"] == "PT_LOAD" and s["p_filesz"]]
        api_offset = member_offset(elf.get_dwarf_info(), b"device", b"api")
    decoder = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    decoder.detail = True
    decoded = {}

    def run(name, samples=(1, 1, 1), latch=1, frozen=False, unavailable=False,
            bad_clock=False, dsp_ack=True, gpio_unavailable=False, clock_unavailable=False,
            early_emergency=False):
        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_THUMB | unicorn.UC_MODE_MCLASS)
        for address, size in ((0x40000, 0x40000), (RETURN, 0x1000),
                              (0x20000000, 0x20000), (0x40000000, 0x60000),
                              (0xE0000000, 0x100000)):
            cpu.mem_map(address, size)
        for address, contents in segments:
            cpu.mem_write(address, contents)
        if early_emergency:
            # No BSS clearing, data copy or device initialization has occurred.
            cpu.mem_write(0x20000000, bytes([0xA5]) * 0x20000)

        def put(address, value):
            cpu.mem_write(address, struct.pack("<I", value & 0xFFFFFFFF))

        def get(address):
            return struct.unpack("<I", cpu.mem_read(address, 4))[0]

        pattern = 0xA5A5A5A5
        for address in (GPIO, GPIO + 4, GPIO + 8, GPIO + 12, GPIO + 0x14,
                        GPIO + 0x30, GPIO + 0x38, GPIO + 0x3C,
                        BT, WATCHDOG, WATCHDOG + 0x3C, WATCHDOG + 0x40,
                        CONTROLLER, POLICY):
            put(address, pattern)
        put(GPIO, (pattern & ~1) | latch)
        # Start with the Power input pull-down disabled: the DT input profile
        # must explicitly enable it before the first boot-policy sample.
        put(GPIO_PULL_DOWN, pattern & ~1)
        put(BT, pattern | 0xC)
        put(WATCHDOG, pattern | 0x1000)
        put(DSP + 12, 0x10 if dsp_ack else 0)
        put(0x40000040, 0x10 if bad_clock else 0)
        put(0x40000050, 15 if bad_clock else 0)
        put(0xE0001000, 1 << 25 if unavailable else 0)

        reads, writes, counter_reads = [], [], 0

        def memory_read(address):
            nonlocal counter_reads
            if address == GPIO_INPUT:
                assert len(reads) < 3, (name, "too many input samples")
                assert get(GPIO + 4) & 1, (name, "Power is not an input before sampling")
                assert get(GPIO_PULL_DOWN) & 1, (name, "Power pull-down missing before sampling")
                assert get(GPIO) & 1 == latch, (name, "power latch changed before boot decision")
                put(address, samples[len(reads)])
                reads.append(counter_reads)
            elif address == COUNTER:
                counter_reads += 1
                put(address, 0 if frozen else counter_reads * 100)

        def memory_write(address, value):
            if address >= 0x40000000:
                writes.append((address, value))
                if address == POWER_ALIAS:
                    put(GPIO, (get(GPIO) & ~1) | (value & 1))

        def execute(function, argument=0):
            for register, value in ((arm.UC_ARM_REG_SP, STACK),
                                    (arm.UC_ARM_REG_LR, RETURN | 1),
                                    (arm.UC_ARM_REG_R0, argument),
                                    (arm.UC_ARM_REG_PRIMASK, 1),
                                    (arm.UC_ARM_REG_BASEPRI, 0x20),
                                    (arm.UC_ARM_REG_PC, symbols[function] | 1)):
                cpu.reg_write(register, value)
            for _ in range(100000):
                pc = cpu.reg_read(arm.UC_ARM_REG_PC)
                if pc == RETURN:
                    break
                instruction = decoded.get(pc)
                if instruction is None:
                    instruction = decoded[pc] = next(decoder.disasm(bytes(cpu.mem_read(pc, 4)), pc, count=1))
                address = None
                if instruction.mnemonic.startswith(("ldr", "str")) and instruction.operands[-1].type == decoded_arm.ARM_OP_MEM:
                    operand = instruction.operands[-1]
                    mem = operand.mem
                    base = ((pc + 4) & ~3 if mem.base == decoded_arm.ARM_REG_PC
                            else cpu.reg_read(REGISTERS[instruction.reg_name(mem.base)]))
                    index = cpu.reg_read(REGISTERS[instruction.reg_name(mem.index)]) if mem.index else 0
                    if operand.shift.type == decoded_arm.ARM_SFT_LSL:
                        index <<= operand.shift.value
                    else:
                        assert not operand.shift.type
                    address = (base + mem.disp + index) & 0xFFFFFFFF
                if early_emergency and address is not None:
                    assert not 0x20000000 <= address < STACK - 0x1000, (
                        name, "early hold accessed application data/BSS", hex(address))
                mmio = address is not None and address >= 0x40000000
                if mmio and instruction.mnemonic.startswith("ldr"):
                    memory_read(address)
                cpu.emu_start(pc | 1, 0xFFFFFFFF, count=1)
                if mmio and instruction.mnemonic.startswith("str"):
                    memory_write(address, get(address))
            assert cpu.reg_read(arm.UC_ARM_REG_PC) == RETURN, (name, function, "unbounded execution")
            assert cpu.reg_read(arm.UC_ARM_REG_SP) == STACK, (name, function, "stack balance")
            assert cpu.reg_read(arm.UC_ARM_REG_PRIMASK) == 1, (name, "PRIMASK changed")
            assert cpu.reg_read(arm.UC_ARM_REG_BASEPRI) == 0x20, (name, "BASEPRI changed")
            return cpu.reg_read(arm.UC_ARM_REG_R0)

        if early_emergency:
            assert execute("ws9652a_peer_hold_emergency") == int(dsp_ack)
            assert writes[:5] == [
                (BT, pattern & ~0xC),
                (WATCHDOG + 0x40, pattern & ~1),
                (WATCHDOG + 0x3C, pattern | 8),
                (WATCHDOG, pattern & ~0x1000),
                (WATCHDOG + 0x40, pattern | 1),
            ], (name, "BT/watchdog stop order or unrelated bits changed")
            assert writes[5][0] == DSP + 8, (name, "DSP hold must follow watchdog stop")
            assert not reads and not counter_reads
            return {"name": name, "pass": True, "dsp_ack": dsp_ack,
                    "watchdog_stopped_before_device_init": True}

        watchdog_devices = [address for symbol, address in symbols.items()
                            if symbol.startswith("__device_dts_ord_")
                            and get(address + api_offset) == symbols["ws9652a_wdt_api"]]
        assert len(watchdog_devices) == 1, "expected one WS9652A watchdog in image"
        assert execute("do_device_init", watchdog_devices[0]) == 0
        assert execute("z_impl_device_is_ready", watchdog_devices[0]) == 1
        assert not writes and not reads and not counter_reads, "watchdog init changed hardware"

        clock_devices = [address for symbol, address in symbols.items()
                         if symbol.startswith("__device_dts_ord_")
                         and get(address + api_offset) == symbols["ws9652a_clock_api"]]
        assert len(clock_devices) == 1, "expected one WS9652A clock provider in image"
        if not clock_unavailable:
            # The board consumes the PRE_KERNEL_1 clock provider. Initialize
            # its real device state just as for GPIO, without ABI assumptions.
            assert execute("do_device_init", clock_devices[0]) == 0
            assert execute("z_impl_device_is_ready", clock_devices[0]) == 1
            assert not writes and not reads and not counter_reads, "clock init changed hardware"
        gpio_devices = [address for symbol, address in symbols.items()
                        if symbol.startswith("__device_dts_ord_")
                        and get(address + api_offset) == symbols["ws9652a_gpio_api"]]
        assert gpio_devices, "no WS9652A GPIO devices in image"
        if not gpio_unavailable:
            # Run Zephyr's real device initialization, as PRE_KERNEL_1 has done
            # before the board gate. GPIO data starts at zero in mapped BSS;
            # do_device_init sets readiness without hardcoding state bitfields.
            for device in gpio_devices:
                assert execute("do_device_init", device) == 0
                assert execute("z_impl_device_is_ready", device) == 1
            assert not writes and not reads and not counter_reads, "GPIO initialization changed hardware"
        result = execute("go4_board_init")
        if gpio_unavailable or clock_unavailable:
            assert result == (-errno.ENODEV & 0xFFFFFFFF), (name, "unavailable provider return")
            assert cpu.mem_read(symbols["boot_requested"], 1)[0] == 0
            assert not writes and not reads and not counter_reads, (
                name, "unavailable provider touched hardware")
            assert get(GPIO) == (pattern & ~1) | latch
            return {"name": name, "pass": True, "boot_requested": False,
                    "input_reads": 0, "counter_reads": 0, "return_code": -errno.ENODEV}
        assert result == 0, (name, "board initialization return")
        expected = all(samples) and not (frozen or unavailable or bad_clock)
        assert cpu.mem_read(symbols["boot_requested"], 1)[0] == expected, (name, "boot policy")
        latch_writes = [(i, value) for i, (address, value) in enumerate(writes)
                        if address == POWER_ALIAS]
        assert [value for _, value in latch_writes] == [latch, int(expected)], (name, "latch sequencing")
        assert get(GPIO) == (pattern & ~1) | int(expected), (name, "other latches changed")
        assert get(GPIO + 4) == pattern | 1
        assert get(GPIO + 8) == pattern & ~1
        assert get(GPIO_PULL_DOWN) == pattern | 1
        assert not any(0x40038000 <= a < 0x40038018 for a, _ in writes), (name, "PMU transaction")
        if not expected:
            assert get(BT) & 0xC == 0
            assert get(WATCHDOG) & 0x1000 == 0
            assert get(CONTROLLER) & 0xC == 0
            assert get(POLICY) & 0xE == 2
            assert get(DSP + 8) & 0x3F == 1
            stop_writes = [i for i, (a, _) in enumerate(writes)
                           if a in (BT, CONTROLLER, POLICY, DSP + 8)]
            assert max(stop_writes) < latch_writes[-1][0], (name, "latch released before stopping peers")
        else:
            assert not any(a in (BT, CONTROLLER, POLICY, DSP + 8) for a, _ in writes)
            assert get(WATCHDOG) == pattern | 0x1000, (name, "boot gate stopped watchdog early")
        if not (frozen or unavailable or bad_clock):
            assert len(reads) == 3
            assert reads[1] - reads[0] >= 4 and reads[2] - reads[1] >= 4
        assert counter_reads <= 4100, (name, "unbounded counter")
        return {"name": name, "pass": True, "boot_requested": bool(expected),
                "input_reads": len(reads), "counter_reads": counter_reads}

    results = [run("early_emergency_hold", early_emergency=True),
               run("early_emergency_no_ack", early_emergency=True, dsp_ack=False)]
    for samples in itertools.product((0, 1), repeat=3):
        for latch in (0, 1):
            results.append(run("samples_" + "".join(map(str, samples)) + f"_latch_{latch}",
                               samples=samples, latch=latch))
    for name, arguments in (("counter_frozen", {"frozen": True}),
                            ("counter_unavailable", {"unavailable": True}),
                            ("clock_invalid", {"bad_clock": True}),
                            ("missing_dsp_ack", {"samples": (0, 0, 0), "dsp_ack": False}),
                            ("gpio_unavailable", {"gpio_unavailable": True}),
                            ("clock_unavailable", {"clock_unavailable": True})):
        results.append(run(name, **arguments))
    for result in results:
        print("PASS", result["name"])
    return {"elf": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "method": "Actual ARM instructions; scripted GPIO/counter MMIO; no electrical model",
            "cases": results}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("--json", type=Path)
    arguments = parser.parse_args()
    report = verify_image(arguments.elf)
    if arguments.json:
        arguments.json.write_text(json.dumps(report, indent=2) + "\n")
