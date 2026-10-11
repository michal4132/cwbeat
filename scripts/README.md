# Go 4 bootloader flasher

`jbl_bl.py` flashes a Zephyr TOP binary to a JBL Go 4 already in SBL USB
recovery. It runs independently of Zephyr, west and `jbl-cli`; no reference dump,
installed candidate or prepared image directory is required. Its only external
Python dependency is `pyserial`, installed by the module's `make setup`.
For use outside that environment, install it with `python -m pip install pyserial`.

```sh
python scripts/jbl_bl.py --port /dev/cu.usbmodem1101 \
  --bin-file /tmp/cwbeat-go4/zephyr/zephyr.bin
```

`--cpu cputop` is optional and is currently the only supported processor.
The standalone command leaves the speaker in recovery by default; add `--reset`
to request normal boot after flashing and verification. There is no separate
execute flag. It accepts up to **256 KiB**, using the complete lower TOP allocation
at physical flash `[0x10000, 0x50000)`. Shorter binaries are padded with erased
bytes across the allocation, clearing any previous image tail. There is no
reserved stock trailer or CRC-correction word.

The tool changes lower TOP sectors and the lower TOP descriptor in both metadata
copies. It preserves stock BT, DSP, bootloader, boot selection, pairing records,
all other descriptors and unrelated metadata bytes. The tool verifies USB identity,
pinned SBL `1.0.2.71`, the active vector table, flash identity, live boot prefix
and installed firmware CRCs before erasing. It then verifies sector readback,
the final TOP image and the expected whole-flash CRC. Live reads stay in memory; no backup
or generated candidate is written to disk.

The installed lower TOP may use the legacy `0x1E400` checksum length or the new
`0x40000` length. After writing TOP, the tool verifies the entire 256 KiB by byte
readback and native CRC before committing its descriptor. Only the descriptor's
length, native CRC and record CRC16 change; the metadata mirror is committed and
read back first, then the primary. The metadata header CRC remains unchanged
because its covered bytes do not change. Repeating an identical installation
verifies it without erasing or writing anything.

Whole-flash verification predicts the CRC change from the known old/new prefix
and TOP bytes using the native CRC's affine delta. This detects unexpected
changes elsewhere without reading or storing those regions. Each exchange or
verification failure stops the session without retry. Installation is not
transactional: interrupted TOP or metadata writes can leave a mismatched image
or metadata copies and prevent normal boot. See the [TOP flash audit](../docs/top-flash.md)
for native bootloader validation and the remaining hardware mapping uncertainty.

By default, the tool displays progress and a readable result summary. A terminal
gets live progress bars; redirected progress uses plain phase lines. Add `--json`
for a structured final result on stdout, with progress kept on stderr.

This supports the lower TOP allocation on Go 4. It refuses invalid
installed firmware CRCs or boot metadata; repairing an interrupted or damaged
installation is outside its scope.

For an offline binary size/vector check:

```sh
python scripts/jbl_bl.py --bin-file /tmp/cwbeat-go4/zephyr/zephyr.bin --dry-run
```

This prints a readable binary summary without opening USB; add `--json` for
structured output. It does not verify the live device or calculate which sectors
need writing.

The initial stack pointer must be 8-byte aligned and descend into the complete
64 KiB application RAM bank: `[0x20000000,0x20010000)`. A pointer at its upper
end is valid; its lower end is not. The stock-peer arena starts at `0x20010000`
and remains excluded. The vector check does not measure stack depth.

Run the offline flasher tests with:

```sh
python -m unittest discover -s tests/flasher -v
```

For the full 64 KiB SRAM layout, `check_reclaimed_ram.py` checks the built ELF
and executes native startup/recovery without device access. It verifies ordinary
data, BSS and NOINIT placement, rejects the old TOPRAM/retained sections, and
checks the DCRDR recovery cookie with valid/invalid SBL and debugger ownership.
It rejects a linked ROM USB backend because its workspace would overlap application storage. Its
inputs, dependencies and invocation are documented in the
[reclaimed RAM validation](../docs/original-top-ram.md#reclaimed-ram-validation).

## west integration

The `jbl-bl` runner invokes the same standalone script with west's Python
interpreter. From this module in `nix develop`:

```sh
west build -b jbl_go4/ws9652a/cputop -d /tmp/cwbeat-go4 app
west flash -d /tmp/cwbeat-go4 --no-rebuild --runner jbl-bl -- \
  --port /dev/cu.usbmodem1101
```

The board enables post-flash reset by default. Add `--no-reset` after west's
`--` separator to leave the speaker in recovery instead.
Reset also runs when the installed image is already up to date, after verification.
On USB power, TOP and its console can restart while the speaker stays logically
off. Press Power or issue `power on` in the console to start Bluetooth and audio.

With the Makefile:

```sh
make flash
make flash FLASH_ARGS="--port /dev/cu.usbmodem1101 --no-reset"
```

Add `--json` after west's `--` separator to request the standalone tool's JSON
result. The default retains its human-readable progress and summary.

`west flash --dry-run --no-rebuild -d /tmp/cwbeat-go4 --runner jbl-bl` only
prints the invocation; it does not run the standalone binary check or access
hardware. A port is required for actual flashing. `west flash --context` lists
the configured runner, but this Zephyr revision does not load external runners
for runner-specific `--context` options.

## Recovery and boot

Enter recovery before invoking the tool. CWbeat provides the `power recovery` shell
command; if the application is unresponsive, use the physical recovery sequence.
A queued reply only accepts the request: peer shutdown has a 29.55-second budget,
followed by up to 2.25 seconds of console grace/drain, then USB reset and
re-enumeration. Let this finish before opening the loader port. The flasher
verifies SBL readiness before any writes; a missing port or timeout is not a
successful recovery.

Recovery entry is outside this tool's scope. Post-flash reset runs only when
requested and after verification succeeds; it never changes flash boot selection.
There is no standalone reset command. A reset acknowledgement does not prove
that TOP has started successfully.
