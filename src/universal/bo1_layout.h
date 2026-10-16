#pragma once
// Layout checks against the 32-bit Windows executable the engine was decompiled from.
//
// Structures loaded from fastfiles keep that layout on every target (their pointer fields are Ptr32, see ptr32.h), so
// their size checks are plain static_asserts. Runtime structures hold native pointers and grow on 64-bit targets;
// BO1_LAYOUT32_ASSERT keeps their checks for 32-bit builds, where they still catch decompilation mistakes. Code must
// size runtime structures with sizeof/offsetof, never with the byte counts from the 32-bit binary.
#if defined(_WIN64) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
#define BO1_PTR64 1
#define BO1_LAYOUT32_ASSERT(...) static_assert(true, "")
#else
#define BO1_PTR64 0
#define BO1_LAYOUT32_ASSERT(...) static_assert(__VA_ARGS__)
#endif
