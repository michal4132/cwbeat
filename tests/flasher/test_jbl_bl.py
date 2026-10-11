"""Offline protocol-backed checks of bounded TOP migration and metadata commit."""

import binascii
import importlib.util
from pathlib import Path
import struct
import sys
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "jbl_bl.py"
SPEC = importlib.util.spec_from_file_location("cwbeat_jbl_bl", SCRIPT)
bl = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = bl
SPEC.loader.exec_module(bl)


def payload(length, fill=0xA7):
    data = bytearray([fill]) * length
    bl.PAIR.pack_into(data, 0, bl.TOP_SRAM_RANGES[-1][1],
                      bl.TOP_CPU_BASE + bl.PAIR.size + bl.THUMB_BIT)
    return bytes(data)


def fixture():
    # Unknown regions deliberately contain nonzero data, unlike delta placeholders.
    image = bytearray(bytes(range(256)) * (bl.FLASH_SIZE // 256))
    sector = bytearray(image[:bl.SECTOR_SIZE])
    sector[:len(bl.METADATA_MAGIC)] = bl.METADATA_MAGIC
    bl.SHORT.pack_into(sector, bl.METADATA_CRC_OFFSET,
                       binascii.crc_hqx(sector[bl.METADATA_CRC_START:bl.METADATA_CRC_OFFSET], 0))
    addresses = {0: bl.TOP_OFFSET, 1: 0x50000, 2: 0x150000,
                 4: 0x230000, 5: 0x270000, 6: 0x370000}
    for index in range(bl.PARTITION_COUNT):
        address = addresses.get(index, 0)
        length = bl.LEGACY_TOP_LENGTH if index == 0 else (bl.SECTOR_SIZE if address else 0)
        capacity = bl.TOP_CAPACITY if index == 0 else length
        crc = bl.crc32(image[address:address + length])
        record = bytearray(bl.PARTITION_RECORD.pack(address, length, 0x12345678, 0xABCDEF01,
                                                    b"retain", crc, 0, capacity // bl.SECTOR_SIZE, 0))
        bl.SHORT.pack_into(record, bl.PARTITION_RECORD_CRC_OFFSET,
                           binascii.crc_hqx(record[:bl.PARTITION_RECORD_CRC_OFFSET], 0))
        start = bl.METADATA_HEADER_SIZE + index * bl.PARTITION_RECORD.size
        sector[start:start + len(record)] = record
    image[:bl.SECTOR_SIZE] = sector
    image[bl.SECTOR_SIZE:bl.SECTOR_SIZE * 2] = sector
    boot = bytearray(bl.BOOT_RECORD.pack(bl.BOOT_MAGIC, 0, 1, 6, 5, 0))
    bl.SHORT.pack_into(boot, len(boot) - bl.SHORT.size, binascii.crc_hqx(boot[:-bl.SHORT.size], 0))
    image[bl.BOOT_RECORD_OFFSET:bl.BOOT_RECORD_OFFSET + len(boot)] = boot
    return bytes(image)


class MemorySession(bl.Session):
    """Execute Session's actual read/write/commit code against an in-memory flash."""

    def __init__(self, image):
        super().__init__(None)
        self.image = bytearray(image)
        self.events = []
        self.before_read = lambda address, length: None
        self.before_crc = lambda address, length: None
        self.before_command = lambda command, address: None
        self.full_top_reads = 0
        self.prefix_reads = 0

    @property
    def erases(self):
        return [event[1] for event in self.events if event[0] == "erase"]

    def read_flash(self, address, length, **kwargs):
        if (address, length) == (bl.TOP_OFFSET, bl.TOP_CAPACITY):
            self.full_top_reads += 1
        if (address, length) == (0, bl.PREFIX_SIZE):
            self.prefix_reads += 1
        self.before_read(address, length)
        result = super().read_flash(address, length, **kwargs)
        self.events.append(("readback", address, length))
        return result

    def check_flash(self, address, length, expected):
        self.before_crc(address, length)
        return super().check_flash(address, length, expected)

    def request(self, data, expected, timeout=bl.REQUEST_TIMEOUT):
        bl.require(not self.failed and not self.finished, "Session failed; no retry")
        command = data[0]
        address = bl.WORD.unpack_from(data, 1)[0] if len(data) >= 1 + bl.WORD.size else None
        self.before_command(command, address)
        if command == bl.COMMAND_VERSION:
            return bl.SBL_VERSION
        if command == bl.COMMAND_READ_WORD:
            return bl.WORD.pack(bl.SBL_VECTOR_BASE)
        if command == bl.COMMAND_FLASH_SELECTION:
            return bytes([bl.FLASH_SELECTOR])
        if command == bl.COMMAND_FLASH_ID:
            identity = bytearray(bl.FLASH_ID_SIZE)
            identity[0] = 0xC2
            identity[bl.FLASH_CAPACITY_INDEX] = 6
            return bytes(identity)
        if command == bl.COMMAND_READ:
            address, length = bl.PAIR.unpack_from(data, 1)
            return bytes(self.image[address:address + length])
        if command == bl.COMMAND_CRC:
            address, length, wanted = bl.TRIPLE.unpack_from(data, 1)
            actual = bl.crc32(self.image[address:address + length])
            self.events.append(("crc", address, length, actual))
            return bytes([actual == wanted]) + bl.WORD.pack(actual)
        if command == bl.COMMAND_ERASE:
            sector, count = bl.PAIR.unpack_from(data, 1)
            assert count == 1
            address = sector * bl.SECTOR_SIZE
            self.image[address:address + bl.SECTOR_SIZE] = bytes([bl.BYTE_MASK]) * bl.SECTOR_SIZE
            self.events.append(("erase", address))
            return b""
        if command == bl.COMMAND_WRITE:
            body = data[1 + bl.WORD.size:]
            self.image[address:address + len(body)] = body
            self.events.append(("write", address, len(body)))
            return b""
        raise AssertionError(f"Unexpected command {command:#x}")


class FlasherTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = fixture()
        cls.prefix = cls.original[:bl.PREFIX_SIZE]
        cls.current = cls.original[bl.TOP_OFFSET:bl.TOP_OFFSET + bl.TOP_CAPACITY]

    def plan(self, data):
        return bl.build_plan(self.prefix, self.current, data)

    def installed(self, data):
        plan = self.plan(data)
        return plan.prefix + plan.image + self.original[bl.TOP_OFFSET + bl.TOP_CAPACITY:]

    def test_exact_capacity_and_overflow(self):
        data = payload(bl.TOP_CAPACITY)
        self.assertEqual(self.plan(data).image, data)
        with self.assertRaisesRegex(ValueError, "fit within"):
            bl.validate_payload(payload(bl.TOP_CAPACITY + 1))

    def test_initial_stack_accepts_full_private_sram(self):
        for stack in (0x20000008, 0x20000090, 0x200001C8, 0x200051D0,
                      0x200051D8, 0x20006FF8, 0x20007000, 0x20007008,
                      0x20007080, 0x20007088, 0x20007090, 0x20007098,
                      0x20007FF8, 0x20008000, 0x20008008,
                      0x2000FFF8, 0x20010000):
            with self.subTest(stack=hex(stack)):
                data = bytearray(payload(33))
                bl.WORD.pack_into(data, 0, stack)
                self.assertEqual(bl.validate_payload(data)[0], stack)

    def test_initial_stack_rejects_shared_ram_and_misalignment(self):
        for stack in (0, 0x1FFFFFF8, 0x20000000, 0x20000004, 0x200051D4, 0x200051D9,
                      0x20006FFC, 0x20007094, 0x20007099, 0x20008004,
                      0x20010004, 0x20010008, 0x20020000, 0xFFFFFFF8):
            with self.subTest(stack=hex(stack)):
                data = bytearray(payload(33))
                bl.WORD.pack_into(data, 0, stack)
                with self.assertRaisesRegex(ValueError, "8-byte aligned.*usable"):
                    bl.validate_payload(data)

    def test_legacy_migration_changes_only_descriptor_fields(self):
        plan = self.plan(payload(bl.TOP_CAPACITY))
        allowed = set()
        for sector in bl.METADATA_WRITE_ORDER:
            base = sector + bl.METADATA_HEADER_SIZE
            for offset, size in ((bl.PARTITION_LENGTH_OFFSET, bl.WORD.size),
                                 (bl.PARTITION_CRC_OFFSET, bl.WORD.size),
                                 (bl.PARTITION_RECORD_CRC_OFFSET, bl.SHORT.size)):
                allowed.update(range(base + offset, base + offset + size))
        changed = {index for index, (old, new) in enumerate(zip(self.prefix, plan.prefix)) if old != new}
        self.assertTrue(changed)
        self.assertTrue(changed <= allowed)
        layout = bl.validate_layout(plan.prefix)
        self.assertEqual(layout.top_length, bl.TOP_CAPACITY)
        self.assertEqual(layout.top_crc, bl.crc32(plan.image))
        self.assertEqual(plan.prefix[:bl.SECTOR_SIZE], plan.prefix[bl.SECTOR_SIZE:bl.SECTOR_SIZE * 2])
        self.assertEqual(bl.validate_layout(self.prefix).top_length, bl.LEGACY_TOP_LENGTH)

    def test_affine_crc_prediction_matches_complete_flash(self):
        for length in (bl.TOP_CAPACITY, 17):
            with self.subTest(length=length):
                plan = self.plan(payload(length))
                expected = bl.expected_flash_crc(bl.crc32(self.original), self.prefix, self.current, plan)
                complete = plan.prefix + plan.image + self.original[bl.TOP_OFFSET + bl.TOP_CAPACITY:]
                self.assertEqual(expected, bl.crc32(complete))

    def test_full_install_verifies_top_before_mirror_then_primary(self):
        data = payload(bl.TOP_CAPACITY)
        session = MemorySession(self.original)
        report = bl.flash(session, data)
        plan = self.plan(data)
        self.assertEqual(bytes(session.image), self.installed(data))
        self.assertEqual(session.erases[-3:], [bl.TOP_OFFSET, bl.SECTOR_SIZE, 0])
        self.assertEqual(report["metadata_sectors_written"], [bl.SECTOR_SIZE, 0])
        first_metadata = session.events.index(("erase", bl.SECTOR_SIZE))
        before = session.events[:first_metadata]
        self.assertIn(("readback", bl.TOP_OFFSET, bl.TOP_CAPACITY), before)
        self.assertIn(("crc", bl.TOP_OFFSET, bl.TOP_CAPACITY, plan.top_crc), before)
        self.assertEqual(report["whole_flash_crc"], bl.crc32(session.image))
        self.assertTrue(report["verified"])
        self.assertFalse(session.failed)

    def test_repeat_is_a_verified_no_op(self):
        data = payload(8193)
        session = MemorySession(self.installed(data))
        report = bl.flash(session, data)
        self.assertTrue(report["unchanged"])
        self.assertEqual(session.erases, [])
        self.assertEqual(report["sectors_written"], [])

    def test_shrink_erases_old_tail(self):
        session = MemorySession(self.installed(payload(bl.TOP_CAPACITY)))
        data = payload(31, fill=0x12)
        bl.flash(session, data)
        top = session.image[bl.TOP_OFFSET:bl.TOP_OFFSET + bl.TOP_CAPACITY]
        self.assertEqual(top[:len(data)], data)
        self.assertEqual(top[len(data):], bytes([bl.BYTE_MASK]) * (bl.TOP_CAPACITY - len(data)))
        self.assertEqual(session.image[bl.TOP_OFFSET + bl.TOP_CAPACITY:],
                         self.original[bl.TOP_OFFSET + bl.TOP_CAPACITY:])

    def test_bad_peer_crc_fails_before_any_write(self):
        session = MemorySession(self.original)
        session.image[0x50000] ^= 1
        with self.assertRaisesRegex(ValueError, "partition 1 CRC"):
            bl.flash(session, payload(33))
        self.assertEqual(session.erases, [])
        self.assertTrue(session.failed)

    def test_prefix_race_fails_before_any_write(self):
        session = MemorySession(self.original)
        def change_prefix(address, length):
            if (address, length) == (0, bl.PREFIX_SIZE) and session.prefix_reads == 2:
                session.image[bl.BOOT_RECORD_OFFSET] ^= 1
        session.before_read = change_prefix
        with self.assertRaisesRegex(ValueError, "prefix changed during preflight"):
            bl.flash(session, payload(33))
        self.assertEqual(session.erases, [])
        self.assertTrue(session.failed)

    def test_full_top_readback_failure_prevents_metadata(self):
        session = MemorySession(self.original)
        def corrupt_top(address, length):
            if (address, length) == (bl.TOP_OFFSET, bl.TOP_CAPACITY) and session.full_top_reads == 3:
                session.image[bl.TOP_OFFSET] ^= 1
        session.before_read = corrupt_top
        with self.assertRaisesRegex(ValueError, "before metadata commit"):
            bl.flash(session, payload(33))
        self.assertFalse(set(session.erases) & set(bl.METADATA_WRITE_ORDER))
        self.assertTrue(session.failed)

    def test_mirror_failure_prevents_primary_and_retry(self):
        session = MemorySession(self.original)
        def corrupt_mirror(address, length):
            if (address, length) == (bl.SECTOR_SIZE, bl.SECTOR_SIZE):
                session.image[address] ^= 1
        session.before_read = corrupt_mirror
        with self.assertRaisesRegex(ValueError, "Metadata sector.*readback"):
            bl.flash(session, payload(33))
        self.assertNotIn(0, session.erases)
        self.assertEqual(session.erases[-1], bl.SECTOR_SIZE)
        self.assertTrue(session.failed)
        with self.assertRaisesRegex(ValueError, "no retry"):
            session.boot_version()

    def test_whole_flash_check_detects_unexpected_peer_change(self):
        session = MemorySession(self.original)
        def corrupt_peer(address, length):
            if (address, length) == (0, bl.FLASH_SIZE) and 0 in session.erases:
                session.image[0x50000] ^= 1
        session.before_crc = corrupt_peer
        with self.assertRaisesRegex(ValueError, "outside the planned"):
            bl.flash(session, payload(33))
        self.assertTrue(session.failed)

    def test_only_top_is_accepted_by_ordinary_write_methods(self):
        for address in (0, bl.SECTOR_SIZE, bl.BOOT_RECORD_OFFSET,
                        bl.TOP_OFFSET - bl.SECTOR_SIZE, bl.TOP_OFFSET + bl.TOP_CAPACITY):
            with self.subTest(address=address):
                session = MemorySession(self.original)
                with self.assertRaisesRegex(ValueError, "escape"):
                    session.erase_sector(address)
                with self.assertRaisesRegex(ValueError, "escape"):
                    session.write_flash(address, b"x")
                self.assertEqual(session.events, [])
        session = MemorySession(self.original)
        with self.assertRaisesRegex(ValueError, "escape"):
            session.write_flash(bl.TOP_OFFSET + bl.TOP_CAPACITY - 1, b"xx")

    def test_metadata_gate_rejects_unrelated_prefix_changes(self):
        plan = self.plan(payload(33))
        prefix = bytearray(plan.prefix)
        prefix[bl.BOOT_RECORD_OFFSET] ^= 1
        session = MemorySession(self.original)
        with self.assertRaisesRegex(ValueError, "more than"):
            session.commit_metadata(self.prefix, bytes(prefix), plan.top_crc)
        self.assertEqual(session.erases, [])
        self.assertTrue(session.failed)

    def test_metadata_gate_rejects_unverified_top(self):
        plan = self.plan(payload(33))
        session = MemorySession(self.original)
        with self.assertRaisesRegex(ValueError, "TOP CRC failed"):
            session.commit_metadata(self.prefix, plan.prefix, plan.top_crc)
        self.assertEqual(session.erases, [])
        self.assertTrue(session.failed)

    def test_metadata_interruption_is_fail_stop(self):
        session = MemorySession(self.original)
        def interrupt(command, address):
            if command == bl.COMMAND_WRITE and address == bl.SECTOR_SIZE:
                raise KeyboardInterrupt
        session.before_command = interrupt
        with self.assertRaises(KeyboardInterrupt):
            bl.flash(session, payload(33))
        self.assertTrue(session.failed)
        self.assertEqual(session.erases[-1], bl.SECTOR_SIZE)
        self.assertNotIn(0, session.erases)


class ResetSession(bl.Session):
    """Record the post-flash reset protocol without opening USB."""

    def __init__(self):
        super().__init__(None)
        self.words = {
            bl.SCB_VTOR: bl.SBL_VECTOR_BASE,
            bl.SBL_ZERO_RETURN_ADDRESS: bl.WORD.unpack(bl.SBL_ZERO_RETURN_CODE)[0],
            bl.SBL_HOUSEKEEPING_HOOK: bl.RESET_CLEARED_RAM_WORD,
        }
        self.writes = []
        self.resets = []
        self.keep_writes = True

    def request(self, data, expected, timeout=bl.REQUEST_TIMEOUT):
        bl.require(not self.failed and not self.finished, "Session failed; no retry")
        command = data[0]
        if command == bl.COMMAND_VERSION:
            return bl.SBL_VERSION
        if command == bl.COMMAND_READ_WORD:
            return bl.WORD.pack(self.words[bl.WORD.unpack_from(data, 1)[0]])
        if command == bl.RESET_COMMAND_WRITE_WORD:
            address, value = bl.PAIR.unpack_from(data, 1)
            self.writes.append((address, value))
            if self.keep_writes:
                self.words[address] = value
            return b""
        if command == bl.RESET_COMMAND_NORMAL_BOOT:
            self.resets.append(data[1])
            return b""
        raise AssertionError(f"Unexpected reset command {command:#x}")


class ResetTests(unittest.TestCase):
    def test_reset_only_patches_sbl_housekeeping_not_private_top_ram(self):
        session = ResetSession()
        report = bl.reset_after_flash(session)
        self.assertEqual(session.writes,
                         [(bl.SBL_HOUSEKEEPING_HOOK, bl.SBL_ZERO_RETURN_THUMB)])
        self.assertEqual(session.resets, [bl.NORMAL_BOOT_RESET_MODE])
        self.assertTrue(report["reset_acknowledged"])
        self.assertFalse(report["application_verified"])
        self.assertTrue(session.finished)

    def test_different_resident_stub_prevents_writes_and_reset(self):
        session = ResetSession()
        session.words[bl.SBL_ZERO_RETURN_ADDRESS] ^= 1
        with self.assertRaisesRegex(RuntimeError, "zero-return code differs"):
            bl.reset_after_flash(session)
        self.assertEqual(session.writes, [])
        self.assertEqual(session.resets, [])
        self.assertTrue(session.failed)

    def test_housekeeping_readback_failure_prevents_reset(self):
        session = ResetSession()
        session.keep_writes = False
        with self.assertRaisesRegex(RuntimeError, "readback differs"):
            bl.reset_after_flash(session)
        self.assertEqual(session.resets, [])
        self.assertTrue(session.failed)


if __name__ == "__main__":
    unittest.main()
