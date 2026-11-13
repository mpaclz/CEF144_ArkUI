#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <thread>
#include <pthread.h>
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
#include "arkui_cef/NodeHandle.h"
#include "include/base/cef_callback.h"

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;

// ---------------------------------------------------------------------------
// OSR native PixelMap registry.
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
// NOTE: the SDK header declares GetWidth/GetHeight with uint32_t* out
// params, not int32_t* as the online docs state.
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
// OSR NativeWindow direct-display registry.
//
// ArkTS attaches the XComponent(SURFACE)'s OHNativeWindow here (created from
// the surfaceId via OH_NativeWindow_CreateNativeWindowFromSurfaceId — the
// XComponent surface lives in this process, as required). The paint tsfn
// then blits frames directly into window buffers (RequestBuffer →
// OH_NativeBuffer_Map → memcpy → FlushBuffer with a damage Region). The
// compositor consumes the buffer queue vsync-aligned — this replaces the
// PixelMap/@State/Image upload chain entirely.
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
    // state forever.
    bool needsFullDamage = true;
    // Diagnostics: consecutive failures. Each failure branch logs on the 1st
    // and every 60th failure so a hilog capture pinpoints the exact broken
    // step without 60/s spam.
    int32_t failCount = 0;
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
                "OSR surface: retuned buffer geometry to frame size %{public}dx%{public}d (was %{public}dx%{public}d)",
                viewW, viewH, oldW, oldH);
        } else {
            if ((++s.failCount % 60) == 1) {
                OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
                    "OSR surface: size mismatch region=%{public}dx%{public}d surface=%{public}dx%{public}d damage=%{public}d,%{public}d %{public}dx%{public}d — posting self-heal resize",
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
                "OSR surface: RequestBuffer failed: err=%{public}d", err);
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
                "OSR surface: bad buffer handle: bh=%{public}d, fmt=%{public}d, stride=%{public}d, size=%{public}dx%{public}d",
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
                "OSR surface: format unsupported: %{public}d (BGRA=%{public}d, RGBA=%{public}d)",
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
                "OSR surface: buffer smaller than frame: buf=%{public}dx%{public}d, frame=%{public}dx%{public}d",
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
                "OSR surface: FromNativeWindowBuffer failed");
        }
        return false;
    }
    void* mapped = nullptr;
    int32_t mapErr = OH_NativeBuffer_Map(nb, &mapped);
    if (mapErr != 0 || !mapped) {
        OH_NativeWindow_NativeWindowAbortBuffer(s.window, buffer);
        if ((++s.failCount % 60) == 1) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                "OSR surface: Map failed: err=%{public}d (check CPU_READ usage)", mapErr);
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
                "OSR surface: BlitFullFrameTo failed (no staging frame?)");
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
        if ((++s.failCount % 60) == 1) {
            OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
                "OSR surface: FlushBuffer failed: err=%{public}d", err);
        }
        return false;
    }
    s.needsFullDamage = false;
    s.failCount = 0;
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
    // Target of key forwarding. Updated by setRenderAreaActive when the user
    // clicks a render area — with multiple OSR instances (tabs) alive at once
    // the filter must route to the browser the user actually interacts with,
    // not the one that most recently (re)registered the filter.
    std::atomic<int64_t> browserId{-1};
    std::atomic<bool> renderAreaActive{false};
    // Modifier tracking from the key-event stream. Two earlier defects made
    // Shift+Tab intermittently degrade into a plain Tab (forward focus):
    //   1. The tracking was skipped while renderAreaActive was false (the
    //      early return below) — every Shift/Ctrl/Alt event seen while the
    //      web was inactive (focus on native controls after every TAB
    //      boundary exit) was missed, leaving the state stale.
    //   2. Only the LEFT modifier keys were tracked (2047/2072/2045) — the
    //      RIGHT Shift (2048) etc. were invisible to the tracker.
    // Tracking is now unconditional and covers both sides.
    std::atomic<bool> ctrlPressed{false};
    std::atomic<bool> shiftPressed{false};
    std::atomic<bool> altPressed{false};
};

static OsrKeyFilterContext* g_keyFilterCtx = nullptr;
// The system filter is registered per WINDOW (multi-window safe: a torn-off
// OSR view in a new window registers its own windowId). Later
// registerOsrKeyFilter calls for an already-registered window only refresh
// the routing target.
static std::unordered_set<int32_t> g_keyFilterWindows;

// Live physical key-state query (mirrors Qt's QKeyEvent::modifiers(): the
// windowing system's per-event modifier state — the model QCefView relies
// on). Used as a cross-check OR-ed with the event-stream tracking: the
// tracking covers injected/virtual keys (which do not register in the
// physical key-state table — e.g. uitest-injected combos), the query covers
// any modifier transitions the filter itself might miss. Returns the
// pressed state of EITHER the left OR the right physical modifier key.
static bool IsModifierKeyPressed(int32_t leftCode, int32_t rightCode) {
    struct Input_KeyState* state = OH_Input_CreateKeyState();
    if (!state) {
        return false;
    }
    bool pressed = false;
    OH_Input_SetKeyCode(state, leftCode);
    if (OH_Input_GetKeyState(state) == INPUT_SUCCESS &&
        OH_Input_GetKeyPressed(state) == KEY_PRESSED) {
        pressed = true;
    }
    if (!pressed) {
        OH_Input_SetKeyCode(state, rightCode);
        if (OH_Input_GetKeyState(state) == INPUT_SUCCESS &&
            OH_Input_GetKeyPressed(state) == KEY_PRESSED) {
            pressed = true;
        }
    }
    OH_Input_DestroyKeyState(&state);
    return pressed;
}

