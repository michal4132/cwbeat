# WS9652A registers

Recovered register names and observed field behavior. `?` marks an inferred
meaning; **unknown** means not decoded. Unlisted bits and reset values are
unknown. MMIO accesses are 32-bit; PMU registers contain 8-bit values. `n`
means the corresponding pin, endpoint or channel number. Observed accesses
do not establish read/write permissions or reserved-bit behavior.

## GPIO

Bank 0: **`0x40014000`**. Bank 1: **`0x40014040`**.

| Offset | Register        | Bits       | Meaning                                                                       |
| ------ | --------------- | ---------- | ----------------------------------------------------------------------------- |
| `0x00` | OUTPUT          | `n`        | Output latch for pin n: `0` low, `1` high. Separate from direction.           |
| `0x04` | MODE            | `n`        | `0` output; `1` input, with the output driver released.                       |
| `0x08` | POLARITY        | `n`        | Bit read as an XOR mask for INPUT[n]; hardware inversion behavior is unknown. |
| `0x0c` | PULL_DOWN       | `n`        | `1` enables the pin's internal pull-down; `0` disables it.                    |
| `0x10` | INPUT           | `n`        | Sampled input bit.                                                            |
| `0x14` | IRQ_MASK        | `n`        | `1` masks the pin interrupt; `0` unmasks.                                     |
| `0x1c` | IRQ_STATUS      | `n`        | Pin interrupt indication. Latching and clearing behavior unknown.             |
| `0x30` | ROUTE30, bank 0 | `n`        | Cleared to select GPIO; alternate functions unknown.                          |
| `0x38` | ROUTE38, bank 0 | `8:7`      | Both set for the recovered GPIO7/8 I²C0 route; clear for GPIO.                |
| `0x38` | ROUTE38, bank 0 | Other pins | Cleared for GPIO; set-bit meanings unknown.                                   |
| `0x3c` | ROUTE3C, bank 0 | `n`        | Cleared for GPIO, also clear for I²C0 pins 7/8. Other selections unknown.     |
| `0x20` | ROUTE20, bank 1 | `n`        | Cleared for GPIO; alternate functions unknown.                                |
| `0x24` | ROUTE24, bank 1 | `n`        | Cleared for GPIO; alternate functions unknown.                                |

| Address            | Register               | Bits | Meaning                                               |
| ------------------ | ---------------------- | ---- | ----------------------------------------------------- |
| `0x40014100 + 4*n` | Bank 0 pin latch alias | `0`  | Write `0`/`1` to clear/set only pin n's output latch. |
| `0x40014200 + 4*n` | Bank 1 pin latch alias | `0`  | Same for bank 1.                                      |

GPIO interrupt: **IRQ5**. Actual implemented/bonded pin counts are unknown.

`PULL_DOWN` is bank-relative offset `0x0c`: bank 0 address `0x4001400c`,
bank 1 address `0x4001404c` (offset `0x4c` from the GPIO block base).
Enabled pull-down resistance measured approximately 22 kΩ on GPIO0.5;
tolerance and resistance on other pins are unknown. A pull-up control has
not been identified.

## Clock control

Base: **`0x40000000`**.

This system clock block is separate from the PMU direct and alternate buses.
Its devicetree node is `clkctrl`, compatible `zgmicro,ws9652a-clock`.
The I2C controller references it through `clocks` phandles: `WS9652A_CLOCK_CORE` (`1`)
selects the core rate and `WS9652A_CLOCK_BUS` (`2`) selects the divided bus rate.
Board and SoC runtime code use the core ID with the named `clkctrl` device.

The Go 4 board DTS supplies the external 26 MHz crystal reference as a root
`xtal` node with `compatible = "fixed-clock"`. The controller references it
through its `xtal` clock input. Early and runtime decoding use that DT frequency;
the recovered absolute PLL tables require a 26 MHz reference at build time.
This fixed reference model does not expose oscillator enable or tuning controls.
WS9652A DCXO controls and the RFPLL/DPLL mapping remain unverified.


