#include "nativeui/tones/JuceHttpTransport.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace soundshed::nano
{
namespace
{
juce::String utf8 (const std::string& text)
{
    return juce::String::fromUTF8 (text.c_str(), (int) text.size());
}

class JuceHttpTransport final : public guitarfx::net::HttpTransport
{
public:
    guitarfx::net::HttpResponse Perform (const guitarfx::net::HttpRequest& request, guitarfx::net::CancelToken& cancel) override
    {
        guitarfx::net::HttpResponse response;
        const auto method = request.method.empty() ? std::string ("GET") : request.method;
        const bool hasBody = method != "GET" && method != "HEAD";

        juce::URL url (utf8 (request.url));

        if (hasBody)
            url = url.withPOSTData (juce::MemoryBlock (request.body.data(), request.body.size()));

        juce::String headers;

        for (const auto& [name, value] : request.headers)
            headers << utf8 (name) << ": " << utf8 (value) << "\r\n";

        // Shared with the abort, which may run on another thread after this returns.
        auto stream = std::make_shared<juce::WebInputStream> (url, hasBody);
        stream->withExtraHeaders (headers).withConnectionTimeout (request.timeoutMs).withNumRedirectsToFollow (5);

        if (hasBody)
            stream->withCustomRequestCommand (utf8 (method));

        if (! cancel.SetAbort ([stream] { stream->cancel(); }))
        {
            response.cancelled = true;
            response.error = "Cancelled";
            return response;
        }

        if (stream->connect (nullptr))
        {
            response.status = stream->getStatusCode();

            const auto received = stream->getResponseHeaders();

            for (const auto& key : received.getAllKeys())
                response.headers[key.toLowerCase().toStdString()] = received[key].toStdString();

            // In pieces, so a download cut short stops at the next one.
            std::vector<char> buffer (64 * 1024);

            for (;;)
            {
                const auto read = stream->read (buffer.data(), (int) buffer.size());

                if (read <= 0)
                    break;

                response.body.append (buffer.data(), (std::size_t) read);
            }
        }
        else
        {
            response.error = "Could not connect to " + url.getDomain().toStdString();
        }

        cancel.ClearAbort();

        if (cancel.IsCancelled())
        {
            response.status = 0;
            response.body.clear();
            response.cancelled = true;
            response.error = "Cancelled";
        }

        return response;
    }
};
} // namespace

std::shared_ptr<guitarfx::net::HttpTransport> makeJuceHttpTransport()
{
    return std::make_shared<JuceHttpTransport>();
}
} // namespace soundshed::nano
