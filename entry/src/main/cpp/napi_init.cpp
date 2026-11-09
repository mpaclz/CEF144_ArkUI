#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <hilog/log.h>
#include <string>
#include <arkui/native_interface.h>
#include <arkui/native_node_napi.h>
#include "window_manager/oh_window_event_filter.h"
#include "multimodalinput/oh_input_manager.h"
#include <multimedia/image_framework/image/pixelmap_native.h>
#include <native_window/external_window.h>
#include <native_window/buffer_handle.h>
#include <native_buffer/native_buffer.h>
#include <native_buffer/buffer_common.h>
#include <poll.h>
#include <unistd.h>

#include "JsThreadSafeCallback.h"
#include "napi/native_api.h"
#include "arkui_cef/ArkUICefBrowser.h"
#include "arkui_cef/ArkUICefApp.h"
#include "arkui_cef/ArkUICefManager.h"
#include "arkui_cef/ArkUICefRenderHandler.h"
#include "arkui_cef/ArkUICefInputMethod.h"
#include "include/base/cef_callback.h"

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;

// ---------------------------------------------------------------------------
// OSR native PixelMap registry (Step 1 of the software-render pipeline
// optimization).
//
// ArkTS registers its double-buffered PixelMaps (pmA/pmB) here after each
// rebuild. The paint tsfn then blits dirty regions directly into the
// PixelMap memory via OH_PixelmapNative_AccessPixels on the JS thread, so
// incremental frames carry no pixel data across the NAPI boundary — the
// JS callback only receives the region metadata and swaps the displayed
// PixelMap reference.
//
// Ownership: the ArkTS PixelMap objects are owned by OsrCefView (released
// via pmA.release()/pmB.release()). The OH_PixelmapNative handles below
// alias the same pixelmaps and are released with OH_PixelmapNative_Release
// on re-registration / unregistration — always BEFORE the ArkTS objects
// are released, so the handles never outlive the memory they point to.
// All registry access happens on the JS thread (tsfn CallJs and the NAPI
// entry points); the mutex guards against future multi-instance use.
// ---------------------------------------------------------------------------
struct OsrNativePixelmaps {
    OH_PixelmapNative* pmA = nullptr;
    OH_PixelmapNative* pmB = nullptr;
    uint32_t rowStride = 0;  // destination bytes per row (may exceed w*4)
    int32_t width = 0;
    int32_t height = 0;
};

std::mutex g_osrPixelmapsMutex;
std::unordered_map<int64_t, OsrNativePixelmaps> g_osrPixelmaps;

void ReleaseOsrNativePixelmaps(OsrNativePixelmaps& pms) {
    if (pms.pmA) {
        OH_PixelmapNative_Release(pms.pmA);
        pms.pmA = nullptr;
    }
    if (pms.pmB) {
        OH_PixelmapNative_Release(pms.pmB);
        pms.pmB = nullptr;
    }
}

// Queries stride/size and validates BGRA_8888 (the format CEF paints in).
// NOTE: the SDK header declares GetWidth/GetHeight with uint32_t* (the
// online docs' int32_t* is outdated) — verified against the compile error.
bool QueryPixelmapInfo(OH_PixelmapNative* pm, uint32_t* stride,
                       int32_t* w, int32_t* h) {
    if (!pm || !stride || !w || !h) {
        return false;
    }
    OH_Pixelmap_ImageInfo* info = nullptr;
    if (OH_PixelmapImageInfo_Create(&info) != IMAGE_SUCCESS || !info) {
        return false;
    }
    bool ok = false;
    int32_t format = 0;
    uint32_t rs = 0;
    uint32_t widthU = 0;
    uint32_t heightU = 0;
    if (OH_PixelmapNative_GetImageInfo(pm, info) == IMAGE_SUCCESS &&
        OH_PixelmapImageInfo_GetPixelFormat(info, &format) == IMAGE_SUCCESS &&
        OH_PixelmapImageInfo_GetRowStride(info, &rs) == IMAGE_SUCCESS &&
        OH_PixelmapImageInfo_GetWidth(info, &widthU) == IMAGE_SUCCESS &&
        OH_PixelmapImageInfo_GetHeight(info, &heightU) == IMAGE_SUCCESS &&
        format == PIXEL_FORMAT_BGRA_8888 &&
        rs >= widthU * 4 &&
        widthU <= static_cast<uint32_t>(INT32_MAX) &&
        heightU <= static_cast<uint32_t>(INT32_MAX)) {
        *stride = rs;
        *w = static_cast<int32_t>(widthU);
        *h = static_cast<int32_t>(heightU);
        ok = true;
    }
    OH_PixelmapImageInfo_Release(info);
    return ok;
}

// Blits one dirty region (compact BGRA rows, w*4 bytes per row) into a
// registered pixelmap. Returns false on any failure so the caller can fall
// back to the ArrayBuffer path (both buffers are then re-written by the
// ArkTS side, so a partial failure between pmA/pmB converges).
bool WriteRegionToPixelmap(OH_PixelmapNative* pm, uint32_t rowStride,
                           int32_t pmW, int32_t pmH,
                           const uint8_t* pixels, int32_t x, int32_t y,
                           int32_t w, int32_t h) {
    if (!pm || !pixels || w <= 0 || h <= 0 || rowStride == 0) {
        return false;
    }
    if (x < 0 || y < 0 || x + w > pmW || y + h > pmH) {
        return false;
    }
    void* addr = nullptr;
    if (OH_PixelmapNative_AccessPixels(pm, &addr) != IMAGE_SUCCESS || !addr) {
        return false;
    }
    const size_t rowBytes = static_cast<size_t>(w) * 4;
    auto* dstBase = static_cast<uint8_t*>(addr);
    const uint8_t* src = pixels;
    for (int32_t row = 0; row < h; row++) {
        uint8_t* dst = dstBase + static_cast<size_t>(y + row) * rowStride +
                       static_cast<size_t>(x) * 4;
        memcpy(dst, src, rowBytes);
        src += rowBytes;
    }
    OH_PixelmapNative_UnaccessPixels(pm);
    return true;
}


// ---------------------------------------------------------------------------
// OSR NativeWindow direct-display registry (display-side optimization).
//
// ArkTS attaches the XComponent(SURFACE)'s OHNativeWindow here (created from
// the surfaceId via OH_NativeWindow_CreateNativeWindowFromSurfaceId — the
// XComponent surface lives in this process, as required). The paint tsfn
// then blits dirty regions DIRECTLY into window buffers (RequestBuffer →
// BufferHandle.virAddr → memcpy → FlushBuffer with a damage Region). The
// compositor consumes the buffer queue vsync-aligned — this replaces the
// PixelMap/@State/Image upload chain entirely (QCefView's QPainter path is
// the Qt equivalent; the damage Region is the HarmonyOS bonus).
//
// All access happens on the JS thread (tsfn CallJs + NAPI entry points);
// the mutex guards against future thread migration.
// ---------------------------------------------------------------------------
struct OsrNativeSurface {
    OHNativeWindow* window = nullptr;
    int32_t width = 0;              // buffer geometry (physical px)
    int32_t height = 0;             // buffer geometry (physical px)
    // Device scale factor (passed at attach). Derives the size tolerance
    // below.
    float deviceScaleFactor = 1.0f;
    // Maximum |CEF frame size - requested size| drift that the retune path
    // accepts. DERIVED, not a magic number: GetViewRect computes CSS =
    // floor(P / dsf) (integer truncation) and CEF paints ceil(CSS * dsf),
    // so for P = q*dsf + r the paint is exactly P - r — the drift is r
    // (P mod dsf), bounded by ceil(dsf). +1 margin for safety.
    int32_t sizeTolerance = 2;
    // Set after the first buffer inspection: true → the surface refused
    // BGRA_8888 and hands out RGBA_8888 buffers, so rows are converted
    // during the blit.
    bool convertToRgba = false;
    // True until the first successful flush (and again after geometry
    // changes): the first frame must declare FULL-buffer damage — the
    // compositor has never presented this surface's content, so a small
    // damage rect would leave the rest of the screen on the initial black
    // state forever. rects=nullptr / rectNumber=0 means "whole buffer is
    // dirty" (documented).
    bool needsFullDamage = true;
    // Diagnostics: consecutive failures / total successful flushes. Each
    // failure branch logs on the 1st and every 60th failure so a hilog
    // capture pinpoints the exact broken step without 60/s spam.
    int32_t failCount = 0;
    int32_t okCount = 0;
};

std::mutex g_osrSurfaceMutex;
std::unordered_map<int64_t, OsrNativeSurface> g_osrSurfaces;

// Waits for the acquire fence returned by RequestBuffer (the buffer may
// still be in use by the previous consumer — the compositor). Bounded wait;
// the fd is closed either way.
void WaitAndCloseFence(int fd) {
    if (fd < 0) {
        return;
    }
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, 100 /* ms, bounded */);
    if (ret == 0) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
            "OSR surface acquire fence wait timed out");
    }
    close(fd);
}

