/* SPDX-License-Identifier: Apache-2.0 */
/* Keep the MFD and ADC logging modules in separate translation units. */
#include <zephyr/device.h>
#undef DT_INST_FOREACH_STATUS_OKAY
#define DT_INST_FOREACH_STATUS_OKAY(fn)
#include "../../../../drivers/mfd/ws9652a/mfd_ws9652a.c"

int test_pmu_init(const struct device *dev)
{
	return init(dev);
}
