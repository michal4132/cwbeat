# SPDX-License-Identifier: Apache-2.0

zephyr_library_amend()
add_subdirectory_ifdef(CONFIG_MFD_WS9652A ws9652a)