// Presents one frame on the surface: writes the ENTIRE staging frame into a
// requested window buffer (window buffers ROTATE — a flush carrying only
// the dirty region would leave the receiving buffer missing all prior
// content; the damage rect only limits the compositor-side upload), then
// flushes with the dirty region as damage. Returns false on any failure so
// the caller can fall back to the pixelmap/ArrayBuffer path.
// IMPORTANT: the FlushBuffer damage Region uses a BOTTOM-LEFT origin
// (documented: "Region.rect以buffer左下角为坐标原点") while CEF dirty rects
// are top-left — the Y axis is flipped below.
bool WriteRegionToSurface(OsrNativeSurface& s,
                          CefRefPtr<arkui_cef::ArkUICefRenderHandler> handler,
                          int64_t browserId,
                          int32_t x, int32_t y, int32_t w, int32_t h,
                          int32_t viewW, int32_t viewH) {
    if (!s.window || !handler || w <= 0 || h <= 0) {
        return false;
    }
    // Size policy: EXACT equality between the CEF frame size and the buffer
    // geometry is mathematically impossible when physical_size / dsf is not
    // an integer — GetViewRect truncates to integer CSS (1084/1.9 = 570)
    // and CEF paints ScaleToCeiled(CSS * dsf) (= 1083), so the frame is
    // permanently r = (physical mod dsf) pixels off the surface (the old
    // PixelMap path hid this via objectFit Fill stretching). Drift within
    // sizeTolerance (dsf-derived, see OsrNativeSurface): RETUNE the buffer
    // geometry to the frame size — the compositor's RenderFit.RESIZE_FILL
    // scales the buffer to the surface area, making the sub-pixel remainder
    // invisible. Larger mismatch: a resize is in flight — drop the frame
    // and self-heal (post a browser resize to the surface size).
    if (viewW != s.width || viewH != s.height) {
        int dw = viewW > s.width ? viewW - s.width : s.width - viewW;
        int dh = viewH > s.height ? viewH - s.height : s.height - viewH;
        if (dw <= s.sizeTolerance && dh <= s.sizeTolerance && viewW > 0 && viewH > 0) {
            int oldW = s.width;
            int oldH = s.height;
            OH_NativeWindow_NativeWindowHandleOpt(s.window, SET_BUFFER_GEOMETRY,
                                                  viewW, viewH);
            s.width = viewW;
            s.height = viewH;
            s.needsFullDamage = true;
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                "OSRdiag retuned buffer geometry to frame size: %{public}dx%{public}d (was %{public}dx%{public}d)",
                viewW, viewH, oldW, oldH);
        } else {
            if ((++s.failCount % 60) == 1) {
                OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
                    "OSRdiag size mismatch: region=%{public}dx%{public}d, surface=%{public}dx%{public}d, damage=%{public}d,%{public}d %{public}dx%{public}d — posting self-heal resize",
                    viewW, viewH, s.width, s.height, x, y, w, h);
                auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
                if (browser) {
                    CefPostTask(TID_UI, CefCreateClosureTask(
                        base::BindOnce(&arkui_cef::ArkUICefBrowser::resize, browser, s.width, s.height)));
                }
            }
            return false;
        }
    }

    OHNativeWindowBuffer* buffer = nullptr;
    int fenceFd = -1;
    int32_t err = OH_NativeWindow_NativeWindowRequestBuffer(s.window, &buffer, &fenceFd);
    if (err != 0 || !buffer) {
        if ((++s.failCount % 60) == 1) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                "OSRdiag RequestBuffer failed: err=%{public}d", err);
        }
        return false;
    }
    WaitAndCloseFence(fenceFd);

    // Buffer metadata (geometry/stride/format). The CPU mapping itself is
    // done via the documented path below — the raw BufferHandle.virAddr is
    // NOT used: without Map/Unmap cache maintenance, CPU writes to the DMA
    // buffer are not guaranteed visible to the compositor (observed as a
    // permanently black surface).
    BufferHandle* bh = OH_NativeWindow_GetBufferHandleFromNative(buffer);
    if (!bh || bh->stride < bh->width * 4) {
        OH_NativeWindow_NativeWindowAbortBuffer(s.window, buffer);
        if ((++s.failCount % 60) == 1) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                "OSRdiag bad buffer handle: bh=%{public}d, fmt=%{public}d, stride=%{public}d, size=%{public}dx%{public}d",
                bh != nullptr, bh ? bh->format : -1, bh ? bh->stride : -1,
                bh ? bh->width : -1, bh ? bh->height : -1);
        }
        return false;
    }

    // Format negotiation: we asked for BGRA_8888 via SET_FORMAT, but the
    // surface is free to hand out RGBA_8888 — detect once, convert per-row
    // afterwards. Any other format bails to the fallback path.
    if (!s.convertToRgba && bh->format != NATIVEBUFFER_PIXEL_FMT_BGRA_8888) {
        if (bh->format == NATIVEBUFFER_PIXEL_FMT_RGBA_8888) {
            s.convertToRgba = true;
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                "OSR surface is RGBA_8888 — enabling BGRA->RGBA row conversion");
        } else {
            OH_NativeWindow_NativeWindowAbortBuffer(s.window, buffer);
            s.failCount++;
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                "OSRdiag surface format unsupported: %{public}d (BGRA=%{public}d, RGBA=%{public}d)",
                bh->format, static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_BGRA_8888),
                static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_RGBA_8888));
            return false;
        }
    }

    // The window buffer must be at least as large as the staging frame
    // (geometry matches by the size check above; this is the safety net).
    if (bh->width < viewW || bh->height < viewH) {
        OH_NativeWindow_NativeWindowAbortBuffer(s.window, buffer);
        if ((++s.failCount % 60) == 1) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                "OSRdiag buffer smaller than frame: buf=%{public}dx%{public}d, frame=%{public}dx%{public}d",
                bh->width, bh->height, viewW, viewH);
        }
        return false;
    }

    // Documented CPU write path (native_buffer.h): FromNativeWindowBuffer
    // converts the window buffer, Map maps the underlying memory into this
    // process (requires the CPU_READ usage we set at attach — see the Map
    // doc note), Unmap releases the mapping with cache maintenance so the
    // compositor observes the writes. API 9-12, available at 17.
    // NOTE: FromNativeWindowBuffer is a BORROWED conversion — unlike its
    // reverse (CreateNativeWindowBufferFromNativeBuffer, documented as
    // "creates a new instance, pair with Destroy"), this direction does NOT
    // add a reference. Do NOT call Unreference on it — doing so decrements
    // the surface library's own refcount and freed the buffer inside the
    // queue (crash in ProducerSurface::FlushBuffer after ~30s of frame
    // churn).
    OH_NativeBuffer* nb = nullptr;
    if (OH_NativeBuffer_FromNativeWindowBuffer(buffer, &nb) != 0 || !nb) {
        OH_NativeWindow_NativeWindowAbortBuffer(s.window, buffer);
        if ((++s.failCount % 60) == 1) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                "OSRdiag FromNativeWindowBuffer failed");
        }
        return false;
    }
    void* mapped = nullptr;
    int32_t mapErr = OH_NativeBuffer_Map(nb, &mapped);
    if (mapErr != 0 || !mapped) {
        OH_NativeWindow_NativeWindowAbortBuffer(s.window, buffer);
        if ((++s.failCount % 60) == 1) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                "OSRdiag Map failed: err=%{public}d (check CPU_READ usage)", mapErr);
        }
        return false;
    }

    // Full-frame blit from the staging buffer (handler-internal mutex held
    // during the copy; the fence wait above is already done so the CEF UI
    // thread block is bounded by the copy itself).
    auto* dstBase = static_cast<uint8_t*>(mapped);
    bool blitOk = handler->BlitFullFrameTo(dstBase, static_cast<size_t>(bh->stride), s.convertToRgba);
    OH_NativeBuffer_Unmap(nb);
    if (!blitOk) {
        OH_NativeWindow_NativeWindowAbortBuffer(s.window, buffer);
        if ((++s.failCount % 60) == 1) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                "OSRdiag BlitFullFrameTo failed (no staging frame?)");
        }
        return false;
    }

    // Damage rect: top-left → bottom-left origin. rectNumber = 1 (merged
    // dirty rect from the staging pull). Rect.w/h are uint32_t. The FIRST
    // flush after attach/geometry change declares an EXPLICIT full-buffer
    // rect (avoid the nullptr/rectNumber=0 "full damage" shorthand — an
    // empty damage list behaves differently across chipset lib versions).
    Region::Rect damageRect = {};
    if (s.needsFullDamage) {
        damageRect.x = 0;
        damageRect.y = 0;
        damageRect.w = static_cast<uint32_t>(s.width);
        damageRect.h = static_cast<uint32_t>(s.height);
    } else {
        damageRect.x = x;
        damageRect.y = s.height - (y + h);
        damageRect.w = static_cast<uint32_t>(w);
        damageRect.h = static_cast<uint32_t>(h);
    }
    Region damage = {};
    damage.rects = &damageRect;
    damage.rectNumber = 1;

    // fenceFd = -1: the CPU write is complete at this point, no release
    // fence is needed.
    err = OH_NativeWindow_NativeWindowFlushBuffer(s.window, buffer, -1, damage);
    if (err != 0) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
            "OSRdiag FlushBuffer failed: err=%{public}d", err);
        return false;
    }
    s.needsFullDamage = false;
    s.failCount = 0;
    s.okCount++;
    if (s.okCount == 1 || (s.okCount % 300) == 0) {
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
            "OSRdiag surface frame #%{public}d flushed: damage=%{public}d,%{public}d %{public}dx%{public}d (full=%{public}d), frame=%{public}dx%{public}d, fmt=%{public}d",
            s.okCount, x, y, w, h, damage.rectNumber == 0 ? 1 : 0,
            viewW, viewH, bh->format);
    }
    return true;
}

void ReleaseOsrNativeSurface(OsrNativeSurface& s) {
    if (s.window) {
        OH_NativeWindow_DestroyNativeWindow(s.window);
        s.window = nullptr;
    }
    s.width = 0;
    s.height = 0;
    s.deviceScaleFactor = 1.0f;
    s.sizeTolerance = 2;
    s.convertToRgba = false;
    s.needsFullDamage = true;
    s.failCount = 0;
    s.okCount = 0;
}
using arkui_cef::ArkUICefBrowser;
using arkui_cef::ArkUICefApp;
using arkui_cef::PaintCallback;
using EMBEDDED_WINDOW_ADAPTER::Node;

// HarmonyOS KeyType values (from @kit.ArkUI)
constexpr int32_t OH_KEYTYPE_DOWN = 0;
constexpr int32_t OH_KEYTYPE_UP   = 1;

// HarmonyOS KeyCode values (verified from oh_key_code.h in OHOS SDK)
// Letters A-Z: KEYCODE_A=2017 .. KEYCODE_Z=2042
// Digits 0-9: KEYCODE_0=2000 .. KEYCODE_9=2009
constexpr int32_t OH_KEYCODE_DPAD_UP    = 2012;
constexpr int32_t OH_KEYCODE_DPAD_DOWN  = 2013;
constexpr int32_t OH_KEYCODE_DPAD_LEFT  = 2014;
constexpr int32_t OH_KEYCODE_DPAD_RIGHT = 2015;
constexpr int32_t OH_KEYCODE_TAB        = 2049;
constexpr int32_t OH_KEYCODE_SPACE      = 2050;
constexpr int32_t OH_KEYCODE_ENTER      = 2054;
constexpr int32_t OH_KEYCODE_BACKSPACE  = 2055;  // KEYCODE_DEL
constexpr int32_t OH_KEYCODE_FORWARD_DEL = 2071;
constexpr int32_t OH_KEYCODE_ALT_LEFT   = 2045;
constexpr int32_t OH_KEYCODE_SHIFT_LEFT = 2047;
constexpr int32_t OH_KEYCODE_CTRL_LEFT  = 2072;
constexpr int32_t OH_KEYCODE_ESCAPE     = 2070;
constexpr int32_t OH_KEYCODE_CAPS_LOCK  = 2074;
constexpr int32_t OH_KEYCODE_PAGE_UP    = 2068;
constexpr int32_t OH_KEYCODE_PAGE_DOWN  = 2069;
constexpr int32_t OH_KEYCODE_MOVE_HOME  = 2081;
constexpr int32_t OH_KEYCODE_MOVE_END   = 2082;
constexpr int32_t OH_KEYCODE_F1  = 2090;
constexpr int32_t OH_KEYCODE_F2  = 2091;
constexpr int32_t OH_KEYCODE_F3  = 2092;
constexpr int32_t OH_KEYCODE_F4  = 2093;
constexpr int32_t OH_KEYCODE_F5  = 2094;
constexpr int32_t OH_KEYCODE_F6  = 2095;
constexpr int32_t OH_KEYCODE_F7  = 2096;
constexpr int32_t OH_KEYCODE_F8  = 2097;
constexpr int32_t OH_KEYCODE_F9  = 2098;
constexpr int32_t OH_KEYCODE_F10 = 2099;
constexpr int32_t OH_KEYCODE_F11 = 2100;
constexpr int32_t OH_KEYCODE_F12 = 2101;

