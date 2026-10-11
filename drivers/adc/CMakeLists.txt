# SPDX-License-Identifier: Apache-2.0
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_ADC_WS9652A adc_ws9652a.c)
