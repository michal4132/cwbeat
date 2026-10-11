/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/soc/ws9652a/reset.h>
#include <zephyr/devicetree.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/sys_io.h>
#include <cmsis_core.h>
void ws9652a_usb_detach_emergency(void)
{
	uintptr_t power = DT_REG_ADDR(DT_NODELABEL(usb0)) + 4u;
	sys_write32(sys_read32(power) & ~UINT32_C(0x40), power);
}
__attribute__((noreturn)) void ws9652a_reset_immediate(void)
{
	__DSB();
	SCB->AIRCR = (SCB->AIRCR & SCB_AIRCR_PRIGROUP_Msk) | (0x5fau << SCB_AIRCR_VECTKEY_Pos) |
		     SCB_AIRCR_SYSRESETREQ_Msk;
	__DSB();
	for (;;) {
	}
}
/* r0=vectors, r1=stack, r2=entry, r3=argument; no C or stack access after MSP. */
__attribute__((naked, noreturn)) void ws9652a_boot_handoff(uintptr_t vectors, uintptr_t stack,
							   uintptr_t entry, uint32_t argument)
{
	__asm__ volatile("ldr r12, =0xe000e010\nmovs r4, #0\nstr r4, [r12]\n"
			 "ldr r12, =0xe000ed04\nldr r4, =0x0a000000\nstr r4, [r12]\n"
			 "ldr r12, =0xe000ed08\nstr r0, [r12]\n"
			 "msr msp, r1\nmov r0, r3\ndsb\nisb\ncpsie i\nbx r2\n");
}

#ifdef CONFIG_WS9652A_PEER
#include <zephyr/drivers/peer/ws9652a.h>
void sys_arch_reboot(int type)
{
	(void)type;
	__disable_irq();
	ws9652a_usb_detach_emergency();
	(void)ws9652a_peer_hold_emergency();
	ws9652a_reset_immediate();
}
#endif