// Windows virtual key codes
constexpr int32_t VK_BACK   = 0x08;
constexpr int32_t VK_TAB    = 0x09;
constexpr int32_t VK_RETURN = 0x0D;
constexpr int32_t VK_SHIFT  = 0x10;
constexpr int32_t VK_CONTROL= 0x11;
constexpr int32_t VK_MENU   = 0x12;  // Alt
constexpr int32_t VK_ESCAPE = 0x1B;
constexpr int32_t VK_SPACE  = 0x20;
constexpr int32_t VK_DELETE = 0x2E;
constexpr int32_t VK_LEFT   = 0x25;
constexpr int32_t VK_UP     = 0x26;
constexpr int32_t VK_RIGHT  = 0x27;
constexpr int32_t VK_DOWN   = 0x28;
constexpr int32_t VK_PRIOR  = 0x21;  // Page Up
constexpr int32_t VK_NEXT   = 0x22;  // Page Down
constexpr int32_t VK_HOME   = 0x23;
constexpr int32_t VK_END    = 0x24;
constexpr int32_t VK_CAPITAL= 0x14;  // Caps Lock
constexpr int32_t VK_F1  = 0x70;
constexpr int32_t VK_F2  = 0x71;
constexpr int32_t VK_F3  = 0x72;
constexpr int32_t VK_F4  = 0x73;
constexpr int32_t VK_F5  = 0x74;
constexpr int32_t VK_F6  = 0x75;
constexpr int32_t VK_F7  = 0x76;
constexpr int32_t VK_F8  = 0x77;
constexpr int32_t VK_F9  = 0x78;
constexpr int32_t VK_F10 = 0x79;
constexpr int32_t VK_F11 = 0x7A;
constexpr int32_t VK_F12 = 0x7B;

CefKeyEvent convertKeyEvent(int32_t keyCode, int32_t keyType, uint32_t modifiers) {
    // HarmonyOS KeyType -> CEF key event type
    // OH_KEYTYPE_DOWN=0, OH_KEYTYPE_UP=1
    // CEF: KEYEVENT_RAWKEYDOWN=0, KEYEVENT_KEYUP=1, KEYEVENT_CHAR=2
    cef_key_event_type_t cefType = KEYEVENT_RAWKEYDOWN;
    if (keyType == OH_KEYTYPE_UP) {
        cefType = KEYEVENT_KEYUP;
    } else if (keyType == 2) {
        // KEYEVENT_CHAR — ArkTS sends type=2 for character events
        cefType = KEYEVENT_CHAR;
    }

    // HarmonyOS KeyCode -> Windows virtual key code
    int32_t winVk = 0;
    // Character for KEYEVENT_CHAR (0 = no char event needed)
    char16_t character = 0;

    // Letters A-Z: KEYCODE_A=2017 .. KEYCODE_Z=2042
    if (keyCode >= 2017 && keyCode <= 2042) {
        winVk = 'A' + (keyCode - 2017);
        if (modifiers & 0x2) {
            character = static_cast<char16_t>('A' + (keyCode - 2017));
        } else {
            character = static_cast<char16_t>('a' + (keyCode - 2017));
        }
    } else if (keyCode >= 2000 && keyCode <= 2009) {
        // Digits 0-9: KEYCODE_0=2000 .. KEYCODE_9=2009
        winVk = '0' + (keyCode - 2000);
        character = static_cast<char16_t>('0' + (keyCode - 2000));
    } else {
        switch (keyCode) {
            case OH_KEYCODE_ENTER:       winVk = VK_RETURN; character = u'\r'; break;
            case OH_KEYCODE_BACKSPACE:   winVk = VK_BACK;   character = u'\b'; break;
            case OH_KEYCODE_FORWARD_DEL: winVk = VK_DELETE; break;
            case OH_KEYCODE_SPACE:       winVk = VK_SPACE;  character = u' '; break;
            case OH_KEYCODE_TAB:         winVk = VK_TAB;    character = u'\t'; break;
            case OH_KEYCODE_ESCAPE:      winVk = VK_ESCAPE; break;
            case OH_KEYCODE_CTRL_LEFT:   winVk = VK_CONTROL;break;
            case OH_KEYCODE_SHIFT_LEFT:  winVk = VK_SHIFT;  break;
            case OH_KEYCODE_ALT_LEFT:    winVk = VK_MENU;   break;
            case OH_KEYCODE_CAPS_LOCK:   winVk = VK_CAPITAL;break;
            case OH_KEYCODE_PAGE_UP:     winVk = VK_PRIOR;  break;
            case OH_KEYCODE_PAGE_DOWN:   winVk = VK_NEXT;   break;
            case OH_KEYCODE_MOVE_HOME:   winVk = VK_HOME;   break;
            case OH_KEYCODE_MOVE_END:    winVk = VK_END;    break;
            case OH_KEYCODE_DPAD_LEFT:   winVk = VK_LEFT;   break;
            case OH_KEYCODE_DPAD_UP:     winVk = VK_UP;     break;
            case OH_KEYCODE_DPAD_RIGHT:  winVk = VK_RIGHT;  break;
            case OH_KEYCODE_DPAD_DOWN:   winVk = VK_DOWN;   break;
            case OH_KEYCODE_F1:  winVk = VK_F1;  break;
            case OH_KEYCODE_F2:  winVk = VK_F2;  break;
            case OH_KEYCODE_F3:  winVk = VK_F3;  break;
            case OH_KEYCODE_F4:  winVk = VK_F4;  break;
            case OH_KEYCODE_F5:  winVk = VK_F5;  break;
            case OH_KEYCODE_F6:  winVk = VK_F6;  break;
            case OH_KEYCODE_F7:  winVk = VK_F7;  break;
            case OH_KEYCODE_F8:  winVk = VK_F8;  break;
            case OH_KEYCODE_F9:  winVk = VK_F9;  break;
            case OH_KEYCODE_F10: winVk = VK_F10; break;
            case OH_KEYCODE_F11: winVk = VK_F11; break;
            case OH_KEYCODE_F12: winVk = VK_F12; break;
            default: break;
        }
    }

    CefKeyEvent event = {};
    event.size = sizeof(CefKeyEvent);
    event.type = cefType;
    event.modifiers = modifiers;
    event.windows_key_code = winVk;
    event.native_key_code = keyCode;
    event.is_system_key = 0;
    event.character = character;
    event.unmodified_character = character;
    event.focus_on_editable_field = 1;
    return event;
}

// ==================== OSR Window Key Event Filter ====================
// Intercepts physical keyboard events at the WINDOW level — BEFORE the IME.
// This lets digits, symbols, and Ctrl shortcuts reach CEF even when
// inputMethodController.attach() is active (needed for CJK IME).
// Letters A-Z are let through to the IME for composition handling.
// Only active when renderAreaActive is true (set from ArkTS when user
// clicks the OSR render area, not the URL bar).

struct OsrKeyFilterContext {
    int64_t browserId;
    std::atomic<bool> renderAreaActive{false};
    std::atomic<bool> ctrlPressed{false};
    std::atomic<bool> shiftPressed{false};
    std::atomic<bool> altPressed{false};
};

static OsrKeyFilterContext* g_keyFilterCtx = nullptr;

static bool OsrKeyFilterCallback(Input_KeyEvent* keyEvent) {
    if (!keyEvent || !g_keyFilterCtx) return false;

    // Only filter when OSR render area is active (not when URL bar has focus)
    if (!g_keyFilterCtx->renderAreaActive.load()) {
        return false;
    }

    int32_t keyCode = OH_Input_GetKeyEventKeyCode(keyEvent);
    int32_t action = OH_Input_GetKeyEventAction(keyEvent);

    // Load modifier state BEFORE updating — the current key event should
    // carry the modifier state that existed BEFORE this key was pressed.
    // E.g., Ctrl key-down event itself should NOT have CONTROL_DOWN modifier.
    bool ctrl = g_keyFilterCtx->ctrlPressed.load();
    bool shift = g_keyFilterCtx->shiftPressed.load();
    bool alt = g_keyFilterCtx->altPressed.load();

    // Update modifier tracking AFTER loading previous state
    if (keyCode == 2072) { g_keyFilterCtx->ctrlPressed.store(action == KEY_ACTION_DOWN); }
    else if (keyCode == 2047) { g_keyFilterCtx->shiftPressed.store(action == KEY_ACTION_DOWN); }
    else if (keyCode == 2045) { g_keyFilterCtx->altPressed.store(action == KEY_ACTION_DOWN); }

    // Letters A-Z without Ctrl/Alt: let IME handle (CJK composition + English text)
    // KeyCode range: KEYCODE_A=2017 .. KEYCODE_Z=2042 (verified from oh_key_code.h)
    if (keyCode >= 2017 && keyCode <= 2042 && !ctrl && !alt) {
        return false;
    }

    // Everything else (digits, symbols, Ctrl shortcuts, Backspace, arrows, etc.):
    // Filter (consume) and forward to CEF via key events
    int32_t cefType = (action == KEY_ACTION_DOWN) ? 0 /*RAWKEYDOWN*/ : 1 /*KEYUP*/;
    uint32_t cefMods = 0;
    if (shift) cefMods |= 0x02;
    if (ctrl)  cefMods |= 0x04;
    if (alt)   cefMods |= 0x08;

    bool isTextKey =
        (keyCode >= 2000 && keyCode <= 2009) ||  // Digits 0-9
        keyCode == 2050 ||                        // Space (KEYCODE_SPACE)
        keyCode == 2054 ||                        // Enter (KEYCODE_ENTER)
        keyCode == 2049;                          // Tab (KEYCODE_TAB)
    bool sendChar = isTextKey && !ctrl && !alt && (action == KEY_ACTION_DOWN);

    CefKeyEvent cefKeyEvent = convertKeyEvent(keyCode, cefType, cefMods);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(g_keyFilterCtx->browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendKeyEvent, browser, cefKeyEvent)));

        if (sendChar) {
            CefKeyEvent cefCharEvent = convertKeyEvent(keyCode, 2 /*CHAR*/, cefMods);
            CefPostTask(TID_UI, CefCreateClosureTask(
                base::BindOnce(&ArkUICefBrowser::sendKeyEvent, browser, cefCharEvent)));
        }
    }

    return true; // Filter (consume) — IME doesn't see this key
}

struct CefCreatedCallbackContext {
    napi_threadsafe_function tsfn = nullptr;
};

struct CefCreatedCallbackData {
    int64_t browserId = -1;
};

// Custom threadsafe function for OSR paint notifications.
// JsThreadSafeCallback doesn't support ArrayBuffer, so we need a dedicated
// threadsafe function that delivers the pending dirty region as an ArrayBuffer.
//
// Pixel data itself lives in the ArkUICefRenderHandler's persistent staging
// buffer (blitted there on the CEF UI thread, never dropped). This tsfn only
// carries a "frame ready" notification which is coalesced by framePending_:
// when the JS thread is still busy, later frames merge into the accumulated
// dirty region instead of queueing up — content is never lost, notifications
// are. This mirrors QCefView's widget->update() coalescing.
struct OsrPaintTsfn {
    napi_threadsafe_function tsfn = nullptr;
    // Notification coalescing (formerly frame skipping — with the staging
    // buffer no pixel data is dropped, only redundant notifications).
    std::atomic<bool> framePending_{false};
    // Browser id is assigned after createOsrBrowser() returns; until then
    // notifications are delivered but consumed as "no data" (safe — the
    // accumulated dirty region stays in the render handler).
    std::atomic<int64_t> browserId_{-1};
    // Reused region buffer to avoid per-frame heap traffic.
    arkui_cef::OsrPendingRegion region_;

    ~OsrPaintTsfn() {
        Release();
    }

