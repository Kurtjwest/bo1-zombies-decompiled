// MSVC CRT / intrinsic compatibility for the macOS (arm64) port.
// Force-included (via -include bo1_platform_prefix.h) into every engine translation unit, the way MSVC makes
// these names available from its standard headers.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include <setjmp.h>
#include <alloca.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

// MSVC names for CRT internals that decompiled code spells out
#define _iobuf __sFILE
static inline int *_errno(void) { return &errno; }
typedef int (*_CoreCrtNonSecureSearchSortCompareFunction)(const void *, const void *);
typedef int (*_CoreCrtSecureSearchSortCompareFunction)(void *, const void *, const void *);

// ---- string comparison / case ----
#ifndef _stricmp
static inline int _stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
#endif
static inline int _strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static inline int _strcmpi(const char *a, const char *b) { return strcasecmp(a, b); }
static inline int stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
static inline int strnicmp(const char *a, const char *b, size_t n) { return strncasecmp(a, b, n); }
static inline int strcmpi(const char *a, const char *b) { return strcasecmp(a, b); }
static inline int _memicmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (size_t i = 0; i < n; ++i)
    {
        int d = tolower(x[i]) - tolower(y[i]);
        if (d)
            return d;
    }
    return 0;
}
static inline char *_strlwr(char *s)
{
    for (char *p = s; *p; ++p)
        *p = (char)tolower((unsigned char)*p);
    return s;
}
static inline char *_strupr(char *s)
{
    for (char *p = s; *p; ++p)
        *p = (char)toupper((unsigned char)*p);
    return s;
}
static inline char *strlwr(char *s) { return _strlwr(s); }
static inline char *strupr(char *s) { return _strupr(s); }
static inline char *_strrev(char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; ++i)
    {
        char t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
    return s;
}
static inline char *_strdup(const char *s) { return strdup(s); }
static inline size_t _strnlen(const char *s, size_t n) { return strnlen(s, n); }

// ---- formatted output with MSVC semantics (return -1 when the output was truncated) ----
static inline int _vsnprintf(char *buf, size_t count, const char *fmt, va_list ap)
{
    int r = vsnprintf(buf, count, fmt, ap);
    if (r < 0 || (size_t)r >= count)
        return -1;
    return r;
}
static inline int __attribute__((format(printf, 3, 4))) _snprintf(char *buf, size_t count, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnprintf(buf, count, fmt, ap);
    va_end(ap);
    return r;
}
static inline int _vscprintf(const char *fmt, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    int r = vsnprintf(NULL, 0, fmt, cp);
    va_end(cp);
    return r;
}
static inline int __attribute__((format(printf, 1, 2))) _scprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    return r;
}
#define _TRUNCATE ((size_t)-1)
static inline int _vsnprintf_s(char *buf, size_t size, size_t count, const char *fmt, va_list ap)
{
    if (!buf || !size)
        return -1;
    size_t lim = (count == _TRUNCATE || count >= size) ? size : count + 1;
    int r = vsnprintf(buf, lim, fmt, ap);
    if (r < 0 || (size_t)r >= lim)
    {
        buf[lim - 1] = 0;
        return -1;
    }
    return r;
}
static inline int __attribute__((format(printf, 4, 5))) _snprintf_s(char *buf, size_t size, size_t count, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnprintf_s(buf, size, count, fmt, ap);
    va_end(ap);
    return r;
}
static inline int vsprintf_s(char *buf, size_t size, const char *fmt, va_list ap)
{
    int r = vsnprintf(buf, size, fmt, ap);
    return (r < 0 || (size_t)r >= size) ? -1 : r;
}
static inline int __attribute__((format(printf, 3, 4))) sprintf_s(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf_s(buf, size, fmt, ap);
    va_end(ap);
    return r;
}
static inline int strcpy_s(char *dst, size_t size, const char *src)
{
    if (!dst || !size)
        return EINVAL;
    size_t n = strlen(src);
    if (n >= size)
    {
        dst[0] = 0;
        return ERANGE;
    }
    memcpy(dst, src, n + 1);
    return 0;
}
static inline int strncpy_s(char *dst, size_t size, const char *src, size_t count)
{
    if (!dst || !size)
        return EINVAL;
    size_t n = strnlen(src, count == _TRUNCATE ? size - 1 : count);
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
    return 0;
}
static inline int strcat_s(char *dst, size_t size, const char *src)
{
    size_t d = strnlen(dst, size);
    size_t s = strlen(src);
    if (d + s >= size)
        return ERANGE;
    memcpy(dst + d, src, s + 1);
    return 0;
}
static inline int strncat_s(char *dst, size_t size, const char *src, size_t count)
{
    size_t d = strnlen(dst, size);
    size_t s = strnlen(src, count);
    if (d + s >= size)
        s = size - d - 1;
    memcpy(dst + d, src, s);
    dst[d + s] = 0;
    return 0;
}
static inline int memcpy_s(void *dst, size_t size, const void *src, size_t count)
{
    if (count > size)
        return ERANGE;
    memcpy(dst, src, count);
    return 0;
}
static inline int memmove_s(void *dst, size_t size, const void *src, size_t count)
{
    if (count > size)
        return ERANGE;
    memmove(dst, src, count);
    return 0;
}
static inline char *strtok_s(char *s, const char *delim, char **ctx) { return strtok_r(s, delim, ctx); }
FILE *bo1_fopen(const char *path, const char *mode);
static inline int fopen_s(FILE **f, const char *name, const char *mode)
{
    *f = bo1_fopen(name, mode);
    return *f ? 0 : errno;
}
#define sscanf_s sscanf
#define fscanf_s fscanf
static inline int _vsnwprintf_s(wchar_t *buf, size_t size, size_t count, const wchar_t *fmt, va_list ap)
{
    (void)count;
    int r = vswprintf(buf, size, fmt, ap);
    return r < 0 ? -1 : r;
}
static inline int _snwprintf_s(wchar_t *buf, size_t size, size_t count, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf_s(buf, size, count, fmt, ap);
    va_end(ap);
    return r;
}
static inline int _wcsicmp(const wchar_t *a, const wchar_t *b) { return wcscasecmp(a, b); }
static inline int _wcsnicmp(const wchar_t *a, const wchar_t *b, size_t n) { return wcsncasecmp(a, b, n); }

