#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare compiled battery conversion with original TOP instructions.

Bus/classifier/sleep boundaries are scripted. No hardware or timer model.
Requires pyelftools and unicorn. The original firmware is an external input.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS
from unicorn import arm_const as arm

STOCK_SHA256 = "0d7b8f82fa37732367a77f46af607297abfd06197f502d5b23e420a96fcd8750"
FLASH_BASE, FLASH_SIZE = 0x40000, 0x40000
RAM_BASE, RAM_SIZE = 0x20000000, 0x40000
CONTEXT, CONFIG, DATA = 0x20030000, 0x20030200, 0x20031000
OUTPUT, DIAGNOSTIC, STACK, RETURN = 0x20032000, 0x20032100, 0x2003F000, 0xF0000
MAX_INSTRUCTIONS = 50000
CLASS_14, SAMPLE, SENTINEL = 0x14, 42, 0xA5
BUS_ERROR, STOCK_BUSY, STOCK_TIMEOUT = 0xB03, 0x430, 0x431
# Zephyr minimal-libc errno values (not the host's errno table).
EIO, EBUSY, ERANGE, ETIMEDOUT = 5, 16, 34, 116
START, STOP = 1, 0
CONTROL, RESULT = 0x05, 0x06
ENABLE_08, ENABLE_09 = 0x08, 0x09
CHECK_CONTROL, CHECK_STATUS, CHECK_LOW, CHECK_HIGH = 0x17, 0x18, 0x19, 0x1A
STATUS, BACKEND, ID_F4, ID_F5 = 0xC0, 0xC7, 0xF4, 0xF5
CHECK_MINIMUM, CHECK_LIMIT, BATTERY_LIMIT = 436, 10, 30
MICROSECONDS = 1_000_000
WORD_BITS = 32
STOCK = dict(entry=0x46B9C, read=0x46E2C, write=0x471EC,
             delay=0x5B4D8, classify=0x4407C)


def load_new(path):
    with path.open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {s.name: s["st_value"] & ~1
                   for s in elf.get_section_by_name(".symtab").iter_symbols()}
        segments = [(s["p_vaddr"], s.data()) for s in elf.iter_segments()
                    if s["p_type"] == "PT_LOAD" and s["p_filesz"]]
    config = path.with_name(".config").read_text()
    assert "CONFIG_MINIMAL_LIBC=y" in config
    tick_hz = int(re.search(r"^CONFIG_SYS_CLOCK_TICKS_PER_SEC=(\d+)$", config, re.M)[1])
    return dict(entry=symbols["adc_ws9652a_sample_locked"],
                read=symbols["ws9652a_pmu_read_locked"],
                write=symbols["ws9652a_pmu_write_locked"],
                verify_backend=symbols["ws9652a_pmu_verify_backend_locked"],
                delay=symbols["z_impl_k_sleep_ticks"], tick_hz=tick_hz,
                segments=segments)


