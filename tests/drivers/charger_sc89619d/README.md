# SC89619D charger tests

The QEMU fixture drives the production charger through Zephyr's charger API and
scripted I2C/GPIO controllers. It checks register transactions, output latch
preservation, error propagation and GPIO ordering without physical hardware.

Run from the workspace with:

```sh
west twister -p qemu_cortex_m3 -T cwbeat/tests/drivers/charger_sc89619d -i
```

Initialization checks the SC89619(D) family identity, then configures an optional
active-low `/CE` output without an initial level. Omitted current properties cause
no register writes. Explicit current settings round down within the documented
range. The driver preserves charge enable, voltage, watchdog and other policy
settings. The family identity does not distinguish the D variant; the board's
compatible selects its pinout.

The supported properties are online, status, charge type, charging/trickle/
termination/input currents and charge voltage. Currents support reads and writes;
voltage is read-only and includes the independent fine adjustment. Other
properties, including notifications, return `-ENOTSUP`. API calls require thread
context; ISR and pre-kernel calls return `-EWOULDBLOCK` before I/O.

Enabling first reads the control register. If this read fails, `/CE` is untouched.
It then deasserts `/CE`, sets the software charge-enable bit, and asserts `/CE`
only on success. A subsequent failure leaves the hardware enable deasserted when
the GPIO operation succeeded. Disabling deasserts `/CE` first and still attempts
software disable if that GPIO operation fails. An error is returned if either
operation fails; the call does not promise transactional rollback. Without a
`ce-gpios` property, only the software gate is controlled. Read-modify-write does
not replay the self-clearing watchdog-reset bit.

The inherited watchdog remains significant: if enabled and allowed to expire,
hardware can restore default register settings, including software charge enable.
The driver does not service or disable it automatically. USB source detection can
also replace the programmed input current limit.

Coverage includes all coarse/fine voltage combinations, current boundaries and
rounding, neighboring fields, unsupported operations, initialization failures,
transport failures with unchanged getter output, GPIO failures and both enable
gate orderings. This fixture does not establish board wiring, analog charging
behavior, interrupt timing or physical current/voltage accuracy.