// ---- number <-> string ----
static inline char *_ui64toa(unsigned long long v, char *buf, int radix)
{
    char tmp[72];
    int i = 0;
    if (radix < 2 || radix > 36)
        radix = 10;
    do
    {
        int d = (int)(v % (unsigned)radix);
        tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= (unsigned)radix;
    } while (v);
    int j = 0;
    while (i)
        buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}
static inline char *_i64toa(long long v, char *buf, int radix)
{
    if (radix == 10 && v < 0)
    {
        buf[0] = '-';
        _ui64toa((unsigned long long)(-(v + 1)) + 1u, buf + 1, radix);
        return buf;
    }
    return _ui64toa((unsigned long long)v, buf, radix);
}
static inline char *_itoa(int v, char *buf, int radix)
{
    if (radix == 10)
        return _i64toa(v, buf, 10);
    return _ui64toa((unsigned int)v, buf, radix);
}
static inline char *_ltoa(int v, char *buf, int radix) { return _itoa(v, buf, radix); }
static inline char *_ultoa(unsigned int v, char *buf, int radix) { return _ui64toa(v, buf, radix); }
static inline char *itoa(int v, char *buf, int radix) { return _itoa(v, buf, radix); }
static inline int _itoa_s(int v, char *buf, size_t size, int radix)
{
    char tmp[72];
    _itoa(v, tmp, radix);
    return strcpy_s(buf, size, tmp);
}
static inline long long _atoi64(const char *s) { return strtoll(s, NULL, 10); }
static inline long long _strtoi64(const char *s, char **end, int base) { return strtoll(s, end, base); }
static inline unsigned long long _strtoui64(const char *s, char **end, int base) { return strtoull(s, end, base); }
static inline double _atof_l(const char *s, void *loc) { (void)loc; return atof(s); }

