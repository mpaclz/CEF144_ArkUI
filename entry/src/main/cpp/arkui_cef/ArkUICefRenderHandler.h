#ifndef ARKUI_CEF_RENDER_HANDLER_H
#define ARKUI_CEF_RENDER_HANDLER_H

#include "include/cef_render_handler.h"
#include "include/cef_drag_data.h"
#include <functional>
#include <mutex>
#include <vector>

namespace arkui_cef {

// PaintCallback: notification that a frame was composited into the persistent
// staging buffer. The JS thread pulls the pending dirty region via
// ConsumePendingRegion(). This mirrors QCefView's widget->update() — the
// notification may be coalesced, but pixel data is never dropped.
using PaintCallback = std::function<void()>;

// A pending dirty region pulled from the staging buffer by the JS thread.
struct OsrPendingRegion {
    std::vector<uint8_t> pixels;  // compact BGRA rows (w * h * 4 bytes)
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool fullFrame = false;       // resize / first frame — rebuild the PixelMap
    int viewW = 0;
    int viewH = 0;
};
using ScrollOffsetCallback = std::function<void(double x, double y)>;
using VirtualKeyboardCallback = std::function<void(bool show)>;
using TextSelectionCallback = std::function<void(const std::string& selected_text)>;
using CursorChangeCallback = std::function<void(int cursorType)>;
using ImeCursorCallback = std::function<void(int left, int top, int width, int height)>;
using StartDragCallback = std::function<void(const std::string& text, const std::string& html, const std::string& url, const std::string& urlTitle, const std::string& fileName, const std::vector<uint8_t>& fileData, int x, int y)>;
using UpdateDragCursorCallback = std::function<void(int operation)>;

class ArkUICefRenderHandler : public CefRenderHandler {
public:
    ArkUICefRenderHandler(int width, int height);
    ~ArkUICefRenderHandler() = default;

    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
    bool GetRootScreenRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenPoint(CefRefPtr<CefBrowser> browser,
                        int viewX, int viewY,
                        int& screenX, int& screenY) override;
    void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                 const RectList& dirtyRects, const void* buffer,
                 int width, int height) override;
    void OnPopupShow(CefRefPtr<CefBrowser> browser, bool show) override;
    void OnPopupSize(CefRefPtr<CefBrowser> browser, const CefRect& rect) override;
    void OnImeCompositionRangeChanged(CefRefPtr<CefBrowser> browser,
                                       const CefRange& selected_range,
                                       const RectList& character_bounds) override;
    void OnAcceleratedPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                            const RectList& dirtyRects,
                            const CefAcceleratedPaintInfo& info) override;
    void OnTextSelectionChanged(CefRefPtr<CefBrowser> browser,
                                const CefString& selected_text,
                                const CefRange& selected_range) override;
    void OnVirtualKeyboardRequested(CefRefPtr<CefBrowser> browser,
                                    TextInputMode input_mode) override;
    void OnScrollOffsetChanged(CefRefPtr<CefBrowser> browser, double x, double y) override;
    bool StartDragging(CefRefPtr<CefBrowser> browser, CefRefPtr<CefDragData> drag_data,
                       DragOperationsMask allowed_ops, int x, int y) override;
    void UpdateDragCursor(CefRefPtr<CefBrowser> browser, DragOperation operation) override;

    void SetSize(int width, int height);
    void SetDeviceScaleFactor(float factor);
    void SetComponentOffset(int x, int y);
    void SetPaintCallback(PaintCallback callback) { paintCallback_ = std::move(callback); }
    void SetScrollOffsetCallback(ScrollOffsetCallback cb) { scrollOffsetCallback_ = std::move(cb); }
    void SetVirtualKeyboardCallback(VirtualKeyboardCallback cb) { virtualKeyboardCallback_ = std::move(cb); }
    void SetTextSelectionCallback(TextSelectionCallback cb) { textSelectionCallback_ = std::move(cb); }
    void SetImeCursorCallback(ImeCursorCallback cb) { imeCursorCallback_ = std::move(cb); }
    void SetStartDragCallback(StartDragCallback cb) { startDragCallback_ = std::move(cb); }
    void SetUpdateDragCursorCallback(UpdateDragCursorCallback cb) { updateDragCursorCallback_ = std::move(cb); }

    void Invalidate();

    // Called on the JS thread: takes the accumulated dirty region out of the
    // staging buffer (resets the accumulation). Returns false when nothing is
    // pending. |out.pixels| is reused across calls to avoid per-frame allocs.
    bool ConsumePendingRegion(OsrPendingRegion& out);

    IMPLEMENT_REFCOUNTING(ArkUICefRenderHandler);

private:
    // Blits a dirty rect from a CEF paint buffer into the staging buffer
    // (physical pixel coords). Must be called with mutex_ held.
    void BlitDirtyRect(const CefRect& rc, const void* buffer, int bufferW, int bufferH);
    // Composites the popup onto |viewBuffer| and returns the physical-pixel
    // rect actually written (for dirty tracking). Must be called with mutex_ held.
    CefRect CompositePopup(const void* viewBuffer, int viewW, int viewH);
    void AddToDirtyRegion(const CefRect& rc);
    void NotifyFrameReady();

    int width_;
    int height_;
    float deviceScaleFactor_ = 1.0f;
    int componentOffsetX_ = 0;
    int componentOffsetY_ = 0;
    PaintCallback paintCallback_;
    std::mutex mutex_;
    bool popupVisible_ = false;
    CefRect popupRect_;

    // Popup buffer (stored in CSS pixel coordinates, BGRA)
    std::vector<uint8_t> popupBuffer_;
    int popupBufferW_ = 0;
    int popupBufferH_ = 0;

    // Persistent staging frame buffer (physical pixels, BGRA), the ArkUI
    // counterpart of QCefView's m_cefViewImage. CEF UI thread blits dirty
    // rects into it; the JS thread pulls accumulated regions out of it.
    std::vector<uint8_t> frameBuffer_;
    int frameW_ = 0;
    int frameH_ = 0;
    CefRect accumDirty_;          // accumulated dirty rect (physical px)
    bool accumDirtyValid_ = false;
    bool fullFramePending_ = false;

    ScrollOffsetCallback scrollOffsetCallback_;
    VirtualKeyboardCallback virtualKeyboardCallback_;
    TextSelectionCallback textSelectionCallback_;
    ImeCursorCallback imeCursorCallback_;
    StartDragCallback startDragCallback_;
    UpdateDragCursorCallback updateDragCursorCallback_;
};

} // namespace arkui_cef
#endif // ARKUI_CEF_RENDER_HANDLER_H
