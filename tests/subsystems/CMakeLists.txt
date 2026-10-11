cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cwbeat_subsystems)
target_sources(app PRIVATE src/main.c ../../subsys/audio/audio.c ../../subsys/bluetooth/bluetooth.c)
# These fixtures compile the owners directly, with their subsystem Kconfig disabled.
target_compile_definitions(app PRIVATE
 CONFIG_CWBEAT_AUDIO_LOG_LEVEL=CONFIG_LOG_DEFAULT_LEVEL
 CONFIG_CWBEAT_BLUETOOTH_LOG_LEVEL=CONFIG_LOG_DEFAULT_LEVEL)
target_include_directories(app PRIVATE ../../include)
if(TEST_COMPLETE_STARTUP)
 target_compile_definitions(app PRIVATE TEST_COMPLETE_STARTUP=1)
endif()
