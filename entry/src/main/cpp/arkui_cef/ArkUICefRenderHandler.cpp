#include "ArkUICefRenderHandler.h"
#include <hilog/log.h>
#include "include/cef_stream.h"

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;

class MemoryWriteHandler : public CefWriteHandler {
public:
    size_t Write(const void* ptr, size_t size, size_t n) override {
        size_t total = size * n;
        const auto* bytes = static_cast<const uint8_t*>(ptr);
        buffer_.insert(buffer_.end(), bytes, bytes + total);
        return n;
    }
    int Seek(int64_t offset, int whence) override { return -1; }
    int64_t Tell() override { return static_cast<int64_t>(buffer_.size()); }
    int Flush() override { return 0; }
    bool MayBlock() override { return false; }
    const std::vector<uint8_t>& GetBuffer() const { return buffer_; }
    IMPLEMENT_REFCOUNTING(MemoryWriteHandler);
private:
    std::vector<uint8_t> buffer_;
};
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

void ArkUICefRenderHandler::SetComponentOffset(int x, int y) {
    std::lock_guard<std::mutex> lock(mutex_);
    componentOffsetX_ = x;
    componentOffsetY_ = y;
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

bool ArkUICefRenderHandler::GetRootScreenRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
    // Return false to use CEF default (uses GetViewRect as root rect in OSR mode).
    // This is sufficient for OSR — full screen rect is not critical.
    return false;
}

bool ArkUICefRenderHandler::GetScreenPoint(CefRefPtr<CefBrowser> browser,
                                           int viewX, int viewY,
                                           int& screenX, int& screenY) {
    std::lock_guard<std::mutex> lock(mutex_);
    screenX = viewX + componentOffsetX_;
    screenY = viewY + componentOffsetY_;
    return true;
}

void ArkUICefRenderHandler::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                                     const RectList& dirtyRects, const void* buffer,
                                     int width, int height) {
    if (type == PET_POPUP) {
        // Store popup pixels, then composite into the staging buffer right
        // away (a popup repaint may arrive without a matching PET_VIEW
        // frame, e.g. selection highlight moving inside a dropdown).
        std::lock_guard<std::mutex> lock(mutex_);
        size_t size = static_cast<size_t>(width) * height * 4;
        popupBuffer_.resize(size);
        memcpy(popupBuffer_.data(), buffer, size);
        popupBufferW_ = width;
        popupBufferH_ = height;
        if (frameW_ > 0 && frameH_ > 0 &&
            popupVisible_ && !popupBuffer_.empty()) {
            CefRect written = CompositePopup(frameBuffer_.data(), frameW_, frameH_);
            if (written.width > 0 && written.height > 0) {
                AddToDirtyRegion(written);
                NotifyFrameReady();
            }
        }
        return;
    }

    if (type != PET_VIEW) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    if (frameW_ != width || frameH_ != height) {
        // First frame or resize: replace the whole staging buffer and mark a
        // full frame so the JS side rebuilds its PixelMap.
        size_t size = static_cast<size_t>(width) * height * 4;
        frameBuffer_.assign(static_cast<const uint8_t*>(buffer),
                            static_cast<const uint8_t*>(buffer) + size);
        frameW_ = width;
        frameH_ = height;
        accumDirty_ = CefRect(0, 0, width, height);
        accumDirtyValid_ = true;
        fullFramePending_ = true;
    } else {
        // Incremental frame: blit only the dirty rects into the staging
        // buffer (QCefView's dirty-region update). CEF's buffer is a full
        // frame, so the source rows are always at full-width stride.
        for (const auto& rc : dirtyRects) {
            BlitDirtyRect(rc, buffer, width, height);
        }
    }

    // If a popup is visible, composite it on the staging buffer and include
    // the popup area in the dirty region.
    if (popupVisible_ && !popupBuffer_.empty() && popupBufferW_ > 0 && popupBufferH_ > 0) {
        CefRect written = CompositePopup(frameBuffer_.data(), frameW_, frameH_);
        if (written.width > 0 && written.height > 0) {
            AddToDirtyRegion(written);
        }
    }

    if (accumDirtyValid_) {
        NotifyFrameReady();
    }
}

