# SPDX-License-Identifier: Apache-2.0
"""Run the standalone JBL bootloader flasher against an existing recovery port."""

from pathlib import Path
import sys

from runners.core import RunnerCaps, ZephyrBinaryRunner


SUPPORTED_CPU = "cputop"
DRY_RUN_PORT = "<recovery-port>"
FLASH_TOOL = Path(__file__).resolve().parents[1] / "jbl_bl.py"


class JblBlBinaryRunner(ZephyrBinaryRunner):
    """Flash TOP in recovery, optionally resetting after successful verification."""

    def __init__(self, cfg, *, port=None, cpu=SUPPORTED_CPU, dry_run=False,
                 json_output=False, reset=False):
        super().__init__(cfg)
        if cpu != SUPPORTED_CPU:
            raise ValueError(f"Unsupported JBL CPU: {cpu}")
        self.port = port
        self.cpu = cpu
        self.dry_run = self.dry_run or dry_run
        self.json_output = json_output
        self.reset = bool(reset)

    @classmethod
    def name(cls):
        return "jbl-bl"

    @classmethod
    def capabilities(cls):
        return RunnerCaps(commands={"flash"}, dry_run=True, reset=True)

    @classmethod
    def do_add_parser(cls, parser):
        parser.add_argument("--port", help="Recovery serial port; required for hardware flashing")
        parser.add_argument("--cpu", choices=(SUPPORTED_CPU,), default=SUPPORTED_CPU,
                            help="Firmware processor (only cputop is supported)")
        parser.add_argument("--json", action="store_true",
                            help="Print the final flasher result as JSON; progress stays on stderr")

    @classmethod
    def do_create(cls, cfg, args):
        return cls(cfg, port=args.port, cpu=args.cpu, dry_run=args.dry_run,
                   json_output=args.json, reset=args.reset)

    def do_run(self, command, **kwargs):
        if command != "flash":
            raise ValueError(f"Unsupported jbl-bl command: {command}")
        if not self.port and not self.dry_run:
            raise ValueError("Flashing requires --port and a device already in recovery")
        self.ensure_output("bin")
        tool = [sys.executable, str(FLASH_TOOL),
                "--port", self.port or DRY_RUN_PORT,
                "--bin-file", str(Path(self.cfg.bin_file).resolve()),
                "--cpu", self.cpu]
        if self.json_output:
            tool.append("--json")
        if self.reset:
            tool.append("--reset")
        # check_call implements west's dry run without invoking the tool.
        self.check_call(tool)
