# SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-2-Clause)
#
# Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved
# Run cmake as:
#  mkdir build
#  cmake ..
#  make
#  -DCMAKE_BUILD_TYPE=Debug
#      Change the optimization level, Debug disables optimization,

cmake_minimum_required(VERSION 2.8.11)
set(LIB_MAJOR_VERSION "0")
set(LIB_MINOR_VERSION "0")
set(LIB_PATCH_VERSION "1")
set(LIB_VERSION_STRING "${LIB_MAJOR_VERSION}.${LIB_MINOR_VERSION}.${LIB_PATCH_VERSION}")

if (CMAKE_VERSION VERSION_LESS 3.0)
	PROJECT(cmperf C)
	set(PROJECT_VERSION ${LIB_VERSION_STRING})
else()
	cmake_policy(SET CMP0048 NEW)
	project(cmperf
		VERSION "${LIB_VERSION_STRING}"
		LANGUAGES C)
endif()
set(CMAKE_C_COMPILER_NAMES gcc clang)
include_directories("include")
add_executable(cmperf
	src/main.c src/server.c src/client.c)
target_compile_options(cmperf PRIVATE -Wall -g)

FIND_PACKAGE(PkgConfig REQUIRED)
include_directories(${NL_INCLUDE_DIRS})

#-------------------------
# Find libraries
# pthread
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

#libibverbs
find_library(LibIBVERBS_LIBRARY NAMES ibverbs)
if (LibIBVERBS_LIBRARY)
	message(STATUS "Found libibverbs")
else()
	message(FATAL_ERROR "Could not find libibverbs")
endif()

#librdmacm
find_library(LibRDMACM_LIBRARY NAMES rdmacm)
if (LibRDMACM_LIBRARY)
	message(STATUS "Found librdmacm")
else()
	message(FATAL_ERROR "Could not find librdmacm")
endif()

target_link_libraries(cmperf ${CMAKE_THREAD_LIBS_INIT} ${LibIBVERBS_LIBRARY} ${LibRDMACM_LIBRARY})
set(CMAKE_REQUIRED_INCLUDES "${NL_INCLUDE_DIRS}")
set(CMAKE_REQUIRED_INCLUDES "${NL_INCLUDE_DIRS}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/buildlib/config.h.in" "${CMAKE_BINARY_DIR}/config.h")
include_directories(${PROJECT_BINARY_DIR})

install(TARGETS cmperf DESTINATION bin)