void ArkUICefRenderHandler::BlitDirtyRect(const CefRect& rc, const void* buffer,
                                           int bufferW, int bufferH) {
    if (frameBuffer_.empty() || frameW_ <= 0 || frameH_ <= 0) {
        return;
    }
    // Clamp the rect to both the incoming buffer and the staging buffer
    // (they have the same size on the incremental path).
    CefRect r = rc;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x + r.width > bufferW) r.width = bufferW - r.x;
    if (r.y + r.height > bufferH) r.height = bufferH - r.y;
    if (r.width <= 0 || r.height <= 0) {
        return;
    }
    const auto* src = static_cast<const uint8_t*>(buffer);
    uint8_t* dst = frameBuffer_.data();
    for (int row = 0; row < r.height; row++) {
        size_t offset = (static_cast<size_t>(r.y + row) * bufferW + r.x) * 4;
        memcpy(dst + offset, src + offset, static_cast<size_t>(r.width) * 4);
    }
    AddToDirtyRegion(r);
}

void ArkUICefRenderHandler::AddToDirtyRegion(const CefRect& rc) {
    if (rc.width <= 0 || rc.height <= 0) {
        return;
    }
    if (!accumDirtyValid_) {
        accumDirty_ = rc;
        accumDirtyValid_ = true;
        return;
    }
    int x1 = accumDirty_.x;
    int y1 = accumDirty_.y;
    int x2 = accumDirty_.x + accumDirty_.width;
    int y2 = accumDirty_.y + accumDirty_.height;
    if (rc.x < x1) x1 = rc.x;
    if (rc.y < y1) y1 = rc.y;
    if (rc.x + rc.width > x2) x2 = rc.x + rc.width;
    if (rc.y + rc.height > y2) y2 = rc.y + rc.height;
    accumDirty_ = CefRect(x1, y1, x2 - x1, y2 - y1);
}

void ArkUICefRenderHandler::NotifyFrameReady() {
    // Called with mutex_ held — only touch the callback (the callback itself
    // must not call back into this handler).
    if (paintCallback_) {
        paintCallback_();
    }
}

bool ArkUICefRenderHandler::ConsumePendingRegionMeta(OsrPendingRegion& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accumDirtyValid_ || frameW_ <= 0 || frameH_ <= 0) {
        return false;
    }

    CefRect r = accumDirty_;
    if (fullFramePending_) {
        r = CefRect(0, 0, frameW_, frameH_);
    }
    // Clamp to the staging buffer as a safety net.
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x + r.width > frameW_) r.width = frameW_ - r.x;
    if (r.y + r.height > frameH_) r.height = frameH_ - r.y;
    if (r.width <= 0 || r.height <= 0) {
        accumDirtyValid_ = false;
        fullFramePending_ = false;
        return false;
    }

    out.x = r.x;
    out.y = r.y;
    out.w = r.width;
    out.h = r.height;
    out.fullFrame = fullFramePending_;
    out.viewW = frameW_;
    out.viewH = frameH_;
    // Metadata only — the pixel rows are compacted lazily by
    // CompactRegionPixels when a fallback path actually needs them (the
    // NativeWindow path never does: it blits the full frame and uses the
    // rect as the damage hint only).
    out.pixels.clear();

    accumDirtyValid_ = false;
    fullFramePending_ = false;
    return true;
}

void ArkUICefRenderHandler::CompactRegionPixels(OsrPendingRegion& region) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The staging frame must still match the rect consumed by
    // ConsumePendingRegionMeta — if a resize raced in between, the payload
    // is stale and dropped (the resize chain delivers a fresh full frame).
    if (region.w <= 0 || region.h <= 0 ||
        region.viewW != frameW_ || region.viewH != frameH_ ||
        region.x + region.w > frameW_ || region.y + region.h > frameH_) {
        region.pixels.clear();
        return;
    }
    size_t rowBytes = static_cast<size_t>(region.w) * 4;
    region.pixels.resize(rowBytes * region.h);
    const uint8_t* src = frameBuffer_.data();
    for (int row = 0; row < region.h; row++) {
        size_t srcOffset = (static_cast<size_t>(region.y + row) * frameW_ + region.x) * 4;
        memcpy(region.pixels.data() + row * rowBytes, src + srcOffset, rowBytes);
    }
}