| Offset | Register     | Bits   | Meaning                                                                      |
| ------ | ------------ | ------ | ---------------------------------------------------------------------------- |
| `0x10` | BUS_DIVIDER  | `10:8` | Primary divisor = field + 1.                                                 |
| `0x10` | BUS_DIVIDER  | `1:0`  | Secondary divisor = field + 1. Bus rate = core rate / both divisors.         |
| `0x40` | CLOCK_SOURCE | `4`    | `0`: 26 MHz source; `1`: PLL selection below.                                |
| `0x50` | CLOCK_SELECT | `9`    | Select PLL family C; takes precedence over bit 8.                            |
| `0x50` | CLOCK_SELECT | `8`    | Select PLL family B when bit 9 is clear; otherwise family A when both clear. |
| `0x50` | CLOCK_SELECT | `3:0`  | Family A frequency index.                                                   |
| `0x50` | CLOCK_SELECT | `3:2`  | Family B/C frequency index; bits 1:0 in these modes are not decoded.         |

| Decoded index | Family A | Family B   | Family C |
| ------------- | -------- | ---------- | -------- |
| `0`           | 96 MHz   | 156.3 MHz  | 192 MHz  |
| `1`           | 128 MHz  | 78.15 MHz  | 96 MHz   |
| `2`           | 96 MHz   | 39.075 MHz | 48 MHz   |
| `4`           | 48 MHz   | —          | —        |
| `5`           | 64 MHz   | —          | —        |
| `6`           | 48 MHz   | —          | —        |
| `8`           | 24 MHz   | —          | —        |

Recovered frequency encodings. Other index values, accuracy and tolerance
are unknown.

The clock-control driver exposes live rate reads only. BT retains ownership of
clock transitions; enable, disable and rate-setting operations return
`-ENOTSUP`. Rate reads compare two register observations, with up to three
attempts to reject changing selections. This cannot lock out BT, detect every
intermediate transition or guarantee that the clock stays unchanged after a
successful read. Raw diagnostic snapshots preserve the source/select/divider
words, but are sequential observations, not an atomic clock-tree snapshot.
Zephyr SysTick calibration and its observation work remain SoC responsibilities.

## Timers and watchdog

Base: **`0x40018000`**. Timer interrupt: **IRQ4**.

| Timer | Load offset | Current-count offset | Enable bit in CONTROL | Reload bit in CONTROL | Group-enable bit |
| ----- | ----------- | -------------------- | --------------------- | --------------------- | ---------------- |
| 0     | `0x04`      | `0x08`               | `0`                   | `1`                   | `30`             |
| 1     | `0x14`      | `0x18`               | `4`                   | `5`                   | `30`             |
| 2     | `0x24`      | `0x28`               | `8`                   | `9`                   | `31`             |
| 4     | `0x1c`      | `0x20`               | `6`                   | `7`                   | `30`             |

| Offset                   | Register       | Bits              | Meaning                                                                                                 |
| ------------------------ | -------------- | ----------------- | ------------------------------------------------------------------------------------------------------- |
| `0x00`                   | CONTROL        | Enable bits above | Set to start the corresponding timer; clear to stop.                                                    |
| `0x00`                   | CONTROL        | Reload bits above | Select reload mode; full mode semantics not characterized.                                              |
| `0x00`                   | CONTROL        | `31:30`           | Shared timer-group enables as mapped above.                                                             |
| `0x00`                   | CONTROL        | `12`              | Watchdog enable.                                                                                        |
| `0x00`                   | CONTROL        | `13`              | Watchdog mode; encoding unknown.                                                                        |
| Load/count offsets above | LOAD / CURRENT | `31:0`            | Load value / descending count. Implemented width unknown.                                               |
| `0x30`                   | IRQ_STATUS     | `4,2,1,0`         | Pending timer4/2/1/0 interrupt, respectively.                                                           |
| `0x30`                   | IRQ_STATUS     | `3`               | Watchdog-related slot indication?                                                                       |
| `0x34`                   | IRQ_ACK        | `4,2,1,0`         | Write the corresponding pending bit to acknowledge.                                                     |
| `0x38`                   | READBACK       | `0`               | Bit read during timer2 counter access; hardware function unknown.                                      |
| `0x3c`                   | IRQ_MASK       | `4:0`             | Per-channel mask: `1` masks. Bit 3 is used while stopping watchdog.                                     |
| `0x40`                   | WDT_UPDATE     | `0`               | Clear before protected watchdog changes; set afterward.                                                 |
| `0x44`                   | WDT_LOAD       | `31:0`            | Watchdog count argument; physical width/time unit unknown.                                              |
| `0x4c`                   | WDT_FEED       | `0`               | Write whole-word `1` to feed watchdog.                                                                  |

## Radio and sleep timing

Addresses observed from **BT**; TOP access is unverified.