// Modifier keycodes (verified from oh_key_code.h):
// ALT_LEFT=2045, ALT_RIGHT=2046, SHIFT_LEFT=2047, SHIFT_RIGHT=2048,
// CTRL_LEFT=2072, CTRL_RIGHT=2073.
static bool OsrKeyFilterCallback(Input_KeyEvent* keyEvent) {
    if (!keyEvent || !g_keyFilterCtx) return false;

    int32_t keyCode = OH_Input_GetKeyEventKeyCode(keyEvent);
    int32_t action = OH_Input_GetKeyEventAction(keyEvent);

    // Load modifier state BEFORE updating — the current key event should
    // carry the modifier state that existed BEFORE this key was pressed.
    // E.g., Ctrl key-down event itself should NOT have CONTROL_DOWN modifier.
    bool ctrl = g_keyFilterCtx->ctrlPressed.load();
    bool shift = g_keyFilterCtx->shiftPressed.load();
    bool alt = g_keyFilterCtx->altPressed.load();

    // Update modifier tracking AFTER loading previous state — ALWAYS, for
    // BOTH left and right keys, regardless of the active state below.
    if (keyCode == 2072 || keyCode == 2073) {
        g_keyFilterCtx->ctrlPressed.store(action == KEY_ACTION_DOWN);
    } else if (keyCode == 2047 || keyCode == 2048) {
        g_keyFilterCtx->shiftPressed.store(action == KEY_ACTION_DOWN);
    } else if (keyCode == 2045 || keyCode == 2046) {
        g_keyFilterCtx->altPressed.store(action == KEY_ACTION_DOWN);
    }

    // Only filter when OSR render area is active (not when URL bar has focus)
    if (!g_keyFilterCtx->renderAreaActive.load()) {
        return false;
    }

    // Cross-check with the live physical key state (short-circuited by the
    // tracked state — the query only runs when tracking says "not pressed").
    if (!shift) shift = IsModifierKeyPressed(2047, 2048);
    if (!ctrl) ctrl = IsModifierKeyPressed(2072, 2073);
    if (!alt) alt = IsModifierKeyPressed(2045, 2046);

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
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(
        g_keyFilterCtx->browserId.load());
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendKeyEvent, browser, cefKeyEvent)));

        if (sendChar) {
            CefKeyEvent cefCharEvent = convertKeyEvent(keyCode, 2 /*CHAR*/, cefMods);
            CefPostTask(TID_UI, CefCreateClosureTask(
                base::BindOnce(&ArkUICefBrowser::sendKeyEvent, browser, cefCharEvent)));
        }
    } else {
        // The routing target browser does not exist (e.g. CEF is still
        // initializing after the first switch to an OSR tab). Release the
        // key to the system instead of swallowing it silently.
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "OSRKEY",
            "filter: target browser %{public}ld not alive, releasing key %{public}d",
            g_keyFilterCtx->browserId.load(), keyCode);
        return false;
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
            bool hasSurface = false;
            {
                std::lock_guard<std::mutex> lock(g_osrSurfaceMutex);
                auto it = g_osrSurfaces.find(browserId);
                if (it != g_osrSurfaces.end() && handler) {
                    hasSurface = true;
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

            // [tear-off fix] No display consumer at all: surface absent AND
            // no PixelMap fallback registered. This is the normal state
            // during tear-off/restore windows (the old surface was detached
            // while the new XComponent has not created its surface yet —
            // possibly for seconds, or until the user switches back to the
            // tab). Compacting an ~11MB full-frame payload on THIS (main)
            // thread — plus a same-size ArrayBuffer copy — while holding the
            // handler mutex (which also blocks the CEF render thread's
            // OnPaint) is what produced THREAD_BLOCK_6S appfreezes under
            // migration stress. The frame is unobservable: the meta was
            // already consumed above, the staging frame buffer keeps the
            // latest full content, and the attach path's cefInvalidate()
            // re-delivers a fresh frame onto the new surface.
            if (!hasSurface) {
                bool hasPixelmaps = false;
                {
                    std::lock_guard<std::mutex> lock(g_osrPixelmapsMutex);
                    hasPixelmaps = g_osrPixelmaps.find(browserId) != g_osrPixelmaps.end();
                }
                if (!hasPixelmaps) {
                    self->framePending_.store(false);
                    return;
                }
            }

            // Priority 2 / 3: PixelMap native write / ArrayBuffer path.
            // Only reached when a PixelMap fallback is registered (or the
            // surface write failed). Compact the pixel rows on demand (the
            // fallback paths need the actual data). If a resize raced the
            // meta consume, |pixels| is empty and this frame is dropped (a
            // fresh full frame follows).
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

// ---------------------------------------------------------------------------
// Windowed drag-out tear-off detection.
//
// Per-browser state machine mirroring the design doc §3:
//   Idle --Press(Left)--> Armed --move > minDragPx--> Dragging(activated)
//   Dragging --windowX/Y inside content rect, within edgePx of a border--> Tearing
//     -> fire onDragOut (once per session), stop until Release resets
//   Release / non-left press / no-left-button move -> Idle
//   activated && no Move for 500ms (cursor flung out of the window, events
//   stop) -> Idle (+ SendCaptureLostEvent to clean up the web-side drag).
//
// The event receiver is registered on the napi parent Stack (NODE_ON_MOUSE).
// It only OBSERVES — OH_ArkUI_NodeEvent_SetConsumed is never called — so the
// events keep flowing into CEF's XComponent and in-window dragging/text
// selection/HTML5 drag remain untouched.
//
// Coordinates: the parent Stack is switched to ARKUI_LENGTH_METRIC_UNIT_PX,
// so GetWindowX/GetDisplayX/GetX report px directly (see ui_input_event.h —
// pointer coordinates follow the receiving node's length metric unit).
// winLeft/winTop therefore carry the CONTENT area origin relative to the
// window top-left (= drawableRect.left/top - windowRect.left/top, i.e. the
// title-bar offset on 2in1 free windows), winW/winH the content size.
//
// All receivers/watch state are touched on the ArkUI main thread (receiver)
// and the JS thread (napi entries); g_dragOutMutex serializes them. The
// watchdog thread scans for stale sessions every 250ms.
// ---------------------------------------------------------------------------
struct DragOutWatch {
    int64_t browserId = -1;
    ArkUI_NodeHandle node = nullptr;   // parent Stack container handle
    int32_t edgePx = 4;                // border trigger band (px)
    int32_t minDragPx = 20;            // drag activation threshold (px)
    // Content rect (px): origin relative to window top-left + size.
    int32_t winLeft = 0;
    int32_t winTop = 0;
    int32_t winW = 0;
    int32_t winH = 0;
    int32_t viewW = 0;
    int32_t viewH = 0;
    // Session state.
    bool armed = false;       // left button pressed
    bool activated = false;   // displacement threshold passed
    bool triggered = false;   // tear-off fired for this press
    bool proximityLogged = false;  // one-shot "cursor approaching edge" log
    float pressLocalX = 0.f;
    float pressLocalY = 0.f;
    float pressScreenX = 0.f;
    float pressScreenY = 0.f;
    int64_t lastMoveMs = 0;
    std::shared_ptr<JsThreadSafeCallback<int64_t, int32_t, int32_t, int32_t,
                                          int32_t, int32_t, int32_t>> onDragOut;
};

std::mutex g_dragOutMutex;
std::unordered_map<int64_t, std::unique_ptr<DragOutWatch>> g_dragOutWatches;
std::atomic<bool> g_dragOutWatchdogRunning{false};
constexpr int64_t DRAG_OUT_STALE_MS = 500;

// browserId -> current napi parent Node* registry. Populated in
// createCefView, updated in adoptCefView, cleared in destroyCefView. Lets
// cefWatchDragOut/adoptCefView resolve the parent Stack without going
// through the NodeContent handle.
std::mutex g_cefParentNodesMutex;
std::unordered_map<int64_t, Node*> g_cefParentNodes;

// In-flight adoptCefView workers (browserId inserted before the worker
// spawns, erased as the FIRST action of the completion callback), plus the
// set of browsers whose destroyCefView arrived while an adopt worker was
// still running ("interrupted" adoption — node cleanup is deferred to the
// completion callback; the browser teardown itself proceeds normally).
// Both guarded by g_cefParentNodesMutex.
std::unordered_set<int64_t> g_cefAdopting;
std::unordered_set<int64_t> g_cefAdoptInterrupted;

int64_t DragOutNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void DragOutNodeEventReceiver(ArkUI_NodeEvent* event);

void ResetDragOutSession(DragOutWatch* w) {
    w->armed = false;
    w->activated = false;
    w->triggered = false;
    w->proximityLogged = false;
    w->lastMoveMs = 0;
}

// Map-only variant: erases the watch entry WITHOUT touching any ArkUI node.
// Used by interrupted-adopt paths (destroy raced an in-flight adopt): the
// related nodes may already be freed or owned by a destroyed page, so no
// node API (unregister/remove receiver) may be called on them.
void CleanupDragOutWatchMapOnly(int64_t browserId) {
    std::lock_guard<std::mutex> lock(g_dragOutMutex);
    g_dragOutWatches.erase(browserId);
}

void CleanupDragOutWatchForNode(int64_t browserId, ArkUI_NodeHandle node) {
    CleanupDragOutWatchMapOnly(browserId);
    if (node) {
        ArkUI_NativeNodeAPI_1* api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
            OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
        if (api) {
            api->unregisterNodeEvent(node, NODE_ON_MOUSE);
            api->removeNodeEventReceiver(node, DragOutNodeEventReceiver);
        }
    }
}

void DragOutWatchdogMain() {
    pthread_setname_np(pthread_self(), "DragOutWatch");
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::vector<int64_t> staleBrowsers;
        {
            std::lock_guard<std::mutex> lock(g_dragOutMutex);
            if (g_dragOutWatches.empty()) {
                // Store the not-running flag FIRST (under the lock), then
                // re-check: a registration that raced in between will either
                // see running==false and start a fresh watchdog, or be
                // picked up by our re-check below.
                g_dragOutWatchdogRunning.store(false, std::memory_order_relaxed);
                if (g_dragOutWatches.empty()) {
                    return;
                }
                g_dragOutWatchdogRunning.store(true, std::memory_order_relaxed);
            }
            int64_t now = DragOutNowMs();
            for (auto& entry : g_dragOutWatches) {
                DragOutWatch* w = entry.second.get();
                if (!w) {
                    continue;
                }
                // Only ACTIVATED sessions can go stale: an armed-but-still
                // session (button pressed, cursor parked) MUST NOT be reset
                // by the 500ms sweep — the press is simply idle, not lost.
                // The "flung outside the window" signature is: activated,
                // then events STOP arriving (no Move for 500ms).
                if (w->activated &&
                    w->lastMoveMs > 0 &&
                    now - w->lastMoveMs > DRAG_OUT_STALE_MS) {
                    // Cursor flung outside the window (events stopped) or the
                    // Release was swallowed. Reset the session; for an
                    // in-progress (activated, not yet triggered) web drag,
                    // also clean the web-side press state.
                    if (!w->triggered) {
                        staleBrowsers.push_back(entry.first);
                    }
                    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                        "dragOut watchdog: reset stale session browserId=%{public}ld",
                        entry.first);
                    ResetDragOutSession(w);
                }
            }
        }
        // Capture-lost posts happen OUTSIDE the lock.
        for (int64_t id : staleBrowsers) {
            auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(id);
            if (browser) {
                CefPostTask(TID_UI, CefCreateClosureTask(
                    base::BindOnce(&ArkUICefBrowser::sendCaptureLostEvent, browser)));
            }
        }
    }
}