bool ArkUICefRenderHandler::BlitFullFrameTo(uint8_t* dst, size_t dstStride, bool swapBR) {
    if (!dst || dstStride == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (frameBuffer_.empty() || frameW_ <= 0 || frameH_ <= 0) {
        return false;
    }
    size_t rowBytes = static_cast<size_t>(frameW_) * 4;
    if (dstStride < rowBytes) {
        return false;
    }
    const uint8_t* src = frameBuffer_.data();
    for (int row = 0; row < frameH_; row++) {
        uint8_t* dstRow = dst + static_cast<size_t>(row) * dstStride;
        if (swapBR) {
            // BGRA -> RGBA: swap bytes 0 and 2 of each 32-bit pixel.
            const uint32_t* srcRow = reinterpret_cast<const uint32_t*>(src + row * rowBytes);
            uint32_t* dstRow32 = reinterpret_cast<uint32_t*>(dstRow);
            for (int i = 0; i < frameW_; i++) {
                uint32_t v = srcRow[i];
                dstRow32[i] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) |
                              ((v & 0x000000FFu) << 16);
            }
        } else {
            memcpy(dstRow, src + row * rowBytes, rowBytes);
        }
    }
    return true;
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

CefRect ArkUICefRenderHandler::CompositePopup(const void* viewBuffer, int viewW, int viewH) {
    // popupRect_ is in CSS pixels (from OnPopupSize, matching GetViewRect's CSS pixel coordinate system).
    // The view buffer from OnPaint is in physical pixels (width_ * height_).
    // We must scale popupRect_ by deviceScaleFactor_ to convert to physical pixel coordinates.
    // Returns the physical-pixel rect actually written into viewBuffer (for dirty tracking).
    if (popupBuffer_.empty() || popupBufferW_ <= 0 || popupBufferH_ <= 0) {
        return CefRect(0, 0, 0, 0);
    }

    auto* viewPixels = static_cast<uint8_t*>(const_cast<void*>(viewBuffer));

    // Convert popup rect from CSS pixels to physical pixels
    int popX = static_cast<int>(popupRect_.x * deviceScaleFactor_);
    int popY = static_cast<int>(popupRect_.y * deviceScaleFactor_);
    int popW = static_cast<int>(popupRect_.width * deviceScaleFactor_);
    int popH = static_cast<int>(popupRect_.height * deviceScaleFactor_);

    // Clamp to view bounds
    if (popX < 0) popX = 0;
    if (popY < 0) popY = 0;
    if (popX + popW > viewW) popW = viewW - popX;
    if (popY + popH > viewH) popH = viewH - popY;
    if (popW <= 0 || popH <= 0) return CefRect(0, 0, 0, 0);

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
    return CefRect(popX, popY, popW, popH);
}

void ArkUICefRenderHandler::OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnPopupShow: show=%{public}d", show);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        popupVisible_ = show;
        if (!show) {
            // Popup closed: clear the rects. Following cefclient
            // (BrowserWindowOsrOhos::OnPopupShow), request a full view
            // repaint right away — the popup pixels were composited into
            // the staging buffer and only a full repaint guarantees the
            // covered area is restored (without this the popup region
            // lingers on screen until something else triggers a repaint).
            ClearPopupRects();
        }
    }
    if (!show) {
        browser->GetHost()->Invalidate(PET_VIEW);
    }
}

void ArkUICefRenderHandler::OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnPopupSize: x=%{public}d, y=%{public}d, w=%{public}d, h=%{public}d",
        rect.x, rect.y, rect.width, rect.height);
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    // rect is in CSS pixels (matching GetViewRect's coordinate system).
    // Keep both the original rect (CEF's internal notion, needed for input
    // coordinate mapping) and the repositioned display rect (clamped into
    // the view so the whole popup stays visible — e.g. a dropdown opening
    // upward near the bottom edge).
    originalPopupRect_ = rect;
    popupRect_ = GetPopupRectInWebView(rect);
}

CefRect ArkUICefRenderHandler::GetPopupRectInWebView(const CefRect& original) {
    CefRect rc(original);
    if (rc.x < 0) rc.x = 0;
    if (rc.y < 0) rc.y = 0;
    // View dimensions in CSS pixels.
    int viewW = static_cast<int>(width_ / deviceScaleFactor_);
    int viewH = static_cast<int>(height_ / deviceScaleFactor_);
    // If the popup goes outside the view, reposition its origin instead of
    // clipping it (mirrors cefclient's GetPopupRectInWebView).
    if (rc.x + rc.width > viewW) rc.x = viewW - rc.width;
    if (rc.y + rc.height > viewH) rc.y = viewH - rc.height;
    if (rc.x < 0) rc.x = 0;
    if (rc.y < 0) rc.y = 0;
    return rc;
}

void ArkUICefRenderHandler::ClearPopupRects() {
    popupRect_.Set(0, 0, 0, 0);
    originalPopupRect_.Set(0, 0, 0, 0);
}

