#include "arkui_cef/ArkUICefBrowser.h"
#include "arkui_cef/ArkUICefClient.h"
#include "arkui_cef/ArkUICefRenderHandler.h"
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
    if (cursorChangeCallback_) {
        client_->SetCursorChangeCallback(cursorChangeCallback_);
    }
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "createBrowser: parentStack: %{public}p, use_osr=%{public}d", parentNode ? parentNode->container : nullptr, use_osr_);
    CefWindowInfo windowInfo;
    windowInfo.runtime_style = CEF_RUNTIME_STYLE_ALLOY;

    if (use_osr_) {
        windowInfo.SetAsWindowless(0);
    } else {
        windowInfo.SetAsChild(
            (CefWindowHandle)parentNode,
            CefRect(0, 0, width_, height_));
    }
    
    CefBrowserSettings browserSettings;
    // Limit OSR frame rate to reduce main-thread load from PixelMap creation.
    // Each frame requires a 10MB+ deep copy + ArrayBuffer allocation + createPixelMap on the
    // ArkTS main thread. At 30fps this floods the event queue and causes APP_INPUT_BLOCK.
    // 10fps is sufficient for interactive web content and prevents freeze.
    if (use_osr_) {
        browserSettings.windowless_frame_rate = 10;
    }

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

    // Update the render handler size and notify CEF of the change
    if (use_osr_ && client_) {
        auto handler = client_->GetArkUICefRenderHandler();
        if (handler) {
            handler->SetSize(width, height);
        }
    }

    browser_->GetHost()->WasResized();
}

void ArkUICefBrowser::setOsrScale(float factor) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "setOsrScale: id=%{public}ld, factor=%{public}f", id_, factor);

    if (use_osr_ && client_) {
        auto handler = client_->GetArkUICefRenderHandler();
        if (handler) {
            handler->SetDeviceScaleFactor(factor);
        }
    }

    if (browser_) {
        browser_->GetHost()->NotifyScreenInfoChanged();
    }
}

void ArkUICefBrowser::setComponentOffset(int x, int y) {
    if (use_osr_ && client_) {
        auto handler = client_->GetArkUICefRenderHandler();
        if (handler) {
            handler->SetComponentOffset(x, y);
        }
    }
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

void ArkUICefBrowser::sendMouseClickEvent(int x, int y, int button, bool mouseUp, int clickCount, uint32_t modifiers) {
    if (!browser_) {
        return;
    }
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    browser_->GetHost()->SendMouseClickEvent(event,
        static_cast<cef_mouse_button_type_t>(button), mouseUp, clickCount);
}

void ArkUICefBrowser::sendMouseMoveEvent(int x, int y, bool mouseLeave, uint32_t modifiers) {
    if (!browser_) {
        return;
    }
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    browser_->GetHost()->SendMouseMoveEvent(event, mouseLeave);
}

void ArkUICefBrowser::sendMouseWheelEvent(int x, int y, int deltaX, int deltaY) {
    if (!browser_) {
        return;
    }
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = 0;
    browser_->GetHost()->SendMouseWheelEvent(event, deltaX, deltaY);
}

void ArkUICefBrowser::sendFocusEvent(bool focused) {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->SetFocus(focused);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "sendFocusEvent: id=%{public}ld, focused=%{public}d", id_, focused);
}

void ArkUICefBrowser::sendCaptureLostEvent() {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->SendCaptureLostEvent();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "sendCaptureLostEvent: id=%{public}ld", id_);
}

void ArkUICefBrowser::invalidate() {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->Invalidate(PET_VIEW);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "invalidate: id=%{public}ld", id_);
}

void ArkUICefBrowser::setWindowlessVisibility(bool visible) {
    if (!browser_) {
        return;
    }
    // SetWindowlessVisibility not available in CEF 114; use SetWindowlessFrameRate to control rendering
    browser_->GetHost()->SetWindowlessFrameRate(visible ? 30 : 0);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "setWindowlessVisibility: id=%{public}ld, visible=%{public}d (via frame rate)", id_, visible);
}

void ArkUICefBrowser::sendTouchEvent(int x, int y, int touchType, int pointerId) {
    if (!browser_) {
        return;
    }
    CefTouchEvent event;
    event.id = pointerId;
    event.x = x;
    event.y = y;
    event.radius_x = 0.0f;
    event.radius_y = 0.0f;
    event.rotation_angle = 0.0f;
    event.pressure = 1.0f;
    event.type = static_cast<cef_touch_event_type_t>(touchType);
    event.modifiers = 0;
    browser_->GetHost()->SendTouchEvent(event);
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