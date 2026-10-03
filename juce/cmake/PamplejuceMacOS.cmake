
# The minimum macOS (CMAKE_OSX_DEPLOYMENT_TARGET) is set in juce/CMakeLists.txt,
# before project(). This file is included after project(), where a FORCE set
# here used to override it.

# Building universal binaries on macOS increases build time
# This is set on CI but not during local dev
if ((DEFINED ENV{CI}) AND (CMAKE_BUILD_TYPE STREQUAL "Release") AND NOT (CMAKE_SYSTEM_NAME STREQUAL "iOS"))
    message("Building for Apple Silicon and x86_64")
    set(CMAKE_OSX_ARCHITECTURES arm64 x86_64)
endif ()

# By default we don't want Xcode schemes to be made for modules, etc
set(CMAKE_XCODE_GENERATE_SCHEME OFF)
