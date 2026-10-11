# Go 4 CONFIG0C hardware characterization

This standalone diagnostic compares the **22 kOhm pull-down enable** bit 5
at GPIO0 `+0x0c` (`0x4001400c`) using the **Volume+ button, GPIO0.5**.
The register was originally called CONFIG0C; the command and trace labels
retain that name so earlier captures remain comparable. A set bit enables the
pull-down and a cleared bit disables it. This is a manual observation experiment;
Twister only builds it. It does not declare an electrical pass based on a
successful register write.

The project adopted the pull-down interpretation and 22 kOhm value on
12 October 2026 from the characterization below. GPIO consumers now request
`GPIO_PULL_DOWN` explicitly; plain `GPIO_INPUT` disables the pull. The test
button's devicetree flags select the initial pull-down. Pinctrl uses
`bias-pull-down` and `bias-disable`. See the
[register reference](../../../docs/ws9652a-registers.md) for the current contract.

The operator confirmed Volume+ is active high: released 0 V, pressed 3.3 V.
Its DTS entry uses `GPIO_ACTIVE_HIGH | GPIO_PULL_DOWN`; ordinary logical GPIO
reads return 0 for released and 1 for pressed. This fixture intentionally
reports raw physical levels to characterize the pad without software inversion.

The image holds BT/DSP, verifies their hold state, and configures Volume+ as
an input once with the existing GPIO driver. That setup selects GPIO routing,
input MODE, a high output latch, zero POLARITY, and CONFIG0C=1. Measurements
then change only CONFIG0C bit 5 using a masked write under the GPIO/pinctrl
lock. Each command attempts to restore the selected bit's value from before
the command, including on a measurement error, and verifies the readback.
A failed restoration blocks further captures; recovery retries it before
requesting reset. Other register bits are preserved.

The gpio-keys, LED and product subsystems are disabled. Power GPIO0.0,
Play/latch-clear GPIO0.3, audio controls, straps and unidentified pins are not
test targets. The board's ordinary boot initialization still runs.

## Build and load

From the repository root:

```sh
nix develop -c west build -p always -b jbl_go4/ws9652a/cputop \
  -d /tmp/cwbeat-config0c-trace tests/boards/jbl_go4_config0c
```

Keep USB attached throughout the experiment: normal board boot can release
the battery power hold. Load this image with the existing
[recovery/flashing procedure](../../../scripts/README.md), using
`/tmp/cwbeat-config0c-trace/zephyr/zephyr.bin`. For example, once SBL recovery is ready:

```sh
nix develop -c python scripts/jbl_bl.py --port /dev/cu.usbmodem1101 \
  --bin-file /tmp/cwbeat-config0c-trace/zephyr/zephyr.bin --reset
```

Open the native USB CDC console (`0ac8:bb91`) and assert DTR. A serial terminal
such as `python -m serial.tools.miniterm /dev/cu.usbmodem1101 115200` can be used
from the development environment. Save the full transcript, firmware hash,
board revision, supply conditions and physical button state for each run.
The diagnostic uses the `config0c> ` prompt. `config0c status` identifies the
trace-capable version as `trace_v2`; the first diagnostic image did not record
individual edges.

## Measure

First run `config0c status`. Initialization must have succeeded and both peers
must be held before sampling is allowed. Status also exposes the register
snapshot so startup output need not have been captured.

Run the following series without touching other buttons:

| Physical state throughout the command | Command | Purpose |
| --- | --- | --- |
| Volume+ released | `config0c compare 2000` | Two-second CONFIG0C=1, 0, then 1 windows |
| Volume+ held down before issuing the command | `config0c compare 2000` | Same three windows under a fixed pressed stimulus |
| Volume+ released again | `config0c compare 2000` | Check repeatability after release |

Repeat the series at least three times. Keep the physical state fixed for all
three windows in each comparison. The repeated CONFIG0C=1 window helps reveal
drift or a result that does not reverse when the bit is restored.

For button activity, run separate longer captures and press/release Volume+
several times within each window:

```text
config0c sample 1 10000
config0c sample 0 10000
config0c sample 1 10000
```

The optional duration is milliseconds per window, default 2000, range
100–10000. Sampling requests a 1 ms sleep between observations. On the first
hardware image this produced 2 ms sample spacing (500 samples/second), not
1000 samples/second. Use measured counts, elapsed time and sample gaps rather
than assuming the requested sleep is the sample period.

