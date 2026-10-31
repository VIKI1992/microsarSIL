cmake_minimum_required(VERSION 3.20)
project(microsarSIL LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(SIL_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
list(APPEND CMAKE_MODULE_PATH ${SIL_ROOT}/cmake)
include(Warnings)
include(Sanitizers)
include(SilOsEcu)

option(SIL_BUILD_DEMO "Build the stand-in demo ECU" ON)
option(SIL_BUILD_TESTS "Build unit and integration tests" ON)
set(SIL_PYTHON "" CACHE FILEPATH "Python 3 interpreter used by the generators")
if(NOT SIL_PYTHON)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  set(SIL_PYTHON ${Python3_EXECUTABLE})
endif()
message(STATUS "microsarSIL: python ${SIL_PYTHON}")

if(WIN32)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(MSVC)
  add_compile_options(/utf-8)
else()
  add_compile_options(-fno-omit-frame-pointer)
endif()

# ---- port layer (C++17) --------------------------------------------------------------------
add_library(silport STATIC sil/port/impl/port.cpp)
target_include_directories(silport PUBLIC sil/port)
sil_strict_warnings(silport)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(silport PUBLIC Threads::Threads)
endif()

# ---- trace recorder and writers ------------------------------------------------------------
add_library(siltrace STATIC sil/trace/SilTrace.c sil/trace/SilTrace_Writers.c)
target_include_directories(siltrace PUBLIC sil/trace)
target_link_libraries(siltrace PUBLIC silport)
sil_strict_warnings(siltrace)

# ---- SilOs: AUTOSAR/MICROSAR OS emulation ---------------------------------------------------
add_library(silos STATIC
  sil/os/SilOs_Core.c sil/os/SilOs_Task.c sil/os/SilOs_Time.c sil/os/SilOs_Compat.c sil/os/SilOs_Ioc.c)
target_include_directories(silos PUBLIC sil/os sil/os/compat sil/hal sil/hal/default)
target_link_libraries(silos PUBLIC siltrace silport)
sil_strict_warnings(silos)

# ---- virtual MCAL (API headers now, implementations with M3/M4) -------------------------------
add_library(silmcal INTERFACE)
target_include_directories(silmcal INTERFACE sil/mcal/inc)
target_link_libraries(silmcal INTERFACE silos)

# ---- runtime ------------------------------------------------------------------------------
add_library(silruntime STATIC sil/runtime/sil_runtime.c)
target_include_directories(silruntime PUBLIC sil/runtime)
target_link_libraries(silruntime PUBLIC silos)
sil_strict_warnings(silruntime)

if(SIL_BUILD_DEMO)
  add_subdirectory(demo)
endif()
if(SIL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
