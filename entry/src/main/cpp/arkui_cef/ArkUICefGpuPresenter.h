#ifndef ARKUI_CEF_GPU_PRESENTER_H
#define ARKUI_CEF_GPU_PRESENTER_H

#include "include/cef_render_handler.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <native_window/external_window.h>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace arkui_cef {

// Copies only the current callback's source image; textures kept across
// callbacks are all allocated by the application itself.
class ArkUICefGpuPresenter {
public:
    ~ArkUICefGpuPresenter();
    bool Initialize(uint64_t surfaceId, int width, int height);
    bool Paint(cef_paint_element_type_t type, const CefAcceleratedPaintInfo& info);
    void Resize(int width, int height);
    void SetScale(float scale);
    void SetPopupVisible(bool show);
    void SetPopupRect(const CefRect& rect);
    void Shutdown();

private:
    struct Texture {
        GLuint id = 0;
        int width = 0;
        int height = 0;
        bool ready = false;
    };
    // CEF lends us the OHNativeWindowBuffer* (info.native_buffer) for the
    // duration of the callback only. This is the same layer as Windows'
    // shared texture handle, macOS' IOSurfaceRef and Linux's dmabuf fd: a
    // platform-native handle consumed via official platform APIs, with no
    // CEF private library involved.
    //
    // The source texture must outlive the callback until its fence signals,
    // so we hold our own reference. The NDK-public
    // NativeObjectReference/Unreference manage that ref-count, mirroring
    // retaining an IOSurfaceRef on macOS. Both entry points operate on the
    // same ref-count DestroyNativeWindowBuffer uses (per the official docs,
    // Destroy is "decrement by 1, destroy at zero"), so even if CEF drops
    // its own reference first, ours keeps the buffer alive.
    struct NativeObjectUnreference {
        void operator()(OHNativeWindowBuffer* buffer) const {
            if (buffer) OH_NativeWindow_NativeObjectUnreference(buffer);
        }
    };
    using ScopedWindowBuffer =
        std::unique_ptr<OHNativeWindowBuffer, NativeObjectUnreference>;
    // In-flight source resources awaiting GPU completion: none of the
    // source texture / EGLImage / native-buffer reference may be released
    // before the fence signals. The UI thread only reaps them via
    // non-blocking polls, replacing a blocking glFinish in the callback.
    struct PendingSource {
        GLuint texture = 0;
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        ScopedWindowBuffer buffer{nullptr};
        GLsync fence = nullptr;
    };
    bool Fail(const char* stage);
    bool MakeCurrent();
    bool BuildProgram();
    bool CopySource(GLuint source, Texture& target, const cef_rect_t& visible,
                    const cef_size_t& coded);
    bool PresentCurrent();
    void PresentExisting();
    void Draw(GLuint texture, const GLfloat* rect, const GLfloat* uv);
    void ReapCompletedSources();
    void DrainPendingSources();
    void ShutdownLocked();

    std::mutex mutex_;
    bool failed_ = false;
    bool stopped_ = false;
    int width_ = 0;
    int height_ = 0;
    float scale_ = 1.0f;
    bool popupVisible_ = false;
    CefRect popupRect_;
    OHNativeWindow* window_ = nullptr;
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;
    PFNEGLCREATEIMAGEKHRPROC createImage_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTarget_ = nullptr;
    GLuint program_ = 0;
    GLuint framebuffer_ = 0;
    GLuint vertexArray_ = 0;
    GLint rectUniform_ = -1;
    GLint uvUniform_ = -1;
    Texture view_;
    Texture popup_;
    // UI thread only (Paint/Shutdown under mutex_); depth is bounded and
    // overflow applies bounded backpressure waiting.
    std::deque<PendingSource> pending_;
    // Diagnostics: Paint arrival-rate counters (one log per second).
    int64_t paintFrameCount_ = 0;
    int64_t paintWindowStartMs_ = 0;
};

} // namespace arkui_cef
#endif
