#pragma once

// Keep profiling call sites permanent and cheap to disable. Tracy is supplied only by the
// opt-in vcpkg `profiling` feature and the profiling-release CMake preset.
#ifndef HEARTSTEAD_HAS_TRACY
#define HEARTSTEAD_HAS_TRACY 0
#endif

#if HEARTSTEAD_HAS_TRACY
#include <tracy/Tracy.hpp>

#define HEARTSTEAD_PROFILE_ZONE() ZoneScoped
#define HEARTSTEAD_PROFILE_ZONE_NAMED(name) ZoneScopedN(name)
#define HEARTSTEAD_PROFILE_ZONE_TEXT(data, size) ZoneText((data), (size))
#define HEARTSTEAD_PROFILE_ZONE_VALUE(value) ZoneValue((value))
#define HEARTSTEAD_PROFILE_FRAME() FrameMark
#define HEARTSTEAD_PROFILE_THREAD_NAME(name) tracy::SetThreadName((name))
#define HEARTSTEAD_PROFILE_PLOT(name, value) TracyPlot((name), static_cast<double>(value))
#define HEARTSTEAD_PROFILE_MESSAGE(data, size) TracyMessage((data), (size))
#define HEARTSTEAD_PROFILE_ALLOC(pointer, size, name) TracyAllocN((pointer), (size), (name))
#define HEARTSTEAD_PROFILE_FREE(pointer, name) TracyFreeN((pointer), (name))
#else
#define HEARTSTEAD_PROFILE_ZONE() static_cast<void>(0)
#define HEARTSTEAD_PROFILE_ZONE_NAMED(name) static_cast<void>(0)
#define HEARTSTEAD_PROFILE_ZONE_TEXT(data, size) static_cast<void>(0)
#define HEARTSTEAD_PROFILE_ZONE_VALUE(value) static_cast<void>(0)
#define HEARTSTEAD_PROFILE_FRAME() static_cast<void>(0)
#define HEARTSTEAD_PROFILE_THREAD_NAME(name) static_cast<void>(0)
#define HEARTSTEAD_PROFILE_PLOT(name, value) static_cast<void>(0)
#define HEARTSTEAD_PROFILE_MESSAGE(data, size) static_cast<void>(0)
#define HEARTSTEAD_PROFILE_ALLOC(pointer, size, name) static_cast<void>(0)
#define HEARTSTEAD_PROFILE_FREE(pointer, name) static_cast<void>(0)
#endif
