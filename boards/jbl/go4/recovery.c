/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
#include <errno.h>
#include <cwbeat/boards/jbl_go4/recovery.h>
#include <zephyr/drivers/peer/ws9652a.h>
#include <cmsis_core.h>

#include <zephyr/soc/ws9652a/reset.h>
#define RECOVERY_COOKIE       UINT32_C(0x3152424a)
#define SBL_BASE              UINT32_C(0x20010000)
#define SBL_IMMUTABLE_BYTES   UINT32_C(0x9000)
/* Complete immutable window SHA256:
 * 15df802acd6a9936ed5624239eec197a4f1d11d48fb0f90c395a028739231630.
 * SBL initialized data begins at 0x20019000 and is excluded. */
#define SBL_FNV1A             UINT32_C(0x0e4d2951)
#define FNV_OFFSET_BASIS      UINT32_C(2166136261)
#define FNV_PRIME             UINT32_C(16777619)
#define SBL_STACK_TOP         UINT32_C(0x20020000)
#define SBL_USB_SERVICE_THUMB UINT32_C(0x200114cd)
#define SBL_USB_ONLY          2u
#define SBL_HOUSEKEEPING_HOOK UINT32_C(0x2001ced8)
/* Resident movs r0,#0; bx lr. Keep the hook independent of writable TOP flash. */
#define SBL_ZERO_RETURN_THUMB UINT32_C(0x20011f6d)
#define WORD(address)         (*(volatile uint32_t *)(uintptr_t)(address))
#define BYTE(address)         (*(volatile uint8_t *)(uintptr_t)(address))

static bool debug_owns_mailbox(void)
{
	return (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) != 0u ||
	       (CoreDebug->DEMCR & CoreDebug_DEMCR_MON_EN_Msk) != 0u;
}

void ws9652a_recovery_startup(void)
{
	/* DCRDR belongs to the core debug domain, which survives system reset.
	 * Reserve it only while no debugger or debug monitor owns it. ROM/SBL
	 * reload their runtime on reset; no application SRAM must survive. */
	if (debug_owns_mailbox() || CoreDebug->DCRDR != RECOVERY_COOKIE) {
		return;
	}
	/* Consume before validation: a damaged SBL must fall back to the application rather
	 * than repeat the request. This path uses no application globals. */
	CoreDebug->DCRDR = 0u;
	__DSB();
	if (CoreDebug->DCRDR != 0u) {
		return;
	}
	uint32_t hash = FNV_OFFSET_BASIS;
	for (uintptr_t address = SBL_BASE; address < SBL_BASE + SBL_IMMUTABLE_BYTES; ++address) {
		hash = (hash ^ BYTE(address)) * FNV_PRIME;
	}
	if (hash != SBL_FNV1A) {
		return;
	}
	/* Default SBL housekeeping can clear/rewrite the flash boot record.
	 * Override it in RAM, including subsequent command-09 calls in recovery. */
	WORD(SBL_HOUSEKEEPING_HOOK) = SBL_ZERO_RETURN_THUMB;
	ws9652a_boot_handoff(SBL_BASE, SBL_STACK_TOP, SBL_USB_SERVICE_THUMB, SBL_USB_ONLY);
}

int recovery_request_immediate(void)
{
	/* This is a cooperative shell/power request, not a fatal-handler path.
	 * Reject unsupported contexts before altering any hardware. */
	if (__get_IPSR() != 0u || (__get_CONTROL() & CONTROL_nPRIV_Msk) != 0u) {
		return -EWOULDBLOCK;
	}
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	if (debug_owns_mailbox()) {
		__set_PRIMASK(primask);
		return -EBUSY;
	}
	uint32_t previous = CoreDebug->DCRDR;

	CoreDebug->DCRDR = RECOVERY_COOKIE;
	__DSB();
	if (CoreDebug->DCRDR != RECOVERY_COOKIE) {
		CoreDebug->DCRDR = previous;
		__DSB();
		__set_PRIMASK(primask);
		return -EIO;
	}
	ws9652a_usb_detach_emergency();
	/* Stock TOP reset stops BT before resetting TOP. BT must stop accessing
	 * the shared arena while the boot loader restores SBL into that RAM. */
	(void)ws9652a_peer_hold_emergency();
	ws9652a_reset_immediate();
}
