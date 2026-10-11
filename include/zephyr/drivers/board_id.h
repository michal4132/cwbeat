/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ZEPHYR_INCLUDE_DRIVERS_BOARD_ID_H_
#define ZEPHYR_INCLUDE_DRIVERS_BOARD_ID_H_

#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Driver API for a board identifier captured at device initialization. */
__subsystem struct board_id_driver_api {
	int (*get)(const struct device *dev, uint32_t *id);
};

/**
 * @brief Read the cached board identifier.
 *
 * The device captures its identifier once during initialization. This call
 * never accesses the identification pins, which may have another use afterward.
 * Zero is a valid identifier. On error, @p id is left unchanged.
 *
 * A device with zephyr,deferred-init must be initialized with device_init()
 * after its GPIO controllers are ready and before its pins are repurposed.
 * The caller must serialize device_init() with other initialization and reads.
 * Initialization errors are reported by device_init(); subsequent reads return
 * -ENODEV because the device is not ready.
 *
 * @param dev Board identifier device.
 * @param id Storage for the identifier.
 * @retval 0 Identifier copied successfully.
 * @retval -EINVAL Either argument is NULL.
 * @retval -ENODEV The device has not initialized successfully.
 */
static inline int board_id_get(const struct device *dev, uint32_t *id)
{
	if (dev == NULL || id == NULL) {
		return -EINVAL;
	}
	if (!device_is_ready(dev)) {
		return -ENODEV;
	}

	return DEVICE_API_GET(board_id, dev)->get(dev, id);
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_BOARD_ID_H_ */
