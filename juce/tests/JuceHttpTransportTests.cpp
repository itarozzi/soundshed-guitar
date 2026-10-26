// Nano's HTTP transport on juce::URL (source/nativeui/tones/JuceHttpTransport.h), run as the
// shared ToneService runs it: under core's net::HttpWorkers.
//
// A request to a service that took the connection and then said nothing finished only when JUCE's
// timeouts did, 15 s or more each: quitting with the Tones page waiting on such a network hung the
// app for that long. Destroying the workers cancels the token, the transport cancels its stream,
// and the request returns at once. The transport also has to carry what ToneService sends and
// reads: a method, headers and a JSON body out; the status, lower-cased headers and body back.

#include "nativeui/tones/JuceHttpTransport.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace
{
    int failures = 0;

    void fail (const juce::String& message)
    {
        std::cerr << "[JuceHttpTransportTests] " << message << std::endl;
        ++failures;
    }

    /// Takes one connection on loopback. Silent, it holds it open without answering, as a
    /// black-holed network does; otherwise it reads the request and answers `reply`.
    class OneShotServer
    {
    public:
        explicit OneShotServer (std::string replyIn = {}) : reply (std::move (replyIn)) {}

        bool start()
        {
            for (int attempt = 0; attempt < 20 && port == 0; ++attempt)
            {
                const int candidate = 20000 + juce::Random::getSystemRandom().nextInt (40000);

                if (listener.createListener (candidate, "127.0.0.1"))
                    port = candidate;
            }

            if (port == 0)
                return false;

            thread = std::thread ([this] {
                client.reset (listener.waitForNextConnection());
                accepted = client != nullptr;

                if (client == nullptr || reply.empty())
                    return;

                // The headers, then as much body as Content-Length says.
                char buffer[4096];

                while (client->waitUntilReady (true, 2000) == 1)
                {
                    const int read = client->read (buffer, (int) sizeof (buffer), false);

                    if (read <= 0)
                        break;

                    request.append (buffer, (std::size_t) read);
                    const auto end = request.find ("\r\n\r\n");

                    if (end == std::string::npos)
                        continue;

                    const auto lower = juce::String (request.substr (0, end)).toLowerCase();
                    const int length = lower.fromFirstOccurrenceOf ("content-length:", false, false).upToFirstOccurrenceOf ("\r\n", false, false).trim().getIntValue();

                    if (request.size() >= end + 4 + (std::size_t) length)
                        break;
                }

                client->write (reply.data(), (int) reply.size());
            });
            return true;
        }

        ~OneShotServer() { stop(); }

        /// Closes the listener and waits for the connection's thread; `request` is complete after.
        void stop()
        {
            listener.close();

            if (thread.joinable())
                thread.join();
        }

        int port = 0;
        std::atomic<bool> accepted { false };
        std::string request; // read after stop()

    private:
        std::string reply;
        juce::StreamingSocket listener;
        std::unique_ptr<juce::StreamingSocket> client;
        std::thread thread;
    };

    using Clock = std::chrono::steady_clock;

    double secondsSince (Clock::time_point start)
    {
        return std::chrono::duration<double> (Clock::now() - start).count();
    }

    void testCutShortOnDestroy()
    {
        OneShotServer server;

        if (! server.start())
        {
            fail ("could not listen on a loopback port");
            return;
        }

        std::atomic<bool> jobFinished { false };
        guitarfx::net::HttpResponse response;
        Clock::time_point destroyStarted;

        {
            guitarfx::net::HttpWorkers workers (soundshed::nano::makeJuceHttpTransport(), 1);

            workers.Run ([&] {
                guitarfx::net::HttpRequest request;
                request.url = "http://127.0.0.1:" + std::to_string (server.port) + "/items?page=1";
                request.headers.emplace_back ("Accept", "application/json");
                response = workers.Perform (request);
                jobFinished = true;
            });

            // The request is in flight once the server has taken the connection.
            const auto waitStarted = Clock::now();

            while (! server.accepted && secondsSince (waitStarted) < 5.0)
                std::this_thread::sleep_for (std::chrono::milliseconds (10));

            if (! server.accepted)
                fail ("the request never reached the server");

            if (jobFinished)
                fail ("the request finished although the server never answered");

            // The player quits with the request still waiting: the workers go.
            destroyStarted = Clock::now();
        }

        const auto took = secondsSince (destroyStarted);

        if (took > 3.0)
            fail ("destroying the workers took " + juce::String (took, 1) + " s: the request in flight was not cut short");

        if (! jobFinished)
            fail ("the job was still running after the workers had gone");

        if (! response.cancelled || response.status != 0)
            fail ("a request cut short answers status 0, cancelled");

        std::cout << "[JuceHttpTransportTests] cut short: destroyed in " << took << " s" << std::endl;
    }

    void testRoundTrip()
    {
        OneShotServer server ("HTTP/1.1 201 Created\r\nX-Session-Id: abc123\r\nContent-Type: application/json\r\nContent-Length: 11\r\n"
                              "Connection: close\r\n\r\n{\"ok\":true}");

        if (! server.start())
        {
            fail ("could not listen on a loopback port");
            return;
        }

        guitarfx::net::HttpResponse response;

        {
            guitarfx::net::HttpWorkers workers (soundshed::nano::makeJuceHttpTransport(), 1);
            std::atomic<bool> done { false };

            workers.Run ([&] {
                guitarfx::net::HttpRequest request;
                request.method = "POST";
                request.url = "http://127.0.0.1:" + std::to_string (server.port) + "/auth/start";
                request.headers.emplace_back ("Content-Type", "application/json");
                request.headers.emplace_back ("x-session-id", "s-1");
                request.body = "{\"email\":\"a@b.c\"}";
                response = workers.Perform (request);
                done = true;
            });

            const auto waitStarted = Clock::now();

            while (! done && secondsSince (waitStarted) < 10.0)
                std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }

        server.stop();
        const auto sent = juce::String (server.request);

        if (response.status != 201)
            fail ("expected status 201, got " + juce::String (response.status) + " (" + juce::String (response.error) + ")");

        if (response.body != "{\"ok\":true}")
            fail ("expected the reply's body, got \"" + juce::String (response.body) + "\"");

        if (response.Header ("x-session-id") != "abc123")
            fail ("the reply's x-session-id did not come back under its lower-case name");

        if (! sent.startsWith ("POST /auth/start"))
            fail ("the request was not a POST to /auth/start: " + sent.upToFirstOccurrenceOf ("\r\n", false, false));

        if (! sent.containsIgnoreCase ("x-session-id: s-1"))
            fail ("the request's x-session-id header did not reach the server");

        if (! sent.endsWith ("{\"email\":\"a@b.c\"}"))
            fail ("the request's body did not reach the server");
    }
}

int main()
{
    testCutShortOnDestroy();
    testRoundTrip();

    if (failures == 0)
        std::cout << "[JuceHttpTransportTests] passed" << std::endl;

    return failures == 0 ? 0 : 1;
}
