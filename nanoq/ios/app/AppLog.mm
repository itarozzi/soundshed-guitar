#include "AppLog.h"

#include "Log.h"

namespace nanoq::ios
{
void AppLog(NSString* format, ...)
{
    va_list args;
    va_start(args, format);
    NSString* line = [[NSString alloc] initWithFormat:format arguments:args];
    va_end(args);

    NSLog(@"%@", line);
    nanoq::Log(line.UTF8String != nullptr ? line.UTF8String : "");
}
} // namespace nanoq::ios
