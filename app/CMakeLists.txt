# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(jumpback)

target_sources(app PRIVATE
	src/main.c
	src/tacho.c
	src/speed.c
	src/speedo.c
)

target_sources_ifdef(CONFIG_ST7306_WAVESHARE app PRIVATE
	drivers/display_st7306_waveshare.c
)

target_include_directories(app PRIVATE include)
