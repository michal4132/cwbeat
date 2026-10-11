#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Flash a Zephyr TOP binary to a JBL Go 4 already in SBL USB recovery.
"""

import argparse
import binascii
from contextlib import contextmanager
from dataclasses import dataclass
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import struct
import sys
import time


BYTE_MASK = 0xFF
SHORT_MASK = 0xFFFF
WORD_MASK = 0xFFFFFFFF
BITS_PER_BYTE = 8
HEADER = struct.Struct("<BBHH")
WORD = struct.Struct("<I")
SHORT = struct.Struct("<H")
PAIR = struct.Struct("<II")
TRIPLE = struct.Struct("<III")
QUERY = 0xE1
ACK = 0xD2
NAK = 0xB4
CHECKSUM_SIZE = 1
SELECTOR_SIZE = 1
MATCH_FLAG_SIZE = 1
MAX_NAK_BODY = 2
CHUNK_SIZE = 512
SECTOR_SIZE = 4096
FLASH_SIZE = 4 * 1024 * 1024
TOP_OFFSET = 0x10000
TOP_CPU_BASE = 0x40000
LEGACY_TOP_LENGTH = 0x1E400
TOP_CAPACITY = 0x40000
PREFIX_SIZE = TOP_OFFSET
PAYLOAD_LIMIT = TOP_CAPACITY
REGION_SIZE = TOP_CAPACITY
# TOP owns the entire lower 64 KiB; the stock-peer arena starts above it.
# A descending stack may start exactly at the private bank's upper end.
TOP_SRAM_RANGES = ((0x20000000, 0x20010000),)
STACK_ALIGNMENT = 8
THUMB_BIT = 1
CRC_POLYNOMIAL = 0x681BB522
CRC_RESULT_HIGH_BIT = 1 << 31
METADATA_MAGIC = bytes.fromhex("a55a5aa5")
METADATA_HEADER_SIZE = 0x40
METADATA_CRC_START = 0x0C
METADATA_CRC_OFFSET = METADATA_HEADER_SIZE - SHORT.size
PARTITION_RECORD = struct.Struct("<IIII6sIHHH")
PARTITION_LENGTH_OFFSET = WORD.size
PARTITION_CRC_OFFSET = struct.calcsize("<IIII6s")
PARTITION_RECORD_CRC_OFFSET = PARTITION_RECORD.size - SHORT.size
PARTITION_COUNT = 10
METADATA_WRITE_ORDER = (SECTOR_SIZE, 0)  # Mirror first, primary last.
FIRMWARE_INDICES = (0, 1, 2, 4, 5, 6)
PREFERRED_INDICES = (0, 1, 2)
BOOT_RECORD_OFFSET = 0x3000
BOOT_RECORD = struct.Struct("<HBBHHH")
BOOT_MAGIC = 0x5AA5
SBL_VERSION = bytes.fromhex("01000247")
SBL_VECTOR_BASE = 0x20010000
SCB_VTOR = 0xE000ED08
SBL_HOUSEKEEPING_HOOK = 0x2001CED8
SBL_ZERO_RETURN_ADDRESS = 0x20011F6C
SBL_ZERO_RETURN_CODE = bytes.fromhex("00207047")  # movs r0, #0; bx lr
SBL_ZERO_RETURN_THUMB = SBL_ZERO_RETURN_ADDRESS | THUMB_BIT
RESET_CLEARED_RAM_WORD = 0
RESET_COMMAND_WRITE_WORD = 0x02
RESET_COMMAND_NORMAL_BOOT = 0x09
NORMAL_BOOT_RESET_MODE = 2
FLASH_SELECTOR = 2
FLASH_ID_SIZE = 16
FLASH_MANUFACTURERS = frozenset((0xC2, 0xC8, 0x51, 0x1C, 0xEF, 0x85, 0x20))
FLASH_CAPACITY_BASE = 65536
FLASH_CAPACITY_MASK = 0x0F
FLASH_CAPACITY_INDEX = 2
USB_VID = 0x0AC8
USB_PID = 0x38B9
BAUD_RATE = 115200
REQUEST_TIMEOUT = 2.0
CRC_TIMEOUT = 9.0
COMMAND_VERSION = 0x00
COMMAND_READ_WORD = 0x01
COMMAND_FLASH_SELECTION = 0x04
COMMAND_FLASH_ID = 0x05
COMMAND_ERASE = 0x06
COMMAND_WRITE = 0x07
COMMAND_READ = 0x08
COMMAND_CRC = 0x0D
CHECK_REPLY_SIZE = MATCH_FLAG_SIZE + WORD.size
KIBIBYTE = 1024
PROGRESS_BAR_WIDTH = 20
PROGRESS_REFRESH_SECONDS = 0.1
PROGRESS_FALLBACK_COLUMNS = 100
PROGRESS_FALLBACK_LINES = 24
PERCENT_SCALE = 100
INTERRUPTED_EXIT_CODE = 130
ELAPSED_DECIMALS = 3
PHASE_LABELS = {
    "connect": "Connect",
    "read": "Read flash",
    "check": "Check firmware",
    "write": "Write TOP",
    "metadata": "Commit metadata",
    "verify": "Verify",
    "reset": "Reset",
}


@dataclass(frozen=True)
class ProgressEvent:
    stage: str
    completed: int = 0
    total: int = 0
    unit: str = ""
    detail: str = ""


def format_size(size):
    return f"{size / KIBIBYTE:.1f} KiB"


class ProgressDisplay:
    """Render measured work; never invent progress while a command is pending."""

    def __init__(self, stream=None, *, reset=False, clock=time.monotonic, terminal=None):
        self.stream = stream if stream is not None else sys.stderr
        self.clock = clock
        self.terminal = (self.stream.isatty() and os.environ.get("TERM") != "dumb"
                         if terminal is None else terminal)
        self.stages = tuple(stage for stage in PHASE_LABELS if reset or stage != "reset")
        self.event = None
        self.last_rendered = None
        self.last_update = -math.inf
        self.line_width = 0
        self.line_active = False

    @staticmethod
    def complete(event):
        return event.total == 0 or event.completed == event.total

    def line(self, event):
        index = self.stages.index(event.stage) + 1
        prefix = f"[{index}/{len(self.stages)}] {PHASE_LABELS[event.stage]:<14}"
        if not event.total:
            status = "skipped"
        elif not event.unit:
            status = "done" if self.complete(event) else "..."
        else:
            filled = PROGRESS_BAR_WIDTH * event.completed // event.total
            percent = PERCENT_SCALE * event.completed // event.total
            bar = "#" * filled + "-" * (PROGRESS_BAR_WIDTH - filled)
            count = (f"{format_size(event.completed)} / {format_size(event.total)}"
                     if event.unit == "bytes" else f"{event.completed}/{event.total} {event.unit}")
            status = f"[{bar}] {percent:3d}%  {count}"
        detail = f"  {event.detail}" if event.detail else ""
        return f"{prefix} {status}{detail}"

    def _render(self, event):
        line = self.line(event)
        if self.terminal:
            columns = shutil.get_terminal_size(
                fallback=(PROGRESS_FALLBACK_COLUMNS, PROGRESS_FALLBACK_LINES)).columns
            line = line[:max(1, columns - 1)]
            print("\r" + line.ljust(self.line_width), end="", file=self.stream, flush=True)
            self.line_width = len(line)
            self.line_active = True
        else:
            print(line, file=self.stream, flush=True)
        self.last_rendered = event
        self.last_update = self.clock()
        if self.complete(event):
            self.finish()

    def __call__(self, event):
        require(event.stage in self.stages and 0 <= event.completed <= event.total,
                "Invalid flash progress event")
        changed = self.event is None or self.event.stage != event.stage
        if changed:
            # Preserve the last real measurement at boundaries; never force 100%.
            if self.terminal and self.event is not None and self.last_rendered != self.event:
                self._render(self.event)
            self.finish()
        self.event = event
        if changed or self.complete(event) or (
                self.terminal and self.clock() - self.last_update >= PROGRESS_REFRESH_SECONDS):
            self._render(event)

    def finish(self):
        if self.line_active:
            print(file=self.stream, flush=True)
        self.line_active = False
        self.line_width = 0

    def failure(self, message):
        self.finish()
        phase = f" during {PHASE_LABELS[self.event.stage].lower()}" if self.event else ""
        print(f"jbl-bl: failed{phase}: {message}", file=self.stream, flush=True)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def _crc_table():
    table = []
    for value in range(BYTE_MASK + 1):
        for _ in range(BITS_PER_BYTE):
            value = (value >> 1) ^ (CRC_POLYNOMIAL if value & 1 else 0)
        table.append(value)
    return tuple(table)


CRC_TABLE = _crc_table()


def crc32(data):
    """Native SBL flash CRC; deliberately not IEEE/zlib CRC32."""
    value = WORD_MASK
    for byte in data:
        value = (value >> BITS_PER_BYTE) ^ CRC_TABLE[(value ^ byte) & BYTE_MASK]
    return value ^ WORD_MASK


def validate_payload(payload):
    require(PAIR.size <= len(payload) <= PAYLOAD_LIMIT,
            f"TOP binary must contain vectors and fit within {PAYLOAD_LIMIT} bytes")
    stack, entry = PAIR.unpack_from(payload)
    require(stack % STACK_ALIGNMENT == 0 and
            any(start < stack <= end for start, end in TOP_SRAM_RANGES),
            f"TOP stack must be {STACK_ALIGNMENT}-byte aligned and descend into usable Zephyr TOP SRAM")
    require(entry & THUMB_BIT and
            TOP_CPU_BASE + PAIR.size <= (entry & ~THUMB_BIT) < TOP_CPU_BASE + len(payload),
            "TOP reset vector must point inside the supplied binary")
    return stack, entry


@dataclass(frozen=True)
class Partition:
    index: int
    address: int
    length: int
    crc: int
    status: int
    capacity: int


@dataclass(frozen=True)
class Layout:
    top_crc: int
    top_length: int
    components: tuple


def validate_layout(prefix):
    require(len(prefix) == PREFIX_SIZE, "Incomplete protected flash prefix")
    first = prefix[:SECTOR_SIZE]
    require(first == prefix[SECTOR_SIZE:SECTOR_SIZE * 2], "Flash metadata copies differ")
    require(first[:len(METADATA_MAGIC)] == METADATA_MAGIC, "Unexpected flash metadata magic")
    stored = SHORT.unpack_from(first, METADATA_CRC_OFFSET)[0]
    require(stored != 0 and binascii.crc_hqx(first[METADATA_CRC_START:METADATA_CRC_OFFSET], 0) == stored,
            "Invalid metadata header CRC")
    parts = []
    for index in range(PARTITION_COUNT):
        offset = METADATA_HEADER_SIZE + index * PARTITION_RECORD.size
        record = first[offset:offset + PARTITION_RECORD.size]
        address, length, _, _, _, expected, status, sectors, stored = PARTITION_RECORD.unpack(record)
        require(binascii.crc_hqx(record[:-SHORT.size], 0) == stored,
                f"Invalid partition {index} metadata CRC")
        require(address % SECTOR_SIZE == 0, f"Unaligned partition {index}")
        capacity = sectors * SECTOR_SIZE
        require(address <= FLASH_SIZE and capacity <= FLASH_SIZE - address and length <= capacity,
                f"Partition {index} escapes physical flash or its allocation")
        parts.append(Partition(index, address, length, expected, status, capacity))
    top = parts[PREFERRED_INDICES[0]]
    require((top.address, top.capacity, top.status) == (TOP_OFFSET, TOP_CAPACITY, 0) and
            top.length in (LEGACY_TOP_LENGTH, TOP_CAPACITY), "Unsupported Go 4 TOP layout")
    for part in parts:
        if part.index != top.index and part.capacity:
            require(part.address + part.capacity <= TOP_OFFSET or
                    part.address >= TOP_OFFSET + TOP_CAPACITY,
                    f"Partition {part.index} overlaps TOP allocation")
    for index in FIRMWARE_INDICES:
        require(parts[index].length > 0, f"Missing firmware partition {index}")
    for index in PREFERRED_INDICES:
        require(parts[index].status == 0, f"Preferred firmware partition {index} is not boot eligible")
    record = prefix[BOOT_RECORD_OFFSET:BOOT_RECORD_OFFSET + BOOT_RECORD.size]
    magic, force_recovery, enabled, lower, upper, stored = BOOT_RECORD.unpack(record)
    require(magic == BOOT_MAGIC and stored != 0 and
            binascii.crc_hqx(record[:-SHORT.size], 0) == stored, "Invalid boot selection record")
    require(force_recovery == 0 and enabled != 0 and lower >= upper,
            "Boot record does not select the supported lower firmware set")
    return Layout(top.crc, top.length, tuple(parts[index] for index in FIRMWARE_INDICES))


def updated_metadata(prefix, top_crc):
    """Change only lower TOP length, native CRC and its descriptor CRC16."""
    validate_layout(prefix)
    result = bytearray(prefix)
    for sector in METADATA_WRITE_ORDER:
        offset = sector + METADATA_HEADER_SIZE
        WORD.pack_into(result, offset + PARTITION_LENGTH_OFFSET, TOP_CAPACITY)
        WORD.pack_into(result, offset + PARTITION_CRC_OFFSET, top_crc)
        record = result[offset:offset + PARTITION_RECORD_CRC_OFFSET]
        SHORT.pack_into(result, offset + PARTITION_RECORD_CRC_OFFSET,
                        binascii.crc_hqx(record, 0))
    validate_layout(result)
    return bytes(result)


@dataclass(frozen=True)
class FlashPlan:
    image: bytes
    prefix: bytes
    writes: tuple
    metadata_writes: tuple
    top_crc: int
    payload_sha256: str


def build_plan(prefix, current_region, payload):
    validate_payload(payload)
    layout = validate_layout(prefix)
    require(len(current_region) == REGION_SIZE, "Incomplete live TOP region")
    require(crc32(current_region[:layout.top_length]) == layout.top_crc, "Installed TOP CRC is invalid")
    image = payload + bytes([BYTE_MASK]) * (TOP_CAPACITY - len(payload))
    top_crc = crc32(image)
    target_prefix = updated_metadata(prefix, top_crc)
    changed = {TOP_OFFSET + offset: image[offset:offset + SECTOR_SIZE]
               for offset in range(0, TOP_CAPACITY, SECTOR_SIZE)
               if image[offset:offset + SECTOR_SIZE] != current_region[offset:offset + SECTOR_SIZE]}
    # The entry is written last; metadata is committed only after full TOP verification.
    order = sorted(set(changed) - {TOP_OFFSET})
    if TOP_OFFSET in changed:
        order.append(TOP_OFFSET)
    metadata_writes = tuple((address, target_prefix[address:address + SECTOR_SIZE])
                            for address in METADATA_WRITE_ORDER
                            if target_prefix[address:address + SECTOR_SIZE] != prefix[address:address + SECTOR_SIZE])
    return FlashPlan(image, target_prefix, tuple((address, changed[address]) for address in order),
                     metadata_writes, top_crc, hashlib.sha256(payload).hexdigest())


def expected_flash_crc(baseline, prefix, current_region, plan):
    """Apply the affine native CRC delta without reading unknown flash bytes.

    Both synthetic arrays have the full flash length. Their unknown suffixes
    are identical zeros, so those bytes and the CRC's affine constant cancel.
    """
    require(len(prefix) == PREFIX_SIZE and len(current_region) == TOP_CAPACITY and
            len(plan.prefix) == PREFIX_SIZE and len(plan.image) == TOP_CAPACITY,
            "Incomplete inputs for whole-flash CRC delta")
    unknown = bytes(FLASH_SIZE - PREFIX_SIZE - TOP_CAPACITY)
    return baseline ^ crc32(prefix + current_region + unknown) ^ crc32(plan.prefix + plan.image + unknown)


def encode_frame(payload):
    require(payload and len(payload) + CHECKSUM_SIZE <= SHORT_MASK, "Invalid command size")
    body = bytes(payload) + bytes([sum(payload) & BYTE_MASK])
    return HEADER.pack(QUERY, QUERY ^ BYTE_MASK, len(body), len(body) ^ SHORT_MASK) + body


def _body_length(header, expected):
    require(len(header) == HEADER.size, "Truncated loader header")
    kind, complement, length, inverse = HEADER.unpack(header)
    require(kind ^ complement == BYTE_MASK and length ^ inverse == SHORT_MASK,
            "Invalid loader header complement")
    require(kind in (ACK, NAK), "Expected loader ACK or NAK")
    require(length <= max(expected + CHECKSUM_SIZE, MAX_NAK_BODY), "Oversized loader reply")
    return length


def decode_reply(header, body, expected):
    require(len(body) == _body_length(header, expected), "Truncated loader body")
    if body:
        require(sum(body[:-CHECKSUM_SIZE]) & BYTE_MASK == body[-CHECKSUM_SIZE],
                "Invalid loader SUM8 checksum")
    if header[0] == NAK:
        raise RuntimeError(f"Bootloader rejected command: {body[:-CHECKSUM_SIZE].hex()}")
    required_body = expected + CHECKSUM_SIZE if expected else 0
    require(len(body) == required_body, f"Expected {expected} checked reply bytes")
    return body[:-CHECKSUM_SIZE] if body else b""


def flash_range(address, length):
    require(isinstance(address, int) and isinstance(length, int) and
            0 <= address < FLASH_SIZE and 0 < length <= FLASH_SIZE - address,
            "Invalid physical flash range")


def top_range(address, length):
    flash_range(address, length)
    require(TOP_OFFSET <= address and address + length <= TOP_OFFSET + REGION_SIZE,
            "Write would escape the lower TOP partition")


class Session:
    """One outstanding command on a serial object; any exchange failure poisons it."""

    def __init__(self, serial_port, *, mutation_check=None):
        self.serial = serial_port
        self.mutation_check = mutation_check or (lambda: None)
        self.failed = False
        self.finished = False

    def _read_exact(self, length, deadline):
        result = bytearray()
        while len(result) < length:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"Loader reply timeout after {len(result)} of {length} bytes")
            self.serial.timeout = remaining
            chunk = self.serial.read(length - len(result))
            if not chunk:
                raise TimeoutError(f"Loader reply timeout after {len(result)} of {length} bytes")
            result.extend(chunk)
        return bytes(result)

    def request(self, payload, expected, timeout=REQUEST_TIMEOUT):
        require(not self.failed and not self.finished,
                "Loader session failed or finished; command will not be retried")
        require(payload and 0 <= expected <= CHUNK_SIZE, "Invalid loader exchange")
        require(math.isfinite(timeout) and timeout > 0, "Invalid loader timeout")
        try:
            require(not self.serial.in_waiting, "Unexpected pending serial data; command not sent")
            deadline = time.monotonic() + timeout
            self.serial.write_timeout = timeout
            frame = encode_frame(payload)
            if self.serial.write(frame) != len(frame):
                raise RuntimeError("Partial loader request; command will not be retried")
            header = self._read_exact(HEADER.size, deadline)
            body = self._read_exact(_body_length(header, expected), deadline)
            result = decode_reply(header, body, expected)
            require(not self.serial.in_waiting, "Unexpected trailing serial data")
            return result
        except Exception as error:
            self.failed = True
            raise RuntimeError(f"Loader command {payload[0]:#04x} failed: {error}; no retry") from error

    def boot_version(self):
        return self.request(bytes([COMMAND_VERSION]), len(SBL_VERSION))

    def read_word(self, address):
        require(address == SCB_VTOR, "Only the SBL vector-table guard may be read")
        return WORD.unpack(self.request(bytes([COMMAND_READ_WORD]) + WORD.pack(address), WORD.size))[0]

    def get_flash_selection(self):
        return self.request(bytes([COMMAND_FLASH_SELECTION]), SELECTOR_SIZE)[0]

    def flash_id(self):
        return self.request(bytes([COMMAND_FLASH_ID]), FLASH_ID_SIZE)

    def read_flash(self, address, length, *, progress=None):
        flash_range(address, length)
        data = bytearray()
        while len(data) < length:
            count = min(CHUNK_SIZE, length - len(data))
            data.extend(self.request(bytes([COMMAND_READ]) + PAIR.pack(address + len(data), count), count))
            if progress is not None:
                progress(len(data), length)
        return bytes(data)

    def check_flash(self, address, length, expected):
        flash_range(address, length)
        data = self.request(bytes([COMMAND_CRC]) + TRIPLE.pack(address, length, expected),
                            CHECK_REPLY_SIZE, CRC_TIMEOUT)
        actual = WORD.unpack_from(data, MATCH_FLAG_SIZE)[0]
        if data[0] not in (0, 1) or bool(data[0]) != (actual == expected) or not actual & CRC_RESULT_HIGH_BIT:
            self.failed = True
            raise ValueError("Invalid native flash CRC result")
        return actual

    def erase_sector(self, address):
        top_range(address, SECTOR_SIZE)
        require(address % SECTOR_SIZE == 0, "Unaligned TOP sector erase")
        self.mutation_check()
        self.request(bytes([COMMAND_ERASE]) + PAIR.pack(address // SECTOR_SIZE, 1), 0)

    def write_flash(self, address, data):
        top_range(address, len(data))
        require(address // SECTOR_SIZE == (address + len(data) - 1) // SECTOR_SIZE,
                "Flash write crosses a sector boundary")
        self.mutation_check()
        for offset in range(0, len(data), CHUNK_SIZE):
            self.request(bytes([COMMAND_WRITE]) + WORD.pack(address + offset) + data[offset:offset + CHUNK_SIZE], 0)

    def commit_metadata(self, original_prefix, target_prefix, top_crc, *, progress=None):
        """The only metadata write path: verified TOP, mirror, then primary.

        Ordinary erase/write methods remain restricted to TOP. The full prefix
        comparison also protects boot selection and all unrelated descriptors.
        """
        try:
            require(target_prefix == updated_metadata(original_prefix, top_crc),
                    "Metadata update changes more than the lower TOP descriptor")
            require(self.check_flash(TOP_OFFSET, TOP_CAPACITY, top_crc) == top_crc,
                    "Full TOP CRC failed before metadata commit")
            require(self.read_flash(0, PREFIX_SIZE) == original_prefix,
                    "Flash prefix changed before metadata commit")
            writes = tuple(address for address in METADATA_WRITE_ORDER
                           if original_prefix[address:address + SECTOR_SIZE] !=
                           target_prefix[address:address + SECTOR_SIZE])
            for address in writes:
                data = target_prefix[address:address + SECTOR_SIZE]
                self.mutation_check()
                self.request(bytes([COMMAND_ERASE]) + PAIR.pack(address // SECTOR_SIZE, 1), 0)
                for offset in range(0, SECTOR_SIZE, CHUNK_SIZE):
                    self.mutation_check()
                    self.request(bytes([COMMAND_WRITE]) + WORD.pack(address + offset) +
                                 data[offset:offset + CHUNK_SIZE], 0)
                require(self.read_flash(address, SECTOR_SIZE) == data,
                        f"Metadata sector {address:#x} readback failed")
                if progress is not None:
                    progress(address)
            return writes
        except BaseException:
            self.failed = True
            raise


def port_identity(info):
    return (info.vid, info.pid, info.serial_number, info.location)


@contextmanager
def open_session(port):
    # Import only for a hardware operation; --help/--dry-run work without pyserial.
    try:
        import serial
        from serial.tools import list_ports
    except ImportError as error:
        raise RuntimeError("Install pyserial to flash: python -m pip install pyserial") from error

    def select_port():
        matches = [info for info in list_ports.comports()
                   if info.device == port and info.vid == USB_VID and info.pid == USB_PID]
        require(len(matches) == 1, f"Port must identify one USB {USB_VID:04x}:{USB_PID:04x} device: {port}")
        return port_identity(matches[0])

    identity = select_port()

    def unchanged():
        require(select_port() == identity, "USB identity changed; refusing flash mutation")

    connection = serial.Serial(port=None, baudrate=BAUD_RATE, timeout=REQUEST_TIMEOUT,
                               write_timeout=REQUEST_TIMEOUT)
    connection.dtr = True
    connection.rts = False
    if os.name == "posix":
        connection.exclusive = True
    connection.port = port
    try:
        connection.open()
        unchanged()
        yield Session(connection, mutation_check=unchanged)
    finally:
        # pyserial closes the descriptor without restoring termios across a reset.
        connection.close()


def reset_after_flash(session):
    """Request normal boot after verified flashing, with RAM-only SBL guards.

    The native reset hook normally writes persistent boot metadata. Replace it
    only with the verified resident zero-return stub. A reset acknowledgement
    does not verify application startup or request the speaker to turn on.
    """
    def read_word(address):
        data = session.request(bytes([COMMAND_READ_WORD]) + WORD.pack(address), WORD.size)
        require(len(data) == WORD.size, f"Short reset guard read at {address:#x}")
        return WORD.unpack(data)[0]

    stage = "identify pinned SBL (no RAM writes or reset sent)"
    try:
        require(session.boot_version() == SBL_VERSION, "Expected Go 4 SBL 1.0.2.71 recovery")
        stage = "verify active SBL vector table (no RAM writes or reset sent)"
        require(read_word(SCB_VTOR) == SBL_VECTOR_BASE, "SBL vector table is not active")
        stage = "verify resident SBL zero-return code (no RAM writes or reset sent)"
        require(read_word(SBL_ZERO_RETURN_ADDRESS) == WORD.unpack(SBL_ZERO_RETURN_CODE)[0],
                "Resident SBL zero-return code differs")
        stage = "verify SBL housekeeping hook (no RAM writes or reset sent)"
        require(read_word(SBL_HOUSEKEEPING_HOOK) in (RESET_CLEARED_RAM_WORD, SBL_ZERO_RETURN_THUMB),
                "Unexpected SBL housekeeping hook")
        writes = ((SBL_HOUSEKEEPING_HOOK, SBL_ZERO_RETURN_THUMB),)
        for address, value in writes:
            stage = f"verify USB identity before RAM write at {address:#x} (reset not sent)"
            session.mutation_check()
            stage = f"write RAM at {address:#x} (write may have been sent; reset not sent)"
            session.request(bytes([RESET_COMMAND_WRITE_WORD]) + PAIR.pack(address, value), 0)
            stage = f"verify RAM readback at {address:#x} (write acknowledged; reset not sent)"
            require(read_word(address) == value, f"Reset guard RAM readback differs at {address:#x}")
        stage = "verify USB identity before normal reset (reset not sent)"
        session.mutation_check()
        stage = f"normal reset mode {NORMAL_BOOT_RESET_MODE} (reset may have been sent)"
        try:
            session.request(bytes([RESET_COMMAND_NORMAL_BOOT, NORMAL_BOOT_RESET_MODE]), 0)
        finally:
            # A lost ACK or interruption must not permit another command on a
            # connection whose device may already be rebooting.
            session.finished = True
    except Exception as error:
        session.failed = True
        raise RuntimeError(f"Post-flash reset [{stage}]: {error}; no retry") from error
    return dict(reset_requested=True, reset_acknowledged=True,
                application_verified=False, remains_in_recovery=False)


def flash(session, payload, progress=lambda event: None):
    try:
        return _flash(session, payload, progress)
    except BaseException:
        # Validation/readback failures must be as fail-stop as transport errors.
        session.failed = True
        raise


def _flash(session, payload, progress):
    validate_payload(payload)
    progress(ProgressEvent("connect", 0, 1, "checks", "Checking bootloader and flash identity"))
    require(session.boot_version() == SBL_VERSION, "Expected Go 4 SBL 1.0.2.71 recovery")
    require(session.read_word(SCB_VTOR) == SBL_VECTOR_BASE, "SBL vector table is not active")
    require(session.get_flash_selection() == FLASH_SELECTOR, "Unexpected selected flash; selector will not be changed")
    identity = session.flash_id()
    require(len(identity) == FLASH_ID_SIZE and identity[0] in FLASH_MANUFACTURERS and
            (FLASH_CAPACITY_BASE << (identity[FLASH_CAPACITY_INDEX] & FLASH_CAPACITY_MASK)) == FLASH_SIZE,
            "Expected supported 4 MiB flash")
    progress(ProgressEvent("connect", 1, 1, "checks", "Bootloader and flash identity verified"))
    read_total = PREFIX_SIZE + REGION_SIZE

    def read_progress(offset, detail):
        return lambda completed, total: progress(
            ProgressEvent("read", offset + completed, read_total, "bytes", detail))

    progress(ProgressEvent("read", 0, read_total, "bytes", "Reading boot and metadata prefix"))
    prefix = session.read_flash(0, PREFIX_SIZE,
                                progress=read_progress(0, "Reading boot and metadata prefix"))
    layout = validate_layout(prefix)
    current = session.read_flash(TOP_OFFSET, REGION_SIZE,
                                 progress=read_progress(PREFIX_SIZE, "Reading complete lower TOP partition"))
    plan = build_plan(prefix, current, payload)
    preflight_recheck_count = 3  # Whole-flash CRC, protected prefix, and current TOP.
    check_total = len(layout.components) + preflight_recheck_count
    checked = 0
    progress(ProgressEvent("check", checked, check_total, "checks", "Checking installed firmware CRCs"))
    for part in layout.components:
        require(session.check_flash(part.address, part.length, part.crc) == part.crc,
                f"Installed firmware partition {part.index} CRC is invalid; no writes sent")
        checked += 1
        progress(ProgressEvent("check", checked, check_total, "checks",
                               f"Firmware partition {part.index} CRC verified"))
    baseline = session.check_flash(0, FLASH_SIZE, 0)
    require(baseline & CRC_RESULT_HIGH_BIT, "Invalid whole-flash CRC baseline")
    checked += 1
    progress(ProgressEvent("check", checked, check_total, "checks", "Whole-flash CRC baseline recorded"))
    # Recheck all live inputs before the first erase, after potentially long CRC scans.
    require(session.read_flash(0, PREFIX_SIZE) == prefix, "Protected prefix changed during preflight; no writes sent")
    checked += 1
    progress(ProgressEvent("check", checked, check_total, "checks", "Protected prefix is unchanged"))
    require(session.read_flash(TOP_OFFSET, REGION_SIZE) == current, "TOP changed during preflight; no writes sent")
    checked += 1
    progress(ProgressEvent("check", checked, check_total, "checks", "Current TOP is unchanged"))
    expected_crc = expected_flash_crc(baseline, prefix, current, plan)
    write_total = len(plan.writes)
    write_detail = ("Writing and reading back TOP sectors" if plan.writes else
                    "No changes; skipping erase and write")
    progress(ProgressEvent("write", 0, write_total, "sectors", write_detail))
    for index, (address, data) in enumerate(plan.writes, start=1):
        session.erase_sector(address)
        session.write_flash(address, data)
        require(session.read_flash(address, SECTOR_SIZE) == data, f"TOP sector {address:#x} readback failed")
        progress(ProgressEvent("write", index, write_total, "sectors",
                               f"TOP sector {address:#08x} written and verified"))
    # Metadata must never describe an image that has not passed full readback and CRC.
    require(session.read_flash(TOP_OFFSET, TOP_CAPACITY) == plan.image,
            "Full TOP readback failed before metadata commit")
    require(session.check_flash(TOP_OFFSET, TOP_CAPACITY, plan.top_crc) == plan.top_crc,
            "Full TOP CRC failed before metadata commit")
    metadata_total = len(plan.metadata_writes)
    committed = 0
    progress(ProgressEvent("metadata", 0, metadata_total, "sectors",
                           "Committing verified TOP descriptor, mirror first" if metadata_total else
                           "Metadata already matches; no writes needed"))

    def metadata_progress(address):
        nonlocal committed
        committed += 1
        progress(ProgressEvent("metadata", committed, metadata_total, "sectors",
                               f"Metadata sector {address:#08x} written and verified"))

    metadata_written = session.commit_metadata(prefix, plan.prefix, plan.top_crc,
                                               progress=metadata_progress)
    require(metadata_written == tuple(address for address, _ in plan.metadata_writes),
            "Metadata commit did not match the verified plan")
    verify_total = 4  # Expected prefix, TOP bytes, TOP CRC, and whole-flash CRC.
    verified = 0
    progress(ProgressEvent("verify", verified, verify_total, "checks", "Checking final flash contents"))
    require(session.read_flash(0, PREFIX_SIZE) == plan.prefix, "Final boot/metadata prefix differs")
    verified += 1
    progress(ProgressEvent("verify", verified, verify_total, "checks", "Boot and metadata prefix verified"))
    require(session.read_flash(TOP_OFFSET, TOP_CAPACITY) == plan.image, "Final TOP readback failed")
    verified += 1
    progress(ProgressEvent("verify", verified, verify_total, "checks", "Complete TOP partition verified"))
    require(session.check_flash(TOP_OFFSET, TOP_CAPACITY, plan.top_crc) == plan.top_crc, "Final TOP CRC failed")
    verified += 1
    progress(ProgressEvent("verify", verified, verify_total, "checks", "Final TOP CRC verified"))
    require(session.check_flash(0, FLASH_SIZE, expected_crc) == expected_crc,
            "Unexpected change outside the planned TOP/metadata update")
    verified += 1
    progress(ProgressEvent("verify", verified, verify_total, "checks", "Expected whole-flash CRC verified"))
    written = [address for address, _ in plan.writes] + list(metadata_written)
    return dict(cpu="cputop", payload_bytes=len(payload), payload_sha256=plan.payload_sha256,
                sectors_written=written, metadata_sectors_written=list(metadata_written), verified=True,
                top_crc=plan.top_crc, top_length=TOP_CAPACITY, whole_flash_crc=expected_crc,
                remains_in_recovery=True, unchanged=not written)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin-file", required=True, type=Path, help="Zephyr TOP binary")
    parser.add_argument("--port", help="Exact USB serial port of a speaker already in SBL recovery")
    parser.add_argument("--cpu", choices=("cputop",), default="cputop")
    parser.add_argument("--dry-run", action="store_true", help="Validate the binary offline; never access USB")
    parser.add_argument("--json", action="store_true", help="Print the result as JSON; progress stays on stderr")
    parser.add_argument("--reset", action="store_true", help="Request normal boot after successful flash verification")
    args = parser.parse_args(argv)
    if not args.dry_run and not args.port:
        parser.error("--port is required for flashing")
    display = ProgressDisplay(reset=args.reset)
    started = time.monotonic()
    verified = False
    reset_acknowledged = False
    try:
        payload = args.bin_file.read_bytes()
        stack, entry = validate_payload(payload)
        if not args.json:
            print("JBL Go 4 / TOP", flush=True)
            print(f"Image: {args.bin_file} ({format_size(len(payload))})", flush=True)
            if not args.dry_run:
                print(f"Port:  {args.port}\n", flush=True)
        if args.dry_run:
            report = dict(cpu=args.cpu, binary=str(args.bin_file.resolve()), payload_bytes=len(payload),
                          payload_sha256=hashlib.sha256(payload).hexdigest(), stack_pointer=stack,
                          reset_vector=entry, flash_start=TOP_OFFSET, flash_end=TOP_OFFSET + REGION_SIZE,
                          metadata_sectors=list(METADATA_WRITE_ORDER),
                          dry_run=True, hardware_accessed=False, live_layout_verified=False,
                          reset_requested=False, reset_after_flash=args.reset)
        else:
            display(ProgressEvent("connect", 0, 1, detail="Opening recovery port"))
            with open_session(args.port) as session:
                report = flash(session, payload, progress=display)
                verified = True
                if args.reset:
                    display(ProgressEvent("reset", 0, 1, detail="Requesting normal boot"))
                    report.update(reset_after_flash(session))
                    reset_acknowledged = True
                    display(ProgressEvent("reset", 1, 1, detail="Reset acknowledged"))
            report["elapsed_seconds"] = round(time.monotonic() - started, ELAPSED_DECIMALS)
        display.finish()
        if args.json:
            print(json.dumps(report, indent=2))
        elif args.dry_run:
            print("Binary validated. Dry run: no device accessed.")
        else:
            elapsed = report["elapsed_seconds"]
            sectors = len(report["sectors_written"])
            if sectors:
                print(f"Flashed and verified {sectors} sectors ({format_size(sectors * SECTOR_SIZE)}) in {elapsed:.1f}s.")
            else:
                print(f"Already up to date. Verified in {elapsed:.1f}s; no flash writes needed.")
            print("Normal boot requested." if args.reset else "Speaker remains in recovery. Use --reset to boot after flashing.")
        return 0
    except KeyboardInterrupt:
        display.finish()
        message = "interrupted"
        if reset_acknowledged:
            message += "; firmware is verified and reset was acknowledged"
        elif verified and args.reset:
            message += "; firmware is verified; reset completion is unknown"
        print(f"\njbl-bl: {message}", file=sys.stderr)
        return INTERRUPTED_EXIT_CODE
    except (OSError, ValueError, RuntimeError) as error:
        if reset_acknowledged:
            message = f"Firmware is verified and reset was acknowledged; finalization failed: {error}"
        elif verified and args.reset:
            message = f"Firmware is verified, but reset failed: {error}"
        else:
            message = str(error)
        display.failure(message)
        return 1


if __name__ == "__main__":
    sys.exit(main())
