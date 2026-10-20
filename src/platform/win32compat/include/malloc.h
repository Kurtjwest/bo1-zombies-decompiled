// <malloc.h> stand-in for the macOS and Linux ports: MSVC's <malloc.h> declares alloca and the heap functions, and
// pulls in the MSVC CRT names (macOS has <malloc/malloc.h> instead; Linux's own <malloc.h> comes next).
#pragma once
#include <stdlib.h>
#include <alloca.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include_next <malloc.h>
#endif
#include "win32compat/msvc_crt.h"