void EnsureDragOutWatchdog() {
    bool expected = false;
    if (g_dragOutWatchdogRunning.compare_exchange_strong(expected, true)) {
        std::thread(DragOutWatchdogMain).detach();
    }
}

// The receiver runs on the ArkUI main thread. Never consumes events.
void DragOutNodeEventReceiver(ArkUI_NodeEvent* event) {
    if (!event) {
        return;
    }
    int32_t targetId = OH_ArkUI_NodeEvent_GetTargetId(event);
    ArkUI_UIInputEvent* input = OH_ArkUI_NodeEvent_GetInputEvent(event);
    if (!input || targetId <= 0) {
        return;
    }
    if (OH_ArkUI_UIInputEvent_GetType(input) != ARKUI_UIINPUTEVENT_TYPE_MOUSE) {
        return;
    }
    int64_t browserId = static_cast<int64_t>(targetId);

    // Mouse action codes (ui_input_event.h): 1=Press 2=Release 3=Move.
    int32_t action = OH_ArkUI_MouseEvent_GetMouseAction(input);
    // Mouse button codes (ui_input_event.h): 1=Left 2=Right 3=Middle.
    int32_t button = OH_ArkUI_MouseEvent_GetMouseButton(input);

    // Decide + mutate under the lock; the JS callback fires outside it.
    bool fire = false;
    float fireLocalX = 0.f, fireLocalY = 0.f;
    float fireScreenX = 0.f, fireScreenY = 0.f;
    float fireCurX = 0.f, fireCurY = 0.f;
    std::shared_ptr<JsThreadSafeCallback<int64_t, int32_t, int32_t, int32_t,
                                          int32_t, int32_t, int32_t>> cb;
    {
        std::lock_guard<std::mutex> lock(g_dragOutMutex);
        auto it = g_dragOutWatches.find(browserId);
        if (it == g_dragOutWatches.end()) {
            return;
        }
        DragOutWatch* w = it->second.get();
        if (!w) {
            return;
        }
        switch (action) {
            case UI_MOUSE_EVENT_ACTION_PRESS: {
                if (button != UI_MOUSE_EVENT_BUTTON_LEFT) {
                    // Non-left press cancels any pending session.
                    ResetDragOutSession(w);
                    return;
                }
                w->armed = true;
                w->activated = false;
                w->triggered = false;
                w->proximityLogged = false;
                w->pressLocalX = OH_ArkUI_PointerEvent_GetX(input);
                w->pressLocalY = OH_ArkUI_PointerEvent_GetY(input);
                w->pressScreenX = OH_ArkUI_PointerEvent_GetDisplayX(input);
                w->pressScreenY = OH_ArkUI_PointerEvent_GetDisplayY(input);
                w->lastMoveMs = DragOutNowMs();
                // Diagnostic: proves NODE_ON_MOUSE events bubble up from the
                // CEF XComponent area to our parent-Stack receiver. One line
                // per left press — low noise, high diagnostic value.
                OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                    "dragOut press: browserId=%{public}ld, win=(%{public}f,%{public}f), "
                    "local=(%{public}f,%{public}f)",
                    browserId, OH_ArkUI_PointerEvent_GetWindowX(input),
                    OH_ArkUI_PointerEvent_GetWindowY(input),
                    w->pressLocalX, w->pressLocalY);
                return;
            }
            case UI_MOUSE_EVENT_ACTION_RELEASE: {
                ResetDragOutSession(w);
                return;
            }
            case UI_MOUSE_EVENT_ACTION_CANCEL: {
                // System-initiated cancel (UI_MOUSE_EVENT_ACTION_CANCEL = 13,
                // see ui_input_event.h): treat like Release — drop the
                // pending session without firing drag-out.
                ResetDragOutSession(w);
                return;
            }
            case UI_MOUSE_EVENT_ACTION_MOVE: {
                if (!w->armed || w->triggered) {
                    return;
                }
                // Hover moves carry no buttons — a stale "armed" session
                // (Release lost outside the component) self-heals here.
                int32_t pressed[8] = {0};
                int32_t pressedCount = 8;
                if (OH_ArkUI_MouseEvent_GetPressedButtons(input, pressed, &pressedCount) ==
                        ARKUI_ERROR_CODE_NO_ERROR) {
                    bool leftHeld = false;
                    for (int32_t i = 0; i < pressedCount; i++) {
                        if (pressed[i] == UI_MOUSE_EVENT_BUTTON_LEFT) {
                            leftHeld = true;
                            break;
                        }
                    }
                    if (!leftHeld) {
                        ResetDragOutSession(w);
                        return;
                    }
                }
                float winX = OH_ArkUI_PointerEvent_GetWindowX(input);
                float winY = OH_ArkUI_PointerEvent_GetWindowY(input);
                float scrX = OH_ArkUI_PointerEvent_GetDisplayX(input);
                float scrY = OH_ArkUI_PointerEvent_GetDisplayY(input);
                w->lastMoveMs = DragOutNowMs();
                if (!w->activated) {
                    double dx = static_cast<double>(scrX) - w->pressScreenX;
                    double dy = static_cast<double>(scrY) - w->pressScreenY;
                    double minDrag = static_cast<double>(w->minDragPx);
                    if (dx * dx + dy * dy < minDrag * minDrag) {
                        return;
                    }
                    w->activated = true;
                    // Diagnostic: displacement threshold passed (session live).
                    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                        "dragOut activated: browserId=%{public}ld, win=(%{public}f,%{public}f)",
                        browserId, winX, winY);
                }
                // Content-relative coordinates (winLeft/Top = content origin
                // within the window; 0 on platforms where drawableRect is
                // window-relative). Trigger band: within edgePx of any
                // content-area boundary. Coordinates BEYOND a boundary (fast
                // flick overshoot — if the OS still delivers events under
                // implicit pointer capture) also trigger, so the band cannot
                // be jumped over.
                float contentX = winX - static_cast<float>(w->winLeft);
                float contentY = winY - static_cast<float>(w->winTop);
                float edge = static_cast<float>(w->edgePx);
                bool nearEdge = contentX <= edge || contentY <= edge ||
                    contentX >= static_cast<float>(w->winW - w->edgePx) ||
                    contentY >= static_cast<float>(w->winH - w->edgePx);
                if (!nearEdge) {
                    // Diagnostic (one-shot per session): cursor came within
                    // 60px of an edge but not into the trigger band yet.
                    if (!w->proximityLogged &&
                        (contentX <= 60.f || contentY <= 60.f ||
                         contentX >= static_cast<float>(w->winW - 60) ||
                         contentY >= static_cast<float>(w->winH - 60))) {
                        w->proximityLogged = true;
                        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                            "dragOut near edge: browserId=%{public}ld, content=(%{public}f,%{public}f), "
                            "win=%{public}dx%{public}d, edgeBand=%{public}d",
                            browserId, contentX, contentY, w->winW, w->winH, w->edgePx);
                    }
                    return;
                }
                w->triggered = true;
                fire = true;
                fireLocalX = w->pressLocalX;
                fireLocalY = w->pressLocalY;
                fireScreenX = w->pressScreenX;
                fireScreenY = w->pressScreenY;
                fireCurX = scrX;
                fireCurY = scrY;
                cb = w->onDragOut;
                break;
            }
            default:
                return;
        }
    }

    if (fire && cb) {
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
            "dragOut triggered: browserId=%{public}ld, cur=(%{public}f,%{public}f)",
            browserId, fireCurX, fireCurY);
        cb->Call(browserId,
                 static_cast<int32_t>(fireLocalX + 0.5f),
                 static_cast<int32_t>(fireLocalY + 0.5f),
                 static_cast<int32_t>(fireScreenX + 0.5f),
                 static_cast<int32_t>(fireScreenY + 0.5f),
                 static_cast<int32_t>(fireCurX + 0.5f),
                 static_cast<int32_t>(fireCurY + 0.5f));
    }
}


} // namespace

