# WS9652A PMU regression tests

The ztest suite compiles the production power policy with mocked bus operations.
It compares the recovered policy sequence with the healthy sequence, checks
identity before writes, recovery eligibility and rejection, diagnostic history,
and the absence of transactions on repeated calls after success or failure.

From the project root:

```sh
nix develop -c python ../zephyr/scripts/twister \
  -T tests/drivers/mfd_ws9652a -p qemu_cortex_m3 \
  --outdir /tmp/cwbeat-pmu-policy-tests --inline-logs
```

The transport fixture compiles the actual adapters, C7 selection, policy and
public PMU API against mocked bus endpoints. It covers both selected transports,
handover, retry bounds and uncertain writes. The I2C-only fixture instead uses
real DTS PMU and ADC devices on a generic I2C bus, with the WS9652A controller
and bridge drivers disabled; it also checks initialization ordering.

```sh
nix develop -c west twister \
  -T tests/drivers/mfd_ws9652a_bootstrap \
  -T tests/drivers/mfd_ws9652a_i2c_only -p qemu_cortex_m3 \
  --outdir /tmp/cwbeat-pmu-transport-tests --inline-logs
```

`verify_backend_native.py` executes the extracted direct byte bridge and gate
provider machine code from a built Go 4 `zephyr.elf`. It requires the Python
packages `capstone`, `pyelftools`, and `unicorn`:

```sh
python tests/drivers/mfd_ws9652a/verify_backend_native.py \
  /path/to/go4-build/zephyr/zephyr.elf --json /tmp/cwbeat-pmu-helper-native.json
```

Its 28 cases check uninterpreted C7 bytes, byte writes, error/timeout paths,
null/busy/lock refusal, unchanged output after failed reads, permitted controller
writes, PRIMASK protection and restoration, calling convention, and ordered raw
clock snapshots. MMIO responses and kernel lock entry points are modeled;
bridge and gate-provider instructions execute unchanged. The adjacent
`../register_bus_ws9652a` and `../pmu_bridge_ws9652a` QEMU fixtures test real
mutex/admission behavior and the native register sequence. These tests do not
access a speaker or validate electrical behavior and timing.

The adjacent `../mfd_ws9652a_runtime` fixture compiles the production MFD wrapper
with a synthetic device and mocked transport. It tests the enabled and disabled
API, pre-kernel/ISR rejection, unchanged output on failure, early abort, two
concurrent readers, serialization against preparation, and bounded contention.
It also checks that identity reads remain available after backend setup when
the later stock profile fails, while child transactions still require completed
preparation.
The enabled fixture defines the opt-in symbol for that translation unit; the
real board build separately exercises its Kconfig dependency on the PMU node.

```sh
nix develop -c python ../zephyr/scripts/twister \
  -T tests/drivers/mfd_ws9652a -T tests/drivers/mfd_ws9652a_runtime \
  -p qemu_cortex_m3 --outdir /tmp/cwbeat-pmu-runtime-tests --inline-logs
```

The CWbeat application enables the validation shell and battery ADC in
[`app/prj.conf`](../../../app/prj.conf). With the PMU and ADC child enabled in
devicetree, another application can enable both identity and battery commands
with:

```text
CONFIG_SHELL=y
CONFIG_MFD_WS9652A_RUNTIME_READ=y
CONFIG_ADC=y
CONFIG_ADC_WS9652A=y
CONFIG_CWBEAT_BATTERY=y
CONFIG_CWBEAT_PMU_DIAGNOSTICS=y
```

Each driver has a separate log level, following Siana's module-local logging
pattern. The default level reports PMU preparation and terminal failures.
Enable detailed transport and admission diagnostics with:

```text
CONFIG_MFD_WS9652A_LOG_LEVEL_DBG=y
CONFIG_I2C_WS9652A_LOG_LEVEL_DBG=y
CONFIG_WS9652A_PMU_BRIDGE_LOG_LEVEL_DBG=y
CONFIG_WS9652A_REGISTER_BUS_LOG_LEVEL_DBG=y
CONFIG_ADC_WS9652A_LOG_LEVEL_DBG=y
CONFIG_WS9652A_PEER_LOG_LEVEL_DBG=y
```

Messages include the device and failed operation. PMU debug logs retain C7
bootstrap statuses and attempt counts; I2C failures include route, address,
phase and STOP status. No logs run inside the byte-transfer or polling loops.

Once preparation has successfully established the selected PMU backend,
`pmu probe 1000 10` asynchronously samples C7/F5/F4 1,000 times with at least
10 ms between completed reads. Backend readiness is retained if the subsequent
stock profile fails, so identity reads can diagnose that state. Before backend
setup they return `-EAGAIN`; probes do not perform preparation themselves.
Child-driver transactions and battery conversions still require the full
preparation sequence to reach `POWER_POLICY_DONE`.