    bool Init(napi_env env, napi_value jsFunction, const char* resourceName) {
        if (!env || !jsFunction) return false;

        napi_valuetype type;
        napi_status status = napi_typeof(env, jsFunction, &type);
        if (status != napi_ok || type != napi_function) return false;

        napi_value resource;
        status = napi_create_string_utf8(env, resourceName, NAPI_AUTO_LENGTH, &resource);
        if (status != napi_ok) return false;

        status = napi_create_threadsafe_function(
            env, jsFunction, nullptr, resource,
            0, 1, nullptr, nullptr, this,
            &OsrPaintTsfn::CallJs, &tsfn);
        return status == napi_ok && tsfn != nullptr;
    }

    void SetBrowserId(int64_t id) {
        browserId_.store(id);
    }

    // Called from the CEF UI thread (inside ArkUICefRenderHandler::OnPaint,
    // with the handler's mutex held): must stay lock-free and non-blocking.
    void Notify() {
        if (!tsfn) return;

        // Coalesce: if a notification is still pending in the JS thread queue,
        // skip this one — the JS side will consume the accumulated dirty
        // region (which already includes this frame) when it catches up.
        bool expected = false;
        if (!framePending_.compare_exchange_strong(expected, true)) {
            return;
        }

        napi_status status = napi_call_threadsafe_function(tsfn, nullptr, napi_tsfn_nonblocking);
        if (status != napi_ok) {
            framePending_.store(false);
        }
    }

    void Release() {
        if (tsfn) {
            napi_release_threadsafe_function(tsfn, napi_tsfn_release);
            tsfn = nullptr;
        }
    }

    static void CallJs(napi_env env, napi_value jsCallback, void* context, void* data) {
        // data is always nullptr — notifications carry no payload; the pixel
        // data is pulled from the render handler's staging buffer below.
        (void)data;
        auto* self = static_cast<OsrPaintTsfn*>(context);
        if (!self) return;

        // Pull the accumulated dirty region METADATA from the render handler
        // on this (JS) thread. Looked up by browser id to avoid a reference
        // cycle (handler -> paintCallback -> tsfn -> handler). region_ is
        // reused across calls to avoid per-frame heap traffic. The pixel
        // rows are compacted lazily below — the NativeWindow path (the
        // common case) never needs them.
        arkui_cef::OsrPendingRegion& region = self->region_;
        bool hasRegion = false;
        int64_t browserId = self->browserId_.load();
        CefRefPtr<arkui_cef::ArkUICefRenderHandler> handler;
        if (browserId > 0) {
            handler = arkui_cef::ArkUICefManager::getInstance().getRenderHandler(browserId);
            if (handler) {
                hasRegion = handler->ConsumePendingRegionMeta(region);
            }
        }

        if (env && jsCallback && hasRegion && region.w > 0 && region.h > 0) {
            // Priority 1: XComponent NativeWindow direct display. Writes the
            // FULL frame straight into a window buffer and flushes it with a
            // damage rect — no pixel compaction, no PixelMap, no JS work. On
            // success the JS callback is skipped entirely (there is nothing
            // to swap or rebuild).
            bool surfaceWritten = false;
            {
                std::lock_guard<std::mutex> lock(g_osrSurfaceMutex);
                auto it = g_osrSurfaces.find(browserId);
                if (it != g_osrSurfaces.end() && handler) {
                    surfaceWritten = WriteRegionToSurface(
                        it->second, handler, browserId,
                        region.x, region.y, region.w, region.h,
                        region.viewW, region.viewH);
                }
            }
            if (surfaceWritten) {
                self->framePending_.store(false);
                return;
            }

            // Priority 2 / 3: PixelMap native write / ArrayBuffer path.
            // Only reached when no surface is attached or the surface write
            // failed (format mismatch, resize window, queue error...).
            // Compact the pixel rows on demand (the fallback paths need the
            // actual data). If a resize raced the meta consume, |pixels| is
            // empty and this frame is dropped (a fresh full frame follows).
            if (handler) {
                handler->CompactRegionPixels(region);
            }
            if (region.pixels.empty()) {
                self->framePending_.store(false);
                return;
            }
            // PixelMap path: write the dirty region into both registered
            // buffers (kept identical for the dirty-region protocol) and
            // hand only metadata to JS. If anything fails, fall back to the
            // ArrayBuffer path — the ArkTS side re-writes both buffers via
            // writePixelsSync, so a partial native failure converges.
            bool nativeWritten = false;
            if (!region.fullFrame) {
                OsrNativePixelmaps pms;
                bool registered = false;
                {
                    std::lock_guard<std::mutex> lock(g_osrPixelmapsMutex);
                    auto it = g_osrPixelmaps.find(browserId);
                    if (it != g_osrPixelmaps.end()) {
                        pms = it->second;
                        registered = true;
                    }
                }
                if (registered && pms.pmA && pms.pmB &&
                    pms.width == region.viewW && pms.height == region.viewH) {
                    nativeWritten =
                        WriteRegionToPixelmap(pms.pmA, pms.rowStride, pms.width,
                                              pms.height, region.pixels.data(),
                                              region.x, region.y, region.w, region.h) &&
                        WriteRegionToPixelmap(pms.pmB, pms.rowStride, pms.width,
                                              pms.height, region.pixels.data(),
                                              region.x, region.y, region.w, region.h);
                    if (!nativeWritten) {
                        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
                            "OSR native pixelmap write failed, falling back to ArrayBuffer");
                    }
                }
            }

            // Region metadata (shared by both paths).
            napi_value xVal, yVal, wVal, hVal, fullFrameVal, viewWVal, viewHVal;
            napi_create_int32(env, region.x, &xVal);
            napi_create_int32(env, region.y, &yVal);
            napi_create_int32(env, region.w, &wVal);
            napi_create_int32(env, region.h, &hVal);
            napi_get_boolean(env, region.fullFrame, &fullFrameVal);
            napi_create_int32(env, region.viewW, &viewWVal);
            napi_create_int32(env, region.viewH, &viewHVal);
            napi_value global;
            napi_get_global(env, &global);

            if (nativeWritten) {
                // Metadata-only call: regionData is undefined — the pixels
                // were already written natively; JS just swaps the display.
                napi_value undefined;
                napi_get_undefined(env, &undefined);
                napi_value argv[8] = { undefined, xVal, yVal, wVal, hVal,
                                       fullFrameVal, viewWVal, viewHVal };
                napi_value result;
                napi_call_function(env, global, jsCallback, 8, argv, &result);
            } else {
                // Copy into a real ArrayBuffer. full-frame payloads feed the
                // ASYNC createPixelMap, which may read the buffer after this
                // CallJs returns — by then the next frame's
                // CompactRegionPixels has already reused/overwritten the
                // shared region_ buffer, so a copy is required.
                napi_value arrayBuffer;
                void* bufferData = nullptr;
                napi_status status = napi_create_arraybuffer(
                    env, region.pixels.size(), &bufferData, &arrayBuffer);
                if (status == napi_ok) {
                    memcpy(bufferData, region.pixels.data(), region.pixels.size());
                    napi_value argv[8] = { arrayBuffer, xVal, yVal, wVal, hVal,
                                           fullFrameVal, viewWVal, viewHVal };
                    napi_value result;
                    napi_call_function(env, global, jsCallback, 8, argv, &result);
                }
            }
        }

        // Clear the pending flag so the next notification can be delivered.
        self->framePending_.store(false);
    }
};

struct OsrPaintCallbackHolder {
    std::shared_ptr<OsrPaintTsfn> tsfn;
    explicit OsrPaintCallbackHolder(std::shared_ptr<OsrPaintTsfn> t) : tsfn(std::move(t)) {}
};

struct OsrStartDragData {
    std::string text;
    std::string html;
    std::string url;
    std::string urlTitle;
    std::string fileName;
    std::vector<uint8_t> fileData;
    int x;
    int y;
};

struct OsrStartDragTsfn {
    napi_threadsafe_function tsfn = nullptr;

    ~OsrStartDragTsfn() { Release(); }

    bool Init(napi_env env, napi_value jsFunction, const char* resourceName) {
        if (!env || !jsFunction) return false;
        napi_valuetype type;
        napi_status status = napi_typeof(env, jsFunction, &type);
        if (status != napi_ok || type != napi_function) return false;
        napi_value resource;
        status = napi_create_string_utf8(env, resourceName, NAPI_AUTO_LENGTH, &resource);
        if (status != napi_ok) return false;
        status = napi_create_threadsafe_function(
            env, jsFunction, nullptr, resource,
            0, 1, nullptr, nullptr, this,
            &OsrStartDragTsfn::CallJs, &tsfn);
        return status == napi_ok && tsfn != nullptr;
    }

    void Call(const std::string& text, const std::string& html,
              const std::string& url, const std::string& urlTitle,
              const std::string& fileName, const std::vector<uint8_t>& fileData,
              int x, int y) {
        if (!tsfn) return;
        auto* data = new OsrStartDragData{text, html, url, urlTitle, fileName, fileData, x, y};
        napi_status status = napi_call_threadsafe_function(tsfn, data, napi_tsfn_nonblocking);
        if (status != napi_ok) {
            delete data;
        }
    }

    void Release() {
        if (tsfn) {
            napi_release_threadsafe_function(tsfn, napi_tsfn_release);
            tsfn = nullptr;
        }
    }

    static void CallJs(napi_env env, napi_value jsCallback, void* context, void* data) {
        auto* dragData = static_cast<OsrStartDragData*>(data);
        if (!dragData) return;
        if (!env || !jsCallback) {
            delete dragData;
            return;
        }

        napi_value text, html, url, urlTitle, fileName, fileDataAb, xVal, yVal;
        napi_create_string_utf8(env, dragData->text.c_str(), NAPI_AUTO_LENGTH, &text);
        napi_create_string_utf8(env, dragData->html.c_str(), NAPI_AUTO_LENGTH, &html);
        napi_create_string_utf8(env, dragData->url.c_str(), NAPI_AUTO_LENGTH, &url);
        napi_create_string_utf8(env, dragData->urlTitle.c_str(), NAPI_AUTO_LENGTH, &urlTitle);
        napi_create_string_utf8(env, dragData->fileName.c_str(), NAPI_AUTO_LENGTH, &fileName);

        void* bufferData = nullptr;
        napi_create_arraybuffer(env, dragData->fileData.size(), &bufferData, &fileDataAb);
        if (!dragData->fileData.empty()) {
            memcpy(bufferData, dragData->fileData.data(), dragData->fileData.size());
        }

        napi_create_int32(env, dragData->x, &xVal);
        napi_create_int32(env, dragData->y, &yVal);

        napi_value argv[8] = { text, html, url, urlTitle, fileName, fileDataAb, xVal, yVal };
        napi_value global;
        napi_get_global(env, &global);
        napi_value result;
        napi_call_function(env, global, jsCallback, 8, argv, &result);

        delete dragData;
    }
};

Node* createParentNode() {
    ArkUI_NativeNodeAPI_1* api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
    if (!api) {
        return nullptr;
    }
    
    ArkUI_NodeHandle stack = api->createNode(ARKUI_NODE_STACK);
    if (!stack) {
        return nullptr;
    }
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "parentStack: %{public}p", stack);
    Node* node = new Node{};
    node->container = stack;
    node->nodeType = ARKUI_NODE_XCOMPONENT;
    
    return node;
}

void destroyNode(Node* node) {
    if (!node) {
        return;
    }
    
    ArkUI_NativeNodeAPI_1* api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
    if (api) {
        if (node->container) {
        api->disposeNode(node->container);
        }
        if (node->node) {
            api->disposeNode(node->node);
        }
    }
    delete node;
}

} // namespace

