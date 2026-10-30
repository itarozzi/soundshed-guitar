#include "controller/ScaleGuideMessages.h"

#include "MessageHandlerRegistry.h"
#include "music/ScaleGuideView.h"

#include <nlohmann/json.hpp>

namespace guitarfx
{
void RegisterScaleGuideMessages(MessageHandlerRegistry& registry, std::function<void(const std::string&)> sendToUI)
{
    registry.Register("scaleGuide", [send = std::move(sendToUI)](const nlohmann::json& payload) {
        const std::string action = payload.value("action", "view");
        nlohmann::json message;

        if (action == "catalog")
        {
            message = music::ScaleGuideCatalogJson();
            message["type"] = "scaleGuideCatalog";
        }
        else
        {
            const auto request = payload.find("request");
            message["view"] = music::BuildScaleGuideView(
                music::ParseScaleGuideRequest(request != payload.end() ? *request : nlohmann::json::object()));
            message["type"] = "scaleGuideView";
        }

        if (const auto id = payload.find("requestId"); id != payload.end() && id->is_number_integer())
        {
            message["requestId"] = *id;
        }

        send(message.dump());
    });
}
} // namespace guitarfx
