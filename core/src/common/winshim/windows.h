#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN  // no COM: its `interface` and `byte` clash with the core's names
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include_next <windows.h>