static napi_value NAPI_Global_startCef(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "startCef called");
    
    arkui_cef::ArkUICefManager::getInstance().start();
    return nullptr;
}

static napi_value NAPI_Global_isCefRunning(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "isCefRunning called");
    
    bool ok = arkui_cef::ArkUICefManager::getInstance().isCefRunning();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "isCefRunning: %{public}d", ok);
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value NAPI_Global_createCefView(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "createCefView called");
    
    size_t argc = 7;
    napi_value args[7];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 7) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "createCefView: expected 7 arguments, got %{public}zu", argc);
        napi_throw_error(env, nullptr, "Expected 7 arguments: nodeContent, url, width, height, onCreated, onTitleChange, onFocusTake");
        return nullptr;
    }
    
    ArkUI_NodeContentHandle contentHandle = nullptr;
    OH_ArkUI_GetNodeContentFromNapiValue(env, args[0], &contentHandle);
    if (contentHandle == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: getNodeContentHandleFromNapiValue failed");
        napi_throw_error(env, nullptr, "Failed to get NodeContent handle");
        return nullptr;
    }
    
    char url[1024] = {};
    size_t urlLen;
    napi_get_value_string_utf8(env, args[1], url, sizeof(url), &urlLen);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", 
        "createCefView: url=%{public}s", url);
    
    int32_t width = 0;
    napi_get_value_int32(env, args[2], &width);
    
    int32_t height = 0;
    napi_get_value_int32(env, args[3], &height);
    
    auto jsCreatedCallback = makeJsThreadSafeCallback<int64_t>(env, args[4], "CefCreatedCallback");
    auto jsTitleChangeCallback = makeJsThreadSafeCallback<std::string>(env, args[5], "CefTitleChangeCallback");
    auto jsFocusTakeCallback = makeJsThreadSafeCallback<bool>(env, args[6], "CefFocusTakeCallback");
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", 
        "createCefView: width=%{public}d, height=%{public}d", width, height);
    
    Node* parentNode = createParentNode();
    if (parentNode == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: createStackNode failed");
        napi_throw_error(env, nullptr, "Failed to create Stack node");
        return nullptr;
    }
    
    int32_t err = OH_ArkUI_NodeContent_AddNode(contentHandle, parentNode->container);
    if (err != ARKUI_ERROR_CODE_NO_ERROR) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: addStackToNodeContent failed");
        delete parentNode;
        napi_throw_error(env, nullptr, "Failed to add Stack to NodeContent");
        return nullptr;
    }
    
    err = OH_ArkUI_NodeContent_SetUserData(contentHandle, parentNode);
    if (err != ARKUI_ERROR_CODE_NO_ERROR) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: setUserData failed");
        delete parentNode;
        napi_throw_error(env, nullptr, "Failed to add user data to NodeContent");
        return nullptr;
    }
    
    int64_t browserId = arkui_cef::ArkUICefManager::getInstance().createBrowser(
       parentNode, url, width, height,
       [jsCreatedCallback](int64_t id) {
        jsCreatedCallback->Call(id);
        jsCreatedCallback->Release();
       },
       [jsTitleChangeCallback](const std::string& title) {
        jsTitleChangeCallback->Call(title);
       },
       [jsFocusTakeCallback](bool next) {
        jsFocusTakeCallback->Call(next);
       });
    
    napi_value result;
    napi_create_int64(env, browserId, &result);
    return result;
}

static napi_value NAPI_Global_destroyCefView(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "destroyCefView called");
    
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "destroyCefView id: %{public}ld",
                 browserId);
    
    ArkUI_NodeContentHandle contentHandle = nullptr;
    OH_ArkUI_GetNodeContentFromNapiValue(env, args[1], &contentHandle);
    if (contentHandle == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: getNodeContentHandleFromNapiValue failed");
        napi_throw_error(env, nullptr, "Failed to get NodeContent handle");
        return nullptr;
    }
    
    arkui_cef::ArkUICefManager::getInstance().destroyBrowser(browserId);
    
    Node* parentNode = static_cast<Node*>(OH_ArkUI_NodeContent_GetUserData(contentHandle));
    if (parentNode) {
        int32_t err = OH_ArkUI_NodeContent_RemoveNode(contentHandle, parentNode->container);
        if (err != ARKUI_ERROR_CODE_NO_ERROR) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: removeNode failed");
        }
        destroyNode(parentNode);
    }
    
    return nullptr;
}

static napi_value NAPI_Global_cefResize(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefRisize called");
    
    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 3) {
        napi_throw_error(env, nullptr, "Expected 3 arguments: browserId, width, height");
        return nullptr;
    }
    
    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    
    int32_t width = 0;
    napi_get_value_int32(env, args[1], &width);
    
    int32_t height = 0;
    napi_get_value_int32(env, args[2], &height);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefRisize %{public}ld, %{public}d X %{public}d",
                 browserId, width, height);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::resize, browser, width, height)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefGoBack(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefGoBack %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::goBack, browser)));
    }
    
    return nullptr;
}

static napi_value NAPI_Global_cefGoForward(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefGoForward %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::goForward, browser)));
    }
    
    return nullptr;
}

static napi_value NAPI_Global_cefReload(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefReload %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::reload, browser)));
    }
    
    return nullptr;
}

static napi_value NAPI_Global_cefHide(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefHide %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::hide, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefShow(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefShow %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::show, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefLoadUrl(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, url");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    char url[1024] = {};
    size_t urlLen;
    napi_get_value_string_utf8(env, args[1], url, sizeof(url), &urlLen);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefLoadUrl %{public}ld, url=%{public}s", 
                 browserId, url);
    
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::loadUrl, browser, std::string(url))));
    }

    return nullptr;
}

static napi_value NAPI_Global_cefSendKeyEvent(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 4) {
        napi_throw_error(env, nullptr,
            "Expected 4-5 arguments: browserId, keyCode, keyType, modifiers, charCode?");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t keyCode = 0;
    napi_get_value_int32(env, args[1], &keyCode);

    int32_t keyType = 0;
    napi_get_value_int32(env, args[2], &keyType);

    uint32_t modifiers = 0;
    napi_get_value_uint32(env, args[3], &modifiers);

    // Optional 5th argument: Unicode char code from ArkUI KeyEvent.unicode
    // When provided (for CHAR events), this overrides the character derived from KeyCode
    int32_t charCode = 0;
    if (argc >= 5) {
        napi_get_value_int32(env, args[4], &charCode);
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefSendKeyEvent: browserId=%{public}ld, keyCode=%{public}d, keyType=%{public}d, charCode=%{public}d",
        browserId, keyCode, keyType, charCode);

    CefKeyEvent keyEvent = convertKeyEvent(keyCode, keyType, modifiers);

    // If ArkTS provided a Unicode char code, use it for the character field
    if (charCode > 0) {
        keyEvent.character = static_cast<char16_t>(charCode);
        keyEvent.unmodified_character = static_cast<char16_t>(charCode);
    }

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendKeyEvent, browser, keyEvent)));
    }

    return nullptr;
}

static napi_value NAPI_Global_hasCef(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "hasCef %{public}ld", browserId);
    bool exists = arkui_cef::ArkUICefManager::getInstance().hasBrowser(browserId);
    
    napi_value result;
    napi_get_boolean(env, exists, &result);
    return result;
}

