#include "ui/UiSession.h"

#include "ui/Drawn.h"

#include <chrono>

namespace nanoq::ui
{
namespace
{
constexpr auto kPumpInterval = std::chrono::milliseconds(16);
}

UiSession::UiSession(Engine& engine, cycfi::elements::view& view)
    : mEngine(engine), mView(view), mClient([&engine](const std::string& json) { engine.SendRequest(json); })
{
    mEngine.SetViewAttached(true);

    // Tells the engine a UI is showing, which is what turns the meter and diagnostics feeds on.
    mEngine.SendRequest(R"({"type":"uiVisibility","visible":true})");

    guitarfx::uiclient::ToneService::Host host;
    host.post = [queue = std::weak_ptr<PostQueue>(mPosted)](std::function<void()> fn) {
        if (const auto q = queue.lock())
        {
            std::lock_guard<std::mutex> lock(q->mutex);
            q->pending.push_back(std::move(fn));
        }
    };
    host.toast = [this](const std::string& title, const std::string& detail, bool isError) {
        if (onToast)
            onToast(title, detail, isError);
    };
    mTones = std::make_unique<guitarfx::uiclient::ToneService>(mClient, mCommands, std::move(host));

    if (mDriver)
        mDriver->onSetting = [this](const std::string& key, const std::string& json) {
            const auto value = nlohmann::json::parse(json, nullptr, false);
            if (!value.is_discarded())
                mCommands.SetSetting(key, value);
        };

    mClient.Start();
    Schedule();
}

void UiSession::PostToUi(std::function<void()> fn)
{
    std::lock_guard<std::mutex> lock(mPosted->mutex);
    mPosted->pending.push_back(std::move(fn));
}

UiSession::~UiSession()
{
    *mAlive = false;
    mTones.reset();
    mEngine.SendRequest(R"({"type":"uiVisibility","visible":false})");
    mEngine.SetViewAttached(false);
}

double UiSession::Now()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

void UiSession::Schedule()
{
    // Elements' timers are one-shot: each tick schedules the next.
    mView.post(kPumpInterval, [this, alive = mAlive] {
        if (!*alive)
            return;
        Pump();
        if (*alive)
            Schedule();
    });
}

void UiSession::Pump()
{
    // The standalone's device requests run here, on the app's main thread, where clap-wrapper
    // drives its devices; their answers are collected just below.
    mEngine.Devices().Pump();

    for (auto& message : mEngine.TakeMessagesForView())
        mClient.Enqueue(std::move(message));

    mClient.Tick(Now());
    mClient.DrainPending();

    std::vector<std::function<void()>> posted;
    {
        std::lock_guard<std::mutex> lock(mPosted->mutex);
        posted.swap(mPosted->pending);
    }
    for (auto& fn : posted)
        fn();

    if (onTick)
        onTick();

    if (mDriver)
        mDriver->Poll(mView);
}

void UiSession::Repaint(cycfi::elements::element& element)
{
    // A widget that keeps its area is redrawn there, or not at all before its first draw: it is
    // new (and the rebuild that made it redraws its region) or not on screen. It is never
    // queued: Elements keeps a queued element as a pointer until the next poll, and a region
    // rebuilt before then (the looper strip, on its next state) would leave it dangling.
    if (const auto* drawn = dynamic_cast<const Drawn*>(&element))
    {
        if (drawn->DrawnArea())
            mView.refresh(*drawn->DrawnArea());
        return;
    }
    mView.refresh(element);
}
} // namespace nanoq::ui
