# The fused WaveNet forward pass (src/dsp/nam/FusedWaveNet.h): how NAM models run wherever their
# shape allows. Built in a directory scope of its own for the reason the NAM core is (see
# cmake/NeuralAmpModelerCore/CMakeLists.txt): unoptimised, its kernel is several times slower than
# the optimised NAM core it replaces, and a Debug app could not play a NAM model in real time.

add_library(SoundshedGuitarFusedWaveNet STATIC
    "${GUITARFX_CORE_ROOT}/src/dsp/nam/FusedWaveNet.h"
    "${GUITARFX_CORE_ROOT}/src/dsp/nam/FusedWaveNet.cpp"
    "${GUITARFX_CORE_ROOT}/src/dsp/nam/FusedWaveNetPack.cpp"
    "${GUITARFX_CORE_ROOT}/src/dsp/nam/FusedWaveNetSimd.h")
set_target_properties(SoundshedGuitarFusedWaveNet PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
guitarfx_disable_linux_gnu_unique(SoundshedGuitarFusedWaveNet)
guitarfx_apply_release_optimizations(SoundshedGuitarFusedWaveNet)
target_include_directories(SoundshedGuitarFusedWaveNet PUBLIC "${GUITARFX_CORE_ROOT}/src")

if(TARGET NeuralAmpModelerCore)
    target_link_libraries(SoundshedGuitarFusedWaveNet PUBLIC NeuralAmpModelerCore)
elseif(TARGET nam)
    target_link_libraries(SoundshedGuitarFusedWaveNet PUBLIC nam)
elseif(DEFINED NeuralAmpModelerCore_SOURCE_DIR)
    target_include_directories(SoundshedGuitarFusedWaveNet PUBLIC
        "${NeuralAmpModelerCore_SOURCE_DIR}"
        "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/eigen"
        "${NeuralAmpModelerCore_SOURCE_DIR}/Dependencies/nlohmann")
endif()

if(TARGET nlohmann_json::nlohmann_json)
    target_link_libraries(SoundshedGuitarFusedWaveNet PUBLIC nlohmann_json::nlohmann_json)
endif()

# The NEON kernel compiles in every arm64 build, but models run on it only with
# GUITARFX_FUSED_WAVENET_NEON on (core/CMakeLists.txt). No effect on other architectures.
if(GUITARFX_FUSED_WAVENET_NEON)
    target_compile_definitions(SoundshedGuitarFusedWaveNet PRIVATE GUITARFX_FWN_NEON_ENABLED=1)
endif()

# Each output must sum its inputs in source order whatever the register tile, or the output stops
# being bit-identical across host block sizes. Fast-math's reassociation would let the compiler
# reorder the two-SSE-half kernel's mul-add chains differently in different tile shapes. The rest
# of fast-math stays on.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    if(MSVC)
        target_compile_options(SoundshedGuitarFusedWaveNet PRIVATE
            $<$<CONFIG:Release>:/clang:-fno-associative-math>
            $<$<CONFIG:RelWithDebInfo>:/clang:-fno-associative-math>)
    else()
        target_compile_options(SoundshedGuitarFusedWaveNet PRIVATE
            $<$<CONFIG:Release>:-fno-associative-math>
            $<$<CONFIG:RelWithDebInfo>:-fno-associative-math>)
    endif()
endif()

# Never link-time optimised, as the NAM core: /GL made that 1.3-2x slower, and the kernel's
# inlining is already decided in its own translation unit.
set_target_properties(SoundshedGuitarFusedWaveNet PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
target_compile_options(SoundshedGuitarFusedWaveNet PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/GL->)

if(GUITARFX_OPTIMISE_NAM_IN_DEBUG)
    if(MSVC)
        string(REGEX REPLACE "[/-](RTC[1csu]+|Od|Ob0)( |$)" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
        target_compile_options(SoundshedGuitarFusedWaveNet PRIVATE $<$<CONFIG:Debug>:/O2> $<$<CONFIG:Debug>:/Ob2>)
    else()
        target_compile_options(SoundshedGuitarFusedWaveNet PRIVATE $<$<CONFIG:Debug>:-O2>)
    endif()
endif()
