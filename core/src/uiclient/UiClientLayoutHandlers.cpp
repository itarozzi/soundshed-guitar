/**
 * UiClientLayoutHandlers.cpp - The custom effect layout library and its images
 * (Topic::EffectLayouts). The engine sends the library on uiReady and after any change to it;
 * the image list comes only when asked for, as the web UI asks the first time a layout draws.
 */

#include "uiclient/EffectLayouts.h"
#include "uiclient/UiClient.h"

namespace guitarfx::uiclient
{
void UiClient::RegisterLayoutHandlers()
{
    On("layoutLibraryLoaded", [this](const nlohmann::json& m) {
        auto& layouts = mState.layouts;
        layouts.library = ParseLayoutLibrary(m.value("layoutLibrary", nlohmann::json::object()));
        layouts.loaded = true;

        // A reloaded library may draw on images saved since the list came: ask again when it
        // next draws.
        if (layouts.imagesLoaded)
        {
            layouts.imagesRequested = false;
        }

        ++layouts.revision;
        Notify(Topic::EffectLayouts);
    });

    On("layoutImagesLoaded", [this](const nlohmann::json& m) {
        auto& layouts = mState.layouts;
        layouts.images.clear();

        if (const auto it = m.find("images"); it != m.end() && it->is_array())
        {
            for (const auto& raw : *it)
            {
                auto image = ParseLayoutImageRef(raw);

                if (!image.imageId.empty())
                {
                    layouts.images[image.imageId] = std::move(image);
                }
            }
        }

        layouts.imagesLoaded = true;
        ++layouts.revision;
        Notify(Topic::EffectLayouts);
    });
}

void UiClient::RequestLayoutImages()
{
    if (mState.layouts.imagesRequested)
    {
        return;
    }

    mState.layouts.imagesRequested = true;
    Send("requestLayoutImages");
}
} // namespace guitarfx::uiclient