static napi_value NAPI_Global_createOsrCefView(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "createOsrCefView called");

    size_t argc = 14;
    napi_value args[14];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 7) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "createOsrCefView: expected 7-14 arguments, got %{public}zu", argc);
        napi_throw_error(env, nullptr,
            "Expected 7-14 arguments: url, width, height, onPaint, onCreated, onTitleChange, onFocusTake [, onScrollOffset, onVirtualKeyboard, onTextSelection, onCursorChange, onImeCursor, onStartDrag, onUpdateDragCursor]");
        return nullptr;
    }

    char url[1024] = {};
    size_t urlLen;
    napi_get_value_string_utf8(env, args[0], url, sizeof(url), &urlLen);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "createOsrCefView: url=%{public}s", url);

    int32_t width = 0;
    napi_get_value_int32(env, args[1], &width);

    int32_t height = 0;
    napi_get_value_int32(env, args[2], &height);

    // Create the OSR paint threadsafe function
    auto paintTsfn = std::make_shared<OsrPaintTsfn>();
    if (!paintTsfn->Init(env, args[3], "OsrPaintCallback")) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "createOsrCefView: failed to init paint threadsafe function");
        napi_throw_error(env, nullptr, "Failed to create paint callback");
        return nullptr;
    }

    auto jsCreatedCallback = makeJsThreadSafeCallback<int64_t>(env, args[4], "OsrCreatedCallback");
    auto jsTitleChangeCallback = makeJsThreadSafeCallback<std::string>(env, args[5], "OsrTitleChangeCallback");
    auto jsFocusTakeCallback = makeJsThreadSafeCallback<bool>(env, args[6], "OsrFocusTakeCallback");

    // Optional callbacks (args 7, 8, 9, 10)
    std::function<void(double, double)> scrollOffsetCb;
    std::function<void(bool)> virtualKeyboardCb;
    std::function<void(const std::string&)> textSelectionCb;
    std::function<void(int)> cursorChangeCb;

    if (argc >= 8) {
        auto jsScrollCallback = makeJsThreadSafeCallback<double>(env, args[7], "OsrScrollOffsetCallback");
        if (jsScrollCallback) {
            // We need to pass two doubles; use a pair wrapper
            // Actually JsThreadSafeCallback only supports single arg.
            // We'll encode as a string "x,y" and parse in JS.
            // But that's messy. Let's create a custom threadsafe function for this.
        }
    }

    // For scroll offset, virtual keyboard, and text selection we use custom TSFNs
    // Since JsThreadSafeCallback doesn't support double pairs, we'll use napi_create_double
    // Let's keep it simpler — use the JsThreadSafeCallback for single-arg callbacks
    // and encode scroll offset as a string.
    if (argc >= 8) {
        napi_valuetype vt;
        napi_typeof(env, args[7], &vt);
        if (vt == napi_function) {
            auto jsScrollCb = makeJsThreadSafeCallback<std::string>(env, args[7], "OsrScrollOffsetCallback");
            if (jsScrollCb) {
                scrollOffsetCb = [jsScrollCb](double x, double y) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%.2f,%.2f", x, y);
                    jsScrollCb->Call(std::string(buf));
                };
            }
        }
    }

    if (argc >= 9) {
        napi_valuetype vt;
        napi_typeof(env, args[8], &vt);
        if (vt == napi_function) {
            auto jsVkCb = makeJsThreadSafeCallback<bool>(env, args[8], "OsrVirtualKeyboardCallback");
            if (jsVkCb) {
                virtualKeyboardCb = [jsVkCb](bool show) {
                    jsVkCb->Call(show);
                };
            }
        }
    }

    if (argc >= 10) {
        napi_valuetype vt;
        napi_typeof(env, args[9], &vt);
        if (vt == napi_function) {
            auto jsTextCb = makeJsThreadSafeCallback<std::string>(env, args[9], "OsrTextSelectionCallback");
            if (jsTextCb) {
                textSelectionCb = [jsTextCb](const std::string& text) {
                    jsTextCb->Call(text);
                };
            }
        }
    }

    if (argc >= 11) {
        napi_valuetype vt;
        napi_typeof(env, args[10], &vt);
        if (vt == napi_function) {
            auto jsCursorCb = makeJsThreadSafeCallback<int32_t>(env, args[10], "OsrCursorChangeCallback");
            if (jsCursorCb) {
                cursorChangeCb = [jsCursorCb](int cursorType) {
                    jsCursorCb->Call(static_cast<int32_t>(cursorType));
                };
            }
        }
    }

    std::function<void(int, int, int, int)> imeCursorCb;
    if (argc >= 12) {
        napi_valuetype vt;
        napi_typeof(env, args[11], &vt);
        if (vt == napi_function) {
            auto jsImeCursorCb = makeJsThreadSafeCallback<int32_t, int32_t, int32_t, int32_t>(
                env, args[11], "OsrImeCursorCallback");
            if (jsImeCursorCb) {
                imeCursorCb = [jsImeCursorCb](int left, int top, int width, int height) {
                    jsImeCursorCb->Call(static_cast<int32_t>(left), static_cast<int32_t>(top),
                                        static_cast<int32_t>(width), static_cast<int32_t>(height));
                };
            }
        }
    }

    std::function<void(const std::string&, const std::string&, const std::string&, const std::string&, const std::string&, const std::vector<uint8_t>&, int, int)> startDragCb;
    if (argc >= 13) {
        napi_valuetype vt;
        napi_typeof(env, args[12], &vt);
        if (vt == napi_function) {
            auto startDragTsfn = std::make_shared<OsrStartDragTsfn>();
            if (startDragTsfn->Init(env, args[12], "OsrStartDragCallback")) {
                startDragCb = [startDragTsfn](const std::string& text, const std::string& html,
                                               const std::string& url, const std::string& urlTitle,
                                               const std::string& fileName, const std::vector<uint8_t>& fileData,
                                               int x, int y) {
                    startDragTsfn->Call(text, html, url, urlTitle, fileName, fileData, x, y);
                };
            }
        }
    }

    std::function<void(int)> updateDragCursorCb;
    if (argc >= 14) {
        napi_valuetype vt;
        napi_typeof(env, args[13], &vt);
        if (vt == napi_function) {
            auto jsUpdateDragCursorCb = makeJsThreadSafeCallback<int32_t>(
                env, args[13], "OsrUpdateDragCursorCallback");
            if (jsUpdateDragCursorCb) {
                updateDragCursorCb = [jsUpdateDragCursorCb](int operation) {
                    jsUpdateDragCursorCb->Call(static_cast<int32_t>(operation));
                };
            }
        }
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "createOsrCefView: width=%{public}d, height=%{public}d", width, height);

    // Wrap the paint notification in a PaintCallback. The pixel data stays in
    // the render handler's staging buffer; the JS thread pulls the region
    // metadata via ConsumePendingRegionMeta (and the pixels via
    // CompactRegionPixels on fallback paths) when CallJs runs.
    auto paintHolder = std::make_shared<OsrPaintCallbackHolder>(paintTsfn);
    PaintCallback paintCallback = [paintHolder]() {
        paintHolder->tsfn->Notify();
    };

    int64_t browserId = arkui_cef::ArkUICefManager::getInstance().createOsrBrowser(
        url, width, height, paintCallback,
        [jsCreatedCallback](int64_t id) {
            jsCreatedCallback->Call(id);
            jsCreatedCallback->Release();
        },
        [jsTitleChangeCallback](const std::string& title) {
            jsTitleChangeCallback->Call(title);
        },
        [jsFocusTakeCallback](bool next) {
            jsFocusTakeCallback->Call(next);
        },
        scrollOffsetCb,
        virtualKeyboardCb,
        textSelectionCb,
        cursorChangeCb,
        imeCursorCb,
        startDragCb,
        updateDragCursorCb);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "createOsrCefView: browserId=%{public}ld", browserId);

    // The browser id is only known now; hook the tsfn to it and force one
    // notification. This closes the race where a first-frame notification
    // was delivered (and consumed as "no data") before the id was assigned:
    // the accumulated dirty region is still intact in the render handler, so
    // this forced notification picks it up. With nothing pending it's a
    // harmless no-op consume.
    paintTsfn->SetBrowserId(browserId);
    paintTsfn->Notify();

    napi_value result;
    napi_create_int64(env, browserId, &result);
    return result;
}

static napi_value NAPI_Global_cefOsrResize(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefOsrResize called");

    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 3) {
        napi_throw_error(env, nullptr, "Expected 3 arguments: browserId, width, height");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t width = 0;
    napi_get_value_int32(env, args[1], &width);

    int32_t height = 0;
    napi_get_value_int32(env, args[2], &height);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefOsrResize %{public}ld, %{public}d X %{public}d", browserId, width, height);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::resize, browser, width, height)));
    }
    return nullptr;
}

static napi_value NAPI_Global_destroyOsrCefView(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "destroyOsrCefView called");

    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "destroyOsrCefView id: %{public}ld", browserId);

    // Release native pixelmap handles before the browser teardown — the
    // ArkTS side releases the PixelMap objects after this call.
    {
        std::lock_guard<std::mutex> lock(g_osrPixelmapsMutex);
        auto it = g_osrPixelmaps.find(browserId);
        if (it != g_osrPixelmaps.end()) {
            ReleaseOsrNativePixelmaps(it->second);
            g_osrPixelmaps.erase(it);
        }
    }
    // Release the attached XComponent surface, if any.
    {
        std::lock_guard<std::mutex> lock(g_osrSurfaceMutex);
        auto it = g_osrSurfaces.find(browserId);
        if (it != g_osrSurfaces.end()) {
            ReleaseOsrNativeSurface(it->second);
            g_osrSurfaces.erase(it);
        }
    }

    arkui_cef::ArkUICefManager::getInstance().destroyBrowser(browserId);
    return nullptr;
}

// Registers the ArkTS double-buffered PixelMaps for native dirty-region
// writes. Call after every rebuildPixelMap (the old pair is replaced) and
// BEFORE releasing the old ArkTS PixelMap objects.
static napi_value NAPI_Global_registerOsrPixelmaps(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 3) {
        napi_throw_error(env, nullptr, "Expected 3 arguments: browserId, pmA, pmB");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    OH_PixelmapNative* nativeA = nullptr;
    OH_PixelmapNative* nativeB = nullptr;
    bool ok =
        OH_PixelmapNative_ConvertPixelmapNativeFromNapi(env, args[1], &nativeA) == IMAGE_SUCCESS &&
        nativeA;
    ok = ok &&
         OH_PixelmapNative_ConvertPixelmapNativeFromNapi(env, args[2], &nativeB) == IMAGE_SUCCESS &&
         nativeB;

    uint32_t strideA = 0, strideB = 0;
    int32_t wA = 0, hA = 0, wB = 0, hB = 0;
    ok = ok && QueryPixelmapInfo(nativeA, &strideA, &wA, &hA) &&
         QueryPixelmapInfo(nativeB, &strideB, &wB, &hB) &&
         wA == wB && hA == hB && strideA == strideB;

    if (!ok) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
            "registerOsrPixelmaps: rejected (format/size mismatch), browserId=%{public}ld",
            browserId);
        if (nativeA) {
            OH_PixelmapNative_Release(nativeA);
        }
        if (nativeB) {
            OH_PixelmapNative_Release(nativeB);
        }
        return nullptr;
    }

    {
        std::lock_guard<std::mutex> lock(g_osrPixelmapsMutex);
        auto it = g_osrPixelmaps.find(browserId);
        if (it != g_osrPixelmaps.end()) {
            ReleaseOsrNativePixelmaps(it->second);
        }
        OsrNativePixelmaps& pms = g_osrPixelmaps[browserId];
        pms.pmA = nativeA;
        pms.pmB = nativeB;
        pms.rowStride = strideA;
        pms.width = wA;
        pms.height = hA;
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "registerOsrPixelmaps: browserId=%{public}ld, %{public}dx%{public}d, stride=%{public}u",
        browserId, wA, hA, strideA);
    return nullptr;
}

// Releases the native pixelmap handles for a browser (the ArkTS PixelMap
// objects are released separately by OsrCefView).
static napi_value NAPI_Global_unregisterOsrPixelmaps(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    {
        std::lock_guard<std::mutex> lock(g_osrPixelmapsMutex);
        auto it = g_osrPixelmaps.find(browserId);
        if (it != g_osrPixelmaps.end()) {
            ReleaseOsrNativePixelmaps(it->second);
            g_osrPixelmaps.erase(it);
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                "unregisterOsrPixelmaps: browserId=%{public}ld", browserId);
        }
    }
    return nullptr;
}

// Attaches an XComponent(SURFACE) to a browser for direct window writes.
// surfaceId must belong to an XComponent in this process. Returns true on
// success; on failure the ArkTS side falls back to the PixelMap display.
static napi_value NAPI_Global_attachOsrSurface(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 5) {
        napi_throw_error(env, nullptr,
            "Expected 5 arguments: browserId, surfaceId, widthPx, heightPx, dsf");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    char surfaceIdStr[32] = {};
    size_t strLen = 0;
    napi_get_value_string_utf8(env, args[1], surfaceIdStr, sizeof(surfaceIdStr), &strLen);
    uint64_t surfaceId = strtoull(surfaceIdStr, nullptr, 10);

    int32_t width = 0;
    napi_get_value_int32(env, args[2], &width);
    int32_t height = 0;
    napi_get_value_int32(env, args[3], &height);
    double dsf = 1.0;
    napi_get_value_double(env, args[4], &dsf);
    if (dsf < 1.0 || dsf > 10.0) {
        dsf = 1.0;
    }

    OHNativeWindow* window = nullptr;
    int32_t err = OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window);
    if (err != 0 || !window) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "attachOsrSurface: CreateNativeWindowFromSurfaceId failed, err=%{public}d", err);
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(g_osrSurfaceMutex);
        auto it = g_osrSurfaces.find(browserId);
        if (it != g_osrSurfaces.end()) {
            ReleaseOsrNativeSurface(it->second);
        }
        OsrNativeSurface& s = g_osrSurfaces[browserId];
        s.window = window;
        s.width = width;
        s.height = height;
        s.deviceScaleFactor = static_cast<float>(dsf);
        // Derived size tolerance — see OsrNativeSurface::sizeTolerance.
        s.sizeTolerance = static_cast<int32_t>(ceilf(static_cast<float>(dsf))) + 1;
        s.convertToRgba = false;
        s.needsFullDamage = true;
        // BGRA_8888 matches CEF's OnPaint output directly; CPU usage makes
        // BufferHandle.virAddr available for the blit. The surface may still
        // hand out RGBA_8888 buffers — detected and handled per buffer.
        uint64_t usage = 0;
        OH_NativeWindow_NativeWindowHandleOpt(window, GET_USAGE, &usage);
        usage |= NATIVEBUFFER_USAGE_CPU_READ | NATIVEBUFFER_USAGE_CPU_WRITE;
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_USAGE, usage);
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_FORMAT,
            static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_BGRA_8888));
        OH_NativeWindow_NativeWindowHandleOpt(window, SET_BUFFER_GEOMETRY, width, height);
    }

    // Read back what the surface actually accepted (GET_BUFFER_GEOMETRY
    // outputs (height, width) — reversed order, documented). This surfaces
    // silent SET failures immediately in the hilog.
    {
        int32_t rbHeight = 0;
        int32_t rbWidth = 0;
        int32_t rbFormat = 0;
        uint64_t rbUsage = 0;
        std::lock_guard<std::mutex> lock(g_osrSurfaceMutex);
        auto it = g_osrSurfaces.find(browserId);
        if (it != g_osrSurfaces.end() && it->second.window) {
            OHNativeWindow* w = it->second.window;
            OH_NativeWindow_NativeWindowHandleOpt(w, GET_BUFFER_GEOMETRY, &rbHeight, &rbWidth);
            OH_NativeWindow_NativeWindowHandleOpt(w, GET_FORMAT, &rbFormat);
            OH_NativeWindow_NativeWindowHandleOpt(w, GET_USAGE, &rbUsage);
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                "attachOsrSurface readback: geometry=%{public}dx%{public}d (asked %{public}dx%{public}d), format=%{public}d (BGRA=%{public}d), usage=0x%{public}x",
                rbWidth, rbHeight, width, height, rbFormat,
                static_cast<int32_t>(NATIVEBUFFER_PIXEL_FMT_BGRA_8888),
                static_cast<uint32_t>(rbUsage));
        }
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "attachOsrSurface: browserId=%{public}ld, surface=%{public}s, %{public}dx%{public}d",
        browserId, surfaceIdStr, width, height);

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