`pmu status` reports progress, first/last values and the first failure;
`pmu stop` synchronously cancels the run. A changed value stops the run with
`-EILSEQ`. No failed transfer is retried, and no PMU register is written by the
probe. Direct reads still set
and clear the controller's ordinary request/gate bits. Timing is measured using
kernel ticks and includes preemption; it is not an electrical timing measurement.
The module's Kconfig defaults remain off; the CWbeat application explicitly
enables these options. Successful identity reads do not establish cross-core
arbitration or validate conversion/charger writes.

## Hardware validation, 13 October 2026

The type-1 Go 4 passed these runs with the previously installed
[guarded BT startup-clock patch](../../../bt_firmware/README.md). The TOP-only
flash preserved BT and verified the expected whole-flash CRC. Probe binary
SHA-256: `1a206769e186489f1f7694776ee8034802d092065677287a5e05342686d3ef1d`.

| Scenario | Successful C7/F5/F4 samples | Result |
| --- | ---: | --- |
| BT awake, no phone | 1,000 | Stable identity, no transport errors |
| Playback, two pause/resume cycles, volume down/up | 4,000 | Stable identity; all six control requests completed with status 0 |
| Native shutdown and subsequent held peers | 4,000 | Stable identity; shutdown completed and audio became quiescent |
| Fresh wake after TOP reset, codec initialization, phone reconnection | 2,000 | Stable identity; probe was already running before BT readiness/mode 3 |
| Explicit probe cancellation during playback | 52 | Count remained unchanged after synchronous stop |

All 11,052 samples (33,156 byte reads) returned `C7=04 F5=01 F4=bb`.
Two additional probes before preparation each stopped on the first attempt
with `-EAGAIN` and zero samples. The final state had the probe inactive, BT
playing, audio ready and all reported subsystem errors zero. Serial was closed.
This tested shutdown followed by a full TOP reset, not same-boot BT restart.
It did not test battery conversions or other runtime PMU register writes.

The enabled and disabled QEMU fixtures plus policy regression passed all
20 cases; the exact probe ELF passed all 23 native transport cases. Both the
default and probe board configurations built successfully. Local evidence:
`/tmp/cwbeat-pmu-runtime-hardware.json`, `/tmp/cwbeat-pmu-runtime-*.log`,
`/tmp/cwbeat-pmu-runtime-flash.json`, `/tmp/cwbeat-pmu-runtime-native.json`,
and `/tmp/cwbeat-pmu-runtime-tests/twister.json`.

## Battery ADC

`CONFIG_ADC_WS9652A=y` enables the ADC child of the PMU, with compatible
`zgmicro,ws9652a-adc`. Its standard Zephyr ADC API performs the class-0x14
conversion from stock TOP `0x46b9c`, including prerequisite check `0x46794`, and
returns the raw conversion byte. Conversion triggers, bounded polling, cleanup
and latched conversion faults belong to the ADC driver. The MFD owns PMU
protocol, identity and preparation, plus the local lock used by each conversion.
Conversions require successful completion of the stock preparation sequence;
backend readiness alone only permits read-only identity probes. Controller MMIO,
gate access and byte transfers live in the separate bridge and I2C providers.
Both native I2C routes require held peers, including when the
[overlay](../../../boards/jbl/go4/pmu_i2c.overlay) selects I2C for this PMU;
the default bridge permits experimental runtime access with local exclusion.

`CONFIG_CWBEAT_BATTERY=y` enables `cwbeat_battery_read()` from
`<cwbeat/battery.h>`. This helper takes an ADC device and reads seven conversions
through `ws9652a_adc_read_raw()`, which exposes the same conversion with detailed
failure diagnostics. Each conversion takes a separate local lock, matching
stock wrapper `0x4237c`. The helper drops the minimum and maximum full-byte results,
averages five with integer division, then masks the average to six bits. Raw
bytes and the filtered ADC code are returned; no millivolt scale, charge state
or percentage is inferred. Sampling and filtering belong to `subsys/battery/`.

With `CONFIG_CWBEAT_PMU_DIAGNOSTICS=y`, the validation shell in
`subsys/diagnostics/` provides PMU identity commands and, when the battery helper
is enabled, `pmu battery`. This takes one seven-conversion sample;
`pmu battery 100 100` takes up to 100 such samples with a 100-ms gap
after each. `pmu status` and `pmu stop` also apply. Probes use their own work
queue so conversion polling sleeps do not block the system work queue.
The first error stops the probe. `samples` counts completed seven-conversion
batches in battery mode; `completed` identifies progress within the last batch.

Preparation preserves the once-per-reset stock sequence, including its `05=1`
write even when the ADC driver is disabled. As in stock, a nonzero 05 observed
by a battery read is cleared once and returns `-EBUSY`; that call yields no
battery value.
A later read requires a separate request. Enable bits in 08/09 remain set as
in stock. Backend and supported identity are checked before each conversion.

The port deliberately tightens stock's error handling: every attempted trigger
gets one disarm attempt, reads/writes are checked, failed prerequisites do not
arm 05, and failed cleanup cannot publish a sample. A failed write blocks
further battery access until reset, retaining the original diagnostic. Other
failures abort the batch and leave the caller's battery output unchanged.
This does not change the preparation policy or introduce arbitrary register writes.

