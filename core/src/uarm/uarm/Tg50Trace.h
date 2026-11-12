#ifndef FROND_TG50_TRACE_H
#define FROND_TG50_TRACE_H
#ifdef FROND_TG50_TRACE
#include <cstdio>
#define TG50_TRACE(...) std::fprintf(stderr, __VA_ARGS__)
#else
#define TG50_TRACE(...) ((void)0)
#endif
#endif
