# Copyright 2026 ALTESSA SOLUTIONS INC.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.21)
project(consumer LANGUAGES C)
find_package(SNRender CONFIG REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE SNRender::snrender)

option(SNRENDER_CONSUMER_SPD "Exercise the installed Atelier API" OFF)
if(SNRENDER_CONSUMER_SPD)
    find_package(SNSpd 1.0 CONFIG REQUIRED)
    target_link_libraries(consumer PRIVATE SNSpd::snspd)
    target_compile_definitions(consumer PRIVATE SNR_CONSUMER_SPD)
endif()