The `../mfd_ws9652a_battery` fixture covers the ADC child and optional battery
helper, including raw ADC access with the helper disabled, context restrictions,
RMW preservation, trimming/mask order, all 107 possible bus failures in the
normal seven-conversion path, busy/startup cleanup,
prerequisite failure, exact polling limits, identity/backend refusal, and
serialization across polling sleeps. Run it alongside the existing fixtures:

```sh
nix develop -c python ../zephyr/scripts/twister \
  -T tests/drivers/mfd_ws9652a -T tests/drivers/mfd_ws9652a_runtime \
  -T tests/drivers/mfd_ws9652a_battery -p qemu_cortex_m3 \
  --outdir /tmp/cwbeat-battery-tests --inline-logs
```

`../mfd_ws9652a_battery/verify_stock_native.py` executes both the original TOP
instructions and the new compiled single-conversion routine. It compares bus
and requested-delay traces on healthy, busy, timeout and read-error paths,
and verifies the deliberate error-handling differences. Bus, revision classifier
and sleep boundaries are scripted; this is not an electrical/timing model.

```sh
python tests/drivers/mfd_ws9652a_battery/verify_stock_native.py \
  --stock /path/to/original/lower-top.bin \
  --elf /path/to/battery-build/zephyr/zephyr.elf \
  --json /tmp/cwbeat-battery-stock-native.json
```

### Battery hardware validation, 13 October 2026

The same type-1 Go 4 passed the following runs with the guarded BT startup-clock
patch retained. The TOP-only flash verified the payload and whole-flash CRC.
Battery probe binary SHA-256:
`2ea8758af262f09b9babf954c13b596d53f60163e11b3e3dc91bf460bbeecbc0`.

| Scenario | Successful batches | ADC conversions | Result |
| --- | ---: | ---: | --- |
| Explicit read after clearing the startup trigger | 1 | 7 | Valid sample; playback remained healthy |
| Playback, two pause/resume cycles, volume down/up | 300 | 2,100 | All six control requests completed with status 0 |
| Playback across USB unplug/reconnect | 1,000 | 7,000 | User confirmed the requested battery-only interval; probe completed without reset |
| Native shutdown and subsequent held peers | 100 | 700 | Shutdown completed and audio became quiescent |
| Fresh wake after TOP reset and codec initialization | 100 | 700 | Sampling began before BT readiness; audio became ready |

All 1,501 batches (10,507 ADC conversions) completed without transport,
polling or cleanup errors. Every batch's trimmed ADC code was 63; each reported
last batch contained seven raw bytes of 63. The longest observed batch took
36.6 ms, including scheduling. The prerequisite value in the reported samples
was 648–651. USB removal/reconnection occurred during the 135-second probe;
the requested unplug interval was about 20 seconds, not independently timed.
These readings do not establish a voltage scale, percentage, charge/full state,
accuracy across the discharge range, or cross-core hardware arbitration.

The first call after preparation in each of the two boots observed `05=1`,
cleared it successfully and returned `-EBUSY` with zero samples and no latched
fault. A separate explicit request then succeeded. The fresh-wake test used a
full TOP reset after shutdown; it did not exercise same-boot BT restart.
At completion, the probe was inactive, BT was playing, audio was ready and
all reported subsystem errors were zero. The serial port was closed.

All 33 QEMU cases passed across policy, enabled/disabled runtime identity and
enabled/disabled battery configurations. The exact probe ELF passed 23 native
transport cases and 16 comparisons against original TOP conversion instructions.
Both the then-default and battery probe board configurations built successfully.
Local evidence: `/tmp/cwbeat-battery-hardware.json`,
`/tmp/cwbeat-battery-*.log`, `/tmp/cwbeat-battery-flash.json`,
`/tmp/cwbeat-battery-stock-native.json`, `/tmp/cwbeat-battery-bus-native.json`,
and `/tmp/cwbeat-battery-tests/twister.json`.

## Refactor verification

The earlier MFD/ADC/subsystem split passed 64 QEMU test cases across policy, runtime
access, ADC/filter, SoC counter and peer-control fixtures. The Go 4 application
built both with battery support and with ADC/filter disabled while retaining
identity diagnostics. The resulting application ELF passed all 24 native
transport/clock-snapshot cases and all 16 compiled conversion cases.

Conversion checks exercised only the new implementation against scripted bus
responses; the full original-firmware comparison was not run because its stock
binary was unavailable. No hardware was flashed or tested for this refactor.
The earlier hardware results above describe the earlier binaries, not this one.

The subsequent transport extraction passed 5 shared-domain and 7 bridge QEMU
cases, plus all 28 native bridge/clock cases against the integrated Go 4 ELF.
This validates local admission/locking and scripted byte-engine behavior;
the new transport layout has not been flashed or hardware-tested.