static napi_value NAPI_Global_startCef(napi_env env, napi_callback_info info) {
    arkui_cef::ArkUICefManager::getInstance().start();
    return nullptr;
}

static napi_value NAPI_Global_isCefRunning(napi_env env, napi_callback_info info) {
    bool ok = arkui_cef::ArkUICefManager::getInstance().isCefRunning();
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value NAPI_Global_createCefView(napi_env env, napi_callback_info info) {
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
    
    int32_t width = 0;
    napi_get_value_int32(env, args[2], &width);
    
    int32_t height = 0;
    napi_get_value_int32(env, args[3], &height);
    
    auto jsCreatedCallback = makeJsThreadSafeCallback<int64_t>(env, args[4], "CefCreatedCallback");
    auto jsTitleChangeCallback = makeJsThreadSafeCallback<std::string>(env, args[5], "CefTitleChangeCallback");
    auto jsFocusTakeCallback = makeJsThreadSafeCallback<bool>(env, args[6], "CefFocusTakeCallback");
    
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

    // Register the parent node for drag-out watch / adopt lookups.
    {
        std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
        g_cefParentNodes[browserId] = parentNode;
    }

    napi_value result;
    napi_create_int64(env, browserId, &result);
    return result;
}

static napi_value NAPI_Global_destroyCefView(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    ArkUI_NodeContentHandle contentHandle = nullptr;
    OH_ArkUI_GetNodeContentFromNapiValue(env, args[1], &contentHandle);
    if (contentHandle == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "destroyCefView: getNodeContentHandleFromNapiValue failed");
        napi_throw_error(env, nullptr, "Failed to get NodeContent handle");
        return nullptr;
    }
    
    arkui_cef::ArkUICefManager::getInstance().destroyBrowser(browserId);

    // adoptCefView worker in flight for this browser? Mark it interrupted and
    // SKIP all node cleanup (no RemoveNode / destroyNode / userData clear):
    // the old and new parent Stacks may be mid-migration on the CEF UI
    // thread — the adopt completion callback owns the final node handling
    // (or nodes leak as a safe fallback). The browser teardown above has
    // already proceeded normally.
    bool adopting = false;
    {
        std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
        g_cefParentNodes.erase(browserId);
        if (g_cefAdopting.count(browserId) > 0) {
            g_cefAdoptInterrupted.insert(browserId);
            adopting = true;
        }
    }
    if (adopting) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
            "destroyCefView: browserId=%{public}ld destroyed while adopt in "
            "flight; node cleanup deferred to adopt completion", browserId);
        // Map-only watch cleanup — nodes must not be touched here.
        CleanupDragOutWatchMapOnly(browserId);
        return nullptr;
    }

    // Drop the drag-out watch (state + node event registration) and the
    // parent-node registry entry BEFORE disposing the node.
    Node* parentNode = static_cast<Node*>(OH_ArkUI_NodeContent_GetUserData(contentHandle));
    if (parentNode) {
        CleanupDragOutWatchForNode(browserId, parentNode->container);
        int32_t err = OH_ArkUI_NodeContent_RemoveNode(contentHandle, parentNode->container);
        if (err != ARKUI_ERROR_CODE_NO_ERROR) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "destroyCefView: removeNode failed");
        }
        destroyNode(parentNode);
        // Clear the (now dangling) user data so a later destroyCefView on the
        // same NodeContent cannot resurrect the freed pointer.
        OH_ArkUI_NodeContent_SetUserData(contentHandle, nullptr);
    }

    return nullptr;
}

static napi_value NAPI_Global_cefResize(napi_env env, napi_callback_info info) {
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
    
    bool exists = arkui_cef::ArkUICefManager::getInstance().hasBrowser(browserId);
    
    napi_value result;
    napi_get_boolean(env, exists, &result);
    return result;
}

static napi_value NAPI_Global_createOsrCefView(napi_env env, napi_callback_info info) {
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

    // Optional callbacks (args 7-13). Scroll offsets are encoded as "x,y"
    // strings since JsThreadSafeCallback only supports single arguments.
    std::function<void(double, double)> scrollOffsetCb;
    std::function<void(bool)> virtualKeyboardCb;
    std::function<void(const std::string&)> textSelectionCb;
    std::function<void(int)> cursorChangeCb;

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
        "createOsrCefView: %{public}dx%{public}d, url=%{public}s", width, height, url);

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

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::resize, browser, width, height)));
    }
    return nullptr;
}