| Absolute address           | Register           | Bits   | Meaning                                                                       |
| -------------------------- | ------------------ | ------ | ----------------------------------------------------------------------------- |
| `0x40034234`               | RADIO_COARSE       | `27:0` | Coarse radio counter.                                                        |
| `0x40034238`               | RADIO_FINE         | Word   | Fine radio counter; field width and unit unknown.                            |
| `0x4003423c`, `0x40034240` | RADIO_COMPARE0     | Word   | Compare pair 0; individual fields unknown.                                    |
| `0x40034244`               | RADIO_CONTROL0     | Word   | Compare pair 0 control; fields unknown.                                       |
| `0x4003425c`, `0x40034260` | RADIO_COMPARE1     | Word   | Compare pair 1; individual fields unknown.                                    |
| `0x40034264`               | RADIO_CONTROL1     | Word   | Compare pair 1 control; fields unknown.                                       |
| `0x40034268`, `0x4003426c` | RADIO_COMPARE2     | Word   | Compare pair 2; individual fields unknown.                                    |
| `0x40034270`               | RADIO_CONTROL2     | Word   | Compare pair 2 control; fields unknown.                                       |
| `0x40034274`               | RADIO_IRQ_MASK     | Word   | Interrupt mask; bit assignments unknown.                                      |
| `0x40034278`               | RADIO_IRQ_ACK      | Word   | Interrupt acknowledgment; bit assignments unknown.                            |
| `0x4003427c`               | RADIO_IRQ_STATUS   | Word   | Interrupt status; bit assignments unknown.                                    |
| `0x4003431c`, `0x40034320` | RADIO_TRIGGER0     | Word   | Scheduled trigger pair, control at `0x40034318`; fields unknown.              |
| `0x40034328`, `0x4003432c` | RADIO_TRIGGER1     | Word   | Scheduled trigger pair, control at `0x40034324`; fields unknown.              |
| `0x40034248`               | SLEEP_TIMING       | Word   | Sleep-related timing value; encoding unknown.                                 |
| `0x40008020/24/28/2c`      | CALIBRATION_SETUP  | Word   | Calibration setup words; fields unknown.                                      |
| `0x40008700`               | CALIBRATION_RESULT | Word   | Calibration measurement; encoding unknown.                                    |
| `0x40006fc0/c4/c8`         | SLEEP_CORRECTION   | Word   | Sleep correction values; fields unknown.                                      |

## PMU access paths

Both paths access the same PMU byte-register space:

| Path | Access sequence | Role of `0x34` |
| --- | --- | --- |
| I2C | Controller `0x4001c000` → internal route 1 → slave `0x34` → PMU register index | Transmitted as address byte `0x68` for writes or `0x69` for reads. |
| Direct bridge | Bridge `0x40038000` → PMU register index and read/write command → data | Used only by the generic API to select the direct path; the recovered code does not supply `0x34` to the bridge. |

The direct path uses memory-mapped bridge registers, not a CPU memory mapping
of each PMU register. CWbeat exposes it as the separate `pmu_bridge` bus provider
(`zgmicro,ws9652a-pmu-bridge`). The logical PMU MFD owns C7 selection, identity,
preparation and child transactions, with byte access through bridge or I2C
adapters. It does not own either controller's MMIO or transfer gate.

The default DTS places one PMU child under `pmu_bridge`. The
[PMU I2C overlay](../boards/jbl/go4/pmu_i2c.overlay) moves that same PMU to
`pmu@34` on `i2c_internal`, retaining a bridge reference for inherited-mode
handover; it does not create a second PMU.

## PMU direct bus

Base: **`0x40038000`**. This accesses the separate PMU byte register space.
The address range is accessed from TOP and BT; physical sharing and
cross-core arbitration are unknown. This is a byte-transfer engine, not a
CPU mapping of the individual PMU registers. Its transfers have no
software-programmed I2C slave-address phase.

| Offset | Register   | Bits  | Meaning                                                      |
| ------ | ---------- | ----- | ------------------------------------------------------------ |
| `0x00` | CONTROL    | `0`   | Bus enable.                                                  |
| `0x00` | CONTROL    | `1`   | Read request; set to begin, clear after completion.          |
| `0x00` | CONTROL    | `2`   | Write request; set to begin, clear after completion.         |
| `0x00` | CONTROL    | `5`   | Preparation control; set during setup, clear during cleanup. |
| `0x00` | CONTROL    | `6`   | Gate control; set during setup, clear during cleanup.        |
| `0x04` | WRITE_DATA | `7:0` | Outgoing PMU byte.                                           |
| `0x08` | READ_DATA  | `7:0` | Incoming PMU byte.                                           |
| `0x0c` | STATUS     | `0`   | Completion indication; sampled as set, then clear.          |
| `0x0c` | STATUS     | `1`   | Transfer error.                                              |
| `0x10` | DIRECTION  | `0`   | Whole-word `1` selects the internal route.                   |
| `0x14` | INDEX      | `7:0` | PMU register index.                                          |

