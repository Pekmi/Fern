#include "../include/fern/clock.h"

#include <cmath>

namespace fern {

LONGLONG RawQpcToHns(LONGLONG rawQpc) {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();

    long double hns = (static_cast<long double>(rawQpc) * HnsPerSecond) /
                      static_cast<long double>(freq.QuadPart);
    return static_cast<LONGLONG>(std::llround(hns));
}

LONGLONG CurrentQpcHns() {
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    return RawQpcToHns(qpc.QuadPart);
}

LONGLONG FrameBoundaryHns(UINT64 frameIndex, int fps) {
    long double hns = (static_cast<long double>(frameIndex) * HnsPerSecond) /
                      static_cast<long double>(fps);
    return static_cast<LONGLONG>(std::llround(hns));
}

}
