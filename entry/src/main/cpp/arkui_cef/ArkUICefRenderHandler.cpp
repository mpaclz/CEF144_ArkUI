#include "ArkUICefRenderHandler.h"
#include <hilog/log.h>

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;
}

namespace arkui_cef {

ArkUICefRenderHandler::ArkUICefRenderHandler(int width, int height)
    : width_(width), height_(height) {}

void ArkUICefRenderHandler::SetSize(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    width_ = width;
    height_ = height;
}

void ArkUICefRenderHandler::SetDeviceScaleFactor(float factor) {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceScaleFactor_ = factor;
}

void ArkUICefRenderHandler::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    std::lock_guard<std::mutex> lock(mutex_);
    // CEF expects the view rect in CSS pixels (logical pixels).
    // width_/height_ are physical pixels; divide by deviceScaleFactor_ to get CSS pixels.
    int cssW = static_cast<int>(width_ / deviceScaleFactor_);
    int cssH = static_cast<int>(height_ / deviceScaleFactor_);
    rect = CefRect(0, 0, cssW, cssH);
}

bool ArkUICefRenderHandler::GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) {
    std::lock_guard<std::mutex> lock(mutex_);
    screen_info.device_scale_factor = deviceScaleFactor_;
    int cssW = static_cast<int>(width_ / deviceScaleFactor_);
    int cssH = static_cast<int>(height_ / deviceScaleFactor_);
    screen_info.rect = CefRect(0, 0, cssW, cssH);
    screen_info.available_rect = screen_info.rect;
    return true;
}

void ArkUICefRenderHandler::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                     const RectList& dirtyRects, const void* buffer,
                                     int width, int height) {
    if (type == PET_POPUP) {
        // Store popup buffer for compositing onto the view buffer
        std::lock_guard<std::mutex> lock(mutex_);
        size_t size = static_cast<size_t>(width) * height * 4;
        popupBuffer_.resize(size);
        memcpy(popupBuffer_.data(), buffer, size);
        popupBufferW_ = width;
        popupBufferH_ = height;
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
            "OnPaint PET_POPUP: w=%{public}d, h=%{public}d", width, height);
        return;
    }

    if (type != PET_VIEW) {
        return;
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnPaint: width=%{public}d, height=%{public}d, dirtyRects=%{public}zu",
        width, height, dirtyRects.size());

    // If a popup is visible, composite it onto the view buffer before sending to ArkTS
    bool hasPopup = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        hasPopup = popupVisible_ && !popupBuffer_.empty() && popupBufferW_ > 0 && popupBufferH_ > 0;
    }

    if (hasPopup) {
        // Make a copy and composite popup onto it
        size_t viewSize = static_cast<size_t>(width) * height * 4;
        std::vector<uint8_t> composited(static_cast<const uint8_t*>(buffer),
                                        static_cast<const uint8_t*>(buffer) + viewSize);
        CompositePopup(composited.data(), width, height);
        if (paintCallback_) {
            paintCallback_(composited.data(), width, height);
        }
    } else {
        if (paintCallback_) {
            paintCallback_(buffer, width, height);
        }
    }
}

void ArkUICefRenderHandler::OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                                const RectList& dirtyRects,
                                                const CefAcceleratedPaintInfo& info) {
    // GPU accelerated paint uses shared textures. We don't have the infrastructure
    // to import GPU textures into ArkUI's Image component, so we log and ignore.
    // The software OnPaint path will still be called if Invalidate(PET_VIEW) is used.
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnAcceleratedPaint: type=%{public}d (not supported, using software path)", type);
}

void ArkUICefRenderHandler::OnTextSelectionChanged(CefRefPtr<CefBrowser> browser,
                                                    const CefString& selected_text,
                                                    const CefRange& selected_range) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnTextSelectionChanged: text_len=%{public}zu, range=[%{public}d,%{public}d]",
        selected_text.ToString().size(), selected_range.from, selected_range.to);
    if (textSelectionCallback_) {
        textSelectionCallback_(selected_text.ToString());
    }
}