| Address      | Register            | Bits | Meaning                                                                                                |
| ------------ | ------------------- | ---- | ------------------------------------------------------------------------------------------------------ |
| `0x4002000c` | BUS_GATE            | `27` | Set during transfers, cleared afterward; electrical scope unknown.                                      |
| `0x424001ec` | Gate bit-band alias | `0`  | Write `1` during transfers, `0` during cleanup. Maps to `0x4002000c` bit 27; electrical scope unknown. |

The `register_bus` provider (`zgmicro,ws9652a-register-bus`) owns this shared
gate and the local mutex used by both engines. The gate's name does not establish
a system clock output; no clock-control mapping is implemented.

## Routed I2C controller / PMU alternate bus

Base: **`0x4001c000`**. Route 0 reaches external I2C0 on GPIO7/8
(SC89619D at `0x6a` on Go 4 type 1); route 1 reaches internal PMU address
`0x34`. Both routes share the controller registers and transfer gate. One
`zgmicro,ws9652a-i2c` parent owns the engine; its
`zgmicro,ws9652a-i2c-channel` children select route 0 (`i2c0`) or route 1
(`i2c_internal`) and provide standard Zephyr I2C APIs. The controller requires
no PMU readiness or backend selection and can serve PMU preparation itself.

| Offset | Register  | Bits     | Meaning                                                                                                   |
| ------ | --------- | -------- | --------------------------------------------------------------------------------------------------------- |
| `0x00` | CONTROL   | `27`     | Controller enable.                                                                                        |
| `0x00` | CONTROL   | Low bits | Clock divider payload; field width and hardware divisor encoding unknown.                                |
| `0x04` | COMMAND   | `5`      | START component; command `0x28` starts an address transfer.                                               |
| `0x04` | COMMAND   | `4`      | STOP request.                                                                                             |
| `0x04` | COMMAND   | `3`      | WRITE component; command `0x08` transmits TX_DATA.                                                        |
| `0x04` | COMMAND   | `2:1`    | Command `0x06` reads the final byte; individual bit meanings unknown.                                     |
| `0x08` | TX_DATA   | `7:0`    | Address, register index, or outgoing byte.                                                                |
| `0x0c` | RX_DATA   | `7:0`    | Incoming byte.                                                                                            |
| `0x10` | STATUS    | `0`      | Transfer busy.                                                                                            |
| `0x10` | STATUS    | `1`      | STOP busy.                                                                                                |
| `0x10` | STATUS    | `2`      | NACK indication.                                                                                          |
| `0x10` | STATUS    | `3`      | Arbitration error?                                                                                        |
| `0x10` | STATUS    | `8`      | STOP complete.                                                                                            |
| `0x20` | DIRECTION | `0`      | Whole-word `0` selects external I2C0; `1` selects the internal PMU route.                                  |

### PMU transport selection and ownership

Both engines implement reads and writes of the same indexed PMU byte-register
space. Stock TOP mode 3 selects the direct engine: `0x49784 -> 0x4a8b6`
first reads C7 directly and, if necessary, uses I2C route 1/address `0x34`
to write `(C7 | 4) & ~8`. The reverse configuration is also implemented:
`0x49784 -> 0x4a874` clears C7 bit 2 and selects I2C for subsequent accesses.
SBL uses I2C for ordinary F5/F4 reads and register 02/1B writes, so this
transport is not limited to bootstrap commands.

The generic API retains address `0x34` even in direct mode. TOP read/write
dispatchers `0x497fc/0x4989c` compare bus 1's address with the direct-address
cache at `0x200005f8`, then call direct helpers `0x44ff8/0x45020` on a match.
Consequently an API call carrying `0x34` does not prove an I2C wire transaction.
BT selects the same mode through `0x799b4 -> 0x85cdc -> 0x87934` and caches
`0x34` at `0x20021cbc`. Its dispatchers `0x85e10/0x85ef4` then use the direct
engine. A failed ordinary direct transfer returns its error without switching
to I2C. Reinitialization can invoke I2C if the direct C7 probe fails or bit 2
is clear, so steady-state direct selection does not exclude all later I2C use.

