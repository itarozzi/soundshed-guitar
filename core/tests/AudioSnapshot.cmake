# AudioSnapshot renders every effect, and a set of whole chains, at their defaults, and
# AudioPerf times effects and small chains, for tools/audio-ab (see tools/audio-ab/README.md).
# They are tools rather than tests, so they are built with the tests but never registered with
# ctest.
#
# tools/audio-ab copies this file and the harness sources into older checkouts and includes it
# from their core/tests/CMakeLists.txt, so it must not rely on anything that file defines
# beyond the SoundshedGuitarCore target.
if(TARGET AudioSnapshot)
    return()
endif()

function(guitarfx_audio_ab_tool name)
    add_executable(${name} ${ARGN}
        "${CMAKE_CURRENT_LIST_DIR}/AudioSnapshotChains.h"
        "${CMAKE_CURRENT_LIST_DIR}/AudioSnapshotSupport.h"
    )
    target_link_libraries(${name} PRIVATE SoundshedGuitarCore)
    if(TARGET NeuralAmpModelerCore)
        target_link_libraries(${name} PRIVATE NeuralAmpModelerCore)
    elseif(TARGET nam)
        target_link_libraries(${name} PRIVATE nam)
    endif()
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
    if(MSVC)
        target_compile_options(${name} PRIVATE /bigobj)
    endif()
    target_compile_options(${name} PRIVATE $<TARGET_PROPERTY:SoundshedGuitarCore,COMPILE_OPTIONS>)
    set_target_properties(${name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "GuitarFX/Tests"
    )
    target_precompile_headers(${name} REUSE_FROM SoundshedGuitarCore)
endfunction()

guitarfx_audio_ab_tool(AudioSnapshot "${CMAKE_CURRENT_LIST_DIR}/AudioSnapshot.cpp")
guitarfx_audio_ab_tool(AudioPerf "${CMAKE_CURRENT_LIST_DIR}/AudioPerf.cpp")