// ---- files / directories ----
// The engine spells paths the Windows way ("main\\players\\config.cfg", "C:\\..."); these translate them to
// host paths (separators, drive letters, case-insensitive lookup) before calling the POSIX functions.
const char *bo1_hostpath(const char *path, char *buf, size_t size); // kernel32_file.cpp
FILE *bo1_fopen(const char *path, const char *mode);
FILE *bo1_freopen(const char *path, const char *mode, FILE *f);
int bo1_stat(const char *path, struct stat *st);
int bo1_rename(const char *from, const char *to);
int bo1_remove(const char *path);
static inline int _access(const char *path, int mode) { char b[1024]; return access(bo1_hostpath(path, b, sizeof(b)), mode & 6); }
static inline int _unlink(const char *path) { char b[1024]; return unlink(bo1_hostpath(path, b, sizeof(b))); }
static inline int _rmdir(const char *path) { char b[1024]; return rmdir(bo1_hostpath(path, b, sizeof(b))); }
static inline int _chdir(const char *path) { char b[1024]; return chdir(bo1_hostpath(path, b, sizeof(b))); }
static inline char *_getcwd(char *buf, int size) { return getcwd(buf, (size_t)size); }
static inline int _fileno(FILE *f) { return fileno(f); }
static inline FILE *_fdopen(int fd, const char *mode) { return fdopen(fd, mode); }
static inline FILE *_fsopen(const char *path, const char *mode, int shflag) { (void)shflag; return bo1_fopen(path, mode); }
static inline long _filelength(int fd)
{
    struct stat st;
    if (fstat(fd, &st))
        return -1;
    return (long)st.st_size;
}
static inline int _putenv(const char *envstring)
{
    char *s = strdup(envstring);
    return putenv(s);
}
int _mkdir(const char *path); // kernel32_file.cpp
#define _stat stat
#define _stat64 stat
#define _stat64i32 stat
#define _fstat fstat
#define _fstat64 fstat
#define _S_IFDIR S_IFDIR
#define _S_IFREG S_IFREG
#define _O_RDONLY 0x0000
#define _O_BINARY 0x0000
#define O_BINARY 0x0000

// _findfirst family (io.h) - wildcard directory enumeration.
struct _finddata64i32_t
{
    unsigned attrib;
    long long time_create;
    long long time_access;
    long long time_write;
    unsigned int size;
    char name[260];
};
#define _finddata_t _finddata64i32_t
#define _finddatai64_t _finddata64i32_t
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20
intptr_t _findfirst64i32(const char *filespec, struct _finddata64i32_t *fileinfo);
int _findnext64i32(intptr_t handle, struct _finddata64i32_t *fileinfo);
int _findclose(intptr_t handle);
#define _findfirst _findfirst64i32
#define _findnext _findnext64i32
#define _findfirsti64 _findfirst64i32
#define _findnexti64 _findnext64i32

void _splitpath(const char *path, char *drive, char *dir, char *fname, char *ext);
void _makepath(char *path, const char *drive, const char *dir, const char *fname, const char *ext);
char *_fullpath(char *absPath, const char *relPath, size_t maxLength);

// ---- memory ----
static inline void *_aligned_malloc(size_t size, size_t alignment)
{
    void *p = NULL;
    if (alignment < sizeof(void *))
        alignment = sizeof(void *);
    if (posix_memalign(&p, alignment, size ? size : 1))
        return NULL;
    return p;
}
static inline void _aligned_free(void *p) { free(p); }
void *_aligned_realloc(void *p, size_t size, size_t alignment); // kernel32_misc.cpp
size_t _msize(void *p);                                         // kernel32_misc.cpp
#define _malloca(size) alloca(size)
#define _freea(p) ((void)0)
#define _alloca alloca

// ---- time ----
typedef long long __time64_t;
static inline __time64_t _time64(__time64_t *t)
{
    time_t now = time(NULL);
    if (t)
        *t = (__time64_t)now;
    return (__time64_t)now;
}
static inline struct tm *_localtime64(const __time64_t *t)
{
    time_t tt = (time_t)*t;
    return localtime(&tt);
}
static inline struct tm *_gmtime64(const __time64_t *t)
{
    time_t tt = (time_t)*t;
    return gmtime(&tt);
}
static inline char *_ctime64(const __time64_t *t)
{
    time_t tt = (time_t)*t;
    return ctime(&tt);
}
static inline __time64_t _mktime64(struct tm *tm) { return (__time64_t)mktime(tm); }
static inline int localtime_s(struct tm *out, const time_t *t) { return localtime_r(t, out) ? 0 : EINVAL; }
static inline int gmtime_s(struct tm *out, const time_t *t) { return gmtime_r(t, out) ? 0 : EINVAL; }
static inline int _localtime64_s(struct tm *out, const __time64_t *t)
{
    time_t tt = (time_t)*t;
    return localtime_r(&tt, out) ? 0 : EINVAL;
}