The direct engine avoids the software address/data/STOP sequence; higher
speed, stronger access rights and equivalence of every undocumented register
side effect have not been established.

Charger route 0 and PMU route 1 use one command/data/status engine in the
observed CPU map and must be used sequentially. Direct PMU access uses another
register block, but both engines manipulate gate `0x424001ec`; simultaneous
use is not established as safe. Identical TOP/BT addresses alone do not prove
whether their register banks and gate are physically shared or banked.

The traced external charger client is TOP; both TOP and BT contain PMU clients.
Local IRQ masking and the firmware's local I2C locks do not establish
cross-core ownership. TOP's external-I2C object uses handle `0x2000036c`;
BT's named `I2C Lock` uses handle `0x2002179c`. The separate shared-lock
infrastructure was not found in the traced I2C/PMU paths. CWbeat requires a
verified held-peer window for both I2C routes. The register-bus provider starts
with admission closed; peer lifecycle code opens it after verified hold and
closes it under the same mutex before release. Direct runtime access uses the
same local mutex without requiring held peers. Neither the mutex nor the prior
successful direct probes establish cross-core hardware arbitration.

## PMU byte registers

Indices below are **8-bit PMU indices**, not MMIO offsets.

Battery and prerequisite conversion definitions apply to revisions identified
by `ID_F5=0x01, ID_F4=0xbb`, or `ID_F5=0x02..0x04`. Other revision mappings
are unknown.

| Index | Register | Bits | Meaning |
| ----- | -------- | ---- | ------- |
| `0x00` | POLICY_00 | `7:0` | Control byte; electrical fields unknown. |
| `0x01` | POLICY_01 | `7:0` | Control byte; electrical fields unknown. |
| `0x02` | POLICY_02 | `3` | Control bit set during initialization; electrical function unknown. |
| `0x02` | POLICY_02 | `2` | Control bit; electrical function unknown. |
| `0x04` | POLICY_04 | `7:0` | Control byte; electrical fields unknown. |
| `0x05` | BATTERY_CONTROL | `7:0` | Write `0x01` to start battery conversion, `0x00` to disarm. Other encodings unknown. |
| `0x06` | BATTERY_RESULT | `7:0` | Battery ADC result byte; effective hardware resolution and voltage scale unknown. |
| `0x07` | POLICY_07 | `7:0` | Control byte; electrical fields unknown. |
| `0x08` | PREREQUISITE_ENABLE_08 | `5` | Set for prerequisite conversion; analog function unknown. |
| `0x09` | POLICY_09 | `7:2` | Cleared during initialization; electrical functions unknown. |
| `0x09` | POLICY_09 | `1` | Preserved during initialization; electrical function unknown. |
| `0x09` | POLICY_09 | `0` | Set for prerequisite conversion; analog function unknown. |
| `0x0c` | POLICY_0C | `7:0` | Control byte; electrical fields unknown. |
| `0x0d` | POLICY_0D | `7:0` | Control byte; electrical fields unknown. |
| `0x12` | POLICY_12 | `7:0` | Control byte; electrical fields unknown. |
| `0x17` | PREREQUISITE_CONTROL | `7:0` | Write `0x01` to start prerequisite conversion, `0x00` to disarm. Other encodings unknown. |
| `0x18` | PREREQUISITE_STATUS | `0` | `1`: prerequisite conversion result ready. Clearing behavior unknown. |
| `0x19` | PREREQUISITE_RESULT_LOW | `7:0` | Low byte of prerequisite conversion result. |
| `0x1a` | PREREQUISITE_RESULT_HIGH | `7:0` | High byte of prerequisite conversion result. |
| `0x1b` | POLICY_1B | `7:0` | Control byte; electrical fields unknown. |
| `0xc0` | STATUS_C0 | `4` | Status flag; physical meaning unknown. |
| `0xc0` | STATUS_C0 | `3:2` | State code; physical states unknown. |
| `0xc0` | STATUS_C0 | `1` | `1`: battery conversion result ready in BATTERY_RESULT. Clearing behavior unknown. |
| `0xc1` | ACK_C1 | `7:0` | Status acknowledgment? Written with the sampled STATUS_C0 byte. |
| `0xc2` | STATUS_C2 | `3` | Cleared by read/modify/write during initialization; physical meaning unknown. |
| `0xc7` | BACKEND | `2` | Direct/internal backend enabled. |
| `0xc7` | BACKEND | `3` | Cleared by normal mode-3 initialization; another initializer argument sets it. Physical meaning unknown; the stock fast path checks only bit 2. |
| `0xf4` | ID_F4 | `7:0` | Revision identifier used with ID_F5=`0x01`. |
| `0xf5` | ID_F5 | `7:0` | Revision identifier. |

