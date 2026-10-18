// HTTP requests cut short when their workers go (source/nativeui/tones/HttpWorkers.h).
//
// The Tones page's requests run on a small pool. Destroying it waited for them to finish, and a
// request to a service that took the connection and then said nothing finished only when JUCE's
// timeouts did, 15 s or more each: quitting with the Tones page waiting on such a network hung the
// app for that long. The workers now cancel what is in flight, and the request returns at once.

#include "nativeui/tones/HttpWorkers.h"

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
        std::cerr << "[HttpWorkersTests] " << message << std::endl;
        ++failures;
    }

    /// Takes one connection and holds it open without answering, as a black-holed network does.
    class SilentServer
    {
    public:
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
            });
            return true;
        }

        ~SilentServer()
        {
            listener.close();

            if (thread.joinable())
                thread.join();
        }

        int port = 0;
        std::atomic<bool> accepted { false };

    private:
        juce::StreamingSocket listener;
        std::unique_ptr<juce::StreamingSocket> client;
        std::thread thread;
    };

    using Clock = std::chrono::steady_clock;

    double secondsSince (Clock::time_point start)
    {
        return std::chrono::duration<double> (Clock::now() - start).count();
    }
}

int main()
{
    SilentServer server;

    if (! server.start())
    {
        fail ("could not listen on a loopback port");
        return 1;
    }

    std::atomic<bool> jobFinished { false };
    soundshed::nano::HttpWorkers::Response response;
    Clock::time_point destroyStarted;

    {
        soundshed::nano::HttpWorkers workers (1);
        const juce::URL url ("http://127.0.0.1:" + juce::String (server.port) + "/items?page=1");

        workers.run ([&] {
            response = workers.perform (url, "GET", "Accept: application/json\r\n", 15000);
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

    if (failures == 0)
        std::cout << "[HttpWorkersTests] passed (destroyed in " << took << " s)" << std::endl;

    return failures == 0 ? 0 : 1;
}
