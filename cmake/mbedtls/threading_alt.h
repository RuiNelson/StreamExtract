#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// mbed TLS's alternate mutex ABI for Windows. SRW locks require no allocation.
typedef struct mbedtls_threading_mutex_t {
  SRWLOCK lock;
  int initialized;
} mbedtls_threading_mutex_t;