The prerequisite result is `(PMU[0x1a] << 8) | PMU[0x19]`; its physical
quantity, effective resolution and units are unknown. Conversion timing and
result-latching behavior are unknown for both conversion types.

## USB device controller

Base: **`0x40048000`**. Indexed registers use the endpoint selected by INDEX.
MUSB-derived meanings marked `?` are not independently established for WS9652A.

| Offset  | Register            | Bits  | Meaning                                                                                   |
| ------- | ------------------- | ----- | ----------------------------------------------------------------------------------------- |
| `0x000` | FADDR               | `6:0` | USB device address, `0..127`.                                                             |
| `0x004` | POWER               | `5`   | High-speed enable? Cleared for full-speed operation.                                      |
| `0x004` | POWER               | `6`   | Soft connect: set to connect, clear to detach.                                            |
| `0x008` | INTR_TX             | `n`   | Endpoint TX event; bit 0 handles all EP0 events. Read-to-clear?                            |
| `0x00c` | INTR_RX             | `n`   | Endpoint RX event, excluding EP0. Read-to-clear?                                          |
| `0x010` | INTR_TX_EN          | `n`   | TX/EP0 event enable.                                                                      |
| `0x014` | INTR_RX_EN          | `n`   | RX event enable.                                                                          |
| `0x018` | INTR_BUS            | `0`   | Suspend event?                                                                            |
| `0x018` | INTR_BUS            | `1`   | Resume event?                                                                             |
| `0x018` | INTR_BUS            | `2`   | USB reset event. Bus event bits read-to-clear?                                            |
| `0x01c` | INTR_BUS_EN         | `2:0` | Enable reset/resume/suspend events, respectively.                                         |
| `0x024` | INDEX               | Word  | Endpoint selector; implemented width unknown.                                             |
| `0x040` | TX_MAXP, indexed    | Word  | IN maximum packet size in bytes; field width unknown.                                     |
| `0x044` | TX_CSR, indexed     | `0`   | TX_READY: offer an IN packet; remains set while pending.                                  |
| `0x044` | TX_CSR, indexed     | `1`   | FIFO not empty?                                                                           |
| `0x044` | TX_CSR, indexed     | `2`   | Underrun indication? Cleared by writing zero.                                             |
| `0x044` | TX_CSR, indexed     | `3`   | FIFO flush command?                                                                       |
| `0x044` | TX_CSR, indexed     | `4`   | Request STALL?                                                                            |
| `0x044` | TX_CSR, indexed     | `5`   | STALL sent indication?                                                                    |
| `0x044` | TX_CSR, indexed     | `6`   | Clear data toggle to DATA0?                                                               |
| `0x048` | RX_MAXP, indexed    | Word  | OUT maximum packet size in bytes; field width unknown.                                    |
| `0x04c` | RX_CSR, indexed     | `0`   | RX_READY: packet available; clear to acknowledge.                                         |
| `0x04c` | RX_CSR, indexed     | `2`   | Overrun indication?                                                                       |
| `0x04c` | RX_CSR, indexed     | `3`   | Receive data error?                                                                       |
| `0x04c` | RX_CSR, indexed     | `4`   | FIFO flush command?                                                                       |
| `0x04c` | RX_CSR, indexed     | `5`   | Request STALL?                                                                            |
| `0x04c` | RX_CSR, indexed     | `6`   | STALL sent indication?                                                                    |
| `0x04c` | RX_CSR, indexed     | `7`   | Clear data toggle to DATA0?                                                               |
| `0x04c` | RX_CSR, indexed     | `15`  | Automatic RX_READY clearing?                                                             |
| `0x050` | RX_COUNT, indexed   | Word  | Received packet byte count.                                                               |
| `0x06c` | RX_PACKETS, indexed | Word  | Receive packet-count limit?                                                               |
| `0x070` | TX_COUNT, indexed   | Word  | Transmit packet byte count.                                                               |
| `0x0a0` | DEVCTL              | `0`   | Session enable?                                                                           |
| `0x104` | EP0_CSR             | `0`   | RX_READY: SETUP/OUT packet available.                                                     |
| `0x104` | EP0_CSR             | `1`   | TX_READY: offer EP0 IN data.                                                              |
| `0x104` | EP0_CSR             | `2`   | STALL sent indication?                                                                    |
| `0x104` | EP0_CSR             | `3`   | DATA_END: final data/control handshake.                                                   |
| `0x104` | EP0_CSR             | `4`   | SETUP_END: previous control transfer interrupted.                                         |
| `0x104` | EP0_CSR             | `5`   | Request STALL?                                                                            |
| `0x104` | EP0_CSR             | `6`   | Command to acknowledge RX_READY.                                                          |
| `0x104` | EP0_CSR             | `7`   | Command to acknowledge SETUP_END.                                                         |
| `0x110` | EP0_RXCOUNT         | Word  | EP0 received packet byte count.                                                           |
| `0x114` | EP0_TXCOUNT         | Word  | EP0 transmit packet byte count.                                                           |
| `0x130` | EP0_CONTROL         | Word  | Unknown; observed initialization value `2`.                                               |
| `0x170` | EP1_TXCOUNT         | Word  | Direct EP1 transmit-count alias.                                                         |
| `0x388` | WRAP_STATUS         | `5:0` | Wrapper events; write sampled set bits to acknowledge. Bit 4: disconnect?                 |
| `0x390` | WRAP_MASK           | `5:0` | Write ones to mask corresponding wrapper events.                                          |
| `0x394` | WRAP_UNMASK         | `5:0` | Write ones to unmask corresponding wrapper events.                                        |
| `0x400` | RUN                 | `13`  | Controller run enable?                                                                    |
| `0x40c` | PHY_CONTROL         | `0,4` | Both set during initialization; electrical meanings unknown.                              |
| `0x414` | RESET               | `0`   | Active-low controller reset? Observed reset sequence: `0`, then `1`.                      |

