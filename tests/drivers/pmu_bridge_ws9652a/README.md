# Direct register bridge tests

Run `west build -b qemu_cortex_m3 tests/drivers/pmu_bridge_ws9652a -t run`.

The fixture compiles the production bridge and shared domain with MMIO replaced
by scripted registers. It checks native register ordering, bounded completion
polling and error precedence, PRIMASK restoration, unchanged output after a
failed read, no-write refusals, and reset admission while peers are held. The
domain uses real Zephyr mutexes; peripheral timing and electrical behavior are
not modeled.

The ARM instruction oracle in `../mfd_ws9652a/verify_backend_native.py` also runs
the bridge from the linked Go4 image. Its kernel mutex entry points are modeled;
the actual bridge and gate-provider instructions execute against scripted MMIO.
