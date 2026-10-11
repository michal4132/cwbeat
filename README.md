# CWbeat

Zephyr firmware for the JBL Go 4's ZGMICRO WS9652A TOP processor. The existing
BT and DSP firmware handles Bluetooth, bonding and audio.

Supported target: `jbl_go4/ws9652a/cputop`.

## Setup and build

With Nix flakes enabled, run from this module's directory:

```sh
nix develop
make setup

# Apply the bundled Zephyr fixes once after initial setup.
git -C ../zephyr apply --check "$PWD"/patches/zephyr/*.patch
git -C ../zephyr apply "$PWD"/patches/zephyr/*.patch

make build
```

The shell provides the toolchain and activates `../.venv`. Setup initializes the
west workspace in the parent directory and installs dependencies. The default
build output is `/tmp/cwbeat-go4/zephyr/zephyr.bin`.

For subsequent builds, enter `nix develop` and run `make build`.

## Flash

The speaker must already be in SBL USB recovery. From CWbeat's USB shell, run
`power recovery` and wait for USB to re-enumerate; shutdown can take about
32 seconds.

Inside `nix develop`, use the board's `jbl-bl` west runner, replacing the example
port with the speaker's recovery port:

```sh
west flash -d /tmp/cwbeat-go4 --no-rebuild --runner jbl-bl -- \
  --port /dev/cu.usbmodem1101
```

The Makefile invokes the same runner:

```sh
make flash FLASH_ARGS="--port /dev/cu.usbmodem1101"
```

The runner updates TOP, preserves stock BT/DSP firmware and pairing records,
and reboots after verification. Add `--no-reset` to the runner arguments to
leave the speaker in recovery.

On USB power, press Power or run `power on` in the USB shell to start Bluetooth
and audio after reboot.

See [flashing and recovery details](scripts/README.md) for additional options.
