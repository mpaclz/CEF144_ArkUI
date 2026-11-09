#include "arkui_cef/ArkUICefBrowser.h"
#include "arkui_cef/ArkUICefClient.h"
#include "arkui_cef/ArkUICefRenderHandler.h"
#include "arkui_cef/NodeHandle.h"

#include <hilog/log.h>
#include "include/cef_drag_data.h"
#include <sstream>

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
    // Initial cap during startup; the ArkTS layer raises this after creation
    // (see OsrCefView onOsrCreated -> cefSetFrameRate). The render pipeline
    // is now synchronous dirty-region updates with native PixelMap writes
    // (no per-frame createPixelMap, no per-frame ArrayBuffer allocation), so
    // 60fps no longer floods the JS thread. CEF has no upper clamp on
    // windowless_frame_rate (osr_util::ClampFrameRate only enforces >= 1).
    if (use_osr_) {
        browserSettings.windowless_frame_rate = 60;
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

    // R2: force a full repaint at the new size. With OSR, WasResized() alone
    // does not guarantee a frame — a static page emits no new damage, so the
    // newly exposed buffer regions keep stale pixels until something else
    // triggers a repaint. Invalidate(PET_VIEW) requests a complete redraw.
    // Windowed (non-OSR) mode is untouched: the system composites its surface.
    if (use_osr_) {
        browser_->GetHost()->Invalidate(PET_VIEW);
    }
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
        browser_->GetHost()->NotifyScreenInfoChanged();    }
}

void ArkUICefBrowser::setComponentOffset(int x, int y) {
    if (use_osr_ && client_) {
        auto handler = client_->GetArkUICefRenderHandler();
        if (handler) {
            handler->SetComponentOffset(x, y);
        }
    }
}

void ArkUICefBrowser::setFrameRate(int frameRate) {
    if (!browser_ || frameRate < 1) {
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "setFrameRate: id=%{public}ld, rate=%{public}d", id_, frameRate);
    browser_->GetHost()->SetWindowlessFrameRate(frameRate);
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
    ApplyOsrPopupOffset(x, y);
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
    ApplyOsrPopupOffset(x, y);
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
    browser_->GetHost()->SendMouseMoveEvent(event, mouseLeave);
}

void ArkUICefBrowser::sendMouseWheelEvent(int x, int y, int deltaX, int deltaY, uint32_t modifiers) {
    if (!browser_) {
        return;
    }
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = modifiers;
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
    ApplyOsrPopupOffset(x, y);
    CefTouchEvent event;
    event.id = pointerId;
    event.x = static_cast<float>(x);
    event.y = static_cast<float>(y);
    event.radius_x = 0.0f;
    event.radius_y = 0.0f;
    event.rotation_angle = 0.0f;
    event.pressure = 1.0f;
    event.type = static_cast<cef_touch_event_type_t>(touchType);
    event.modifiers = 0;
    browser_->GetHost()->SendTouchEvent(event);
}

void ArkUICefBrowser::ApplyOsrPopupOffset(int& x, int& y) {
    // OSR popups may be repositioned for display (GetPopupRectInWebView).
    // Map the incoming view coordinate back onto CEF's original popup rect
    // so clicks land on the right option. No-op in windowed mode or when
    // no popup is visible.
    if (!use_osr_ || !client_) {
        return;
    }
    auto handler = client_->GetArkUICefRenderHandler();
    if (handler) {
        handler->ApplyPopupOffset(x, y);
    }
}

namespace {
// Counts UTF-16 code units in a UTF-8 string (surrogate pairs = 2 units).
// CEF composition ranges use UTF-16 offsets, while the text travels as UTF-8.
size_t Utf16Length(const std::string& utf8) {
    size_t units = 0;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        size_t advance = 1;
        if (c >= 0xF0) {
            advance = 4;  // > U+FFFF: surrogate pair → 2 code units
            units += 2;
        } else if (c >= 0xE0) {
            advance = 3;
            units += 1;
        } else if (c >= 0xC0) {
            advance = 2;
            units += 1;
        } else {
            units += 1;
        }
        i += (i + advance <= utf8.size()) ? advance : (utf8.size() - i);
    }
    return units;
}
}

