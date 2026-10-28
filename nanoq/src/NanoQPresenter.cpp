#include "NanoQPresenter.h"

#include "Log.h"
#include "WindowChrome.h"
#include "ui/UiRoot.h"

#include <elements.hpp>

namespace nanoq
{
NanoQPresenter::NanoQPresenter(NanoQController& ctl) : q_plug::presenter(ctl), mCtl(ctl)
{
}

NanoQPresenter::~NanoQPresenter() = default;

void NanoQPresenter::on_attach(cycfi::elements::view& view)
{
    ui::UiRoot::Options options;
    options.getZoom = [this] { return zoom(); };
    options.setZoom = [this](float scale) { zoom(scale); };
    mRoot = std::make_unique<ui::UiRoot>(mCtl.engine(), view, std::move(options));
    MakeStandaloneWindowResizable(static_cast<void*>(view.host()));

    const auto size = view.size();
    const auto limits = view.limits();
    Log("view attached: size " + std::to_string(size.x) + "x" + std::to_string(size.y) + ", scale " +
        std::to_string(view.scale()) + ", limits min " + std::to_string(limits.min.x) + "x" +
        std::to_string(limits.min.y) + " max " + std::to_string(limits.max.x) + "x" + std::to_string(limits.max.y));
}

void NanoQPresenter::on_detach()
{
    mRoot.reset();
}
} // namespace nanoq
