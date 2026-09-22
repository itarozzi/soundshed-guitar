# AudioSnapshot renders every effect, and a set of whole chains, at their defaults for
# tools/audio-ab (see tools/audio-ab/README.md). It is a tool rather than a test, so it is
# built with the tests but never registered with ctest.
#
# tools/audio-ab copies this file and the harness sources into older checkouts and includes it
# from their core/tests/CMakeLists.txt, so it must not rely on anything that file defines
# beyond the SoundshedGuitarCore target.
if(TARGET AudioSnapshot)
    return()
endif()

add_executable(AudioSnapshot
    "${CMAKE_CURRENT_LIST_DIR}/AudioSnapshot.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/AudioSnapshotChains.h"
    "${CMAKE_CURRENT_LIST_DIR}/AudioSnapshotSupport.h"
)
target_link_libraries(AudioSnapshot PRIVATE SoundshedGuitarCore)
if(TARGET NeuralAmpModelerCore)
    target_link_libraries(AudioSnapshot PRIVATE NeuralAmpModelerCore)
elseif(TARGET nam)
    target_link_libraries(AudioSnapshot PRIVATE nam)
endif()
target_include_directories(AudioSnapshot PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
if(MSVC)
    target_compile_options(AudioSnapshot PRIVATE /bigobj)
endif()
target_compile_options(AudioSnapshot PRIVATE $<TARGET_PROPERTY:SoundshedGuitarCore,COMPILE_OPTIONS>)
set_target_properties(AudioSnapshot PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "GuitarFX/Tests"
)
target_precompile_headers(AudioSnapshot REUSE_FROM SoundshedGuitarCore)
