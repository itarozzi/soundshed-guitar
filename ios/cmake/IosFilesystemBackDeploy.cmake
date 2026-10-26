# ── <filesystem> on iOS 12 ──────────────────────────────────────────────────────
# The core, NAM and the JUCE adapter use std::filesystem throughout. iOS 12's libc++.dylib
# does not have it (it arrived in iOS 13), so libc++'s headers refuse it below a 13.0
# minimum, and code that got past them would fail to load on the device. For a minimum below
# 13.0 (ios/build.sh legacy, for the iPad mini 2 and the other devices that stop at iOS 12):
#
#   - every C++ and Objective-C++ file force-includes FilesystemBackDeploy.h, which lifts
#     libc++'s mark from <filesystem> and from nothing else;
#   - LLVM's own implementation (ios/compat/libcxx-filesystem) is built as a static library
#     and linked into every target, so the symbols resolve inside the app.
#
# Included from juce/CMakeLists.txt before the core is added, so that the compile option and
# the link reach every target made after it, the core's fetched dependencies included. A
# no-op for any other build.

if(NOT IOS OR NOT CMAKE_OSX_DEPLOYMENT_TARGET OR CMAKE_OSX_DEPLOYMENT_TARGET VERSION_GREATER_EQUAL 13.0)
    return()
endif()

set(_ssg_fs_dir "${CMAKE_CURRENT_LIST_DIR}/../compat/libcxx-filesystem")
message(STATUS "iOS ${CMAKE_OSX_DEPLOYMENT_TARGET}: building libc++ <filesystem> into the app (ios/compat/libcxx-filesystem)")

add_compile_options("$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:SHELL:-include \"${_ssg_fs_dir}/FilesystemBackDeploy.h\">")

# clang uses a precompiled header only when its -include comes first, and Xcode puts the
# core's (target_precompile_headers) after the one above, so it would be ignored and then not
# found. Legacy builds are occasional: they go without precompiled headers.
set(CMAKE_DISABLE_PRECOMPILE_HEADERS ON)

add_library(guitarfx_libcxx_filesystem STATIC
    "${_ssg_fs_dir}/directory_entry.cpp"
    "${_ssg_fs_dir}/directory_iterator.cpp"
    "${_ssg_fs_dir}/filesystem_clock.cpp"
    "${_ssg_fs_dir}/filesystem_error.cpp"
    "${_ssg_fs_dir}/operations.cpp"
    "${_ssg_fs_dir}/path.cpp"
    # Ours: the abort handler those sources call, which iOS 12's libc++ does not have.
    "${_ssg_fs_dir}/VerboseAbort.cpp")
# As libc++ builds them: its sources expect _LIBCPP_BUILDING_LIBRARY and C++23 at least.
target_compile_definitions(guitarfx_libcxx_filesystem PRIVATE _LIBCPP_BUILDING_LIBRARY)
target_compile_features(guitarfx_libcxx_filesystem PRIVATE cxx_std_23)
target_compile_options(guitarfx_libcxx_filesystem PRIVATE -w)
set_target_properties(guitarfx_libcxx_filesystem PROPERTIES FOLDER "GuitarFX/iOS")

# Every target made from here on, in this directory and below, links it.
link_libraries(guitarfx_libcxx_filesystem)
