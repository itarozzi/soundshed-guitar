#pragma once

#include "NanoQController.h"

#include <q_plug/presenter.hpp>

#include <memory>

namespace nanoq
{
namespace ui
{
class UiRoot;
} // namespace ui

/// The editor. Exists only while the host has one open: attaching builds the UI (the engine
/// session and the shell, ui/UiRoot.h), detaching drops it, and the engine goes on running
/// without a view.
class NanoQPresenter final : public q_plug::presenter
{
  public:
    explicit NanoQPresenter(NanoQController& ctl);
    ~NanoQPresenter() override;

  protected:
    void on_attach(cycfi::elements::view& view) override;
    void on_detach() override;

  private:
    NanoQController& mCtl;
    std::unique_ptr<ui::UiRoot> mRoot;
};
} // namespace nanoq
