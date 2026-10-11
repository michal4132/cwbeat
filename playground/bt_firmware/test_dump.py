"""Offline dump checks with fake hardware; no original firmware needed."""

from contextlib import contextmanager
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import dump
from test_bt_patch import FakeSession


class DumpTests(unittest.TestCase):
    def run_dump(self, output, *, bad_crc=False, bad_identity=False):
        session = FakeSession(bytes(dump.patch.FLASH_SIZE))
        if bad_crc:
            session.check_flash = lambda *args: 0
        if bad_identity:
            session.flash_id = lambda: b'wrong'

        @contextmanager
        def open_session(port):
            try:
                yield session
            finally:
                session.closed = True

        try:
            with patch.object(dump.patch.bl, 'open_session', open_session):
                return dump.dump('FAKE', output)
        finally:
            self.assertTrue(session.closed)
            self.assertFalse(any(op[0] in ('erase', 'write') for op in session.ops))

    def test_verified_dump(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'dump'
            report = self.run_dump(output)
            self.assertEqual((output / 'backup.bin').stat().st_size, dump.patch.FLASH_SIZE)
            self.assertEqual(report['sha256'], dump.patch.sha((output / 'backup.bin').read_bytes()))
            self.assertFalse(report['matches_patch_source'])
            self.assertTrue((output / 'result.json').is_file())

    def test_failed_verification_saves_no_backup(self):
        for failure in ('bad_crc', 'bad_identity'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / 'dump'
                with self.assertRaises(ValueError):
                    self.run_dump(output, **{failure: True})
                self.assertFalse((output / 'backup.bin').exists())

    def test_output_guards_before_connection(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(dump.patch.bl, 'open_session') as connect:
            with self.assertRaises(FileExistsError):
                dump.dump('FAKE', tmp)
            with self.assertRaisesRegex(ValueError, 'outside the repository'):
                dump.dump('FAKE', Path(__file__).resolve().parents[2] / 'dump-output')
            connect.assert_not_called()


if __name__ == '__main__':
    unittest.main()