static napi_value NAPI_Global_destroyOsrCefView(napi_env env, napi_callback_info info) {
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

    // The routing target is set once here (first browser to register) and is
    // afterwards owned by setRenderAreaActive (click/visibility driven).
    // Blindly overwriting it per registration would hand the keyboard to a
    // background tab when several views register up front.
    if (!g_keyFilterCtx) {
        g_keyFilterCtx = new OsrKeyFilterContext();
        g_keyFilterCtx->browserId.store(browserId);
    }

    if (g_keyFilterWindows.find(windowId) == g_keyFilterWindows.end()) {
        WindowManager_ErrorCode ret = OH_NativeWindowManager_RegisterKeyEventFilter(
            windowId, OsrKeyFilterCallback);
        if (ret != WindowManager_ErrorCode::OK) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "OSRKEY",
                "register: FAILED windowId=%{public}d ret=%{public}d",
                windowId, static_cast<int>(ret));
        } else {
            g_keyFilterWindows.insert(windowId);
            OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "OSRKEY",
                "register: ok windowId=%{public}d target=%{public}ld (windows=%{public}zu)",
                windowId, browserId, g_keyFilterWindows.size());
            // Warm up the IMS connection once per process (first successful
            // registration). Physical keys are routed through the input
            // method service, which drops events for apps that never
            // established a connection (the first IMF attach). A one-shot
            // attach+detach (panel NOT shown) here makes key routing work
            // from the very first key press after startup.
            if (g_keyFilterWindows.size() == 1) {
                arkui_cef::ImeEventCallbacks stub{};
                auto warmup = std::make_unique<arkui_cef::ArkUICefInputMethod>();
                bool ok = warmup->attach(false, windowId, 0, 0, 2, 24, stub);
                if (ok) {
                    warmup->detach();
                }
                OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "OSRKEY",
                    "ims warmup: %{public}s", ok ? "ok" : "failed");
            }
        }
    } else {
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "OSRKEY",
            "register: windowId=%{public}d already registered, target=%{public}ld",
            windowId, browserId);
    }

    return nullptr;
}

// Tracks which render area owns keyboard input. Called with active=true when
// the user clicks/touches an OSR render area (making that browser the key
// forwarding target), and with active=false when focus moves to another
// component (e.g. the URL bar).
static napi_value NAPI_Global_setRenderAreaActive(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, active (boolean)");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    bool active = false;
    napi_get_value_bool(env, args[1], &active);

    if (active) {
        if (!g_keyFilterCtx) {
            g_keyFilterCtx = new OsrKeyFilterContext();
        }
        g_keyFilterCtx->browserId.store(browserId);
        g_keyFilterCtx->renderAreaActive.store(true);
    } else if (g_keyFilterCtx && g_keyFilterCtx->browserId.load() == browserId) {
        // Deactivate only when the caller still owns the routing target —
        // another view may have taken over since this one was activated.
        g_keyFilterCtx->renderAreaActive.store(false);
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

// ==================== Drag-out tear-off (windowed + OSR adoption) ====================

// cefWatchDragOut(browserId, edgePx, minDragPx, winLeft, winTop, winW, winH,
//                 viewW, viewH, onDragOut)
// Registers NODE_ON_MOUSE observation on the napi parent Stack of the given
// windowed browser. winLeft/winTop = content-area origin relative to the
// window top-left (title-bar offset), winW/winH = content size (px);
// viewW/viewH = CEF view size (px). onDragOut(browserId, pressLocalX,
// pressLocalY, pressScreenX, pressScreenY, curScreenX, curScreenY) — all px.
// Returns true when the watch is active.
static napi_value NAPI_Global_cefWatchDragOut(napi_env env, napi_callback_info info) {
    size_t argc = 10;
    napi_value args[10] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 10) {
        napi_throw_error(env, nullptr,
            "Expected 10 arguments: browserId, edgePx, minDragPx, winLeft, winTop, winW, winH, viewW, viewH, onDragOut");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t edgePx = 4;
    napi_get_value_int32(env, args[1], &edgePx);
    int32_t minDragPx = 20;
    napi_get_value_int32(env, args[2], &minDragPx);
    int32_t winLeft = 0, winTop = 0, winW = 0, winH = 0, viewW = 0, viewH = 0;
    napi_get_value_int32(env, args[3], &winLeft);
    napi_get_value_int32(env, args[4], &winTop);
    napi_get_value_int32(env, args[5], &winW);
    napi_get_value_int32(env, args[6], &winH);
    napi_get_value_int32(env, args[7], &viewW);
    napi_get_value_int32(env, args[8], &viewH);

    auto onDragOut = makeJsThreadSafeCallback<int64_t, int32_t, int32_t, int32_t,
                                               int32_t, int32_t, int32_t>(
        env, args[9], "CefDragOutCallback");
    if (!onDragOut) {
        napi_throw_error(env, nullptr, "Failed to create drag-out callback");
        return nullptr;
    }

    Node* parentNode = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
        auto it = g_cefParentNodes.find(browserId);
        if (it != g_cefParentNodes.end()) {
            parentNode = it->second;
        }
    }
    if (!parentNode || !parentNode->container) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "cefWatchDragOut: parent node not found, browserId=%{public}ld", browserId);
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }

    ArkUI_NativeNodeAPI_1* api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
    if (!api) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "cefWatchDragOut: node api unavailable");
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }

    {
        std::lock_guard<std::mutex> lock(g_dragOutMutex);
        auto it = g_dragOutWatches.find(browserId);
        if (it != g_dragOutWatches.end()) {
            // Re-enable with fresh config (no duplicate node registration).
            DragOutWatch* w = it->second.get();
            w->edgePx = edgePx;
            w->minDragPx = minDragPx;
            w->winLeft = winLeft;
            w->winTop = winTop;
            w->winW = winW;
            w->winH = winH;
            w->viewW = viewW;
            w->viewH = viewH;
            w->onDragOut = onDragOut;
            ResetDragOutSession(w);
        } else {
            // Pointer coordinates follow the receiving node's length metric
            // unit — switch the parent Stack to px so GetWindowX/GetDisplayX
            // report px directly (no vp->px conversion needed).
            api->setLengthMetricUnit(parentNode->container, ARKUI_LENGTH_METRIC_UNIT_PX);
            int32_t err = api->registerNodeEvent(parentNode->container, NODE_ON_MOUSE,
                                                 static_cast<int32_t>(browserId), nullptr);
            if (err != ARKUI_ERROR_CODE_NO_ERROR) {
                OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                    "cefWatchDragOut: registerNodeEvent failed err=%{public}d", err);
                napi_value result;
                napi_get_boolean(env, false, &result);
                return result;
            }
            err = api->addNodeEventReceiver(parentNode->container, DragOutNodeEventReceiver);
            if (err != ARKUI_ERROR_CODE_NO_ERROR) {
                OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                    "cefWatchDragOut: addNodeEventReceiver failed err=%{public}d", err);
                api->unregisterNodeEvent(parentNode->container, NODE_ON_MOUSE);
                napi_value result;
                napi_get_boolean(env, false, &result);
                return result;
            }
            auto w = std::make_unique<DragOutWatch>();
            w->browserId = browserId;
            w->node = parentNode->container;
            w->edgePx = edgePx;
            w->minDragPx = minDragPx;
            w->winLeft = winLeft;
            w->winTop = winTop;
            w->winW = winW;
            w->winH = winH;
            w->viewW = viewW;
            w->viewH = viewH;
            w->onDragOut = onDragOut;
            g_dragOutWatches.emplace(browserId, std::move(w));
        }
    }

    EnsureDragOutWatchdog();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefWatchDragOut: browserId=%{public}ld edge=%{public}d minDrag=%{public}d "
        "content=(%{public}d,%{public}d,%{public}d,%{public}d) view=%{public}dx%{public}d",
        browserId, edgePx, minDragPx, winLeft, winTop, winW, winH, viewW, viewH);

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

