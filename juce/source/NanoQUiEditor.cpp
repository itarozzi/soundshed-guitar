#include "NanoQUiEditor.h"

#include "ui/EmbeddedUi.h"
#include "ui/UiLink.h"

#include <mutex>
#include <string>
#include <vector>

// ── The engine's end ────────────────────────────────────────────────────────

/// Nano Q's UI on the processor's WebView bridge: requests go in as the page's messages do,
/// and what the controller sends the page is kept for the UI's next pump.
class NanoQUiEditor::Link final : public nanoq::ui::UiLink
{
  public:
    explicit Link(PluginProcessorAdapter& processor) : mProcessor(processor)
    {
        // The processor calls this on the message thread, which is also the UI's.
        mProcessor.setWebMessageCallback([this](const juce::String& message) {
            std::scoped_lock lock(mMutex);

            if (mAttached && mOutbox.size() < kMaxOutbox)
            {
                mOutbox.push_back(message.toStdString());
            }
        });
    }

    ~Link() override
    {
        mProcessor.setWebMessageCallback(nullptr);
    }

    void SetViewAttached(bool attached) override
    {
        std::scoped_lock lock(mMutex);
        mAttached = attached;

        if (!attached)
        {
            mOutbox.clear();
        }
    }

    std::vector<std::string> TakeMessagesForView() override
    {
        std::vector<std::string> out;
        std::scoped_lock lock(mMutex);
        out.swap(mOutbox);
        return out;
    }

    void SendRequest(std::string json) override
    {
        // Handled after the UI event that sent it, as a WebView message is, and dropped if the
        // processor has gone by then.
        juce::MessageManager::callAsync(
            [processor = juce::WeakReference<PluginProcessorAdapter>(&mProcessor), json = std::move(json)] {
                if (auto* p = processor.get())
                {
                    p->handleWebMessage(juce::String(json));
                }
            });
    }

    std::filesystem::path AssetRoot() const override
    {
        return mProcessor.GetBundledAssetsPath();
    }

    std::optional<std::filesystem::path> ResolveLayoutImageFile(const std::string& imageId) override
    {
        return mProcessor.getController().ResolveLayoutImageFile(imageId);
    }

  private:
    static constexpr size_t kMaxOutbox = 4096; // as Nano Q's engine

    PluginProcessorAdapter& mProcessor;
    std::mutex mMutex;
    std::vector<std::string> mOutbox;
    bool mAttached = false;
};

// ── Where the child window goes ─────────────────────────────────────────────

/// What juce::HWNDComponent watches, for a window HWNDComponent cannot own: it destroys the
/// window it is given when it lets go, and the Elements view destroys its own.
class NanoQUiEditor::Placement final : public juce::ComponentMovementWatcher
{
  public:
    explicit Placement(NanoQUiEditor& editor) : juce::ComponentMovementWatcher(&editor), mEditor(editor)
    {
    }

    using juce::ComponentMovementWatcher::componentMovedOrResized;
    using juce::ComponentMovementWatcher::componentVisibilityChanged;

    void componentMovedOrResized(bool, bool) override
    {
        mEditor.placeChildWindow();
    }

    void componentPeerChanged() override
    {
        mEditor.attachToPeer();
    }

    void componentVisibilityChanged() override
    {
        mEditor.attachToPeer();
    }

  private:
    NanoQUiEditor& mEditor;
};

// ── The editor ──────────────────────────────────────────────────────────────

NanoQUiEditor::NanoQUiEditor(PluginProcessorAdapter& processor)
    : SoundshedEditorBase(processor, SizeLimits{640, 400, 8192, 8192, 1280, 800}),
      mLink(std::make_unique<Link>(processor))
{
    setOpaque(true);
    applyInitialEditorSize();
    mPlacement = std::make_unique<Placement>(*this);

    if (isShowing())
    {
        attachToPeer();
    }
}

NanoQUiEditor::~NanoQUiEditor()
{
    mPlacement.reset();
    mUi.reset();
}

void NanoQUiEditor::paint(juce::Graphics& g)
{
    // Only ever seen for the moment before the child window covers it.
    g.fillAll(juce::Colour(0xff1c1d21));
}

void NanoQUiEditor::resized()
{
    // The child window follows through Placement, which hears of the move as well.
    rememberEditorSize();
}

void NanoQUiEditor::attachToPeer()
{
    auto* peer = getPeer();

    if (peer != mPeer)
    {
        mUi.reset();
        mPeer = peer;

        if (mPeer != nullptr)
        {
            mUi = std::make_unique<nanoq::ui::EmbeddedUi>(*mLink, mPeer->getNativeHandle());
        }
    }

    placeChildWindow();

    if (mUi != nullptr)
    {
        // Nothing async to wait for: the view draws on its first paint.
        notifyReadyToShow();
    }
}

void NanoQUiEditor::placeChildWindow()
{
    if (mUi == nullptr || mPeer == nullptr)
    {
        return;
    }

    const auto area =
        (mPeer->getAreaCoveredBy(*this).toFloat() * mPeer->getPlatformScaleFactor()).getSmallestIntegerContainer();
    mUi->Place(area.getX(), area.getY(), area.getWidth(), area.getHeight(), isShowing());
}
