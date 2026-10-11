# WS9652A memory map

Known regions only. Range ends are exclusive.

## TOP CPU memory

| Start        | End          | Size    | Region                              |
| ------------ | ------------ | ------- | ----------------------------------- |
| `0x00040000` | `0x00080000` | 256 KiB | Configured application flash window |
| `0x20000000` | `0x20010000` | 64 KiB  | Private SRAM                        |
| `0x20010000` | `0x20020000` | 64 KiB  | Shared SRAM; SBL during boot        |

## Physical flash

| Start.       | End          | Size    | Region                                      |
| ------------ | ------------ | ------- | ------------------------------------------- |
| `0x00007000` | `0x00010000` | 36 KiB  | SBL; loaded at TOP `0x20010000`             |
| `0x00010000` | `0x00050000` | 256 KiB | Lower TOP image; mapped at TOP `0x00040000` |

## TOP MMIO

| Address/base | Block/register                                         |
| ------------ | ------------------------------------------------------ |
| `0x40000000` | Clock control                                          |
| `0x40000080` | BT control                                             |
| `0x40014000` | GPIO bank 0 and pin control                            |
| `0x40014040` | GPIO bank 1 and pin control                            |
| `0x40014100` | GPIO bank 0 pin output aliases                         |
| `0x40014200` | GPIO bank 1 pin output aliases                         |
| `0x40018000` | Timers and watchdog                                    |
| `0x4001c000` | I2C controller: external route 0, internal PMU route 1 |
| `0x40020008` | Shutdown control                                       |
| `0x4002000c` | Shared direct/I2C transfer gate, bit 27                |
| `0x4002d03c` | Shutdown policy                                        |
| `0x4002e000` | USB DMA                                                |
| `0x4002f000` | DMA interrupt control                                  |
| `0x4002f900` | USB clock gate                                         |
| `0x40030080` | BT flash mapping                                       |
| `0x40030088` | TOP flash mapping                                      |
| `0x40038000` | Direct PMU bridge                                      |
| `0x40048000` | USB controller                                         |
| `0x4004c000` | Peer mailbox and DSP control                           |
| `0x424001ec` | Bit-band alias of `0x4002000c` bit 27                  |

PMU registers use a separate 8-bit index space accessed through the direct
bridge or internal I2C route at address `0x34`. The logical PMU MFD has no MMIO
region. Separate providers own the direct bridge, routed I2C controller and
shared gate; the two I2C channel nodes select routes, not separate register banks.