// cefUpdateDragOutRect(browserId, winLeft, winTop, winW, winH, viewW, viewH)
// Refreshes the detection rects after a window move/resize or a view
// onAreaChange. Same coordinate semantics as cefWatchDragOut.
static napi_value NAPI_Global_cefUpdateDragOutRect(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value args[7] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 7) {
        napi_throw_error(env, nullptr,
            "Expected 7 arguments: browserId, winLeft, winTop, winW, winH, viewW, viewH");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t winLeft = 0, winTop = 0, winW = 0, winH = 0, viewW = 0, viewH = 0;
    napi_get_value_int32(env, args[1], &winLeft);
    napi_get_value_int32(env, args[2], &winTop);
    napi_get_value_int32(env, args[3], &winW);
    napi_get_value_int32(env, args[4], &winH);
    napi_get_value_int32(env, args[5], &viewW);
    napi_get_value_int32(env, args[6], &viewH);

    std::lock_guard<std::mutex> lock(g_dragOutMutex);
    auto it = g_dragOutWatches.find(browserId);
    if (it == g_dragOutWatches.end()) {
        return nullptr;
    }
    DragOutWatch* w = it->second.get();
    if (w) {
        w->winLeft = winLeft;
        w->winTop = winTop;
        w->winW = winW;
        w->winH = winH;
        w->viewW = viewW;
        w->viewH = viewH;
    }
    return nullptr;
}

// cefUnwatchDragOut(browserId)
static napi_value NAPI_Global_cefUnwatchDragOut(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    Node* parentNode = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
        auto it = g_cefParentNodes.find(browserId);
        if (it != g_cefParentNodes.end()) {
            parentNode = it->second;
        }
    }
    CleanupDragOutWatchForNode(browserId, parentNode ? parentNode->container : nullptr);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefUnwatchDragOut: browserId=%{public}ld", browserId);
    return nullptr;
}

// adoptCefView(browserId, oldNodeContent, newNodeContent, width, height,
//              onAdopted(ok: boolean))
// Windowed migration, fully ASYNC:
//  1. JS main thread: validate args, build the new parent Stack and attach it
//     to newNodeContent (fast node ops), mark the adoption in flight
//     (g_cefAdopting), then return immediately.
//  2. Worker thread: browser->reparent() — blocks up to ~3s waiting for the
//     CEF UI thread. NEVER call this on the ArkUI main thread: the CEF UI
//     thread's new-widget creation waits (WaitForSurface, no timeout) for the
//     surface-created callback which is delivered ON the ArkUI main thread —
//     a synchronous call from main would deadlock both threads.
//  3. Completion (main thread, via PostToMainThread): first leave
//     g_cefAdopting / read g_cefAdoptInterrupted, then branch:
//     - interrupted (destroyCefView raced the worker): touch NO nodes —
//       only erase the registry/watch-map entries and notify(false).
//     - rc == 0: retire the old parent Stack, update the registry, notify(true).
//     - rc == -3 (outcome unknown): unmount + clear userData on the new
//       NodeContent, intentionally LEAK the new parent Stack, notify(false).
//     - other rc < 0: full cleanup of the new parent Stack (RemoveNode +
//       destroyNode + userData clear), notify(false).
static napi_value NAPI_Global_adoptCefView(napi_env env, napi_callback_info info) {
    size_t argc = 6;
    napi_value args[6] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 6) {
        napi_throw_error(env, nullptr,
            "Expected 6 arguments: browserId, oldNodeContent, newNodeContent, width, height, onAdopted");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    ArkUI_NodeContentHandle oldContent = nullptr;
    OH_ArkUI_GetNodeContentFromNapiValue(env, args[1], &oldContent);
    ArkUI_NodeContentHandle newContent = nullptr;
    OH_ArkUI_GetNodeContentFromNapiValue(env, args[2], &newContent);

    int32_t width = 0;
    napi_get_value_int32(env, args[3], &width);
    int32_t height = 0;
    napi_get_value_int32(env, args[4], &height);

    auto onAdopted = makeJsThreadSafeCallback<bool>(env, args[5], "CefAdoptedCallback");

    auto notify = [onAdopted](bool ok) {
        if (onAdopted) {
            onAdopted->Call(ok);
            onAdopted->Release();
        }
    };

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (!browser || !browser->isWindowed()) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "adoptCefView: browser not found or not windowed, browserId=%{public}ld", browserId);
        notify(false);
        return nullptr;
    }
    if (!oldContent || !newContent || width <= 0 || height <= 0) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "adoptCefView: invalid arguments, browserId=%{public}ld", browserId);
        notify(false);
        return nullptr;
    }

    // Old parent node: registry first, NodeContent user data as fallback.
    Node* oldParent = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
        auto it = g_cefParentNodes.find(browserId);
        if (it != g_cefParentNodes.end()) {
            oldParent = it->second;
        }
    }
    if (!oldParent) {
        oldParent = static_cast<Node*>(OH_ArkUI_NodeContent_GetUserData(oldContent));
    }
    if (!oldParent || !oldParent->container) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "adoptCefView: old parent node not found, browserId=%{public}ld", browserId);
        notify(false);
        return nullptr;
    }

    // Build the new parent Stack and attach it to the new NodeContent.
    Node* newParent = createParentNode();
    if (!newParent) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "adoptCefView: createParentNode failed");
        notify(false);
        return nullptr;
    }
    int32_t err = OH_ArkUI_NodeContent_AddNode(newContent, newParent->container);
    if (err != ARKUI_ERROR_CODE_NO_ERROR) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "adoptCefView: AddNode failed err=%{public}d", err);
        destroyNode(newParent);
        notify(false);
        return nullptr;
    }
    OH_ArkUI_NodeContent_SetUserData(newContent, newParent);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "adoptCefView: begin reparent (async) browserId=%{public}ld size=%{public}dx%{public}d",
        browserId, width, height);

    // Mark the adoption as in flight so a racing destroyCefView(browserId)
    // knows to defer node cleanup to our completion callback (interrupted
    // adoption) instead of yanking the nodes out from under the CEF UI
    // thread.
    {
        std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
        g_cefAdopting.insert(browserId);
    }

    // Async reparent on a detached worker thread. The CefRefPtr copy keeps
    // the browser alive across the migration; all ArkUI node cleanup and
    // registry updates are posted back to the main thread.
    std::thread([browser, browserId, oldParent, newParent, oldContent, newContent,
                 width, height, notify]() {
        int rc = browser->reparent(newParent, width, height);
        arkui_cef::PostToMainThread([browser, browserId, oldParent, newParent,
                                     oldContent, newContent, rc, notify]() {
            // FIRST action: leave the adopting set and fetch + clear the
            // interrupted flag under the same lock. Every branch below
            // notifies exactly once.
            bool interrupted = false;
            {
                std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
                g_cefAdopting.erase(browserId);
                interrupted = g_cefAdoptInterrupted.erase(browserId) > 0;
            }

            if (interrupted) {
                // destroyCefView ran while the migration worker was in
                // flight: the browser is already gone, and old/new
                // NodeContents plus both parent Stacks may be freed or owned
                // by a destroyed page — do NOT touch any ArkUI node here
                // (oldParent/newParent/both NodeContents stay untouched; the
                // nodes are intentionally leaked as the safe fallback). Only
                // clean the maps and notify failure.
                OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
                    "adoptCefView: interrupted by destroyCefView, "
                    "browserId=%{public}ld rc=%{public}d (nodes untouched)",
                    browserId, rc);
                {
                    std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
                    g_cefParentNodes.erase(browserId);
                }
                // Map-only cleanup: CleanupDragOutWatchForNode would call
                // unregisterNodeEvent(oldParent->container) — nodes must not
                // be touched in this path.
                CleanupDragOutWatchMapOnly(browserId);
                notify(false);
                return;
            }

            if (rc == 0) {
                // Success: retire the old parent Stack (drag-out watch first —
                // unregister node events BEFORE disposing the node), update the
                // registry, clear the stale user data on the old NodeContent.
                CleanupDragOutWatchForNode(browserId, oldParent->container);
                int32_t err = OH_ArkUI_NodeContent_RemoveNode(oldContent, oldParent->container);
                if (err != ARKUI_ERROR_CODE_NO_ERROR) {
                    OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "napi",
                        "adoptCefView: RemoveNode old failed err=%{public}d (continuing)", err);
                }
                destroyNode(oldParent);
                OH_ArkUI_NodeContent_SetUserData(oldContent, nullptr);
                {
                    std::lock_guard<std::mutex> lock(g_cefParentNodesMutex);
                    g_cefParentNodes[browserId] = newParent;
                }
                OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
                    "adoptCefView: success browserId=%{public}ld", browserId);
                notify(true);
            } else if (rc == -3) {
                // -3 = outcome unknown: the migration task may still arrive
                // late on the CEF UI thread and succeed, re-parenting the
                // browser under newParent->container. Only UNMOUNT the new
                // parent Stack (RemoveNode) — the Stack node itself is
                // intentionally leaked (destroyNode could yank a subtree the
                // late task is building into; the leak is bounded and safer
                // than a use-after-free).
                OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                    "adoptCefView: reparent rc=-3 (outcome unknown), "
                    "browserId=%{public}ld, newParent intentionally leaked",
                    browserId);
                OH_ArkUI_NodeContent_RemoveNode(newContent, newParent->container);
                OH_ArkUI_NodeContent_SetUserData(newContent, nullptr);
                notify(false);
            } else {
                // Failure (definitive): dispose the fresh node, keep the
                // registry untouched, and clear the user data so a later
                // destroyCefView on the same NodeContent cannot resurrect
                // the freed pointer.
                OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
                    "adoptCefView: reparent failed rc=%{public}d browserId=%{public}ld",
                    rc, browserId);
                OH_ArkUI_NodeContent_RemoveNode(newContent, newParent->container);
                destroyNode(newParent);
                OH_ArkUI_NodeContent_SetUserData(newContent, nullptr);
                notify(false);
            }
        });
    }).detach();
    return nullptr;
}