void ArkUICefRenderHandler::OnVirtualKeyboardRequested(CefRefPtr<CefBrowser> browser,
                                                        TextInputMode input_mode) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnVirtualKeyboardRequested: input_mode=%{public}d", input_mode);
    if (virtualKeyboardCallback_) {
        virtualKeyboardCallback_(input_mode != CEF_TEXT_INPUT_MODE_NONE);
    }
}

void ArkUICefRenderHandler::OnScrollOffsetChanged(CefRefPtr<CefBrowser> browser, double x, double y) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "ScrollOffsetChanged: x=%{public}f, y=%{public}f", x, y);
    if (scrollOffsetCallback_) {
        scrollOffsetCallback_(x, y);
    }
}

void ArkUICefRenderHandler::Invalidate() {
    // Trigger a full repaint by telling CEF the view was resized
    // This is a no-op if there's no browser; the caller (ArkUICefBrowser) handles that
}

void ArkUICefRenderHandler::CompositePopup(const void* viewBuffer, int viewW, int viewH) {
    // popupRect_ is in CSS pixels (from OnPopupSize, matching GetViewRect's CSS pixel coordinate system).
    // The view buffer from OnPaint is in physical pixels (width_ * height_).
    // We must scale popupRect_ by deviceScaleFactor_ to convert to physical pixel coordinates.
    if (popupBuffer_.empty() || popupBufferW_ <= 0 || popupBufferH_ <= 0) {
        return;
    }

    auto* viewPixels = static_cast<uint8_t*>(const_cast<void*>(viewBuffer));

    // Convert popup rect from CSS pixels to physical pixels
    int popX = static_cast<int>(popupRect_.x * deviceScaleFactor_);
    int popY = static_cast<int>(popupRect_.y * deviceScaleFactor_);
    int popW = static_cast<int>(popupRect_.width * deviceScaleFactor_);
    int popH = static_cast<int>(popupRect_.height * deviceScaleFactor_);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "CompositePopup: css(%{public}d,%{public}d,%{public}d,%{public}d) -> phys(%{public}d,%{public}d,%{public}d,%{public}d) dsf=%{public}f view=%{public}dx%{public}d popupBuf=%{public}dx%{public}d",
        popupRect_.x, popupRect_.y, popupRect_.width, popupRect_.height,
        popX, popY, popW, popH, deviceScaleFactor_, viewW, viewH, popupBufferW_, popupBufferH_);

    // Clamp to view bounds
    if (popX < 0) popX = 0;
    if (popY < 0) popY = 0;
    if (popX + popW > viewW) popW = viewW - popX;
    if (popY + popH > viewH) popH = viewH - popY;
    if (popW <= 0 || popH <= 0) return;

    // Clamp to popup buffer bounds
    if (popW > popupBufferW_) popW = popupBufferW_;
    if (popH > popupBufferH_) popH = popupBufferH_;

    // Copy popup pixels onto the view buffer (BGRA, 4 bytes per pixel)
    for (int row = 0; row < popH; row++) {
        int dstOffset = ((popY + row) * viewW + popX) * 4;
        int srcOffset = row * popupBufferW_ * 4;
        int copyBytes = popW * 4;
        if (dstOffset + copyBytes <= viewW * viewH * 4 &&
            srcOffset + copyBytes <= static_cast<int>(popupBuffer_.size())) {
            memcpy(viewPixels + dstOffset, popupBuffer_.data() + srcOffset, copyBytes);
        }
    }
}

void ArkUICefRenderHandler::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnPopupShow: show=%{public}d", show);
    popupVisible_ = show;
}

void ArkUICefRenderHandler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnPopupSize: x=%{public}d, y=%{public}d, w=%{public}d, h=%{public}d",
        rect.x, rect.y, rect.width, rect.height);
    popupRect_ = rect;
}

void ArkUICefRenderHandler::OnImeCompositionRangeChanged(
    CefRefPtr<CefBrowser> browser,
    const CefRange& selected_range,
    const RectList& character_bounds) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnImeCompositionRangeChanged: from=%{public}d, to=%{public}d, bounds_count=%{public}zu",
        selected_range.from, selected_range.to, character_bounds.size());
}

} // namespace arkui_cef
