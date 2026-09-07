# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

# Lets the board overlay's display node resolve against the binding in
# dts/bindings, and picks up the app-local Kconfig for the driver below.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(screen_blink)

target_sources(app PRIVATE
	src/main.c 
	src/tacho.c
)

# The landscape display driver. Lives in the app rather than in Zephyr's
# drivers/display because it is specific to this board's panel orientation.
target_sources_ifdef(CONFIG_ST7306_WAVESHARE app PRIVATE
	drivers/display_st7306_waveshare.c
)

# Lets sources say #include "tacho.h" rather than a path relative to src/.
target_include_directories(app PRIVATE include)
