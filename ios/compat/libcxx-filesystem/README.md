# libc++ `<filesystem>` for iOS 12

Used only by an iOS build whose minimum is below 13.0 (`ios/build.sh legacy`, for the iPad
mini 2 and other devices that stop at iOS 12). See docs/ios-build.md, "iOS 12 (legacy) builds".

The `.cpp` and `.h` files are unmodified copies of `libcxx/src/filesystem/` from LLVM
**llvmorg-20.1.8**, the release that Xcode 26's libc++ headers (`_LIBCPP_VERSION 200100`)
come from. `int128_builtins.cpp` is left out: it is for targets without a 128-bit divide.
`LICENSE.TXT` is libc++'s licence (Apache-2.0 with LLVM exceptions).

`VerboseAbort.cpp` is ours too: the sources are built as libc++ itself is
(`_LIBCPP_BUILDING_LIBRARY`), so their assertions call `std::__libcpp_verbose_abort`, which the
system libc++ has only from iOS 16.3. libc++ lets a program define it; this one prints and aborts.

`FilesystemBackDeploy.h` is ours. It is force-included into every C++ file and lifts libc++'s
"unavailable before iOS 13" mark from `<filesystem>` alone, so the code compiles and calls
these copies instead of the system library, which on iOS 12 does not have them.

When Xcode's libc++ moves to a new LLVM release, replace the files with that release's
`libcxx/src/filesystem/` and update the tag here. A mismatch shows up as a compile error in
these files, not at run time: they are built against the SDK's own headers.
