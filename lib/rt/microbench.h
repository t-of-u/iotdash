#pragma once

#include "rt_base.h"

namespace rt {

// Run the raster micro benchmarks on a w x rows RGB565 buffer; returns one line per test.
std::string microBench(uint16_t *buf, int w, int rows);
std::string clipTest();

}  // namespace rt
