#include "arkui_cef/ArkUICefBrowser.h"
#include "arkui_cef/ArkUICefClient.h"
#include "arkui_cef/NodeHandle.h"

#include <hilog/log.h>

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;
}
namespace arkui_cef {

ArkUICefBrowser::ArkUICefBrowser(const int64_t id, int width, int height)
    : id_(id),
      width_(width),
      height_(height),
      browserCreated_(false),
      browserClosing_(false),
      client_(nullptr) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "constructor: id=%{public}ld, width=%{public}d, height=%{public}d",
        id_, width_, height_);
}

ArkUICefBrowser::~ArkUICefBrowser() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "destructor: id=%{public}ld", id_);
    
    if (browserCreated_ && !browserClosing_) {
        close();
    }
}

void ArkUICefBrowser::createBrowser(CefRefPtr<ArkUICefClient> client, Node* parentNode, const std::string& url) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser", 
        "createBrowser: url=%{public}s", url.c_str());
    
    if (browserCreated_) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
            "createBrowser: browser already created");
        return;
    }
    
    client_ = client;

    if (loadCallback_) {
        client_->SetLoadCallback(loadCallback_);
    }
    if (titleCallback_) {
        client_->SetTitleCallback(titleCallback_);
    }
    if (focusTakeCallback_) {
        client_->SetFocusTakeCallback(focusTakeCallback_);
    }
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser", 
        "createBrowser: parentStack: %{public}p", parentNode->container);
    CefWindowInfo windowInfo;
    windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;
    windowInfo.SetAsChild(
        (CefWindowHandle)parentNode,
        CefRect(0, 0, width_, height_));
    
    CefBrowserSettings browserSettings;
    
    bool success = CefBrowserHost::CreateBrowser(
        windowInfo, client_, url, browserSettings, nullptr, nullptr);
    
    if (!success) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
            "createBrowser: CefBrowserHost::CreateBrowser failed");
        client_ = nullptr;
    } else {
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
            "createBrowser: CreateBrowser called successfully");
    }
    
    return;
}

void ArkUICefBrowser::close() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "close: id=%{public}ld, browserCreated_=%{public}d", id_, browserCreated_);
    
    if (!browserCreated_) {
        return;
    }
    
    browserClosing_ = true;
    if (browser_) {
        browser_->GetHost()->CloseBrowser(true);
    }
}

void ArkUICefBrowser::loadUrl(const std::string& url) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "loadUrl: id=%{public}ld, url=%{public}s", id_, url.c_str());
    
    if (!browser_ || !browser_->GetMainFrame()) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
            "loadUrl: browser not ready");
        return;
    }
    
    browser_->GetMainFrame()->LoadURL(url);
}

void ArkUICefBrowser::goBack() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "goBack: id=%{public}ld", id_);
    
    if (!browser_ || !browser_->CanGoBack()) {
        return;
    }
    
    browser_->GoBack();
}

void ArkUICefBrowser::goForward() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "goForward: id=%{public}ld", id_);
    
    if (!browser_ || !browser_->CanGoForward()) {
        return;
    }
    
    browser_->GoForward();
}

void ArkUICefBrowser::reload() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "reload: id=%{public}ld", id_);
    
    if (!browser_) {
        return;
    }
    
    browser_->Reload();
}

void ArkUICefBrowser::resize(int width, int height) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "resize: id=%{public}ld, width=%{public}d, height=%{public}d",
        id_, width, height);
    
    width_ = width;
    height_ = height;
    
    if (!browser_) {
        return;
    }
    
    browser_->GetHost()->WasResized();
}

void ArkUICefBrowser::show() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "show: id=%{public}ld", id_);
    Node* node = getWindowHandle();
    if (node)
        arkui_cef::show(node->container);
}

void ArkUICefBrowser::hide() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "hide: id=%{public}ld", id_);
    Node* node = getWindowHandle();
    if (node)
        arkui_cef::hide(node->container);
}

void ArkUICefBrowser::sendKeyEvent(const CefKeyEvent& event) {
    if (!browser_) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
            "sendKeyEvent: browser not ready, id=%{public}ld", id_);
        return;
    }
    browser_->GetHost()->SendKeyEvent(event);
}

ArkUICefBrowser::Node* ArkUICefBrowser::getWindowHandle() const {
    if (!browser_) {
        return nullptr;
    }
    return reinterpret_cast<Node*>(browser_->GetHost()->GetWindowHandle());
}

void ArkUICefBrowser::onBrowserCreated(CefRefPtr<CefBrowser> browser) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "onBrowserCreated: id=%{public}ld, identifier=%{public}d", id_, browser->GetIdentifier());
    browser_ = browser;
    browserCreated_ = true;
    if (createCallback_) {
        createCallback_(id_);
    }
}

void ArkUICefBrowser::onBrowserClosed() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "onBrowserClosed: id=%{public}ld", id_);
    
    browser_ = nullptr;
    browserCreated_ = false;
}
} // namespace arkui_cef