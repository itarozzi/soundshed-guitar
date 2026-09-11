include_guard(GLOBAL)

include(CheckCXXCompilerFlag)
include("${CMAKE_CURRENT_LIST_DIR}/GuitarfxWindowsArch.cmake")

# ---------------------------------------------------------------------------
# SIMD baseline for x86-64 Release/RelWithDebInfo builds.
#
#   avx2  /arch:AVX2, -mavx2.  Intel Haswell (2013) / AMD Excavator (2015) on.
#   avx   /arch:AVX,  -mavx.   Intel Sandy Bridge / AMD Bulldozer (2011) on.
#         Still compiles every 256-bit kernel in core/src/dsp/simd/SimdMath.h,
#         because those guard on __AVX__ rather than __AVX2__ and the codebase
#         uses no AVX2-only intrinsics. Only the compiler's own FMA contraction
#         and 256-bit integer auto-vectorisation are given up.
#   sse2  No /arch: flag, i.e. the MSVC x64 baseline. Drops the 256-bit kernels
#         entirely, for CPUs with no AVX at all (Gemini Lake / Jasper Lake
#         Celeron and Pentium Silver, pre-2011 x86-64).
#
# Only affects x86-64; arm64 picks its own baseline below and other
# architectures are left on the compiler default.
# ---------------------------------------------------------------------------
set(GUITARFX_CORE_SIMD_LEVEL "avx2" CACHE STRING
    "SIMD baseline for x86-64 release builds: avx2, avx, or sse2")
set_property(CACHE GUITARFX_CORE_SIMD_LEVEL PROPERTY STRINGS avx2 avx sse2)

# Superseded by GUITARFX_CORE_SIMD_LEVEL. OFF has always meant "emit no /arch:
# flag", so it maps to the sse2 tier to keep existing callers building what they
# built before.
option(GUITARFX_CORE_ENABLE_AVX2 "Deprecated; use GUITARFX_CORE_SIMD_LEVEL instead" ON)
if(NOT GUITARFX_CORE_ENABLE_AVX2 AND GUITARFX_CORE_SIMD_LEVEL STREQUAL "avx2")
    set(GUITARFX_CORE_SIMD_LEVEL "sse2" CACHE STRING
        "SIMD baseline for x86-64 release builds: avx2, avx, or sse2" FORCE)
    message(STATUS "GUITARFX_CORE_ENABLE_AVX2=OFF is deprecated; treating it as GUITARFX_CORE_SIMD_LEVEL=sse2")
endif()

string(TOLOWER "${GUITARFX_CORE_SIMD_LEVEL}" _guitarfx_simd_level)
if(NOT _guitarfx_simd_level MATCHES "^(avx2|avx|sse2)$")
    message(FATAL_ERROR
        "GUITARFX_CORE_SIMD_LEVEL must be one of: avx2, avx, sse2 (got \"${GUITARFX_CORE_SIMD_LEVEL}\")")
endif()
set(GUITARFX_CORE_SIMD_LEVEL "${_guitarfx_simd_level}" CACHE STRING
    "SIMD baseline for x86-64 release builds: avx2, avx, or sse2" FORCE)

if(MSVC)
    check_cxx_compiler_flag("/arch:AVX2" GUITARFX_MSVC_HAS_ARCH_AVX2)
    check_cxx_compiler_flag("/arch:AVX" GUITARFX_MSVC_HAS_ARCH_AVX)
    check_cxx_compiler_flag("/arch:armv8.2" GUITARFX_MSVC_HAS_ARCH_ARMV82)
    check_cxx_compiler_flag("/arch:armv8.1" GUITARFX_MSVC_HAS_ARCH_ARMV81)
endif()

message(STATUS "Core SIMD baseline: ${GUITARFX_CORE_SIMD_LEVEL} (applies to x86-64 targets only)")

# Applies the architecture baseline flags for Release/RelWithDebInfo to a target.
# <visibility> is PRIVATE for concrete targets and INTERFACE for carrier targets
# such as JUCE's SharedCode.
function(guitarfx_apply_simd_arch_flags target_name visibility)
    if(MSVC)
        guitarfx_detect_windows_arch(_arch)

        if(_arch MATCHES "^(x64|amd64|x86_64)$")
            set(_flag "")
            if(GUITARFX_CORE_SIMD_LEVEL STREQUAL "avx2" AND GUITARFX_MSVC_HAS_ARCH_AVX2)
                set(_flag "/arch:AVX2")
            elseif(GUITARFX_CORE_SIMD_LEVEL STREQUAL "avx" AND GUITARFX_MSVC_HAS_ARCH_AVX)
                set(_flag "/arch:AVX")
            endif()
            if(_flag)
                target_compile_options(${target_name} ${visibility}
                    $<$<CONFIG:Release>:${_flag}>
                    $<$<CONFIG:RelWithDebInfo>:${_flag}>)
            endif()
        elseif(_arch MATCHES "^(arm64|arm64ec|aarch64)$")
            if(GUITARFX_MSVC_HAS_ARCH_ARMV82)
                target_compile_options(${target_name} ${visibility}
                    $<$<CONFIG:Release>:/arch:armv8.2>
                    $<$<CONFIG:RelWithDebInfo>:/arch:armv8.2>)
            elseif(GUITARFX_MSVC_HAS_ARCH_ARMV81)
                target_compile_options(${target_name} ${visibility}
                    $<$<CONFIG:Release>:/arch:armv8.1>
                    $<$<CONFIG:RelWithDebInfo>:/arch:armv8.1>)
            endif()
        endif()

        return()
    endif()

    # Everything below is gcc/clang. sse2 is the x86-64 ABI baseline, so it needs
    # no flag of its own.
    set(_flag "")
    if(GUITARFX_CORE_SIMD_LEVEL STREQUAL "avx2")
        set(_flag "-mavx2")
    elseif(GUITARFX_CORE_SIMD_LEVEL STREQUAL "avx")
        set(_flag "-mavx")
    endif()
    if(NOT _flag)
        return()
    endif()

    if(APPLE)
        set(_osx_arches "${CMAKE_OSX_ARCHITECTURES}")
        if(NOT _osx_arches)
            set(_osx_arches "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        if(NOT _osx_arches MATCHES "x86_64")
            return()
        endif()

        # -Xarch_x86_64 keeps the flag off the arm64 slice of a universal build.
        if(CMAKE_CONFIGURATION_TYPES)
            target_compile_options(${target_name} ${visibility}
                $<$<CONFIG:Release>:-Xarch_x86_64>
                $<$<CONFIG:Release>:${_flag}>
                $<$<CONFIG:RelWithDebInfo>:-Xarch_x86_64>
                $<$<CONFIG:RelWithDebInfo>:${_flag}>)
        elseif(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
            target_compile_options(${target_name} ${visibility}
                "SHELL:-Xarch_x86_64 ${_flag}")
        endif()

        return()
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
        target_compile_options(${target_name} ${visibility}
            $<$<CONFIG:Release>:${_flag}>
            $<$<CONFIG:RelWithDebInfo>:${_flag}>)
    endif()
endfunction()
