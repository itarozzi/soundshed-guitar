#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace soundshed::nano
{
/// A few worker threads for HTTP requests, every one of which the destructor cuts short.
///
/// A request to a service that takes the connection and then says nothing waits for JUCE's
/// timeouts (15 s to connect, as long again for the reply), and juce::ThreadPool alone waits for
/// its jobs before killing their threads: quitting with the Tones page waiting on such a network
/// hung the app for up to 23 s. Destroying the workers cancels every request in flight instead,
/// which returns at once, and comes back as soon as the jobs have let go.
class HttpWorkers
{
public:
    struct Response
    {
        int status = 0; // 0: no answer (no connection, or cut short)
        std::string body;
        juce::StringPairArray headers;
        bool cancelled = false;
    };

    explicit HttpWorkers (int threads);
    ~HttpWorkers();

    /// Runs `job` on a worker. In it, perform() makes the requests; a job may make several.
    void run (std::function<void()> job);

    /// One request, blocking, on a worker. `method` is GET, POST, PUT or DELETE; any but GET sends
    /// the URL's POST data as the body. `headers` are "Name: value\r\n" lines. A request the
    /// destructor cut short answers status 0 with `cancelled` set.
    [[nodiscard]] Response perform (const juce::URL& url, const juce::String& method, const juce::String& headers, int timeoutMs);

private:
    juce::ThreadPool pool;
    std::mutex mutex;
    std::set<juce::WebInputStream*> inFlight;
    bool stopping = false;

    JUCE_DECLARE_NON_COPYABLE (HttpWorkers)
};
} // namespace soundshed::nano
