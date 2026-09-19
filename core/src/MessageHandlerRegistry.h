#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
/**
 * UI message types that a controller service answers itself.
 *
 * MessageDispatcher used to route every message to a PluginController::Handle* method,
 * so a feature with its own service still needed a forwarding method on the controller
 * for each message — declared in the controller's header, defined in a controller file,
 * and doing nothing but parse the payload and call the service. A service that owns a
 * feature outright now registers its messages here when it is constructed, and the
 * dispatcher tries this table before its own routes. The controller composes the service;
 * it no longer has to expose the feature's operations.
 *
 * Message thread only: filled while the controller is constructed, read by the dispatcher.
 */
class MessageHandlerRegistry
{
  public:
    using Handler = std::function<void(const nlohmann::json&)>;

    /// Registers `handler` for `type`. A type has at most one handler: registering one twice
    /// keeps the first and returns false, since two owners for one message is a wiring bug.
    bool Register(std::string type, Handler handler);

    /// Runs the handler registered for `type` with the whole message. False when there is
    /// none, so the caller can route the message elsewhere.
    bool Dispatch(const std::string& type, const nlohmann::json& message) const;

    [[nodiscard]] bool Handles(const std::string& type) const;

    /// Every registered type, sorted — for protocol audits and tests.
    [[nodiscard]] std::vector<std::string> Types() const;

  private:
    std::unordered_map<std::string, Handler> mHandlers;
};
} // namespace guitarfx
