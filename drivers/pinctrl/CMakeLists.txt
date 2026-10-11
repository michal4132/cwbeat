# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_PINCTRL_WS9652A pinctrl_ws9652a.c)
