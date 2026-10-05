#pragma once
// The tiny check framework every test file shares (counters live in plugin_test.cpp).
#include <cstdio>

namespace sft {
extern int g_fail, g_pass;
}

#define CHECK(c)                                                                          \
    do {                                                                                  \
        if (c) ++sft::g_pass;                                                             \
        else { ++sft::g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }  \
    } while (0)