// ---- floating point ----
static inline int _finite(double x) { return isfinite(x); }
static inline int _isnan(double x) { return isnan(x); }
static inline int _finitef(float x) { return isfinite(x); }
static inline int _isnanf(float x) { return isnan(x); }
static inline double _hypot(double a, double b) { return hypot(a, b); }
static inline float _hypotf(float a, float b) { return hypotf(a, b); }
static inline double _copysign(double a, double b) { return copysign(a, b); }
static inline double _chgsign(double a) { return -a; }
static inline double _logb(double x) { return logb(x); }
static inline double _nextafter(double a, double b) { return nextafter(a, b); }
// The x87/SSE control-word API: rounding control is always round-to-nearest on arm64 here.
#define _MCW_EM 0x0008001f
#define _MCW_RC 0x00000300
#define _MCW_PC 0x00030000
#define _MCW_DN 0x03000000
#define _RC_NEAR 0x00000000
#define _RC_DOWN 0x00000100
#define _RC_UP 0x00000200
#define _RC_CHOP 0x00000300
#define _PC_24 0x00020000
#define _PC_53 0x00010000
#define _PC_64 0x00000000
#define _DN_SAVE 0x00000000
#define _DN_FLUSH 0x01000000
#define _EM_INVALID 0x00000010
#define _EM_DENORMAL 0x00080000
#define _EM_ZERODIVIDE 0x00000008
#define _EM_OVERFLOW 0x00000004
#define _EM_UNDERFLOW 0x00000002
#define _EM_INEXACT 0x00000001
#define _SW_INEXACT 0x00000001
#define _SW_UNDERFLOW 0x00000002
#define _SW_OVERFLOW 0x00000004
#define _SW_ZERODIVIDE 0x00000008
#define _SW_INVALID 0x00000010
#define _SW_DENORMAL 0x00080000
static inline unsigned int _controlfp(unsigned int newv, unsigned int mask) { (void)newv; (void)mask; return _MCW_EM | _PC_53; }
static inline int _controlfp_s(unsigned int *cur, unsigned int newv, unsigned int mask) { if (cur) *cur = _controlfp(newv, mask); return 0; }
static inline unsigned int _control87(unsigned int newv, unsigned int mask) { return _controlfp(newv, mask); }
static inline unsigned int _clearfp(void) { return 0; }
static inline unsigned int _statusfp(void) { return 0; }
static inline void _fpreset(void) {}

// ---- process ----
static inline int _getpid(void) { return (int)getpid(); }
#define _set_error_mode(x) (0)
#define _set_abort_behavior(x, y) (0u)
#define _OUT_TO_DEFAULT 0
#define _OUT_TO_STDERR 1
#define _OUT_TO_MSGBOX 2
#define _WRITE_ABORT_MSG 0x1
#define _CALL_REPORTFAULT 0x2
typedef void (*_invalid_parameter_handler)(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t);
static inline _invalid_parameter_handler _set_invalid_parameter_handler(_invalid_parameter_handler h) { (void)h; return NULL; }
static inline void _invalid_parameter_noinfo(void) {}
typedef void (*_purecall_handler)(void);
static inline _purecall_handler _set_purecall_handler(_purecall_handler h) { (void)h; return NULL; }
#define _CrtSetReportMode(t, m) (0)
#define _CrtSetDbgFlag(f) (0)
#define _CrtDbgReport(...) (0)
#define _CrtCheckMemory() (1)
#define _CRT_WARN 0
#define _CRT_ERROR 1
#define _CRT_ASSERT 2
#define _CRTDBG_MODE_DEBUG 0x2
#define _CRTDBG_ALLOC_MEM_DF 0x01
#define _CRTDBG_LEAK_CHECK_DF 0x20

// ---- x86-only intrinsics mapped to arm64 equivalents ----
// __rdtsc: the arm64 virtual counter (24 MHz on Apple silicon) is monotonic and readable from user space.
static inline unsigned long long __rdtsc(void)
{
    unsigned long long v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
// __readfsdword(0x24) is the x86 TEB's current thread id; the engine only ever reads that offset.
unsigned int Sys_Compat_CurrentThreadId(void); // kernel32_sync.cpp (same value as GetCurrentThreadId)
static inline unsigned int __readfsdword(unsigned int offset) { return offset == 0x24 ? Sys_Compat_CurrentThreadId() : 0u; }
static inline void __cpuid(int info[4], int leaf) { (void)leaf; info[0] = info[1] = info[2] = info[3] = 0; }
static inline void __cpuidex(int info[4], int leaf, int sub) { (void)leaf; (void)sub; info[0] = info[1] = info[2] = info[3] = 0; }
static inline unsigned long long _xgetbv(unsigned int x) { (void)x; return 0; }

#ifdef __cplusplus
}
#endif

