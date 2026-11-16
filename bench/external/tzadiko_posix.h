#pragma once

// Tzadiko/Orderbook calls the Windows localtime_s from a background thread.
// The order-book logic is unchanged; this only supplies the POSIX equivalent.
#include <ctime>

inline void localtime_s(std::tm* result, const std::time_t* time) { localtime_r(time, result); }
