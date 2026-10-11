/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/drivers/gpio.h>
#include "private.h"
static const struct gpio_dt_spec codec =
	GPIO_DT_SPEC_GET(DT_NODELABEL(audio_controls), codec_control_gpios);
static const struct gpio_dt_spec type1 =
	GPIO_DT_SPEC_GET(DT_NODELABEL(audio_controls), type1_control_gpios);
int audio_hardware_prepare(void)
{
	if (!gpio_is_ready_dt(&codec) || !gpio_is_ready_dt(&type1)) {
		return -ENODEV;
	}
	/* Prepare while the peer is held: stock 0x4092A(0) lowers the codec
	 * control, but GPIO12 keeps its inherited latch until a codec command. */
	int err = gpio_pin_configure_dt(&codec, GPIO_OUTPUT_INACTIVE);
	return err ? err : gpio_pin_configure_dt(&type1, GPIO_OUTPUT);
}
int audio_hardware_write(unsigned pin, bool high)
{
	return gpio_pin_set_dt(pin == 16 ? &codec : &type1, high);
}