// ---------------------------------------------------------------------------
// OSR adopt callback re-binding (free function + payload struct).
//
// The CEF binary distribution's vendored base::BindOnce (include/cef/include/
// base/cef_bind.h) only supports stateless callables and plain function
// pointers — capturing lambdas do NOT compile against it. Pack every value
// into OsrAdoptRebindPayload and bind RebindOsrAdoptCallbacks (a free
// function) with the payload as its single shared_ptr argument.
// ---------------------------------------------------------------------------

struct OsrAdoptRebindPayload {
    CefRefPtr<arkui_cef::ArkUICefBrowser> browser;
    CefRefPtr<arkui_cef::ArkUICefRenderHandler> handler;
    arkui_cef::PaintCallback paintCallback;
    arkui_cef::ScrollOffsetCallback scrollOffsetCb;
    arkui_cef::VirtualKeyboardCallback virtualKeyboardCb;
    arkui_cef::TextSelectionCallback textSelectionCb;
    arkui_cef::ImeCursorCallback imeCursorCb;
    arkui_cef::StartDragCallback startDragCb;
    arkui_cef::UpdateDragCursorCallback updateDragCursorCb;
    std::function<void(const std::string&)> titleCb;
    std::function<void(bool)> focusTakeCb;
    std::function<void(int)> cursorChangeCb;
};

// Runs on the CEF UI thread (posted via CefPostTask): the Set* callbacks are
// invoked there, so the assignment must also happen there to avoid racing an
// in-flight invocation.
static void RebindOsrAdoptCallbacks(std::shared_ptr<OsrAdoptRebindPayload> payload) {
    if (!payload || !payload->browser || !payload->handler) {
        return;
    }
    payload->handler->SetPaintCallback(payload->paintCallback);
    payload->handler->SetScrollOffsetCallback(payload->scrollOffsetCb);
    payload->handler->SetVirtualKeyboardCallback(payload->virtualKeyboardCb);
    payload->handler->SetTextSelectionCallback(payload->textSelectionCb);
    payload->handler->SetImeCursorCallback(payload->imeCursorCb);
    payload->handler->SetStartDragCallback(payload->startDragCb);
    payload->handler->SetUpdateDragCursorCallback(payload->updateDragCursorCb);
    payload->browser->SetTitleCallback(payload->titleCb);
    payload->browser->SetFocusTakeCallback(payload->focusTakeCb);
    payload->browser->SetCursorChangeCallback(payload->cursorChangeCb);
}