// Updates the surface buffer geometry (physical px) after a resize.
static napi_value NAPI_Global_updateOsrSurfaceGeometry(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 3) {
        napi_throw_error(env, nullptr, "Expected 3 arguments: browserId, widthPx, heightPx");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t width = 0;
    napi_get_value_int32(env, args[1], &width);
    int32_t height = 0;
    napi_get_value_int32(env, args[2], &height);

    std::lock_guard<std::mutex> lock(g_osrSurfaceMutex);
    auto it = g_osrSurfaces.find(browserId);
    if (it == g_osrSurfaces.end()) {
        return nullptr;
    }
    OsrNativeSurface& s = it->second;
    if (s.width == width && s.height == height) {
        return nullptr;
    }
    s.width = width;
    s.height = height;
    s.needsFullDamage = true;
    if (s.window) {
        OH_NativeWindow_NativeWindowHandleOpt(s.window, SET_BUFFER_GEOMETRY, width, height);
    }
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "updateOsrSurfaceGeometry: browserId=%{public}ld, %{public}dx%{public}d",
        browserId, width, height);
    return nullptr;
}

// Detaches the XComponent surface from a browser.
static napi_value NAPI_Global_detachOsrSurface(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    {
        std::lock_guard<std::mutex> lock(g_osrSurfaceMutex);
        auto it = g_osrSurfaces.find(browserId);
        if (it != g_osrSurfaces.end()) {
            ReleaseOsrNativeSurface(it->second);
            g_osrSurfaces.erase(it);
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                "detachOsrSurface: browserId=%{public}ld", browserId);
        }
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendMouseClick(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value args[7] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 5) {
        napi_throw_error(env, nullptr,
            "Expected 5-7 arguments: browserId, x, y, button, mouseUp [, clickCount] [, modifiers]");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t x = 0;
    napi_get_value_int32(env, args[1], &x);
    int32_t y = 0;
    napi_get_value_int32(env, args[2], &y);
    int32_t button = 0;
    napi_get_value_int32(env, args[3], &button);
    bool mouseUp = false;
    napi_get_value_bool(env, args[4], &mouseUp);
    int32_t clickCount = 1;
    if (argc >= 6) {
        napi_get_value_int32(env, args[5], &clickCount);
    }
    uint32_t modifiers = 0;
    if (argc >= 7) {
        napi_get_value_uint32(env, args[6], &modifiers);
    }

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendMouseClickEvent, browser, x, y, button, mouseUp, clickCount, modifiers)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendMouseMove(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 4) {
        napi_throw_error(env, nullptr,
            "Expected 4-5 arguments: browserId, x, y, mouseLeave, modifiers");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t x = 0;
    napi_get_value_int32(env, args[1], &x);
    int32_t y = 0;
    napi_get_value_int32(env, args[2], &y);
    bool mouseLeave = false;
    napi_get_value_bool(env, args[3], &mouseLeave);
    uint32_t modifiers = 0;
    if (argc >= 5) {
        napi_get_value_uint32(env, args[4], &modifiers);
    }

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendMouseMoveEvent, browser, x, y, mouseLeave, modifiers)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendMouseWheel(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value args[6] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 5) {
        napi_throw_error(env, nullptr,
            "Expected 5-6 arguments: browserId, x, y, deltaX, deltaY [, modifiers]");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t x = 0;
    napi_get_value_int32(env, args[1], &x);
    int32_t y = 0;
    napi_get_value_int32(env, args[2], &y);
    int32_t deltaX = 0;
    napi_get_value_int32(env, args[3], &deltaX);
    int32_t deltaY = 0;
    napi_get_value_int32(env, args[4], &deltaY);
    uint32_t modifiers = 0;
    if (argc >= 6) {
        napi_get_value_uint32(env, args[5], &modifiers);
    }

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendMouseWheelEvent, browser, x, y, deltaX, deltaY, modifiers)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefOsrSetScale(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, scaleFactor");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    double scaleFactor = 1.0;
    napi_get_value_double(env, args[1], &scaleFactor);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefOsrSetScale: browserId=%{public}ld, scale=%{public}f", browserId, scaleFactor);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::setOsrScale, browser, static_cast<float>(scaleFactor))));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSetComponentOffset(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 3) {
        napi_throw_error(env, nullptr, "Expected 3 arguments: browserId, offsetX, offsetY");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t offsetX = 0;
    napi_get_value_int32(env, args[1], &offsetX);

    int32_t offsetY = 0;
    napi_get_value_int32(env, args[2], &offsetY);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::setComponentOffset, browser, offsetX, offsetY)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSetFrameRate(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, frameRate");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t frameRate = 0;
    napi_get_value_int32(env, args[1], &frameRate);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::setFrameRate, browser, frameRate)));
    }
    return nullptr;
}

namespace {
// Runs arbitrary JS in the browser's main frame. Results can be reported
// via console.log — ArkUICefClient::OnConsoleMessage forwards them to hilog.
void executeJavaScript(CefRefPtr<arkui_cef::ArkUICefBrowser> browser, const std::string& code) {
    if (!browser) {
        return;
    }
    CefRefPtr<CefBrowser> cefBrowser = browser->getCefBrowser();
    if (!cefBrowser || !cefBrowser->GetMainFrame()) {
        return;
    }
    cefBrowser->GetMainFrame()->ExecuteJavaScript(code, cefBrowser->GetMainFrame()->GetURL(), 0);
}
} // namespace

static napi_value NAPI_Global_cefExecuteJavaScript(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, code");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    char code[8192] = {};
    size_t codeLen = 0;
    napi_get_value_string_utf8(env, args[1], code, sizeof(code), &codeLen);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&executeJavaScript, browser, std::string(code))));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendFocusEvent(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, focused");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    bool focused = false;
    napi_get_value_bool(env, args[1], &focused);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendFocusEvent, browser, focused)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendCaptureLost(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendCaptureLostEvent, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefInvalidate(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::invalidate, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefWasHidden(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, hidden");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    bool hidden = false;
    napi_get_value_bool(env, args[1], &hidden);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::wasHidden, browser, hidden)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendTouchEvent(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 5) {
        napi_throw_error(env, nullptr,
            "Expected 5 arguments: browserId, x, y, touchType, pointerId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t x = 0;
    napi_get_value_int32(env, args[1], &x);
    int32_t y = 0;
    napi_get_value_int32(env, args[2], &y);
    int32_t touchType = 0;
    napi_get_value_int32(env, args[3], &touchType);
    int32_t pointerId = 0;
    napi_get_value_int32(env, args[4], &pointerId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendTouchEvent, browser, x, y, touchType, pointerId)));
    }
    return nullptr;
}

static napi_value NAPI_Global_registerPopupTitleCallback(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: cefBrowserId, callback");
        return nullptr;
    }

    int32_t cefBrowserId = 0;
    napi_get_value_int32(env, args[0], &cefBrowserId);

    auto jsCallback = makeJsThreadSafeCallback<std::string>(env, args[1], "PopupTitleCallback");

    if (jsCallback) {
        arkui_cef::ArkUICefManager::getInstance().RegisterPopupTitleCallback(
            cefBrowserId,
            [jsCallback](const std::string& title) {
                jsCallback->Call(title);
            });
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
            "registerPopupTitleCallback: registered for cefBrowserId=%{public}d", cefBrowserId);
    } else {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "registerPopupTitleCallback: failed to create callback");
    }

    return nullptr;
}

static napi_value NAPI_Global_unregisterPopupTitleCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: cefBrowserId");
        return nullptr;
    }

    int32_t cefBrowserId = 0;
    napi_get_value_int32(env, args[0], &cefBrowserId);

    arkui_cef::ArkUICefManager::getInstance().UnregisterPopupTitleCallback(cefBrowserId);
    return nullptr;
}

static napi_value NAPI_Global_setPopupBrowserCreatedCallback(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "setPopupBrowserCreatedCallback called");

    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: callback");
        return nullptr;
    }

    auto jsPopupBrowserCreatedCallback = makeJsThreadSafeCallback<int>(env, args[0], "PopupBrowserCreatedCallback");

    if (jsPopupBrowserCreatedCallback) {
        arkui_cef::ArkUICefManager::getInstance().SetPopupBrowserCreatedCallback(
            [jsPopupBrowserCreatedCallback](int cefBrowserId) {
                jsPopupBrowserCreatedCallback->Call(cefBrowserId);
            });
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "setPopupBrowserCreatedCallback: registered");
    } else {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", "setPopupBrowserCreatedCallback: failed to create callback");
    }

    return nullptr;
}

static napi_value NAPI_Global_registerOsrKeyFilter(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: windowId, browserId");
        return nullptr;
    }

    int32_t windowId = 0;
    napi_get_value_int32(env, args[0], &windowId);

    int64_t browserId = 0;
    napi_get_value_int64(env, args[1], &browserId);

    if (g_keyFilterCtx) delete g_keyFilterCtx;
    g_keyFilterCtx = new OsrKeyFilterContext{ browserId };

    WindowManager_ErrorCode ret = OH_NativeWindowManager_RegisterKeyEventFilter(
        windowId, OsrKeyFilterCallback);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "registerOsrKeyFilter: windowId=%{public}d, browserId=%{public}ld, ret=%{public}d",
        windowId, browserId, (int)ret);

    return nullptr;
}

static napi_value NAPI_Global_setRenderAreaActive(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: active (boolean)");
        return nullptr;
    }

    bool active = false;
    napi_get_value_bool(env, args[0], &active);

    if (g_keyFilterCtx) {
        g_keyFilterCtx->renderAreaActive.store(active);
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
            "setRenderAreaActive: %{public}s", active ? "true" : "false");
    }

    return nullptr;
}

static napi_value NAPI_Global_cefImeSetComposition(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, text");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    char text[4096] = {};
    size_t textLen;
    napi_get_value_string_utf8(env, args[1], text, sizeof(text), &textLen);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::imeSetComposition, browser, std::string(text))));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefImeCommitText(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, text");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    char text[4096] = {};
    size_t textLen;
    napi_get_value_string_utf8(env, args[1], text, sizeof(text), &textLen);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::imeCommitText, browser, std::string(text))));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefImeCancelComposition(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::imeCancelComposition, browser)));
    }
    return nullptr;
}

