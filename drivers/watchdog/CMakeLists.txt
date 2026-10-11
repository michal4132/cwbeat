# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()
zephyr_library_sources_ifdef(CONFIG_WDT_WS9652A wdt_ws9652a.c)
