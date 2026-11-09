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
      client_(nullptr) {}

ArkUICefBrowser::~ArkUICefBrowser() {
    if (browserCreated_ && !browserClosing_) {
        close();
    }
}

void ArkUICefBrowser::createBrowser(CefRefPtr<ArkUICefClient> client, Node* parentNode, const std::string& url) {
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
    }
}

void ArkUICefBrowser::close() {
    if (!browserCreated_) {
        return;
    }
    
    browserClosing_ = true;
    if (browser_) {
        browser_->GetHost()->CloseBrowser(true);
    }
}

void ArkUICefBrowser::loadUrl(const std::string& url) {
    if (!browser_ || !browser_->GetMainFrame()) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "ArkUICefBrowser",
            "loadUrl: browser not ready");
        return;
    }
    
    browser_->GetMainFrame()->LoadURL(url);
}

void ArkUICefBrowser::goBack() {
    if (!browser_ || !browser_->CanGoBack()) {
        return;
    }
    
    browser_->GoBack();
}

void ArkUICefBrowser::goForward() {
    if (!browser_ || !browser_->CanGoForward()) {
        return;
    }
    
    browser_->GoForward();
}

void ArkUICefBrowser::reload() {
    if (!browser_) {
        return;
    }
    
    browser_->Reload();
}

void ArkUICefBrowser::resize(int width, int height) {
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

    // With OSR, force a full repaint at the new size. WasResized() alone
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

void ArkUICefBrowser::setFrameRate(int frameRate) {
    if (!browser_ || frameRate < 1) {
        return;
    }
    browser_->GetHost()->SetWindowlessFrameRate(frameRate);
}

void ArkUICefBrowser::show() {
    Node* node = getWindowHandle();
    if (node)
        arkui_cef::show(node->container);
}

void ArkUICefBrowser::hide() {
    Node* node = getWindowHandle();
    if (node)
        arkui_cef::hide(node->container);
}

void ArkUICefBrowser::sendKeyEvent(const CefKeyEvent& event) {
    if (!browser_) {
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
}

void ArkUICefBrowser::sendCaptureLostEvent() {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->SendCaptureLostEvent();
}

void ArkUICefBrowser::invalidate() {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->Invalidate(PET_VIEW);
}

void ArkUICefBrowser::wasHidden(bool hidden) {
    if (!browser_) {
        return;
    }
    // Official OSR visibility API (QCefView uses the same, see
    // QCefViewPrivate::onViewVisibilityChanged): stops layouting and OnPaint
    // in the renderer while hidden. The previous frame-rate trick
    // (SetWindowlessFrameRate 0/30) is broken in CEF144 — ClampFrameRate
    // maps 0 back to the default 30 instead of stopping rendering.
    browser_->GetHost()->WasHidden(hidden);
    if (!hidden) {
        // Kick-start frame production after un-hiding. WasHidden(true)
        // evicts the delegated frame host and stops the video consumer; the
        // plain WasHidden(false) resume path can leave the compositor idle
        // (observed as a permanently frozen page). Mirrors cefclient
        // (BrowserWindowOsrOhos::CreateBrowser: "Send resize notification so
        // the compositor is assigned the correct viewport size and begins
        // rendering"): WasResized() re-synchronizes visual properties and
        // Invalidate(PET_VIEW) requests a fresh full frame.
        browser_->GetHost()->WasResized();
        browser_->GetHost()->Invalidate(PET_VIEW);
    }
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
}

void ArkUICefBrowser::imeCommitText(const std::string& text) {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->ImeCommitText(text, CefRange(UINT32_MAX, UINT32_MAX), 0);
}

void ArkUICefBrowser::imeCancelComposition() {
    if (!browser_) {
        return;
    }
    browser_->GetHost()->ImeCancelComposition();
}

void ArkUICefBrowser::dragTargetDragEnter(const std::string& text, const std::string& html,
                                           const std::string& url, const std::string& filePaths,
                                           int x, int y, int allowedOps) {
    if (!browser_) {
        return;
    }

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
    browser_->GetHost()->DragTargetDragLeave();
}

void ArkUICefBrowser::dragTargetDrop(int x, int y) {
    if (!browser_) {
        return;
    }
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
    browser_->GetHost()->DragSourceEndedAt(x, y,
        static_cast<CefBrowserHost::DragOperationsMask>(op));
}

void ArkUICefBrowser::dragSourceSystemDragEnded() {
    if (!browser_) {
        return;
    }
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