#pragma once

// Enable via -DENABLE_DEBUG (set in Makefile with DEBUG=1)

#ifdef ENABLE_DEBUG
  #include <cstdio>
  #ifdef __CUDACC__
    #define DEBUG_PRINT(...) do { printf(__VA_ARGS__); printf("\n"); } while(0)
  #else
    #define DEBUG_PRINT(...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while(0)
  #endif
  #define DEBUG_ONLY(...) do { __VA_ARGS__; } while (0)
#else
  #define DEBUG_PRINT(...) do {} while(0)
  #define DEBUG_ONLY(...) do {} while(0)
#endif
