#pragma once
// Casts between pointers and 32-bit integers in code written for a 32-bit address space.
//
// Much of the engine (decompiled from the 32-bit Windows executable) keeps addresses in int/unsigned int variables,
// struct fields and bytecode, and does arithmetic on them: (T *)(base + 12), (unsigned int)ptr & ~15, ...
// Every such cast spells its operand through one of these macros:
//   (T *)BO1_I2P(intValue)   integer -> pointer
//   (int)BO1_P2I(pointer)    pointer -> integer
// On 32-bit Windows they are the plain casts the code always had. On 64-bit targets the integer is the pointer's
// 32-bit handle (src/universal/ptr32.h): offsets into the same block stay offsets, alignment masks still work, and
// the value converts back to the full address.

#include <universal/ptr32.h>

#if BO1_PTR32_IS_CLASS
#define BO1_I2P(x) (::bo1ptr::DecodeInt(x))
#define BO1_P2I(x) (::bo1ptr::EncodeAny(x))
#else
#define BO1_I2P(x) (x)
#define BO1_P2I(x) (x)
#endif
