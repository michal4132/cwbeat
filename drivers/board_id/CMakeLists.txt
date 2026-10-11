# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(board_id_gpio.c)

# Register the custom device API's iterable section with Zephyr's API scanner.
zephyr_syscall_include_directories(../../include/zephyr/drivers)
