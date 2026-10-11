/* SPDX-License-Identifier: Apache-2.0 */
#include "private.h"
#include <errno.h>
#include <cwbeat/boards/jbl_go4/power_off.h>
#include <cwbeat/boards/jbl_go4/recovery.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/reboot.h>

static const struct gpio_dt_spec power_hold =
	GPIO_DT_SPEC_GET(DT_NODELABEL(power_controls), hold_gpios);

bool power_hardware_boot_requested(void)
{
	return power_boot_requested();
}

void power_hardware_on(void)
{
	(void)gpio_pin_set_dt(&power_hold, 1);
}

bool power_hardware_console_done(void)
{
#if defined(CONFIG_SHELL_BACKEND_SERIAL) && DT_HAS_CHOSEN(zephyr_shell_uart)
	const struct device *uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_shell_uart));

	if (!device_is_ready(uart)) {
		return true;
	}
#if defined(CONFIG_UART_LINE_CTRL)
	uint32_t dtr = 0;

	if (!uart_line_ctrl_get(uart, UART_LINE_CTRL_DTR, &dtr) && !dtr) {
		return true;
	}
#endif
	/* Standard CDC ACM exposes no transmit-completion query. Its FIFO and
	 * USB endpoint can still hold output, so allow the power state machine's
	 * bounded drain deadline to expire while the USB workers remain runnable. */
	return false;
#else
	return true;
#endif
}

int power_hardware_terminal(enum power_command_kind command)
{
	switch (command) {
	case POWER_OFF:
		return power_off_request() ? 0 : -EIO;
	case POWER_RESET:
		sys_reboot(SYS_REBOOT_COLD);
		return -EIO;
	case POWER_RECOVERY:
		return recovery_request_immediate();
	default:
		return 0;
	}
}
