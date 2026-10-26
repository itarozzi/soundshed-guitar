#pragma once

#include "net/Http.h"

#include <memory>

namespace soundshed::nano
{
/// The Nano's HTTP: guitarfx::net::HttpTransport on juce::URL, so the shared ToneService has a
/// network wherever JUCE does (Android included, where core's own fallback transport has none).
///
/// A request the CancelToken cancels returns at once: the stream is cancelled from the other
/// thread rather than left to JUCE's timeouts (15 s to connect, as long again for the reply), so
/// quitting with the Tones page waiting on a black-holed network does not hang the app.
[[nodiscard]] std::shared_ptr<guitarfx::net::HttpTransport> makeJuceHttpTransport();
} // namespace soundshed::nano
