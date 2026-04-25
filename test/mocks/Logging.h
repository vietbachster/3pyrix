#pragma once

#include <cstdio>

#ifndef LOG_ERR
#define LOG_ERR(origin, format, ...) std::fprintf(stderr, "[ERR] [%s] " format "\n", origin, ##__VA_ARGS__)
#endif

#ifndef LOG_INF
#define LOG_INF(origin, format, ...) std::fprintf(stdout, "[INF] [%s] " format "\n", origin, ##__VA_ARGS__)
#endif

#ifndef LOG_DBG
#define LOG_DBG(origin, format, ...) std::fprintf(stdout, "[DBG] [%s] " format "\n", origin, ##__VA_ARGS__)
#endif
