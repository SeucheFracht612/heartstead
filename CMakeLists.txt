cmake_minimum_required(VERSION 3.25)

project(
  Heartstead
  VERSION 0.1.0
  DESCRIPTION "Custom engine foundation for the Heartstead co-op settlement survival game"
  LANGUAGES CXX
)

option(HEARTSTEAD_BUILD_SAMPLES "Build Heartstead sample executables" ON)
option(HEARTSTEAD_BUILD_APPS "Build Heartstead runnable applications" ON)
option(HEARTSTEAD_BUILD_TOOLS "Build Heartstead development tools" ON)
option(HEARTSTEAD_BUILD_TESTS "Build Heartstead tests" ON)
option(HEARTSTEAD_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
set(HEARTSTEAD_SANITIZERS
    ""
    CACHE STRING "Comma-separated sanitizer list for Clang/GCC builds: address,undefined,thread"
)
set_property(CACHE HEARTSTEAD_SANITIZERS PROPERTY STRINGS "" "address" "undefined" "thread"
                                                   "address,undefined")
option(HEARTSTEAD_ENABLE_VULKAN "Build the Vulkan renderer backend when the SDK is available" ON)
option(HEARTSTEAD_ENABLE_X11_NATIVE "Build the X11 native platform backend when available" ON)
option(HEARTSTEAD_ENABLE_XRANDR "Use XRandR for richer native display metadata when available" ON)
option(HEARTSTEAD_ENABLE_JOLT "Build the Jolt physics backend" ON)
option(HEARTSTEAD_ENABLE_MINIAUDIO "Build the miniaudio playback backend" ON)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
if(HEARTSTEAD_ENABLE_JOLT)
  find_package(Jolt CONFIG REQUIRED)
endif()
if(HEARTSTEAD_ENABLE_MINIAUDIO)
  find_path(MINIAUDIO_INCLUDE_DIRS "miniaudio.h" REQUIRED)
endif()
if(HEARTSTEAD_ENABLE_VULKAN)
  find_package(Vulkan QUIET)
endif()
if(HEARTSTEAD_ENABLE_X11_NATIVE)
  find_package(X11 QUIET)
endif()

set(HEARTSTEAD_SANITIZER_COMPILE_OPTIONS)
set(HEARTSTEAD_SANITIZER_LINK_OPTIONS)
if(HEARTSTEAD_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "HEARTSTEAD_SANITIZERS is currently supported only for Clang/GCC builds")
  endif()

  string(REPLACE "," ";" HEARTSTEAD_SANITIZER_LIST "${HEARTSTEAD_SANITIZERS}")
  list(FIND HEARTSTEAD_SANITIZER_LIST "address" HEARTSTEAD_HAS_ADDRESS_SANITIZER)
  list(FIND HEARTSTEAD_SANITIZER_LIST "thread" HEARTSTEAD_HAS_THREAD_SANITIZER)
  if(NOT HEARTSTEAD_HAS_ADDRESS_SANITIZER EQUAL -1
     AND NOT HEARTSTEAD_HAS_THREAD_SANITIZER EQUAL -1)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
  endif()

  foreach(sanitizer IN LISTS HEARTSTEAD_SANITIZER_LIST)
    if(NOT sanitizer STREQUAL "address"
       AND NOT sanitizer STREQUAL "undefined"
       AND NOT sanitizer STREQUAL "thread")
      message(FATAL_ERROR "Unsupported HEARTSTEAD_SANITIZERS entry: ${sanitizer}")
    endif()
  endforeach()

  set(HEARTSTEAD_SANITIZER_FLAG "-fsanitize=${HEARTSTEAD_SANITIZERS}")
  list(APPEND HEARTSTEAD_SANITIZER_COMPILE_OPTIONS ${HEARTSTEAD_SANITIZER_FLAG}
       -fno-omit-frame-pointer)
  list(APPEND HEARTSTEAD_SANITIZER_LINK_OPTIONS ${HEARTSTEAD_SANITIZER_FLAG})
endif()

function(heartstead_configure_target target_name)
  target_compile_features(${target_name} PUBLIC cxx_std_23)

  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    if(HEARTSTEAD_WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  else()
    target_compile_options(
      ${target_name}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wnon-virtual-dtor
    )
    if(HEARTSTEAD_WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE -Werror)
    endif()
  endif()

  if(HEARTSTEAD_SANITIZER_COMPILE_OPTIONS)
    target_compile_options(${target_name} PRIVATE ${HEARTSTEAD_SANITIZER_COMPILE_OPTIONS})
  endif()
  if(HEARTSTEAD_SANITIZER_LINK_OPTIONS)
    get_target_property(heartstead_target_type ${target_name} TYPE)
    if(NOT heartstead_target_type STREQUAL "STATIC_LIBRARY"
       AND NOT heartstead_target_type STREQUAL "INTERFACE_LIBRARY"
       AND NOT heartstead_target_type STREQUAL "OBJECT_LIBRARY")
      target_link_options(${target_name} PRIVATE ${HEARTSTEAD_SANITIZER_LINK_OPTIONS})
    endif()
  endif()
endfunction()

add_subdirectory(engine)
add_subdirectory(game)

if(HEARTSTEAD_BUILD_APPS)
  add_subdirectory(apps)
endif()

if(HEARTSTEAD_BUILD_SAMPLES)
  add_subdirectory(samples)
endif()

if(HEARTSTEAD_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

include(CTest)
if(BUILD_TESTING AND HEARTSTEAD_BUILD_TESTS)
  add_subdirectory(tests)
endif()