def run(stock_blob, new_image, original, case):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    cpu.ctl_set_cpu_model(arm.UC_CPU_ARM_CORTEX_M3)
    for address, size in ((FLASH_BASE, FLASH_SIZE), (RAM_BASE, RAM_SIZE), (RETURN, 0x1000)):
        cpu.mem_map(address, size)
    code = STOCK if original else new_image
    if original:
        cpu.mem_write(FLASH_BASE, stock_blob)
    else:
        for address, contents in new_image["segments"]:
            cpu.mem_write(address, contents)
    # The ADC conversion context holds opaque parent-device and child-data
    # pointers. Bus boundaries are mocked before dereferencing the parent.
    cpu.mem_write(CONTEXT, struct.pack("<II", CONFIG, DATA))
    cpu.mem_write(OUTPUT, bytes([SENTINEL]))
    cpu.mem_write(DIAGNOSTIC, bytes(16))
    registers = {BACKEND: 4, ID_F5: 1, ID_F4: 0xBB, ENABLE_08: 0x80, ENABLE_09: 0x40,
                 CONTROL: case.get("initial_control", STOP), CHECK_CONTROL: STOP,
                 CHECK_HIGH: case.get("check", CHECK_MINIMUM) >> 8,
                 CHECK_LOW: case.get("check", CHECK_MINIMUM) & 0xFF, RESULT: SAMPLE}
    if case.get("enabled"):
        registers[ENABLE_08] |= 0x20
        registers[ENABLE_09] |= 1
    poll_counts = {CHECK_STATUS: 0, STATUS: 0}
    occurrences, trace = {}, []
    arguments = ((OUTPUT, 0, 0) if original else (CONTEXT, OUTPUT, DIAGNOSTIC))
    for reg, value in zip((arm.UC_ARM_REG_R0, arm.UC_ARM_REG_R1, arm.UC_ARM_REG_R2), arguments):
        cpu.reg_write(reg, value)
    saved = {getattr(arm, f"UC_ARM_REG_R{i}"): 0x76540000 + i for i in range(4, 12)}
    for register, value in saved.items():
        cpu.reg_write(register, value)
    cpu.reg_write(arm.UC_ARM_REG_SP, STACK)
    cpu.reg_write(arm.UC_ARM_REG_LR, RETURN | 1)
    cpu.reg_write(arm.UC_ARM_REG_PC, code["entry"] | 1)

    def finish(value):
        cpu.reg_write(arm.UC_ARM_REG_R0, value & 0xFFFFFFFF)
        cpu.reg_write(arm.UC_ARM_REG_PC, cpu.reg_read(arm.UC_ARM_REG_LR))

    for steps in range(MAX_INSTRUCTIONS):
        pc = cpu.reg_read(arm.UC_ARM_REG_PC)
        if pc == RETURN:
            break
        if original and pc == code["classify"]:
            finish(CLASS_14)
            continue
        if pc == code["delay"]:
            delay = cpu.reg_read(arm.UC_ARM_REG_R0)
            if not original:
                delay |= cpu.reg_read(arm.UC_ARM_REG_R1) << WORD_BITS
                delay = delay * MICROSECONDS // code["tick_hz"]
                cpu.reg_write(arm.UC_ARM_REG_R1, 0)
            assert delay in (300, 600), delay
            trace.append(dict(kind="delay", us=delay))
            finish(0)
            continue
        verify_backend = not original and pc == code["verify_backend"]
        if verify_backend or pc in (code["read"], code["write"]):
            kind = "read" if verify_backend or pc == code["read"] else "write"
            index = BACKEND if verify_backend else cpu.reg_read(
                arm.UC_ARM_REG_R0 if original else arm.UC_ARM_REG_R1)
            operand = cpu.reg_read(arm.UC_ARM_REG_R1 if original or verify_backend
                                   else arm.UC_ARM_REG_R2)
            key = (kind, index)
            occurrences[key] = occurrences.get(key, 0) + 1
            failed = case.get("fail") == (kind, index, occurrences[key])
            status = BUS_ERROR if failed else 0
            if kind == "read":
                value = registers.get(index, 0)
                if index in poll_counts:
                    poll_counts[index] += 1
                    ready_at = case.get("check_ready" if index == CHECK_STATUS else "ready", 1)
                    value = (1 if index == CHECK_STATUS else 2) if poll_counts[index] >= ready_at else 0
                cpu.mem_write(operand, bytes([value]))
            else:
                value = operand
                assert index in (ENABLE_08, ENABLE_09, CHECK_CONTROL, CONTROL), index
                # A failed transaction may still have applied its value.
                registers[index] = value
            trace.append(dict(kind=kind, index=index, value=value, status=status))
            finish(status)
            continue
        cpu.emu_start(pc | 1, 0xFFFFFFFF, count=1)
    else:
        raise AssertionError("unbounded conversion")
    result = cpu.reg_read(arm.UC_ARM_REG_R0)
    if result & (1 << (WORD_BITS - 1)):
        result -= 1 << WORD_BITS
    assert cpu.reg_read(arm.UC_ARM_REG_SP) == STACK
    assert cpu.reg_read(arm.UC_ARM_REG_PRIMASK) == 0
    assert cpu.reg_read(arm.UC_ARM_REG_BASEPRI) == 0
    assert all(cpu.reg_read(reg) == value for reg, value in saved.items())
    value = cpu.mem_read(OUTPUT, 1)[0]
    if not original:
        assert value == (SAMPLE if result == 0 else SENTINEL)
    diagnostic = struct.unpack("<IIHBBBB", cpu.mem_read(DIAGNOSTIC, 14))
    return dict(status=result, value=value, instructions=steps, trace=trace,
                controls={"05": registers[CONTROL], "17": registers[CHECK_CONTROL]},
                diagnostic=dict(zip(("bus_status", "cleanup_status", "prerequisite", "index",
                                     "value", "write", "blocked"), diagnostic)))


