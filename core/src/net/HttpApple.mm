// The macOS and iOS transport: NSURLSession, which follows redirects and uses the system's
// certificates and proxy. A request waits on a semaphore for its task; cancelling cancels the
// task, which completes at once with NSURLErrorCancelled.

#include "net/Http.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cctype>

namespace guitarfx::net
{
namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

class AppleTransport final : public HttpTransport
{
public:
    AppleTransport()
    {
        NSURLSessionConfiguration* configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
        configuration.HTTPCookieStorage = nil;
        configuration.URLCache = nil;
        mSession = [NSURLSession sessionWithConfiguration:configuration];
    }

    ~AppleTransport() override
    {
        [mSession invalidateAndCancel];
    }

    HttpResponse Perform(const HttpRequest& request, CancelToken& cancel) override
    {
        HttpResponse response;
        @autoreleasepool
        {
            NSString* address = [NSString stringWithUTF8String:request.url.c_str()];
            NSURL* url = address != nil ? [NSURL URLWithString:address] : nil;
            if (url == nil)
            {
                response.error = "Not a URL: " + request.url;
                return response;
            }

            NSMutableURLRequest* urlRequest = [NSMutableURLRequest requestWithURL:url];
            urlRequest.HTTPMethod = [NSString stringWithUTF8String:(request.method.empty() ? "GET" : request.method.c_str())];
            urlRequest.timeoutInterval = std::max(1, request.timeoutMs) / 1000.0;
            for (const auto& [name, value] : request.headers)
                [urlRequest setValue:[NSString stringWithUTF8String:value.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:name.c_str()]];
            if (request.method != "GET" && request.method != "HEAD")
                urlRequest.HTTPBody = [NSData dataWithBytes:request.body.data() length:request.body.size()];

            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            __block HttpResponse* out = &response;
            NSURLSessionDataTask* task =
                [mSession dataTaskWithRequest:urlRequest
                            completionHandler:^(NSData* data, NSURLResponse* answer, NSError* error) {
                              if (error != nil)
                              {
                                  out->cancelled = error.code == NSURLErrorCancelled;
                                  out->error = out->cancelled ? std::string("Cancelled")
                                                              : std::string(error.localizedDescription.UTF8String ?: "Network error");
                              }
                              else
                              {
                                  if ([answer isKindOfClass:[NSHTTPURLResponse class]])
                                  {
                                      NSHTTPURLResponse* http = (NSHTTPURLResponse*)answer;
                                      out->status = static_cast<int>(http.statusCode);
                                      [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL* stop) {
                                        (void)stop;
                                        if ([key isKindOfClass:[NSString class]] && [value isKindOfClass:[NSString class]])
                                            out->headers[Lower(std::string([(NSString*)key UTF8String]))] =
                                                std::string([(NSString*)value UTF8String]);
                                      }];
                                  }
                                  if (data != nil)
                                      out->body.assign(static_cast<const char*>(data.bytes), data.length);
                              }
                              dispatch_semaphore_signal(done);
                            }];

            if (!cancel.SetAbort([task] { [task cancel]; }))
            {
                response.cancelled = true;
                response.error = "Cancelled";
                return response;
            }
            [task resume];
            dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
            cancel.ClearAbort();
        }
        return response;
    }

private:
    NSURLSession* mSession = nil;
};
} // namespace

std::shared_ptr<HttpTransport> MakePlatformTransport()
{
    return std::make_shared<AppleTransport>();
}
} // namespace guitarfx::net
