// Ours, not LLVM's. libc++'s <filesystem> sources are compiled with _LIBCPP_BUILDING_LIBRARY,
// which assumes the library being built provides std::__libcpp_verbose_abort: their assertions
// call it. The system libc++ has it only from iOS 16.3, so without a definition here the app
// would import it and fail to load on iOS 12. libc++ documents the function as one a program
// may define for itself; this one prints the message and aborts, as libc++'s own does.
#include <__verbose_abort>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

_LIBCPP_BEGIN_NAMESPACE_STD

void __libcpp_verbose_abort (const char* format, ...) _LIBCPP_VERBOSE_ABORT_NOEXCEPT
{
    va_list args;
    va_start (args, format);
    std::vfprintf (stderr, format, args);
    va_end (args);
    std::abort();
}

_LIBCPP_END_NAMESPACE_STD