The trace version records the first and last sampled levels, sample-gap
minimum/maximum, and the timestamp, new level and preceding sampled run
duration for each retained edge. It also reports initial/final sampled runs
and raw INPUT immediately before and after CONFIG0C writes. All times are
kernel millisecond observations, not the physical instant of an edge. Short
pulses between samples can be missed. No software debounce is applied.

Each window retains at most 128 edges. High/low and edge totals continue
counting after the buffer fills; the reported dropped-edge count makes an
incomplete trace explicit. Sampling does not print to the console. The last
window is restored before printing its trace, followed immediately by a
100 ms read-only observation with the saved CONFIG0C value and the same
register/peer guards. This distinguishes the final sampled level under the
test setting from the level after restoring the normal setting.

The shell command finishes after its bounded windows and restoration
observation; it does not wait for further input while capturing.

Readback mismatches, loss of peer hold, or changes to guarded OUTPUT, MODE,
POLARITY, interrupt mask, routes or unrelated CONFIG0C bits invalidate a
capture. A successful capture establishes only that the measurement completed
with those software checks satisfied. Record failures as inconclusive, fix
the cause, and repeat rather than interpreting them as CONFIG0C behavior.

## Software follow-up after the first observations

The following is the historical investigation record, including the
uncertainties recorded before the pull-down interpretation and 22 kOhm value
were adopted. Those earlier cautions do not describe the current driver API.

User-supplied captures on 12 October 2026 used the first diagnostic binary,
SHA-256 `ad9a1b40cfafca1c0bdb3d45019c1e487f800819c4aaac0cfd2ce84cd30608d1`.
Fixed-state comparisons labelled "not pressed" returned low and those labelled
"pressed" returned high, at all three CONFIG0C settings. A later description
said pressing produced low with CONFIG0C=1, so the mapping of physical state
to raw level needed confirmation. All register guards, peer checks and
restoration readbacks passed.

The subsequent activity sequence produced:

| CONFIG0C[5] | Samples | High | Low | Rising | Falling |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1, first run | 5000 | 671 | 4329 | 3 | 3 |
| 0 | 5000 | 4248 | 752 | 13 | 12 |
| 1, repeat | 5000 | 1187 | 3813 | 3 | 3 |

The operator confirmed three physical press/release cycles and a released
button at the end of the CONFIG0C=0 run. Its 13 rising versus 12 falling
sampled edges imply an initial sampled low and a final sampled high. That
differs from the earlier released-state label, but the later polarity
description conflicts with that label. It is not proof of a particular pull,
debounce or hysteresis function. The original capture did not record input
levels after restoration. Differences in the high sample totals alone are
not comparable without matched physical timing.

Two further activity captures reported CONFIG0C=0 with 5000 samples,
4630 high/370 low and 1 rising/0 falling edge; CONFIG0C=1 had
2829 high/2171 low and 1 rising/1 falling edge. The operator described
CONFIG0C=0 as remaining mostly high while operating the button and
CONFIG0C=1 as low while pressed. Edge totals establish sampled transitions,
but without action timestamps they do not establish which edge corresponds
to a physical press or release.

The subsequent [trace_v2 console log](../../../tio_tty.usbmodem1101_2026-10-12T16:16:26.log)
contains two `config0c sample 1 2000` runs. The operator reported completing
the requested released-first, held-second sequence; the log itself has no
physical-state labels. With that order:

| Physical state from requested order | Main observation | Post-restoration observation |
| --- | --- | --- |
| Released | 1000 low, 0 high; first=last=0 | 50 low; before/after restoration=0 |
| Held | 1000 high, 0 low; first=last=1 | 50 high; before/after restoration=1 |

Both runs had no edges, 2 ms minimum/maximum sample gaps, no register or peer
failures, and verified CONFIG0C=1 restoration. This supports released=0 and
pressed=1 in the tested CONFIG0C=1 profile, resolving the earlier conflicting
description subject to the reported test order. It does not yet explain the
CONFIG0C=0 activity behavior.

The next [trace_v2 activity log](../../../tio_tty.usbmodem1101_2026-10-12T16:19:48.log)
contains `sample 0 10000` followed by `sample 1 10000`, after a request to
start released, press, release, and remain released through restoration.
The transcript records raw input times, not the operator's action times:

| CONFIG0C[5] | Main observation | Restoration and recovery |
| --- | --- | --- |
| 0 | Starts low; rises at 2406 ms; remains high through the last sample at 9998 ms; no falling edge | Immediate reads before/after restoring 1 are both high; first guarded recovery sample is low; all 50 recovery samples are low |
| 1 | Starts low; rises at 2312 ms; falls at 7120 ms; ends low | Immediate reads before/after restoration and all 50 recovery samples are low |

Both runs contain 5000 main samples with exactly 2 ms sample gaps. No edges
were dropped, every register/peer guard passed, and restoration was verified.
If the requested release occurred before the CONFIG0C=0 window ended, this
shows a missing sampled return to the released level under that setting.
The return to low following restoration associates the discrepancy with the
CONFIG0C setting in this profile. The immediate post-write read is still high;
it must not be described as an immediate low read. That read and the first
guarded recovery sample both have `t_ms=0`, so this trace does not resolve the
transition's precise latency. It also does not identify an electrical mechanism
or establish the physical release time from the raw edges.

The following [held-before-start log](../../../tio_tty.usbmodem1101_2026-10-12T16:27:28.log)
was supplied after instructions to hold Volume+ before starting
`config0c sample 0 10000`, release about two seconds into the capture, and
remain released through the prompt. Raw INPUT is already high immediately
before clearing CONFIG0C and remains high immediately afterward. All 5000
main samples are high, from 0 through 9998 ms, with no sampled edges and
exactly 2 ms sample gaps. The immediate restoration reads are again high,
followed by 50 low recovery samples starting at recovery `t_ms=0`. All guards
pass and restoration is verified.

Assuming the requested release occurred during this window, the missing
return to low does not require a rising transition while CONFIG0C is zero:
the high level can be established before clearing the bit. Together these
captures support keeping CONFIG0C[5]=1 for reliable Volume+ release reporting
in the tested profile. The GPIO driver's input configuration already sets
CONFIG0C; no driver change or electrical register renaming follows from these
results. Repeating the same digital experiment alone cannot distinguish pad
voltage behavior from behavior inside the input path.

The operator then measured the Volume+ signal relative to board GND with a
multimeter connected during `config0c sample 0 10000`:

| Setting | Released voltage | Pressed voltage |
| --- | ---: | ---: |
| CONFIG0C=1 before command | 0.31 mV | 3.29 V |
| CONFIG0C=0 during command | 28 mV | 3.29 V |
| CONFIG0C=1 after restoration | 0.31 mV | Not reported |

