#pragma once

#include <cstddef>

#ifdef __linux__
inline constexpr size_t CACHE_LINE_SIZE = 64;
#else
inline constexpr size_t CACHE_LINE_SIZE = 64;
#endif