## USB DMA

Base: **`0x4002e000`**. Documented endpoints: **n = 0, 1, 2**.

| Offset  | Register   | Bits   | Meaning                                                    |
| ------- | ---------- | ------ | ---------------------------------------------------------- |
| `0x0c0` | RX_CONTROL | `n`    | Trigger endpoint n RX DMA.                                 |
| `0x0c0` | RX_CONTROL | `n+5`  | Reset endpoint n RX DMA; self-clearing?                    |
| `0x0c0` | RX_CONTROL | `n+10` | Enable endpoint n RX DMA.                                  |
| `0x120` | TX_CONTROL | `n`    | Trigger endpoint n TX DMA.                                 |
| `0x120` | TX_CONTROL | `n+5`  | Reset endpoint n TX DMA; self-clearing?                    |
| `0x120` | TX_CONTROL | `n+10` | Enable endpoint n TX DMA.                                  |

| Endpoint | RX address offset | RX capacity offset | RX actual count offset | TX address offset | TX length offset |
| -------- | ----------------- | ------------------ | ---------------------- | ----------------- | ---------------- |
| 0        | `0x0c8`           | `0x0d0`            | `0x108`                | `0x128`           | `0x130`          |
| 1        | `0x0d8`           | `0x0e0`            | `0x10c`                | `0x138`           | `0x140`          |
| 2        | `0x100`           | `0x104`            | `0x110`                | `0x148`           | `0x150`          |

| Register type             | Bits   | Meaning                                                         |
| ------------------------- | ------ | --------------------------------------------------------------- |
| Buffer address            | `30:2` | Address payload; accessible DMA regions not fully mapped.       |
| Buffer address            | `1:0`  | Written zero for word alignment.                                |
| Buffer address            | `31`   | Written zero; address translation role unknown.                 |
| Capacity / length / count | Word   | Byte count; implemented width unknown. TX length zero is valid. |

| Absolute address | Register   | Bits  | Meaning                                                     |
| ---------------- | ---------- | ----- | ----------------------------------------------------------- |
| `0x4002f004`     | DMA_MASK0  | `9:4` | USB DMA interrupt channels: set to mask.                    |
| `0x4002f008`     | DMA_MASK1  | `9:4` | Same USB channel mask, second interrupt group.              |
| `0x4002f00c`     | DMA_ACK0   | `9:4` | Write ones to acknowledge selected USB DMA channels.        |
| `0x4002f010`     | DMA_ACK1   | `9:4` | Same acknowledgment, second interrupt group.                |
| `0x4002f900`     | CLOCK_GATE | `5`   | Cleared during USB initialization; active-low clock enable? |

## Peer control and mailbox