// Engine code (not the compatibility layer itself, built with BO1_COMPAT_IMPL) gets the path-translating CRT
// entry points. Function-like macros, so 'struct stat' and 'using ::fopen' are unaffected. remove() is left alone:
// the physics containers have member functions of that name, and the engine only passes it host paths built by
// FS_BuildOSPath.
#if !defined(BO1_COMPAT_IMPL)
#define fopen(p, m) bo1_fopen(p, m)
#define freopen(p, m, f) bo1_freopen(p, m, f)
#define stat(p, s) bo1_stat(p, s)
#define rename(a, b) bo1_rename(a, b)
#endif

// The engine defines its own random()/crandom() (com_math.cpp); POSIX's long random(void) would clash.
#if defined(__cplusplus) && !defined(__OBJC__)
#define random bo1_random
#define crandom bo1_crandom
#endif

// MSVC's rand(): 15-bit results (RAND_MAX 0x7FFF) from a per-thread LCG seeded with 1. The engine scales rand() by
// 1/32767 or 1/32768 and masks it with 0x7FFF; with the 31-bit rand() of macOS, GaussianRandom's rejection loop
// never ends and the scaled values leave their ranges. (kernel32_misc.cpp)
#ifdef __cplusplus
extern "C" {
#endif
int bo1_msvc_rand(void);
void bo1_msvc_srand(unsigned int seed);
#ifdef __cplusplus
}
// standard library headers call std::rand() (std::random_shuffle), which the macros below turn into these names
namespace std
{
using ::bo1_msvc_rand;
using ::bo1_msvc_srand;
}
#endif
#undef RAND_MAX
#define RAND_MAX 0x7FFF
#define rand() bo1_msvc_rand()
#define srand(seed) bo1_msvc_srand(seed)

#ifdef __cplusplus
// _BitScanForward/_BitScanReverse with MSVC's DWORD (unsigned long, 32-bit there) index argument: accept any integer
// index type; clang's builtins only take unsigned int*.
template <typename I, typename M> static inline unsigned char bo1_BitScanForward(I *index, M mask)
{
    unsigned long long m = (unsigned long long)mask;
    if (!m)
        return 0;
    *index = (I)__builtin_ctzll(m);
    return 1;
}
template <typename I, typename M> static inline unsigned char bo1_BitScanReverse(I *index, M mask)
{
    unsigned long long m = (unsigned long long)mask;
    if (!m)
        return 0;
    *index = (I)(63 - __builtin_clzll(m));
    return 1;
}
#define _BitScanForward(i, m) bo1_BitScanForward(i, (unsigned int)(m))
#define _BitScanReverse(i, m) bo1_BitScanReverse(i, (unsigned int)(m))
#define _BitScanForward64(i, m) bo1_BitScanForward(i, (unsigned long long)(m))
#define _BitScanReverse64(i, m) bo1_BitScanReverse(i, (unsigned long long)(m))
#endif

#ifdef __cplusplus
// MSVC's secure CRT templates that infer the destination size from an array argument.
template <size_t N> static inline int sprintf_s(char (&buf)[N], const char *fmt, ...) __attribute__((format(printf, 2, 3)));
template <size_t N> static inline int sprintf_s(char (&buf)[N], const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsprintf_s(buf, N, fmt, ap);
    va_end(ap);
    return r;
}
template <size_t N> static inline int vsprintf_s(char (&buf)[N], const char *fmt, va_list ap) { return vsprintf_s(buf, N, fmt, ap); }
template <size_t N> static inline int _snprintf_s(char (&buf)[N], size_t count, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnprintf_s(buf, N, count, fmt, ap);
    va_end(ap);
    return r;
}
template <size_t N> static inline int _vsnprintf_s(char (&buf)[N], size_t count, const char *fmt, va_list ap) { return _vsnprintf_s(buf, N, count, fmt, ap); }
template <size_t N> static inline int strcpy_s(char (&dst)[N], const char *src) { return strcpy_s(dst, N, src); }
template <size_t N> static inline int strncpy_s(char (&dst)[N], const char *src, size_t count) { return strncpy_s(dst, N, src, count); }
template <size_t N> static inline int strcat_s(char (&dst)[N], const char *src) { return strcat_s(dst, N, src); }
template <size_t N> static inline int strncat_s(char (&dst)[N], const char *src, size_t count) { return strncat_s(dst, N, src, count); }
template <size_t N> static inline int _itoa_s(int v, char (&buf)[N], int radix) { return _itoa_s(v, buf, N, radix); }
template <size_t N> static inline int _snwprintf_s(wchar_t (&buf)[N], size_t count, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = _vsnwprintf_s(buf, N, count, fmt, ap);
    va_end(ap);
    return r;
}
#endif
