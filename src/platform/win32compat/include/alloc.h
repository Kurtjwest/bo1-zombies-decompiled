// <malloc.h> stand-in for the macOS and Linux ports (macOS has <malloc/malloc.h> instead).
#pragma once
#include <stdlib.h>
#include <alloca.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
#include "win32compat/msvc_crt.h"