Release was described as a fast return to 28 mV during CONFIG0C=0. This
reversible voltage change strengthens the suspected pull-down-enable
interpretation for the tested input profile. The meter's finite input
resistance provides a path to GND and can discharge a floating input; this
plausibly explains why the probed voltage now falls after release despite
the earlier unprobed raw input staying high. Instrument loading of
high-impedance sources is described in the
[Keysight measurement tutorial](https://www.keysight.com/us/en/assets/9018-01063/user-manuals/9018-01063.pdf).
The meter's input resistance and range were not recorded, and no serial
trace from the probed capture has yet been supplied. Do not infer a pull
resistance from these voltages or claim the probed raw INPUT returned low
without that trace. The exact internal circuit and CONFIG0C semantics in
other pad configurations remain unverified.

The operator subsequently reported direct resistance-mode readings between
GPIO0.5 and GND of 22 kOhm at idle and 25 MOhm while the command ran. These
readings support a switched pull-down, with 22 kOhm a plausible effective
resistance under the meter's test conditions. Input mode itself does not
invalidate that measurement, and the powered state alone does not prove the
reading wrong. An ohmmeter applies its own test stimulus: the reading can be
meaningful if other currents are negligible and the test voltage stays in
the pad's normal range, but those conditions have not been characterized.
Offsets and parallel leakage can affect the result, particularly the
25 MOhm reading; it should not be taken as a literal internal resistor value.
The standard unpowered procedure for ordinary resistance measurements is
described in
[Fluke's resistance measurement procedure](https://www.fluke.com/en-us/learn/blog/digital-multimeters/how-to-measure-resistance).
Removing power here also removes the configured internal pull state, so it
does not verify the enabled pull. Retain both readings as reported in-circuit
observations: approximately 22 kOhm is a candidate effective enabled pull,
with very high apparent resistance when disabled. The exact resistance and
its dependence on pin voltage remain unverified. See also
[Keysight's description of test current and offset compensation](https://www.keysight.com/us/en/assets/9018-03876/service-manuals/9018-03876.pdf).

A read-only stock audit also cautions against calling this a debounce bit:
TOP helper `0x44da4` sets CONFIG0C from `option & 1` in output mode. In input
mode it sets the output latch high, selects CONFIG0C as `!(option & 2)`, and
sets POLARITY from `option & 1`. Bank-1 helper `0x44cf8` has the same contract.
This was checked against original TOP SHA-256
`0d7b8f82fa37732367a77f46af607297abfd06197f502d5b23e420a96fcd8750`,
matching the original full flash at `[0x10000:0x2e400]`. These are recovered
software operations, not decoded electrical meanings.

With `trace_v2`, first run `config0c sample 1 2000` with the button released
throughout, then repeat while holding it pressed before the command starts.
Label both traces explicitly. These establish the physical mapping without
inferring it from aggregate high/low counts during a moving stimulus.

Then repeat the three ten-second `sample` commands above. For each run,
start released, press at approximately two seconds, release at approximately
five seconds, and leave Volume+ released for the rest of the capture and
restoration observation. Note the physical action times; firmware timestamps
describe raw samples, not labelled presses. Repeat the complete 1→0→1 series
at least three times. Also run one series with no button presses to check
for spontaneous transitions.

Inspect whether extra edges cluster around presses/releases or continue
during the untouched final interval, whether the final sampled level is high,
and whether restoration to CONFIG0C=1 returns the known released level. A
positive result would localize when the discrepancy occurs; it still would
not identify the electrical mechanism. Keep MODE, output latch and routing
fixed throughout.

## Offline software checks

The host check compiles the actual diagnostic C source against scripted
MMIO/time/shell stubs. It exercises trace accounting, buffer overflow, input
validation and capture/restoration failure paths without accessing a speaker:

```sh
python3 tests/boards/jbl_go4_config0c/verify_trace_native.py
```

It requires a host C compiler (`CC`, default `cc`). These checks validate the
software's observations and recovery behavior, not WS9652A electrical behavior.
On macOS, `SDKROOT` can select an installed SDK compatible with the compiler.
The local verification used `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk`
to avoid this machine's default compiler/SDK mismatch.

The `trace_v2` image used for the hardware captures above is 60,960 bytes, SHA-256
`678163a1bacb7e81a2a71c0f01cf341545e6a5e513ffb248669392b24ec75115`.
All 28 native scenario groups passed. Flash readback and whole-flash CRC
verification passed, followed by a successful boot and `config0c status`.
A hardware `config0c sample 1 100` smoke capture and its 100 ms restoration
observation each recorded 50 samples, 2 ms minimum/maximum gaps, no guard
failures, and verified restoration. Physical button state was not labelled
during this smoke check; it establishes fixture operation, not polarity or
CONFIG0C semantics. The serial port was closed afterward.

After adopting the pull-down API, all 83 focused GPIO, pinctrl, board-ID and
stock-peer test cases passed, including board-ID configurations with and without
pinctrl. All 21 boot verification scenarios and 28 diagnostic host scenario
groups passed. The production application and this diagnostic both built for
`jbl_go4/ws9652a/cputop`. This validation covers the software migration; the
rebuilt images were not flashed as part of that change.

## Interpret and finish

Compare the raw high/low distribution for the same physical stimulus across
pull-down enabled→disabled→enabled. If both settings track the button
identically, report **no digital difference observed in this experiment**.
External circuitry or a connected meter can keep an otherwise floating input
at a defined level even with the internal pull disabled. The capture checks
the observed behavior; it does not measure the pull-down resistance.

Optional oscilloscope measurements at a verified Volume+ net can record pad
voltage and transition shape alongside the digital captures. The board's
existing external circuitry may mask internal effects. This fixture does not
configure outputs, inject an external voltage, test floating pads or characterize
interrupts; those require a separate experiment with verified wiring.

Use `config0c recovery` to return to SBL and restore the normal application
with the existing flasher. This command requests a reset after a short console
drain; confirm SBL enumeration (`0ac8:38b9`) before flashing. If initialization
failed, status and recovery remain available. No automatic recovery or
product startup is scheduled by the diagnostic.
