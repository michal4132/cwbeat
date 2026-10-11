# Shared register bus tests

Run `west build -b qemu_cortex_m3 tests/drivers/register_bus_ws9652a -t run`.

The fixture compiles the production provider with only gate MMIO replaced. Real
Zephyr mutexes and threads check recursive ownership, the bounded contention
timeout, closed admission after waiting for another owner, and ISR/pre-kernel
refusal. Initialization and lock operations must not touch the gate.
