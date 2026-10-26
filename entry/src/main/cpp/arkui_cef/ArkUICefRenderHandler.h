#ifndef ARKUI_CEF_RENDER_HANDLER_H
#define ARKUI_CEF_RENDER_HANDLER_H

#include "include/cef_render_handler.h"
#include <functional>
#include <mutex>
#include <vector>

namespace arkui_cef {

using PaintCallback = std::function<void(const void* buffer, int width, int height)>;
using ScrollOffsetCallback = std::function<void(double x, double y)>;
using VirtualKeyboardCallback = std::function<void(bool show)>;
using TextSelectionCallback = std::function<void(const std::string& selected_text)>;

class ArkUICefRenderHandler : public CefRenderHandler {
public:
    ArkUICefRenderHandler(int width, int height);
    ~ArkUICefRenderHandler() = default;

    void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
    bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& screen_info) override;
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

    void SetSize(int width, int height);
    void SetDeviceScaleFactor(float factor);
    void SetPaintCallback(PaintCallback callback) { paintCallback_ = std::move(callback); }
    void SetScrollOffsetCallback(ScrollOffsetCallback cb) { scrollOffsetCallback_ = std::move(cb); }
    void SetVirtualKeyboardCallback(VirtualKeyboardCallback cb) { virtualKeyboardCallback_ = std::move(cb); }
    void SetTextSelectionCallback(TextSelectionCallback cb) { textSelectionCallback_ = std::move(cb); }

    void Invalidate();
    void SetPopupBuffer(const void* buffer, int width, int height);

    IMPLEMENT_REFCOUNTING(ArkUICefRenderHandler);

private:
    void CompositePopup(const void* viewBuffer, int viewW, int viewH);

    int width_;
    int height_;
    float deviceScaleFactor_ = 1.0f;
    PaintCallback paintCallback_;
    std::mutex mutex_;
    bool popupVisible_ = false;
    CefRect popupRect_;

    // Popup buffer (stored in CSS pixel coordinates, BGRA)
    std::vector<uint8_t> popupBuffer_;
    int popupBufferW_ = 0;
    int popupBufferH_ = 0;

    ScrollOffsetCallback scrollOffsetCallback_;
    VirtualKeyboardCallback virtualKeyboardCallback_;
    TextSelectionCallback textSelectionCallback_;
};

} // namespace arkui_cef
#endif // ARKUI_CEF_RENDER_HANDLER_H