def conversion_cases():
    return [
        dict(name="normal", stock=0, new=0, same=True),
        dict(name="already_enabled", enabled=True, stock=0, new=0, same=True),
        dict(name="last_poll", check_ready=CHECK_LIMIT, ready=BATTERY_LIMIT,
             stock=0, new=0, same=True),
        dict(name="busy_startup", initial_control=START, stock=STOCK_BUSY, new=-EBUSY, same=True),
        dict(name="battery_timeout", ready=BATTERY_LIMIT + 1,
             stock=STOCK_TIMEOUT, new=-ETIMEDOUT, same=True),
        dict(name="battery_status_error", fail=("read", STATUS, 1),
             stock=BUS_ERROR, new=-EIO, same=True),
        dict(name="battery_result_error", fail=("read", RESULT, 1),
             stock=BUS_ERROR, new=-EIO, same=True),
        dict(name="low_prerequisite", check=CHECK_MINIMUM - 1, stock=STOCK_TIMEOUT, new=-ERANGE),
        dict(name="prerequisite_timeout", check_ready=CHECK_LIMIT + 1,
             stock=STOCK_TIMEOUT, new=-ETIMEDOUT),
        dict(name="prerequisite_status_error", fail=("read", CHECK_STATUS, 1), stock=0, new=-EIO),
        dict(name="prerequisite_high_error", fail=("read", CHECK_HIGH, 1),
             stock=STOCK_TIMEOUT, new=-EIO),
        dict(name="enable_error", fail=("write", ENABLE_08, 1), stock=0, new=-EIO, blocked=True),
        dict(name="prerequisite_start_error", fail=("write", CHECK_CONTROL, 1),
             stock=0, new=-EIO, blocked=True),
        dict(name="prerequisite_stop_error", fail=("write", CHECK_CONTROL, 2),
             stock=0, new=-EIO, blocked=True),
        dict(name="battery_start_error", fail=("write", CONTROL, 1), stock=0, new=-EIO, blocked=True),
        dict(name="battery_stop_error", fail=("write", CONTROL, 2), stock=0, new=-EIO, blocked=True),
    ]


def verify(stock_path, elf_path):
    stock_blob = stock_path.read_bytes()
    assert hashlib.sha256(stock_blob).hexdigest() == STOCK_SHA256
    new_image = load_new(elf_path)
    results = []
    for case in conversion_cases():
        stock = run(stock_blob, new_image, True, case)
        new = run(stock_blob, new_image, False, case)
        assert stock["status"] == case["stock"], (case["name"], stock["status"], case["stock"])
        assert new["status"] == case["new"], (case["name"], new["status"], case["new"])
        assert bool(new["diagnostic"]["blocked"]) == case.get("blocked", False)
        assert new["controls"] == {"05": STOP, "17": STOP}
        # The replacement additionally verifies backend and identity before any writes.
        conversion_trace = [event for event in new["trace"]
                            if event.get("index") not in (BACKEND, ID_F5, ID_F4)]
        if case.get("same"):
            assert conversion_trace == stock["trace"], (case["name"], "conversion trace mismatch")
        results.append(dict(case=case, stock=stock, replacement=new, passed=True))
        print("PASS", case["name"], "stock=", stock["status"], "replacement=", new["status"])
    return dict(stock_sha256=STOCK_SHA256, elf=str(elf_path),
                elf_sha256=hashlib.sha256(elf_path.read_bytes()).hexdigest(),
                method="Actual ARM instructions, mocked bus/classifier/sleep boundaries; no hardware timing model",
                cases=results)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()
    args.json.write_text(json.dumps(verify(args.stock, args.elf), indent=2) + "\n")
