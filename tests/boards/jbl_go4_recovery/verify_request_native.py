#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the linked ARM recovery request, without hardware or flash writes.

Usage: python verify_request_native.py /path/to/zephyr.elf
Requires capstone, pyelftools and Unicorn. Debug registers are ordinary emulated
memory. The peer-hold boundary is acknowledged by the harness; its existing
hardware sequence and actual reset-domain retention require hardware validation.
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path

import capstone
from capstone import arm as ca
from elftools.elf.elffile import ELFFile
import unicorn
from unicorn import arm_const as ua

COOKIE = 0x3152424A
DHCSR, DCRDR, DEMCR = 0xE000EDF0, 0xE000EDF8, 0xE000EDFC
USB_POWER = 0x40048004
STACK, RETURN = 0x20010000, 0xF0000
WORD = struct.Struct("<I")
REGISTERS = {name: getattr(ua, "UC_ARM_REG_" + name.upper())
             for name in ("r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                          "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc")}
REGISTERS.update(sb=ua.UC_ARM_REG_R9, sl=ua.UC_ARM_REG_R10,
                 fp=ua.UC_ARM_REG_R11, ip=ua.UC_ARM_REG_R12)


def audit(path):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        assert elf["e_machine"] == "EM_ARM"
        symbols = {s.name: s["st_value"] for s in elf.get_section_by_name(".symtab").iter_symbols()}
        entry = symbols["recovery_request_immediate"]
        detach, hold, reset = (symbols[name] & ~1 for name in
                               ("ws9652a_usb_detach_emergency",
                                "ws9652a_peer_hold_emergency", "ws9652a_reset_immediate"))
        segments = [(s["p_vaddr"], s.data()) for s in elf.iter_segments()
                    if s["p_type"] == "PT_LOAD" and s["p_filesz"]]
    decoder = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB | capstone.CS_MODE_MCLASS)
    decoder.detail = True
    decoded = {}

    def run(name, expected=None, primask=0, dhcsr=0, demcr=0x1000000,
            ipsr=0, control=0, writable=True):
        cpu = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_THUMB | unicorn.UC_MODE_MCLASS)
        cpu.ctl_set_cpu_model(ua.UC_CPU_ARM_CORTEX_M3)
        for address, size in ((0x40000, 0x40000), (RETURN, 0x1000),
                              (0x20000000, 0x20000), (0x40048000, 0x1000),
                              (0xE000E000, 0x1000)):
            cpu.mem_map(address, size)
        cpu.mem_write(0x20000000, bytes([0xA5]) * 0x20000)
        for address, contents in segments:
            cpu.mem_write(address, contents)

        def put(address, value):
            cpu.mem_write(address, WORD.pack(value & 0xFFFFFFFF))

        def get(address):
            return WORD.unpack(cpu.mem_read(address, 4))[0]

        original_cookie = 0x87654321
        for address, value in ((DHCSR, dhcsr), (DEMCR, demcr),
                               (DCRDR, original_cookie), (USB_POWER, 0xA55A0040)):
            put(address, value)
        for register, value in ((ua.UC_ARM_REG_SP, STACK), (ua.UC_ARM_REG_LR, RETURN | 1),
                                (ua.UC_ARM_REG_PRIMASK, primask), (ua.UC_ARM_REG_BASEPRI, 0x20),
                                (ua.UC_ARM_REG_CONTROL, control), (ua.UC_ARM_REG_IPSR, ipsr),
                                (ua.UC_ARM_REG_PC, entry | 1)):
            cpu.reg_write(register, value)
        saved = {number: 0x45670000 + number for number in range(4, 12)}
        for number, value in saved.items():
            cpu.reg_write(REGISTERS[f"r{number}"], value)
        before = bytes(cpu.mem_read(0x20000000, 0x20000))
        calls, writes = [], []
        for steps in range(500):
            pc = cpu.reg_read(ua.UC_ARM_REG_PC)
            if pc in (RETURN, reset):
                break
            if pc in (detach, hold):
                calls.append("detach" if pc == detach else "hold")
                assert cpu.reg_read(ua.UC_ARM_REG_PRIMASK) == 1, (name, "unmasked teardown")
                assert get(DCRDR) == COOKIE, (name, "missing cookie at teardown")
                if pc == hold:
                    # Stop at the existing hardware boundary, then model its acknowledgement.
                    cpu.reg_write(ua.UC_ARM_REG_R0, 1)
                    cpu.reg_write(ua.UC_ARM_REG_PC, cpu.reg_read(ua.UC_ARM_REG_LR))
                    continue
            instruction = decoded.get(pc)
            if instruction is None:
                instruction = decoded[pc] = next(decoder.disasm(bytes(cpu.mem_read(pc, 4)), pc, count=1))
            address = None
            if instruction.mnemonic.startswith("str") and instruction.operands[-1].type == ca.ARM_OP_MEM:
                memory = instruction.operands[-1].mem
                assert not memory.index, (name, "unexpected indexed store")
                address = (cpu.reg_read(REGISTERS[instruction.reg_name(memory.base)]) + memory.disp) & 0xFFFFFFFF
            cpu.emu_start(pc | 1, 0xFFFFFFFF, count=1)
            if address is not None and address >= 0x40000000:
                assert address in (DCRDR, USB_POWER), (name, "unexpected peripheral write", hex(address))
                writes.append((hex(address), get(address)))
                if address == DCRDR and not writable:
                    put(DCRDR, original_cookie)
        else:
            raise AssertionError((name, "unbounded request"))

        assert get(DHCSR) == dhcsr and get(DEMCR) == demcr, (name, "debug control changed")
        assert cpu.reg_read(ua.UC_ARM_REG_CONTROL) == control, (name, "privilege changed")
        # Special-register reads are architecturally zero when unprivileged.
        assert cpu.reg_read(ua.UC_ARM_REG_BASEPRI) == (0 if control else 0x20), (name, "BASEPRI changed")
        after = bytes(cpu.mem_read(0x20000000, 0x20000))
        assert all(old == new or STACK - 128 <= 0x20000000 + offset < STACK
                   for offset, (old, new) in enumerate(zip(before, after))), (name, "non-stack RAM write")
        if expected is None:
            assert pc == reset and calls == ["detach", "hold"], (name, "reset order", calls)
            assert cpu.reg_read(ua.UC_ARM_REG_PRIMASK) == 1
            assert get(DCRDR) == COOKIE and get(USB_POWER) == 0xA55A0000
            assert writes == [(hex(DCRDR), COOKIE), (hex(USB_POWER), 0xA55A0000)]
            outcome = "reset requested after cookie verification, USB detach and peer hold"
        else:
            assert pc == RETURN and not calls, (name, "failure reached teardown")
            assert cpu.reg_read(ua.UC_ARM_REG_R0) == expected & 0xFFFFFFFF, (name, "errno")
            assert cpu.reg_read(ua.UC_ARM_REG_SP) == STACK
            assert cpu.reg_read(ua.UC_ARM_REG_PRIMASK) == primask, (name, "PRIMASK restore")
            assert get(DCRDR) == original_cookie and get(USB_POWER) == 0xA55A0040
            assert all(cpu.reg_read(REGISTERS[f"r{number}"]) == value for number, value in saved.items())
            assert writes == ([] if writable else [(hex(DCRDR), COOKIE), (hex(DCRDR), original_cookie)])
            outcome = f"rejected with {expected}; mailbox, USB, IRQ state and peers preserved"
        return dict(case=name, outcome=outcome, instructions=steps + 1)

    results = [run("handler_context", -11, ipsr=16),
               run("unprivileged_context", -11, control=1)]
    for mask in (0, 1):
        results.extend((run(f"debugger_primask_{mask}", -16, primask=mask, dhcsr=1),
                        run(f"monitor_primask_{mask}", -16, primask=mask, demcr=0x1010000),
                        run(f"mailbox_unavailable_primask_{mask}", -5, primask=mask, writable=False),
                        run(f"recovery_primask_{mask}", primask=mask)))
    return dict(elf_sha256=hashlib.sha256(path.read_bytes()).hexdigest(), cases=results,
                limitations="Emulated debug-register reads and writes; peer hold acknowledged by harness. "
                            "Stops before SYSRESETREQ. No proof of physical reset retention or USB enumeration.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.elf), indent=2))
