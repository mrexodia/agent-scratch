// Minimal stand-in for <windows.h> so the vendored detection code builds on
// any platform. The detection code only needs POINT and LONG.
#pragma once

#include <cstdint>
#include <cstring>

typedef int32_t LONG;

typedef struct tagPOINT {
    LONG x;
    LONG y;
} POINT;
