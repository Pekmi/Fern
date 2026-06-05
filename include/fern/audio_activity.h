#pragma once

#include <windows.h>

namespace fern {

struct AudioActivityRange {
    LONGLONG startHns = 0;
    LONGLONG durationHns = 0;
};

}
