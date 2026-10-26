// Force-included (-include) into every C++ and Objective-C++ translation unit of an iOS build
// whose minimum is below 13.0, by ios/cmake/IosFilesystemBackDeploy.cmake.
//
// libc++ marks everything in <filesystem> that lives in the shared library as unavailable
// before iOS 13, because iOS 12's libc++.dylib does not have it. This lifts that one mark and
// nothing else: the implementation is compiled into the app instead (the .cpp files beside
// this header, from LLVM 20.1.8, the libc++ release Xcode 26's headers come from), so the
// symbols resolve inside the binary and never reach for the system library. Every other
// availability mark stays, so a newer library feature still fails to compile rather than
// failing to load on the device.
//
// It works because the macros are expanded where libc++'s headers use them, not where
// availability.h defines them: <__config> brings in availability.h once, and every libc++
// header after this point sees the redefinitions.
#pragma once

#ifdef __cplusplus
 #include <__config>

 #undef _LIBCPP_AVAILABILITY_HAS_FILESYSTEM_LIBRARY
 #define _LIBCPP_AVAILABILITY_HAS_FILESYSTEM_LIBRARY 1
 #undef _LIBCPP_AVAILABILITY_FILESYSTEM_LIBRARY
 #define _LIBCPP_AVAILABILITY_FILESYSTEM_LIBRARY
 #undef _LIBCPP_AVAILABILITY_FILESYSTEM_LIBRARY_PUSH
 #define _LIBCPP_AVAILABILITY_FILESYSTEM_LIBRARY_PUSH
 #undef _LIBCPP_AVAILABILITY_FILESYSTEM_LIBRARY_POP
 #define _LIBCPP_AVAILABILITY_FILESYSTEM_LIBRARY_POP
#endif
