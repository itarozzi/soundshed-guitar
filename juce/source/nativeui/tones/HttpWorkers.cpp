#include "nativeui/tones/HttpWorkers.h"

#include <vector>

namespace soundshed::nano
{
HttpWorkers::HttpWorkers (int threads) : pool (threads) {}

HttpWorkers::~HttpWorkers()
{
    {
        const std::lock_guard<std::mutex> lock (mutex);
        stopping = true;

        for (auto* stream : inFlight)
            stream->cancel();
    }

    // A cancelled request returns at once, so this waits only for the jobs to get there. Left to
    // itself the pool would wait on the timeouts, then kill a thread still inside the network stack.
    pool.removeAllJobs (true, 2000);
}

void HttpWorkers::run (std::function<void()> job)
{
    pool.addJob ([this, job = std::move (job)] {
        {
            const std::lock_guard<std::mutex> lock (mutex);

            if (stopping)
                return;
        }

        job();
    });
}

HttpWorkers::Response HttpWorkers::perform (const juce::URL& url, const juce::String& method, const juce::String& headers, int timeoutMs)
{
    Response response;
    const bool hasBody = method != "GET";

    juce::WebInputStream stream (url, hasBody);
    stream.withExtraHeaders (headers).withConnectionTimeout (timeoutMs).withNumRedirectsToFollow (5);

    if (hasBody)
        stream.withCustomRequestCommand (method);

    {
        const std::lock_guard<std::mutex> lock (mutex);

        if (stopping)
        {
            response.cancelled = true;
            return response;
        }

        inFlight.insert (&stream);
    }

    // Takes the stream back out of the destructor's reach; true when it was cut short.
    const auto release = [this, &stream] {
        const std::lock_guard<std::mutex> lock (mutex);
        inFlight.erase (&stream);
        return stopping;
    };

    if (! stream.connect (nullptr))
    {
        response.cancelled = release();
        return response;
    }

    response.status = stream.getStatusCode();
    response.headers = stream.getResponseHeaders();

    // In pieces, so a download cut short stops at the next one.
    std::vector<char> buffer (64 * 1024);

    for (;;)
    {
        const auto read = stream.read (buffer.data(), (int) buffer.size());

        if (read <= 0)
            break;

        response.body.append (buffer.data(), (std::size_t) read);
    }

    if (release())
    {
        response.cancelled = true;
        response.status = 0;
    }

    return response;
}
} // namespace soundshed::nano
