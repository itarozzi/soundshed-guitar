#pragma once

#include "uiclient/ToneService.h"

#include <juce_graphics/juce_graphics.h>

#include <map>
#include <string>

namespace soundshed::nano
{
/// Tone sharing pictures as juce::Images: ToneService fetches the bytes, this decodes each once.
class ToneImages
{
public:
    explicit ToneImages (guitarfx::uiclient::ToneService& serviceIn) : service (serviceIn) {}

    /// The picture at `url`, or an invalid image while it is fetched (the service's listeners
    /// hear when it arrives) or when it could not be had. PNG and JPEG; JUCE reads no WEBP, and
    /// such a pack simply shows its icon.
    [[nodiscard]] juce::Image get (const std::string& url)
    {
        if (url.empty())
            return {};

        if (const auto it = decoded.find (url); it != decoded.end())
            return it->second;

        const auto* bytes = service.Image (url);

        if (bytes == nullptr)
            return {};

        auto picture = bytes->empty() ? juce::Image() : juce::ImageFileFormat::loadFrom (bytes->data(), bytes->size());
        decoded[url] = picture;
        return picture;
    }

private:
    guitarfx::uiclient::ToneService& service;
    std::map<std::string, juce::Image> decoded;
};
} // namespace soundshed::nano