| Absolute address   | Register         | Bits  | Meaning                                                                    |
| ------------------ | ---------------- | ----- | -------------------------------------------------------------------------- |
| `0x40000080`       | BT_CONTROL       | `3:2` | Clear both to hold BT, set both to release. Individual bit roles unknown.  |
| `0x40030080`       | BT_FLASH_OFFSET  | Word  | Physical flash offset of BT image; lower image uses `0x50000`.             |
| `0x40030084`       | BT_MAPPING       | Word  | `0` before changing mapping, `1` enables the selected mapping.             |
| `0x4004c000`       | MAILBOX_TX       | `0`   | Set to notify peer; clear mechanism unknown.                               |
| `0x4004c008`       | DSP_COMMAND      | `4:0` | Command code; `1` requests DSP hold.                                       |
| `0x4004c008`       | DSP_COMMAND      | `5`   | Request/pending bit; set with command, clear after polling.                |
| `0x4004c00c`       | DSP_STATUS       | `4`   | DSP hold acknowledgment.                                                   |
| `0x4004c020/24/28` | Peer state words | Word  | Unknown; cleared during DSP hold.                                          |
| `0x40020008`       | SHUTDOWN_CONTROL | `3:2` | Cleared during controller shutdown; physical roles unknown.                |
| `0x4002d03c`       | SHUTDOWN_POLICY  | `1`   | Set during shutdown; physical role unknown.                                |
| `0x4002d03c`       | SHUTDOWN_POLICY  | `3:2` | Cleared during shutdown; physical roles unknown.                           |

## Cortex-M3 registers

Selected core control and debug register fields.

| Absolute address | Register     | Bits    | Meaning                                                                                |
| ---------------- | ------------ | ------- | -------------------------------------------------------------------------------------- |
| `0xe000e010`     | SYSTICK_CTRL | `0`     | Counter enable.                                                                        |
| `0xe000e010`     | SYSTICK_CTRL | `1`     | SysTick exception enable.                                                              |
| `0xe000e010`     | SYSTICK_CTRL | `2`     | `1` core clock; `0` external/reference clock.                                          |
| `0xe000e010`     | SYSTICK_CTRL | `16`    | COUNTFLAG: counter reached zero; reading CTRL clears it.                               |
| `0xe000e014`     | SYSTICK_LOAD | `23:0`  | Reload value; interval is reload + 1 cycles.                                           |
| `0xe000e018`     | SYSTICK_VAL  | `23:0`  | Current down-count; writing clears count and COUNTFLAG.                                |
| `0xe0001000`     | DWT_CTRL     | `0`     | Enable cycle counter.                                                                  |
| `0xe0001000`     | DWT_CTRL     | `25`    | NOCYCCNT: cycle counter unavailable when set.                                          |
| `0xe0001004`     | DWT_CYCCNT   | `31:0`  | Core-cycle count.                                                                      |
| `0xe000e180`     | NVIC_ICER0   | `31:0`  | Write bit n to disable external IRQ n.                                                 |
| `0xe000e280`     | NVIC_ICPR0   | `31:0`  | Write bit n to clear external IRQ n pending state.                                     |
| `0xe000ed04`     | ICSR         | `25`    | Write one to clear pending SysTick.                                                    |
| `0xe000ed04`     | ICSR         | `27`    | Write one to clear pending PendSV.                                                     |
| `0xe000ed08`     | VTOR         | `29:7`  | Vector-table base; upper-bit support depends on core revision.                          |
| `0xe000ed0c`     | AIRCR        | `31:16` | Write key `0x05fa` for control writes.                                                 |
| `0xe000ed0c`     | AIRCR        | `10:8`  | Interrupt priority grouping.                                                           |
| `0xe000ed0c`     | AIRCR        | `2`     | Request system reset.                                                                  |
| `0xe000edf0`     | DHCSR        | `0`     | C_DEBUGEN: debugging enabled.                                                          |
| `0xe000edf8`     | DCRDR        | `31:0`  | Debug core register data word.                                                         |
| `0xe000edfc`     | DEMCR        | `16`    | Debug monitor enable.                                                                  |
| `0xe000edfc`     | DEMCR        | `24`    | Enable trace/DWT facilities.                                                           |

Register sources: [drivers](../drivers), [SoC](../soc/zgmicro/ws9652a),
[timer register audit](official-timers.md),
[Arm Cortex-M3 definitions](https://github.com/ARM-software/CMSIS_6/blob/1c1840af7a7e757d6e2fec3ddb0e5ce0dfcc93c8/CMSIS/Core/Include/core_cm3.h).
