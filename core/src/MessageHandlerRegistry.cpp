#include "MessageHandlerRegistry.h"

#include <algorithm>

namespace guitarfx
{
bool MessageHandlerRegistry::Register(std::string type, Handler handler)
{
    if (type.empty() || !handler)
    {
        return false;
    }

    return mHandlers.emplace(std::move(type), std::move(handler)).second;
}

bool MessageHandlerRegistry::Dispatch(const std::string& type, const nlohmann::json& message) const
{
    const auto it = mHandlers.find(type);

    if (it == mHandlers.end())
    {
        return false;
    }

    it->second(message);
    return true;
}

bool MessageHandlerRegistry::Handles(const std::string& type) const
{
    return mHandlers.count(type) != 0;
}

std::vector<std::string> MessageHandlerRegistry::Types() const
{
    std::vector<std::string> types;
    types.reserve(mHandlers.size());

    for (const auto& entry : mHandlers)
    {
        types.push_back(entry.first);
    }

    std::sort(types.begin(), types.end());
    return types;
}
} // namespace guitarfx