// adoptOsrCefView(browserId, onPaint, onTitleChange, onFocusTake,
//                 onScrollOffset, onVirtualKeyboard, onTextSelection,
//                 onCursorChange, onImeCursor, onStartDrag, onUpdateDragCursor)
// OSR migration callback re-binding: rebinds every createOsrCefView callback
// (except onCreated — the browser already exists) onto the new window's
// OsrCefView instance. No browser is created or destroyed. The Set* calls
// are posted to the CEF UI thread because that is where every callback is
// invoked — assigning anywhere else would race an in-flight invocation.
// Returns true when the rebind was posted.
static napi_value NAPI_Global_adoptOsrCefView(napi_env env, napi_callback_info info) {
    size_t argc = 11;
    napi_value args[11] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 4) {
        napi_throw_error(env, nullptr,
            "Expected 4-11 arguments: browserId, onPaint, onTitleChange, onFocusTake "
            "[, onScrollOffset, onVirtualKeyboard, onTextSelection, onCursorChange, "
            "onImeCursor, onStartDrag, onUpdateDragCursor]");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    auto handler = arkui_cef::ArkUICefManager::getInstance().getRenderHandler(browserId);
    if (!browser || !browser->IsOsr() || !handler) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "adoptOsrCefView: browser/render handler not found or not OSR, browserId=%{public}ld",
            browserId);
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }

    // onPaint — dedicated tsfn (staging-buffer pull path), same as create.
    auto paintTsfn = std::make_shared<OsrPaintTsfn>();
    if (!paintTsfn->Init(env, args[1], "OsrPaintCallback")) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "adoptOsrCefView: failed to init paint tsfn");
        napi_value result;
        napi_get_boolean(env, false, &result);
        return result;
    }
    paintTsfn->SetBrowserId(browserId);
    auto paintHolder = std::make_shared<OsrPaintCallbackHolder>(paintTsfn);
    PaintCallback paintCallback = [paintHolder]() {
        paintHolder->tsfn->Notify();
    };

    auto jsTitleChangeCallback = makeJsThreadSafeCallback<std::string>(env, args[2], "OsrTitleChangeCallback");
    auto jsFocusTakeCallback = makeJsThreadSafeCallback<bool>(env, args[3], "OsrFocusTakeCallback");
    if (!jsTitleChangeCallback || !jsFocusTakeCallback) {
        napi_throw_error(env, nullptr, "Failed to create adopt callbacks");
        return nullptr;
    }
    std::function<void(const std::string&)> titleCb =
        [jsTitleChangeCallback](const std::string& title) { jsTitleChangeCallback->Call(title); };
    std::function<void(bool)> focusTakeCb =
        [jsFocusTakeCallback](bool next) { jsFocusTakeCallback->Call(next); };

    // Optional callbacks — identical wrapping to createOsrCefView.
    std::function<void(double, double)> scrollOffsetCb;
    std::function<void(bool)> virtualKeyboardCb;
    std::function<void(const std::string&)> textSelectionCb;
    std::function<void(int)> cursorChangeCb;
    std::function<void(int, int, int, int)> imeCursorCb;
    std::function<void(const std::string&, const std::string&, const std::string&,
                       const std::string&, const std::string&, const std::vector<uint8_t>&, int, int)> startDragCb;
    std::function<void(int)> updateDragCursorCb;

    if (argc >= 5) {
        napi_valuetype vt;
        napi_typeof(env, args[4], &vt);
        if (vt == napi_function) {
            auto jsScrollCb = makeJsThreadSafeCallback<std::string>(env, args[4], "OsrScrollOffsetCallback");
            if (jsScrollCb) {
                scrollOffsetCb = [jsScrollCb](double x, double y) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%.2f,%.2f", x, y);
                    jsScrollCb->Call(std::string(buf));
                };
            }
        }
    }
    if (argc >= 6) {
        napi_valuetype vt;
        napi_typeof(env, args[5], &vt);
        if (vt == napi_function) {
            auto jsVkCb = makeJsThreadSafeCallback<bool>(env, args[5], "OsrVirtualKeyboardCallback");
            if (jsVkCb) {
                virtualKeyboardCb = [jsVkCb](bool show) { jsVkCb->Call(show); };
            }
        }
    }
    if (argc >= 7) {
        napi_valuetype vt;
        napi_typeof(env, args[6], &vt);
        if (vt == napi_function) {
            auto jsTextCb = makeJsThreadSafeCallback<std::string>(env, args[6], "OsrTextSelectionCallback");
            if (jsTextCb) {
                textSelectionCb = [jsTextCb](const std::string& text) { jsTextCb->Call(text); };
            }
        }
    }
    if (argc >= 8) {
        napi_valuetype vt;
        napi_typeof(env, args[7], &vt);
        if (vt == napi_function) {
            auto jsCursorCb = makeJsThreadSafeCallback<int32_t>(env, args[7], "OsrCursorChangeCallback");
            if (jsCursorCb) {
                cursorChangeCb = [jsCursorCb](int cursorType) {
                    jsCursorCb->Call(static_cast<int32_t>(cursorType));
                };
            }
        }
    }
    if (argc >= 9) {
        napi_valuetype vt;
        napi_typeof(env, args[8], &vt);
        if (vt == napi_function) {
            auto jsImeCursorCb = makeJsThreadSafeCallback<int32_t, int32_t, int32_t, int32_t>(
                env, args[8], "OsrImeCursorCallback");
            if (jsImeCursorCb) {
                imeCursorCb = [jsImeCursorCb](int left, int top, int width, int height) {
                    jsImeCursorCb->Call(static_cast<int32_t>(left), static_cast<int32_t>(top),
                                        static_cast<int32_t>(width), static_cast<int32_t>(height));
                };
            }
        }
    }
    if (argc >= 10) {
        napi_valuetype vt;
        napi_typeof(env, args[9], &vt);
        if (vt == napi_function) {
            auto startDragTsfn = std::make_shared<OsrStartDragTsfn>();
            if (startDragTsfn->Init(env, args[9], "OsrStartDragCallback")) {
                startDragCb = [startDragTsfn](const std::string& text, const std::string& html,
                                              const std::string& url, const std::string& urlTitle,
                                              const std::string& fileName, const std::vector<uint8_t>& fileData,
                                              int x, int y) {
                    startDragTsfn->Call(text, html, url, urlTitle, fileName, fileData, x, y);
                };
            }
        }
    }
    if (argc >= 11) {
        napi_valuetype vt;
        napi_typeof(env, args[10], &vt);
        if (vt == napi_function) {
            auto jsUpdateDragCursorCb = makeJsThreadSafeCallback<int32_t>(
                env, args[10], "OsrUpdateDragCursorCallback");
            if (jsUpdateDragCursorCb) {
                updateDragCursorCb = [jsUpdateDragCursorCb](int operation) {
                    jsUpdateDragCursorCb->Call(static_cast<int32_t>(operation));
                };
            }
        }
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "adoptOsrCefView: rebinding callbacks, browserId=%{public}ld", browserId);

    // Rebind on the CEF UI thread (callback invocation thread — no race).
    // NOTE: the CEF binary distribution's vendored base::BindOnce (include/cef/
    // include/base/cef_bind.h) supports only stateless callables and plain
    // function pointers — a capturing lambda does NOT compile. Pack all values
    // into a single payload and bind a free function taking it by shared_ptr.
    auto payload = std::make_shared<OsrAdoptRebindPayload>();
    payload->browser = browser;
    payload->handler = handler;
    payload->paintCallback = paintCallback;
    payload->scrollOffsetCb = scrollOffsetCb;
    payload->virtualKeyboardCb = virtualKeyboardCb;
    payload->textSelectionCb = textSelectionCb;
    payload->imeCursorCb = imeCursorCb;
    payload->startDragCb = startDragCb;
    payload->updateDragCursorCb = updateDragCursorCb;
    payload->titleCb = titleCb;
    payload->focusTakeCb = focusTakeCb;
    payload->cursorChangeCb = cursorChangeCb;
    CefPostTask(TID_UI, CefCreateClosureTask(
        base::BindOnce(&RebindOsrAdoptCallbacks, payload)));

    // Force a full repaint so the freshly attached surface (and the fallback
    // PixelMap path) receives a complete frame immediately.
    CefPostTask(TID_UI, CefCreateClosureTask(
        base::BindOnce(&ArkUICefBrowser::invalidate, browser)));

    napi_value result;
    napi_get_boolean(env, true, &result);
    return result;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
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
        {"cefWatchDragOut", nullptr, NAPI_Global_cefWatchDragOut, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefUpdateDragOutRect", nullptr, NAPI_Global_cefUpdateDragOutRect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefUnwatchDragOut", nullptr, NAPI_Global_cefUnwatchDragOut, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"adoptCefView", nullptr, NAPI_Global_adoptCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"adoptOsrCefView", nullptr, NAPI_Global_adoptOsrCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    
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
    napi_module_register(&demoModule);
}
