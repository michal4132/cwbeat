#!/usr/bin/env python3
"""Build the tested BT stub and require exact checked-in patch bytes. Offline only."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parent
SOURCE_FLASH_SHA256 = "a3e2192ba8c1d3ef39c3049a9648f35466f9d95aa9b66ae438eac2e861862968"
SOURCE_BT_SHA256 = "e95e20967e694928df11cec42d36416662a79eb3f8afa2a35f027ef63bee518c"
STUB_SHA256 = "cb211fa1ad9c55c33df40623fab803f499779d85314f5e65d592c07aab6ee280"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def external_path(path, label):
    path = path.expanduser().resolve()
    if path.is_relative_to(ROOT.parent.parent):
        raise ValueError(f"{label} must be outside the repository: {path}")
    return path


def load_manifest():
    manifest = json.loads((ROOT / "manifest.json").read_text())
    expected = {
        "source_flash_sha256": SOURCE_FLASH_SHA256,
        "source_bt_sha256": SOURCE_BT_SHA256,
        "bt_flash_offset": 0x50000,
        "bt_length": 0xDD000,
        "stub_length": 276,
        "stub_sha256": STUB_SHA256,
    }
    for name, value in expected.items():
        if manifest.get(name) != value:
            raise ValueError(f"Manifest changed tested constraint: {name}")
    patches = manifest["patches"]
    if len(patches) != 2:
        raise ValueError("Expected exactly the tested hook and stub spans")
    for patch, cpu, length in zip(patches, (0x7C4A4, 0xD8000), (4, 276)):
        if patch["cpu_offset"] != cpu or patch["flash_offset"] != 0x50000 + cpu:
            raise ValueError("Unexpected patch address")
        if any(
            len(bytes.fromhex(patch[key])) != length for key in ("old_hex", "new_hex")
        ):
            raise ValueError("Unexpected patch length")
    if patches[0]["old_hex"] != "00210846" or patches[0]["new_hex"] != "5bf0acfd":
        raise ValueError("Hook differs from tested instructions")
    if bytes.fromhex(patches[1]["old_hex"]) != b"\xff" * 276:
        raise ValueError("Expected stub placement is no longer erased padding")
    if sha256(bytes.fromhex(patches[1]["new_hex"])) != STUB_SHA256:
        raise ValueError("Stub manifest bytes differ from tested stub")
    return manifest


def verify_build(elf_path, manifest):
    with elf_path.open("rb") as stream:
        elf = ELFFile(stream)
        if elf["e_machine"] != "EM_ARM" or elf["e_type"] != "ET_EXEC":
            raise ValueError("Expected a linked ARM executable")
        if any(
            section["sh_type"] in ("SHT_REL", "SHT_RELA")
            for section in elf.iter_sections()
        ):
            raise ValueError("ELF still contains relocations")
        payloads = []
        for name, patch in zip((".hook", ".stub"), manifest["patches"]):
            section = elf.get_section_by_name(name)
            if section is None or section["sh_addr"] != patch["cpu_offset"]:
                raise ValueError(f"Incorrect {name} section address")
            data = section.data()
            if data != bytes.fromhex(patch["new_hex"]):
                raise ValueError(f"Built {name} differs from tested manifest bytes")
            payloads.append(data)
        allocated = {
            section.name for section in elf.iter_sections() if section["sh_flags"] & 2
        }
        if allocated != {".hook", ".stub"}:
            raise ValueError(f"Unexpected allocated ELF sections: {sorted(allocated)}")
    return tuple(payloads)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--toolchain-prefix",
        required=True,
        help="GNU ARM tool prefix, e.g. /opt/toolchain/bin/arm-none-eabi-",
    )
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    manifest = load_manifest()
    output = external_path(args.output_dir, "Build output")
    output.mkdir(parents=True, exist_ok=True)
    prefix = str(Path(args.toolchain_prefix).expanduser())
    assembler, linker = prefix + "as", prefix + "ld"
    versions = {
        name: subprocess.check_output([tool, "--version"], text=True).splitlines()[0]
        for name, tool in (("assembler", assembler), ("linker", linker))
    }
    object_path = output / "stub.o"
    elf_path = output / "stub.elf"
    subprocess.run(
        [
            assembler,
            "-mcpu=cortex-m3",
            "-mthumb",
            "-o",
            str(object_path),
            str(ROOT / "adopt_live_pll.S"),
        ],
        check=True,
    )
    subprocess.run(
        [
            linker,
            "--build-id=none",
            "-T",
            str(ROOT / "layout.ld"),
            "-o",
            str(elf_path),
            str(object_path),
        ],
        check=True,
    )
    hook, stub = verify_build(elf_path, manifest)
    (output / "stub.bin").write_bytes(stub)
    report = {
        "exact_manifest_bytes": True,
        "toolchain": versions,
        "source_sha256": sha256((ROOT / "adopt_live_pll.S").read_bytes()),
        "linker_script_sha256": sha256((ROOT / "layout.ld").read_bytes()),
        "manifest_sha256": sha256((ROOT / "manifest.json").read_bytes()),
        "hook_hex": hook.hex(),
        "stub_bytes": len(stub),
        "stub_sha256": sha256(stub),
        "hardware_accessed": False,
    }
    (output / "build-report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
