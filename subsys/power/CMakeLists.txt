# Copyright 2026 Michał Bogdziewicz michal@bogdziewicz.xyz
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(power.c hardware.c)
zephyr_library_sources_ifdef(CONFIG_CWBEAT_POWER_AUTOSTART autostart.c)
zephyr_library_sources_ifdef(CONFIG_CWBEAT_POWER_SHELL shell.c)
