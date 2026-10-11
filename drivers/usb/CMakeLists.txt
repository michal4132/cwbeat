# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_include_directories(${ZEPHYR_BASE}/drivers/usb/udc)
zephyr_library_sources_ifdef(CONFIG_UDC_WS9652A udc_ws9652a.c ws9652a_usb_hw.c)
