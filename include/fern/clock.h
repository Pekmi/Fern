#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>

namespace fern {

constexpr LONGLONG HnsPerSecond = 10000000LL;

LONGLONG RawQpcToHns(LONGLONG rawQpc);
LONGLONG CurrentQpcHns();
LONGLONG FrameBoundaryHns(UINT64 frameIndex, int fps);

}