void ArkUICefBrowser::imeSetComposition(const std::string& text) {
    if (!browser_) {
        return;
    }
    CefCompositionUnderline underline;
    // Ranges are UTF-16 offsets — use the UTF-16 length, not the UTF-8 byte
    // count (a UTF-8 byte count over-extends the underline for CJK text,
    // which is 3 bytes per character but 1 UTF-16 unit each).
    underline.range = CefRange(0, Utf16Length(text));
    underline.background_color = 0;
    browser_->GetHost()->ImeSetComposition(text, {underline},
                                           CefRange(UINT32_MAX, UINT32_MAX),
                                           CefRange(UINT32_MAX, UINT32_MAX));
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "imeSetComposition: id=%{public}ld, utf8_len=%{public}zu, utf16_len=%{public}zu",
        id_, text.length(), Utf16Length(text));
}

void ArkUICefBrowser::imeCommitText(const std::string& text) {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->ImeCommitText(text, CefRange(UINT32_MAX, UINT32_MAX), 0);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "imeCommitText: id=%{public}ld, len=%{public}zu", id_, text.length());
}

void ArkUICefBrowser::imeCancelComposition() {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->ImeCancelComposition();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "imeCancelComposition: id=%{public}ld", id_);
}

void ArkUICefBrowser::dragTargetDragEnter(const std::string& text, const std::string& html,
                                           const std::string& url, const std::string& filePaths,
                                           int x, int y, int allowedOps) {
    if (!browser_) {
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "dragTargetDragEnter: id=%{public}ld, text_len=%{public}zu, url=%{public}s, filePaths_len=%{public}zu, x=%{public}d, y=%{public}d",
        id_, text.size(), url.c_str(), filePaths.size(), x, y);

    CefRefPtr<CefDragData> dragData = CefDragData::Create();
    if (!text.empty()) dragData->SetFragmentText(text);
    if (!html.empty()) dragData->SetFragmentHtml(html);
    if (!url.empty()) {
        dragData->SetLinkURL(url);
        dragData->SetLinkTitle(url);
    }
    // Add file paths (newline-separated) for file drag-in
    if (!filePaths.empty()) {
        std::istringstream stream(filePaths);
        std::string path;
        while (std::getline(stream, path)) {
            if (!path.empty()) {
                // Extract display name from path (last component after '/')
                size_t lastSlash = path.rfind('/');
                std::string displayName = (lastSlash != std::string::npos)
                    ? path.substr(lastSlash + 1) : path;
                dragData->AddFile(path, displayName);
                OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
                    "dragTargetDragEnter: AddFile path=%{public}s, name=%{public}s",
                    path.c_str(), displayName.c_str());
            }
        }
    }
    dragData->ResetFileContents();

    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = 0;

    browser_->GetHost()->DragTargetDragEnter(dragData, event,
        static_cast<CefBrowserHost::DragOperationsMask>(allowedOps));
}

void ArkUICefBrowser::dragTargetDragOver(int x, int y, int allowedOps) {
    if (!browser_) {
        return;
    }
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = 0;

    browser_->GetHost()->DragTargetDragOver(event,
        static_cast<CefBrowserHost::DragOperationsMask>(allowedOps));
}

void ArkUICefBrowser::dragTargetDragLeave() {
    if (!browser_) {
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "dragTargetDragLeave: id=%{public}ld", id_);
    browser_->GetHost()->DragTargetDragLeave();
}

void ArkUICefBrowser::dragTargetDrop(int x, int y) {
    if (!browser_) {
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "dragTargetDrop: id=%{public}ld, x=%{public}d, y=%{public}d", id_, x, y);
    CefMouseEvent event;
    event.x = x;
    event.y = y;
    event.modifiers = 0;

    browser_->GetHost()->DragTargetDrop(event);
}

void ArkUICefBrowser::dragSourceEndedAt(int x, int y, int op) {
    if (!browser_) {
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "dragSourceEndedAt: id=%{public}ld, x=%{public}d, y=%{public}d, op=%{public}d", id_, x, y, op);
    browser_->GetHost()->DragSourceEndedAt(x, y,
        static_cast<CefBrowserHost::DragOperationsMask>(op));
}

void ArkUICefBrowser::dragSourceSystemDragEnded() {
    if (!browser_) {
        return;
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
        "dragSourceSystemDragEnded: id=%{public}ld", id_);
    browser_->GetHost()->DragSourceSystemDragEnded();
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