void ArkUICefRenderHandler::ApplyPopupOffset(int& x, int& y) {
    // Coordinates are in CSS pixels (the event coordinate system). When the
    // popup was repositioned for display, clicks on the DISPLAYED popup
    // region must be mapped back to CEF's internal (original) popup rect.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!popupVisible_ || popupRect_.width <= 0 || popupRect_.height <= 0) {
        return;
    }
    if (x >= popupRect_.x && x < popupRect_.x + popupRect_.width &&
        y >= popupRect_.y && y < popupRect_.y + popupRect_.height) {
        x += originalPopupRect_.x - popupRect_.x;
        y += originalPopupRect_.y - popupRect_.y;
    }
}

void ArkUICefRenderHandler::OnImeCompositionRangeChanged(
    CefRefPtr<CefBrowser> browser,
    const CefRange& selected_range,
    const RectList& character_bounds) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnImeCompositionRangeChanged: from=%{public}d, to=%{public}d, bounds_count=%{public}zu",
        selected_range.from, selected_range.to, character_bounds.size());

    if (character_bounds.empty()) {
        return;
    }

    // character_bounds are in CSS pixels (view coordinates, matching GetViewRect).
    // CursorInfo expects px (physical pixels) relative to the window.
    // Convert: (view_css + componentOffset_css) * deviceScaleFactor = window_px
    // Use first_bound (matches cefclient behavior) — cursor at composition start.
    int left, top, cursorWidth, cursorHeight;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const CefRect& first = character_bounds.front();
        left = static_cast<int>((first.x + componentOffsetX_) * deviceScaleFactor_);
        top = static_cast<int>((first.y + componentOffsetY_) * deviceScaleFactor_);
        cursorWidth = static_cast<int>(first.width * deviceScaleFactor_);
        cursorHeight = static_cast<int>(first.height * deviceScaleFactor_);
    }
    if (cursorWidth <= 0) {
        cursorWidth = 2;
    }
    if (cursorHeight <= 0) {
        cursorHeight = 24;
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "OnImeCompositionRangeChanged -> cursor px: left=%{public}d, top=%{public}d, w=%{public}d, h=%{public}d",
        left, top, cursorWidth, cursorHeight);

    if (imeCursorCallback_) {
        imeCursorCallback_(left, top, cursorWidth, cursorHeight);
    }
}

bool ArkUICefRenderHandler::StartDragging(CefRefPtr<CefBrowser> browser,
                                           CefRefPtr<CefDragData> drag_data,
                                           DragOperationsMask allowed_ops,
                                           int x, int y) {
    if (!drag_data) {
        return false;
    }

    std::string text = drag_data->GetFragmentText().ToString();
    std::string html = drag_data->GetFragmentHtml().ToString();
    std::string url = drag_data->GetLinkURL().ToString();
    std::string urlTitle = drag_data->GetLinkTitle().ToString();

    // Extract file contents if dragging a file (e.g. image drag from <img>)
    std::string fileName = drag_data->GetFileName().ToString();
    std::vector<uint8_t> fileData;
    if (!fileName.empty()) {
        size_t fileSize = drag_data->GetFileContents(nullptr);
        if (fileSize > 0 && fileSize < 50 * 1024 * 1024) { // 50MB limit
            CefRefPtr<MemoryWriteHandler> handler = new MemoryWriteHandler();
            CefRefPtr<CefStreamWriter> writer = CefStreamWriter::CreateForHandler(handler);
            drag_data->GetFileContents(writer);
            fileData = handler->GetBuffer();
        }
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "StartDragging: text_len=%{public}zu, html_len=%{public}zu, url=%{public}s, fileName=%{public}s, fileData_size=%{public}zu, x=%{public}d, y=%{public}d",
        text.size(), html.size(), url.c_str(), fileName.c_str(), fileData.size(), x, y);

    if (startDragCallback_) {
        startDragCallback_(text, html, url, urlTitle, fileName, fileData, x, y);
        return true;
    }
    return false;
}

void ArkUICefRenderHandler::UpdateDragCursor(CefRefPtr<CefBrowser> browser,
                                              DragOperation operation) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefRenderHandler",
        "UpdateDragCursor: operation=%{public}d", static_cast<int>(operation));
    if (updateDragCursorCallback_) {
        updateDragCursorCallback_(static_cast<int>(operation));
    }
}

} // namespace arkui_cef
