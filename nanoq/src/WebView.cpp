#include "WebView.h"

#include "Log.h"

#if defined(_WIN32) && defined(NANOQ_WEBVIEW2)
#include <windows.h>
#include <wrl.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <algorithm>
#include <cstdio>
#endif

namespace nanoq
{
#if defined(_WIN32) && defined(NANOQ_WEBVIEW2)
namespace
{
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

std::wstring Wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), n);
    return wide;
}

std::string Hex(HRESULT hr)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
    return text;
}

std::string Narrow(const wchar_t* text)
{
    if (text == nullptr || *text == 0)
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow.data(), n, nullptr, nullptr);
    return narrow;
}

/// A WebView2 URI out-parameter, freed.
template <typename Args>
std::string UriOf(Args* args)
{
    LPWSTR uri = nullptr;
    if (FAILED(args->get_Uri(&uri)))
        return {};
    auto text = Narrow(uri);
    CoTaskMemFree(uri);
    return text;
}

class WebView2Host final : public WebView
{
public:
    WebView2Host(HWND parent, Options options) : mParent(parent), mOptions(std::move(options))
    {
        // The view is a child window: Elements must not paint over it.
        SetWindowLongPtrW(mParent, GWL_STYLE, GetWindowLongPtrW(mParent, GWL_STYLE) | WS_CLIPCHILDREN);

        // The host's UI thread is normally STA already; a plugin cannot assume so.
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        mUninitialize = co == S_OK || co == S_FALSE;

        auto options2 = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        // The player starts on a click in Nano Q, which the page never sees as a gesture of its own.
        options2->put_AdditionalBrowserArguments(L"--autoplay-policy=no-user-gesture-required");

        std::error_code ec;
        std::filesystem::create_directories(mOptions.dataFolder, ec);
        auto alive = mAlive;
        const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, mOptions.dataFolder.wstring().c_str(), options2.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, alive](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                    if (!*alive)
                        return S_OK;
                    if (FAILED(result) || environment == nullptr)
                    {
                        Log("WebView2 environment failed: " + Hex(result));
                        mFailed = true;
                        return S_OK;
                    }
                    environment->CreateCoreWebView2Controller(
                        mParent, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                     [this, alive](HRESULT made, ICoreWebView2Controller* controller) -> HRESULT {
                                         if (!*alive)
                                         {
                                             if (controller != nullptr)
                                                 controller->Close();
                                             return S_OK;
                                         }
                                         if (FAILED(made) || controller == nullptr)
                                         {
                                             Log("WebView2 controller failed: " + Hex(made));
                                             mFailed = true;
                                             return S_OK;
                                         }
                                         Ready(controller);
                                         return S_OK;
                                     })
                                     .Get());
                    return S_OK;
                })
                .Get());
        if (FAILED(hr))
        {
            Log("WebView2 unavailable: " + Hex(hr));
            mFailed = true;
        }
    }

    ~WebView2Host() override
    {
        *mAlive = false;
        if (mController)
            mController->Close();
        mWebView.Reset();
        mController.Reset();
        if (mUninitialize)
            CoUninitialize();
    }

    void Navigate(const std::string& url) override
    {
        if (url == mUrl)
            return;
        mUrl = url;
        if (mWebView)
            mWebView->Navigate(Wide(url).c_str());
    }

    void Place(int x, int y, int width, int height) override
    {
        const RECT bounds{x, y, x + std::max(0, width), y + std::max(0, height)};
        if (EqualRect(&bounds, &mBounds))
            return;
        mBounds = bounds;
        if (mController)
            mController->put_Bounds(mBounds);
    }

    void Show(bool visible) override
    {
        if (visible == mVisible)
            return;
        mVisible = visible;
        if (mController)
            mController->put_IsVisible(visible ? TRUE : FALSE);
    }

    bool PlayingAudio() const override
    {
        ComPtr<ICoreWebView2_8> view8;
        BOOL playing = FALSE;
        if (mWebView && SUCCEEDED(mWebView.As(&view8)) && SUCCEEDED(view8->get_IsDocumentPlayingAudio(&playing)))
            return playing == TRUE;
        return false;
    }

    bool Failed() const override
    {
        return mFailed;
    }

private:
    void Ready(ICoreWebView2Controller* controller)
    {
        mController = controller;
        mController->get_CoreWebView2(&mWebView);
        if (!mWebView)
        {
            mFailed = true;
            return;
        }

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(mWebView->get_Settings(&settings)))
        {
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
        }
        // Dark behind the page while it loads, as the cards around it.
        ComPtr<ICoreWebView2Controller2> controller2;
        if (SUCCEEDED(mController.As(&controller2)))
            controller2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0, 0, 0});

        // The page itself may move on within what is allowed; anything else, and every new
        // window, goes to the browser.
        mWebView->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                             [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                                 const auto uri = UriOf(args);
                                                 if (mOptions.allowNavigation && !mOptions.allowNavigation(uri))
                                                 {
                                                     args->put_Cancel(TRUE);
                                                     if (mOptions.openExternal && (uri.rfind("https://", 0) == 0 || uri.rfind("http://", 0) == 0))
                                                         mOptions.openExternal(uri);
                                                 }
                                                 return S_OK;
                                             })
                                             .Get(),
                                         nullptr);
        mWebView->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                             [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                                                 args->put_Handled(TRUE);
                                                 const auto uri = UriOf(args);
                                                 if (mOptions.openExternal && (uri.rfind("https://", 0) == 0 || uri.rfind("http://", 0) == 0))
                                                     mOptions.openExternal(uri);
                                                 return S_OK;
                                             })
                                             .Get(),
                                         nullptr);

        // A page that fails to load says why in the log (no network, a blocked embed).
        mWebView->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                              [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                                                  BOOL ok = FALSE;
                                                  args->get_IsSuccess(&ok);
                                                  if (!ok)
                                                  {
                                                      COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                                      args->get_WebErrorStatus(&status);
                                                      Log("WebView could not load " + mUrl + ": error " + std::to_string(static_cast<int>(status)));
                                                  }
                                                  return S_OK;
                                              })
                                              .Get(),
                                          nullptr);

        mController->put_Bounds(mBounds);
        mController->put_IsVisible(mVisible ? TRUE : FALSE);
        if (!mUrl.empty())
            mWebView->Navigate(Wide(mUrl).c_str());
    }

    HWND mParent;
    Options mOptions;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    ComPtr<ICoreWebView2Controller> mController;
    ComPtr<ICoreWebView2> mWebView;
    std::string mUrl;
    RECT mBounds{0, 0, 0, 0};
    bool mVisible = false;
    bool mFailed = false;
    bool mUninitialize = false;
};
} // namespace

std::unique_ptr<WebView> WebView::Create(void* parentWindow, Options options)
{
    if (parentWindow == nullptr)
        return nullptr;
    // No runtime installed: say so now rather than after a failed start.
    LPWSTR version = nullptr;
    if (FAILED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) || version == nullptr)
    {
        Log("WebView2 Runtime not installed: the Jam tab's pages open in the browser");
        return nullptr;
    }
    CoTaskMemFree(version);
    return std::make_unique<WebView2Host>(static_cast<HWND>(parentWindow), std::move(options));
}

float WebView::PixelScale(void* window)
{
    return window != nullptr ? static_cast<float>(GetDpiForWindow(static_cast<HWND>(window))) / 96.0f : 1.0f;
}
#else
std::unique_ptr<WebView> WebView::Create(void*, Options)
{
    return nullptr;
}

float WebView::PixelScale(void*)
{
    return 1.0f;
}
#endif
} // namespace nanoq