// ==================== IMF C API NAPI Functions ====================

static std::unique_ptr<arkui_cef::ArkUICefInputMethod> g_imeCApi;

static napi_value NAPI_Global_cefImeCAttach(napi_env env, napi_callback_info info) {
    size_t argc = 10;
    napi_value args[10] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 10) {
        napi_throw_error(env, nullptr,
            "Expected 10 args: windowId, cursorLeft, cursorTop, cursorWidth, cursorHeight, "
            "onInsertText, onDeleteLeft, onSendEnterKey, onSetPreviewText, onFinishTextPreview");
        return nullptr;
    }

    int32_t windowId = 0;
    napi_get_value_int32(env, args[0], &windowId);
    double cursorLeft = 0, cursorTop = 0, cursorWidth = 0, cursorHeight = 0;
    napi_get_value_double(env, args[1], &cursorLeft);
    napi_get_value_double(env, args[2], &cursorTop);
    napi_get_value_double(env, args[3], &cursorWidth);
    napi_get_value_double(env, args[4], &cursorHeight);

    auto jsInsertText = makeJsThreadSafeCallback<std::string>(env, args[5], "ImeInsertText");
    auto jsDeleteLeft = makeJsThreadSafeCallback<int32_t>(env, args[6], "ImeDeleteLeft");
    auto jsSendEnterKey = makeJsThreadSafeCallback<>(env, args[7], "ImeSendEnterKey");
    auto jsSetPreviewText = makeJsThreadSafeCallback<std::string>(env, args[8], "ImeSetPreviewText");
    auto jsFinishPreview = makeJsThreadSafeCallback<>(env, args[9], "ImeFinishPreview");

    if (!jsInsertText || !jsDeleteLeft || !jsSendEnterKey || !jsSetPreviewText || !jsFinishPreview) {
        napi_throw_error(env, nullptr, "Failed to create IME callbacks");
        return nullptr;
    }

    arkui_cef::ImeEventCallbacks callbacks;
    callbacks.onInsertText = [jsInsertText](const std::string& text) {
        jsInsertText->Call(text);
    };
    callbacks.onDeleteBackward = [jsDeleteLeft](int32_t length) {
        jsDeleteLeft->Call(length);
    };
    callbacks.onSendEnterKey = [jsSendEnterKey]() {
        jsSendEnterKey->Call();
    };
    callbacks.onSetPreviewText = [jsSetPreviewText](const std::string& text) {
        jsSetPreviewText->Call(text);
    };
    callbacks.onFinishTextPreview = [jsFinishPreview]() {
        jsFinishPreview->Call();
    };

    g_imeCApi = std::make_unique<arkui_cef::ArkUICefInputMethod>();
    bool ok = g_imeCApi->attach(true, windowId, cursorLeft, cursorTop, cursorWidth, cursorHeight, callbacks);

    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value NAPI_Global_cefImeCUpdateCursor(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 4) {
        napi_throw_error(env, nullptr, "Expected 4 args: left, top, width, height");
        return nullptr;
    }

    double left = 0, top = 0, width = 0, height = 0;
    napi_get_value_double(env, args[0], &left);
    napi_get_value_double(env, args[1], &top);
    napi_get_value_double(env, args[2], &width);
    napi_get_value_double(env, args[3], &height);

    bool ok = false;
    if (g_imeCApi) {
        ok = g_imeCApi->updateCursor(left, top, width, height);
    }

    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value NAPI_Global_cefImeCDetach(napi_env env, napi_callback_info info) {
    if (g_imeCApi) {
        g_imeCApi->detach();
        g_imeCApi.reset();
    }
    return nullptr;
}

// Panel show/hide on the persistent session (no Attach/Detach churn).
// Returns false when the session is detached/dead — the ArkTS layer
// self-heals by re-attaching.
static napi_value NAPI_Global_cefImeCShowKeyboard(napi_env env, napi_callback_info info) {
    bool ok = false;
    if (g_imeCApi) {
        ok = g_imeCApi->showKeyboard();
    }
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value NAPI_Global_cefImeCHideKeyboard(napi_env env, napi_callback_info info) {
    bool ok = false;
    if (g_imeCApi) {
        ok = g_imeCApi->hideKeyboard();
    }
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value NAPI_Global_cefDragTargetDragEnter(napi_env env, napi_callback_info info) {
    size_t argc = 8;
    napi_value args[8] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 8) {
        napi_throw_error(env, nullptr,
            "Expected 8 arguments: browserId, text, html, url, filePaths, x, y, allowedOps");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    char text[4096] = {};
    size_t len;
    napi_get_value_string_utf8(env, args[1], text, sizeof(text), &len);

    char html[16384] = {};
    napi_get_value_string_utf8(env, args[2], html, sizeof(html), &len);

    char url[2048] = {};
    napi_get_value_string_utf8(env, args[3], url, sizeof(url), &len);

    char filePaths[8192] = {};
    napi_get_value_string_utf8(env, args[4], filePaths, sizeof(filePaths), &len);

    int32_t x = 0, y = 0, allowedOps = 0;
    napi_get_value_int32(env, args[5], &x);
    napi_get_value_int32(env, args[6], &y);
    napi_get_value_int32(env, args[7], &allowedOps);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefDragTargetDragEnter: browserId=%{public}ld, x=%{public}d, y=%{public}d, ops=%{public}d",
        browserId, x, y, allowedOps);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::dragTargetDragEnter, browser,
                           std::string(text), std::string(html), std::string(url),
                           std::string(filePaths), x, y, allowedOps)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefDragTargetDragOver(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 4) {
        napi_throw_error(env, nullptr, "Expected 4 arguments: browserId, x, y, allowedOps");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t x = 0, y = 0, allowedOps = 0;
    napi_get_value_int32(env, args[1], &x);
    napi_get_value_int32(env, args[2], &y);
    napi_get_value_int32(env, args[3], &allowedOps);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::dragTargetDragOver, browser, x, y, allowedOps)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefDragTargetDragLeave(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::dragTargetDragLeave, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefDragTargetDrop(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 3) {
        napi_throw_error(env, nullptr, "Expected 3 arguments: browserId, x, y");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t x = 0, y = 0;
    napi_get_value_int32(env, args[1], &x);
    napi_get_value_int32(env, args[2], &y);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefDragTargetDrop: browserId=%{public}ld, x=%{public}d, y=%{public}d", browserId, x, y);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::dragTargetDrop, browser, x, y)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefDragSourceEndedAt(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 4) {
        napi_throw_error(env, nullptr, "Expected 4 arguments: browserId, x, y, op");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t x = 0, y = 0, op = 0;
    napi_get_value_int32(env, args[1], &x);
    napi_get_value_int32(env, args[2], &y);
    napi_get_value_int32(env, args[3], &op);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefDragSourceEndedAt: browserId=%{public}ld, x=%{public}d, y=%{public}d, op=%{public}d",
        browserId, x, y, op);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::dragSourceEndedAt, browser, x, y, op)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefDragSourceSystemDragEnded(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::dragSourceSystemDragEnded, browser)));
    }
    return nullptr;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "Init called");
    
    napi_property_descriptor desc[] = {
        {"startCef", nullptr, NAPI_Global_startCef, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isCefRunning", nullptr, NAPI_Global_isCefRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createCefView", nullptr, NAPI_Global_createCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyCefView", nullptr, NAPI_Global_destroyCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefResize", nullptr, NAPI_Global_cefResize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefGoBack", nullptr, NAPI_Global_cefGoBack, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefGoForward", nullptr, NAPI_Global_cefGoForward, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefReload", nullptr, NAPI_Global_cefReload, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefHide", nullptr, NAPI_Global_cefHide, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefShow", nullptr, NAPI_Global_cefShow, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefLoadUrl", nullptr, NAPI_Global_cefLoadUrl, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hasCef", nullptr, NAPI_Global_hasCef, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerPopupTitleCallback", nullptr, NAPI_Global_registerPopupTitleCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unregisterPopupTitleCallback", nullptr, NAPI_Global_unregisterPopupTitleCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPopupBrowserCreatedCallback", nullptr, NAPI_Global_setPopupBrowserCreatedCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendKeyEvent", nullptr, NAPI_Global_cefSendKeyEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createOsrCefView", nullptr, NAPI_Global_createOsrCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyOsrCefView", nullptr, NAPI_Global_destroyOsrCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerOsrPixelmaps", nullptr, NAPI_Global_registerOsrPixelmaps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unregisterOsrPixelmaps", nullptr, NAPI_Global_unregisterOsrPixelmaps, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"attachOsrSurface", nullptr, NAPI_Global_attachOsrSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"updateOsrSurfaceGeometry", nullptr, NAPI_Global_updateOsrSurfaceGeometry, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"detachOsrSurface", nullptr, NAPI_Global_detachOsrSurface, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefOsrResize", nullptr, NAPI_Global_cefOsrResize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefOsrSetScale", nullptr, NAPI_Global_cefOsrSetScale, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSetComponentOffset", nullptr, NAPI_Global_cefSetComponentOffset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSetFrameRate", nullptr, NAPI_Global_cefSetFrameRate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefExecuteJavaScript", nullptr, NAPI_Global_cefExecuteJavaScript, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendMouseClick", nullptr, NAPI_Global_cefSendMouseClick, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendMouseMove", nullptr, NAPI_Global_cefSendMouseMove, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendMouseWheel", nullptr, NAPI_Global_cefSendMouseWheel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendFocusEvent", nullptr, NAPI_Global_cefSendFocusEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendCaptureLost", nullptr, NAPI_Global_cefSendCaptureLost, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefInvalidate", nullptr, NAPI_Global_cefInvalidate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefWasHidden", nullptr, NAPI_Global_cefWasHidden, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendTouchEvent", nullptr, NAPI_Global_cefSendTouchEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefImeSetComposition", nullptr, NAPI_Global_cefImeSetComposition, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefImeCommitText", nullptr, NAPI_Global_cefImeCommitText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefImeCancelComposition", nullptr, NAPI_Global_cefImeCancelComposition, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerOsrKeyFilter", nullptr, NAPI_Global_registerOsrKeyFilter, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setRenderAreaActive", nullptr, NAPI_Global_setRenderAreaActive, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefImeCAttach", nullptr, NAPI_Global_cefImeCAttach, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefImeCUpdateCursor", nullptr, NAPI_Global_cefImeCUpdateCursor, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefImeCDetach", nullptr, NAPI_Global_cefImeCDetach, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefImeCShowKeyboard", nullptr, NAPI_Global_cefImeCShowKeyboard, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefImeCHideKeyboard", nullptr, NAPI_Global_cefImeCHideKeyboard, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefDragTargetDragEnter", nullptr, NAPI_Global_cefDragTargetDragEnter, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefDragTargetDragOver", nullptr, NAPI_Global_cefDragTargetDragOver, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefDragTargetDragLeave", nullptr, NAPI_Global_cefDragTargetDragLeave, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefDragTargetDrop", nullptr, NAPI_Global_cefDragTargetDrop, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefDragSourceEndedAt", nullptr, NAPI_Global_cefDragSourceEndedAt, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefDragSourceSystemDragEnded", nullptr, NAPI_Global_cefDragSourceSystemDragEnded, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "Init done");
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "arkui_cef",
    .nm_priv = ((void*)0),
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterArkui_cefModule(void)
{
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", 
        "RegisterArkui_cefModule called");
    napi_module_register(&demoModule);
}
