# The clang flags the decompiled engine compiles with wherever it is built with clang: macOS and Linux
# (cmake/port.cmake) and Windows with MinGW (cmake/windows.cmake). The MSVC project keeps its own (CMakeLists.txt).
include_guard(GLOBAL)

set(BO1_CLANG_LANGUAGE_FLAGS
    # MSVC language extensions used throughout the decompiled engine (__int64, __declspec(align), __forceinline,
    # _InterlockedXxx/_BitScanXxx builtins, __debugbreak, anonymous struct members, ...)
    -fms-extensions
    # The engine (like the MSVC build) relies on wrapping signed arithmetic and on type punning.
    -fwrapv
    -fno-strict-aliasing
    # Decompiled functions sometimes fall off the end on paths that never happen; keep MSVC's behaviour
    # (an unspecified value) instead of treating those paths as unreachable.
    -fno-strict-return
    -fno-delete-null-pointer-checks
    # ... and newer Clang releases turn the warning about those paths into an error by default
    -Wno-return-type
    # The engine was written for MSVC on x86, where a volatile read is an acquire and a volatile write a release
    # (/volatile:ms, and x86 stores and loads are ordered anyway). Its cross-thread flags and counters (render
    # command counts, worker states, the dvar read/write counts, ...) depend on that; ARM reorders plain accesses,
    # so volatile accesses get the same acquire/release semantics here (ldapr/stlr; free on x86).
    -fms-volatile
    # SSE arithmetic never fuses a multiply and an add; keep the same rounding (Clang would contract a * b + c
    # into an FMA on ARM), so physics, collision and prediction compute what the x86 game computes.
    -ffp-contract=off
    # MSVC's char is signed on every processor; ARM Linux's is unsigned by default
    -fsigned-char
)
set(BO1_CLANG_COMMON_WARNING_FLAGS
    -Wno-ignored-attributes
    -Wno-microsoft
    -Wno-deprecated-declarations
    -Wno-unknown-warning-option
)
set(BO1_CLANG_CXX_FLAGS
    # MSVC /permissive equivalents for decompiled code
    -Wno-c++11-narrowing
    -Wno-register
    -Wno-writable-strings
    -Wno-invalid-offsetof
    -Wno-unused-value
    -Wno-switch
    -Wno-parentheses
    -Wno-dangling-else
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-braces
    -Wno-null-conversion
    -Wno-tautological-compare
    -Wno-tautological-constant-out-of-range-compare
    -Wno-shift-negative-value
    -Wno-constant-conversion
    -Wno-unused-private-field
    -Wno-inconsistent-missing-override
    -Wno-delete-non-abstract-non-virtual-dtor
    -Wno-format
    -Wno-format-security
    -Wno-char-subscripts
    -Wno-unknown-pragmas
    -Wno-pointer-bool-conversion
    -Wno-undefined-var-template
    -Wno-c++20-designator
    -Wno-nonportable-include-path
    -Wno-comment
    -Werror=return-stack-address
)
