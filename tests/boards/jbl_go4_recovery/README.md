# Go 4 software recovery

Run the request audit against the actual linked board image:

```sh
.venv/bin/python3 tests/boards/jbl_go4_recovery/verify_request_native.py \
  /path/to/zephyr.elf
```

Ten cases cover privileged and unsupported contexts, debugger and monitor
ownership, unavailable mailbox writes, interrupt-mask preservation, and the
cookie → USB detach → peer hold → reset sequence. The script executes linked
Thumb instructions using Unicorn without patching them or using native callbacks.
The peer-hold result is supplied at that separate driver's call boundary.

The startup half is covered by `scripts/check_reclaimed_ram.py`, including a
corrupt SBL and rejection of the cookie while a debugger owns the mailbox.

The mailbox is Cortex-M3 DCRDR (`0xe000edf8`), containing `0x3152424a` only
between the recovery request and early startup. It requires no reserved SRAM.
The [ARM Cortex-M3 TRM](https://documentation-service.arm.com/static/6036810d5319e554d4ba108e)
describes the debug domain's exclusion from system reset in section 6.3.2 and
software communication through DCRDR in section 10.2.3. Recovery refuses to
claim it when halting debug or a debug monitor is enabled.

These offline tests model debug registers as memory. Actual WS9652A reset
retention, ROM/SBL preservation of the cookie, and USB recovery enumeration
must also pass hardware validation.
