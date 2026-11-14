#include "ArkUICefGpuPresenter.h"

#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <chrono>
#include <cmath>
#include <memory>

namespace arkui_cef {
namespace {
constexpr unsigned int kLogDomain = 0xFF00;
constexpr EGLenum kNativeBufferOhos = 0x34E1;
constexpr uint64_t kNoModifier = 0x00ffffffffffffffULL;
// fence 异步回收参数：在途源资源上限与有界等待时长（纳秒）。
constexpr size_t kMaxPendingSources = 3;
constexpr GLuint64 kBackpressureTimeoutNs = 8ULL * 1000 * 1000;   // 8ms
constexpr GLuint64 kDrainTimeoutNs = 1000ULL * 1000 * 1000;       // 1s（仅销毁路径）
// Diagnostics window for the Paint arrival-rate log.
constexpr int64_t kFpsLogWindowMs = 1000;
const GLfloat kFullRect[] = {-1, -1, 1, 1};
const GLfloat kPresentRect[] = {-1, 1, 1, -1};
const GLfloat kFullUv[] = {0, 0, 1, 1};

// 应用不共享 Chromium context，也不能改变调用线程原有 EGL current 状态。
class CurrentBinding {
public:
    explicit CurrentBinding(EGLDisplay ownedDisplay)
        : ownedDisplay_(ownedDisplay), display_(eglGetCurrentDisplay()),
          context_(eglGetCurrentContext()), draw_(eglGetCurrentSurface(EGL_DRAW)),
          read_(eglGetCurrentSurface(EGL_READ)), api_(eglQueryAPI()) {}
    ~CurrentBinding() {
        bool restored = true;
        if (context_ != EGL_NO_CONTEXT) {
            restored = eglBindAPI(api_) && eglMakeCurrent(display_, draw_, read_, context_);
        } else {
            if (ownedDisplay_ != EGL_NO_DISPLAY)
                restored = eglMakeCurrent(ownedDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            restored = eglBindAPI(api_) && restored;
        }
        if (!restored) OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, "GpuOsrPresenter",
            "[OHOS_GPU_OSR] failed to restore caller thread EGL binding, error=%{public}x", eglGetError());
    }
private:
    EGLDisplay ownedDisplay_;
    EGLDisplay display_;
    EGLContext context_;
    EGLSurface draw_;
    EGLSurface read_;
    EGLenum api_;
};

GLuint CompileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled != GL_TRUE) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, "GpuOsrPresenter",
            "[OHOS_GPU_OSR] shader compile failed: %{public}s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}
}

ArkUICefGpuPresenter::~ArkUICefGpuPresenter() { Shutdown(); }

bool ArkUICefGpuPresenter::Fail(const char* stage) {
    if (!failed_) OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, "GpuOsrPresenter",
        "[OHOS_GPU_OSR] %{public}s failed, EGL=%{public}x GL=%{public}x",
        stage, eglGetError(), glGetError());
    failed_ = true;
    return false;
}

bool ArkUICefGpuPresenter::MakeCurrent() {
    return eglBindAPI(EGL_OPENGL_ES_API) &&
           eglMakeCurrent(display_, surface_, surface_, context_);
}

bool ArkUICefGpuPresenter::Initialize(uint64_t surfaceId, int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (surfaceId == 0 || width <= 0 || height <= 0 || window_ || stopped_)
        return Fail("surface parameters");
    if (OH_NativeWindow_CreateNativeWindowFromSurfaceId(surfaceId, &window_) != 0 || !window_)
        return Fail("NativeWindow from surfaceId");
    width_ = width;
    height_ = height;
    if (OH_NativeWindow_NativeWindowHandleOpt(window_, SET_BUFFER_GEOMETRY, width, height) != 0 ||
        OH_NativeWindow_NativeWindowHandleOpt(window_, SET_FORMAT, NATIVEBUFFER_PIXEL_FMT_RGBA_8888) != 0)
        return Fail("NativeWindow format/size request");
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr))
        return Fail("eglInitialize");
    CurrentBinding binding(display_);
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return Fail("eglBindAPI");
    const EGLint configAttributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display_, configAttributes, &config, 1, &count) || count != 1)
        return Fail("ES3 EGLConfig");
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, contextAttributes);
    if (context_ == EGL_NO_CONTEXT) return Fail("独立 EGLContext");
    surface_ = eglCreateWindowSurface(display_, config,
        reinterpret_cast<EGLNativeWindowType>(window_), nullptr);
    if (surface_ == EGL_NO_SURFACE || !MakeCurrent()) return Fail("EGLWindowSurface");
    createImage_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroyImage_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    imageTarget_ = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!createImage_ || !destroyImage_ || !imageTarget_) return Fail("Native EGLImage extensions");
    if (!BuildProgram()) return Fail("texture composite program build");
    glGenFramebuffers(1, &framebuffer_);
    glGenVertexArrays(1, &vertexArray_);
    if (!framebuffer_ || !vertexArray_ || glGetError() != GL_NO_ERROR)
        return Fail("GL resource allocation");
    OH_LOG_Print(LOG_APP, LOG_INFO, kLogDomain, "GpuOsrPresenter",
        "[OHOS_GPU_OSR] native presenter ready, surfaceId=%{public}llu size=%{public}dx%{public}d",
        static_cast<unsigned long long>(surfaceId), width, height);
    return true;
}

bool ArkUICefGpuPresenter::BuildProgram() {
    const char* vertex = R"(#version 300 es
        precision highp float;
        uniform vec4 uRect;
        uniform vec4 uUv;
        out vec2 vUv;
        void main() {
            vec2 p[4] = vec2[4](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,1));
            vec2 q = p[gl_VertexID];
            gl_Position = vec4(mix(uRect.xy, uRect.zw, q), 0, 1);
            vUv = mix(uUv.xy, uUv.zw, q);
        })";
    const char* fragment = R"(#version 300 es
        precision highp float;
        uniform sampler2D uTexture;
        in vec2 vUv;
        out vec4 color;
        void main() { color = texture(uTexture, vUv); })";
    GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    rectUniform_ = glGetUniformLocation(program_, "uRect");
    uvUniform_ = glGetUniformLocation(program_, "uUv");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "uTexture"), 0);
    return linked == GL_TRUE && rectUniform_ >= 0 && uvUniform_ >= 0 && glGetError() == GL_NO_ERROR;
}

void ArkUICefGpuPresenter::Draw(GLuint texture, const GLfloat* rect, const GLfloat* uv) {
    glUseProgram(program_);
    glBindVertexArray(vertexArray_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform4fv(rectUniform_, 1, rect);
    glUniform4fv(uvUniform_, 1, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

bool ArkUICefGpuPresenter::CopySource(GLuint source, Texture& target,
                                    const cef_rect_t& visible, const cef_size_t& coded) {
    GLint maximum = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maximum);
    if (visible.width > maximum || visible.height > maximum) return false;
    if (!target.id) glGenTextures(1, &target.id);
    glBindTexture(GL_TEXTURE_2D, target.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (target.width != visible.width || target.height != visible.height) {
        target.ready = false;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, visible.width, visible.height,
                     0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        target.width = visible.width;
        target.height = visible.height;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.id, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
    glViewport(0, 0, target.width, target.height);
    glDisable(GL_BLEND);
    const GLfloat uv[] = {
        static_cast<GLfloat>(visible.x) / coded.width,
        static_cast<GLfloat>(visible.y) / coded.height,
        static_cast<GLfloat>(visible.x + visible.width) / coded.width,
        static_cast<GLfloat>(visible.y + visible.height) / coded.height};
    Draw(source, kFullRect, uv);
    return glGetError() == GL_NO_ERROR;
}

bool ArkUICefGpuPresenter::Paint(cef_paint_element_type_t type, const CefAcceleratedPaintInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || failed_) return false;
    if (type != PET_VIEW && type != PET_POPUP) return Fail("paint element type");
    if (info.size < sizeof(cef_accelerated_paint_info_t) ||
        info.extra.size < sizeof(cef_accelerated_paint_info_common_t) || info.plane_count != 1 ||
        info.planes[0].fd < 0 || info.modifier != kNoModifier ||
        (info.format != CEF_COLOR_TYPE_RGBA_8888 && info.format != CEF_COLOR_TYPE_BGRA_8888)) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, "GpuOsrPresenter",
            "[OHOS_GPU_OSR] accelerated frame metadata validation failed size=%{public}zu expect=%{public}zu extra=%{public}zu planes=%{public}d "
            "fd=%{public}d modifier=%{public}llu format=%{public}d native_buffer=%{public}llu addr=%{public}llu "
            "head=%{public}llx,%{public}llx",
            info.size, sizeof(cef_accelerated_paint_info_t), info.extra.size,
            info.plane_count, info.planes[0].fd,
            static_cast<unsigned long long>(info.modifier), static_cast<int>(info.format),
            static_cast<unsigned long long>(info.native_buffer),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&info)),
            static_cast<unsigned long long>(reinterpret_cast<const uint64_t*>(&info)[0]),
            static_cast<unsigned long long>(reinterpret_cast<const uint64_t*>(&info)[34]));
        return Fail("accelerated frame metadata");
    }
    const auto& coded = info.extra.coded_size;
    const auto& visible = info.extra.visible_rect;
    if (coded.width <= 0 || coded.height <= 0 || visible.x < 0 || visible.y < 0 ||
        visible.width <= 0 || visible.height <= 0 || visible.x > coded.width || visible.y > coded.height ||
        visible.width > coded.width - visible.x || visible.height > coded.height - visible.y)
        return Fail("coded_size/visible_rect");
    // native_buffer 是本进程可直接使用的 OHNativeWindowBuffer*，借用期仅限本次
    // 回调；跨进程解析由 CEF 内部完成，应用不参与。
    OHNativeWindowBuffer* borrowed =
        reinterpret_cast<OHNativeWindowBuffer*>(info.native_buffer);
    if (!borrowed) return Fail("accelerated frame without native buffer");
    // 源纹理需在 fence 点亮后才回收，故自主加一次引用以便跨回调持有。
    if (OH_NativeWindow_NativeObjectReference(borrowed) != 0)
        return Fail("native buffer ref-count acquire");
    ScopedWindowBuffer native(borrowed);
    const BufferHandle* actual = OH_NativeWindow_GetBufferHandleFromNative(native.get());
    const int nativeFormat = info.format == CEF_COLOR_TYPE_BGRA_8888
        ? NATIVEBUFFER_PIXEL_FMT_BGRA_8888 : NATIVEBUFFER_PIXEL_FMT_RGBA_8888;
    const auto& plane = info.planes[0];
    if (!actual || actual->width != coded.width ||
        actual->height != coded.height || actual->format != nativeFormat || actual->stride <= 0 ||
        actual->size <= 0 || plane.offset != 0 || plane.stride != static_cast<uint32_t>(actual->stride) ||
        plane.size != static_cast<uint64_t>(actual->size) ||
        plane.stride < static_cast<uint64_t>(coded.width) * 4 ||
        plane.size < static_cast<uint64_t>(plane.stride) * coded.height) {
        // 几何不一致是瞬态的（典型场景：GPU 进程重启窗口期，跨进程 lease 解析
        // 拿到旧代际缓冲）。只丢弃本帧，不做永久熔断——永久熔断会把一次瞬态
        // 不一致放大为整个会话的画面冻结。硬失败（EGL/GL）仍保留熔断。
        OH_LOG_Print(LOG_APP, LOG_WARN, kLogDomain, "GpuOsrPresenter",
            "[OHOS_GPU_OSR] frame geometry mismatch with paint planes, dropping this frame actual=%{public}p w=%{public}d h=%{public}d fmt=%{public}d "
            "stride=%{public}d size=%{public}d | coded=%{public}dx%{public}d nativeFmt=%{public}d | "
            "plane off=%{public}d stride=%{public}u size=%{public}llu",
            static_cast<const void*>(actual), actual ? actual->width : -1, actual ? actual->height : -1,
            actual ? actual->format : -1, actual ? actual->stride : -1, actual ? actual->size : -1,
            coded.width, coded.height, nativeFormat,
            plane.offset, plane.stride,
            static_cast<unsigned long long>(plane.size));
        return false;
    }
    CurrentBinding binding(display_);
    if (!MakeCurrent()) return Fail("callback EGL make current");
    const EGLint attributes[] = {EGL_NONE};
    EGLImageKHR image = createImage_(display_, EGL_NO_CONTEXT, kNativeBufferOhos,
                                    reinterpret_cast<EGLClientBuffer>(native.get()), attributes);
    if (image == EGL_NO_IMAGE_KHR) return Fail("EGL_NATIVE_BUFFER_OHOS import");
    GLuint source = 0;
    glGenTextures(1, &source);
    glBindTexture(GL_TEXTURE_2D, source);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    imageTarget_(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(image));
    bool copied = source != 0 && glGetError() == GL_NO_ERROR &&
        CopySource(source, type == PET_VIEW ? view_ : popup_, visible, coded);
    if (!copied) {
        // 复制未入队成功：没有 GPU 读取，可立即释放源资源。
        glBindTexture(GL_TEXTURE_2D, 0);
        if (source) glDeleteTextures(1, &source);
        destroyImage_(display_, image);
        return Fail("source texture copy");
    }
    // 复制命令已入队但未执行。不再 glFinish 阻塞 UI 线程，而是插 fence 并把
    // 源纹理/EGLImage/缓冲区引用移交在途队列；fence 点亮后异步回收。
    // 我们已用 NDK 引用计数自主持有一份引用，因此 CEF 在回调返回后释放它自己
    // 那份也不会让帧像素失效——这不违反「帧内容仅回调期有效」契约。
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (fence) glFlush();  // fence 本身不强制提交命令流，显式刷出让 GPU 尽早执行
    if (!fence) {
        // fence 不可用则退回保守路径：阻塞等待后同步释放（正确性优先）。
        glFinish();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDeleteTextures(1, &source);
        bool destroyed = destroyImage_(display_, image) == EGL_TRUE;
        native.reset();
        if (glGetError() != GL_NO_ERROR || !destroyed) return Fail("sync copy/source resource release");
        (type == PET_VIEW ? view_ : popup_).ready = true;
        if (!PresentCurrent()) return Fail("owned-texture present");
        return true;
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    PendingSource pending;
    pending.texture = source;
    pending.image = image;
    pending.buffer = std::move(native);
    pending.fence = fence;
    pending_.push_back(std::move(pending));
    // 背压：在途队列积压说明 GPU 落后于捕获节奏，有界等待最老 fence，
    // 防止源缓冲区引用无限堆积。
    if (pending_.size() > kMaxPendingSources) {
        glClientWaitSync(pending_.front().fence, GL_SYNC_FLUSH_COMMANDS_BIT,
                         kBackpressureTimeoutNs);
    }
    ReapCompletedSources();
    (type == PET_VIEW ? view_ : popup_).ready = true;
    if (!PresentCurrent()) return Fail("owned-texture present");
    // 呈现命令入队后也回收一次（present 完成的帧其源多半已可释放）。
    ReapCompletedSources();
    // 诊断：统计 Paint 实际到达帧率，用于区分「捕获掉速」与「呈现异步」。
    {
        auto now = std::chrono::steady_clock::now();
        int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count();
        if (paintWindowStartMs_ == 0) paintWindowStartMs_ = nowMs;
        paintFrameCount_++;
        int64_t elapsed = nowMs - paintWindowStartMs_;
        if (elapsed >= kFpsLogWindowMs) {
            OH_LOG_Print(LOG_APP, LOG_INFO, kLogDomain, "GpuOsrPresenter",
                "[OHOS_GPU_OSR] Paint arrival rate=%{public}.1f fps (%{public}lld frames/%{public}lld ms)",
                paintFrameCount_ * 1000.0 / elapsed,
                static_cast<long long>(paintFrameCount_),
                static_cast<long long>(elapsed));
            paintFrameCount_ = 0;
            paintWindowStartMs_ = nowMs;
        }
    }
    return true;
}

void ArkUICefGpuPresenter::ReapCompletedSources() {
    // 仅在持有 mutex_ 且自有 context current 时调用。timeout=0 纯轮询，绝不阻塞。
    while (!pending_.empty()) {
        PendingSource& front = pending_.front();
        GLenum wait = glClientWaitSync(front.fence, 0, 0);
        if (wait == GL_TIMEOUT_EXPIRED) break;  // 队首未完成，其后必然也未完成
        if (wait == GL_WAIT_FAILED) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, "GpuOsrPresenter",
                "[OHOS_GPU_OSR] fence query failed GL=%{public}x, fence=%{public}p, pending=%{public}zu",
                glGetError(), static_cast<const void*>(front.fence), pending_.size());
            glClientWaitSync(front.fence, GL_SYNC_FLUSH_COMMANDS_BIT, kBackpressureTimeoutNs);
        }
        if (front.texture) glDeleteTextures(1, &front.texture);
        if (front.image != EGL_NO_IMAGE_KHR) destroyImage_(display_, front.image);
        front.buffer.reset();  // 归还我们自主持有的缓冲区引用
        glDeleteSync(front.fence);
        pending_.pop_front();
    }
}

void ArkUICefGpuPresenter::DrainPendingSources() {
    // 销毁路径：阻塞等待全部在途 fence，确保 GPU 不再触碰源资源后才拆 context。
    while (!pending_.empty()) {
        PendingSource& front = pending_.front();
        glClientWaitSync(front.fence, GL_SYNC_FLUSH_COMMANDS_BIT, kDrainTimeoutNs);
        if (front.texture) glDeleteTextures(1, &front.texture);
        if (front.image != EGL_NO_IMAGE_KHR) destroyImage_(display_, front.image);
        front.buffer.reset();
        glDeleteSync(front.fence);
        pending_.pop_front();
    }
}

bool ArkUICefGpuPresenter::PresentCurrent() {
    if (!view_.ready) return true;
    EGLint width = 0;
    EGLint height = 0;
    if (!eglQuerySurface(display_, surface_, EGL_WIDTH, &width) ||
        !eglQuerySurface(display_, surface_, EGL_HEIGHT, &height) || width <= 0 || height <= 0)
        return false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_BLEND);
    Draw(view_.id, kPresentRect, kFullUv);
    if (popupVisible_ && popup_.ready && popupRect_.width > 0 && popupRect_.height > 0) {
        const GLfloat rect[] = {
            2.0f * popupRect_.x * scale_ / width - 1.0f,
            1.0f - 2.0f * popupRect_.y * scale_ / height,
            2.0f * (static_cast<float>(popupRect_.x) + popupRect_.width) * scale_ / width - 1.0f,
            1.0f - 2.0f * (static_cast<float>(popupRect_.y) + popupRect_.height) * scale_ / height};
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        Draw(popup_.id, rect, kFullUv);
        glDisable(GL_BLEND);
    }
    return glGetError() == GL_NO_ERROR && eglSwapBuffers(display_, surface_) == EGL_TRUE;
}

void ArkUICefGpuPresenter::PresentExisting() {
    if (stopped_ || failed_ || !view_.ready) return;
    CurrentBinding binding(display_);
    if (!MakeCurrent() || !PresentCurrent()) Fail("popup state present");
}

void ArkUICefGpuPresenter::Resize(int width, int height) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || failed_) return;
    if (width <= 0 || height <= 0) { Fail("resize dimensions"); return; }
    if (width == width_ && height == height_) return;
    if (OH_NativeWindow_NativeWindowHandleOpt(window_, SET_BUFFER_GEOMETRY, width, height) != 0) {
        Fail("resize NativeWindow"); return;
    }
    width_ = width;
    height_ = height;
}

void ArkUICefGpuPresenter::SetScale(float scale) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::isfinite(scale) && scale > 0) scale_ = scale;
}

void ArkUICefGpuPresenter::SetPopupVisible(bool show) {
    std::lock_guard<std::mutex> lock(mutex_);
    popupVisible_ = show;
    if (!show) popup_.ready = false;
    PresentExisting();
}

void ArkUICefGpuPresenter::SetPopupRect(const CefRect& rect) {
    std::lock_guard<std::mutex> lock(mutex_);
    popupRect_ = rect;
    popup_.ready = false;
    PresentExisting();
}

void ArkUICefGpuPresenter::ShutdownLocked() {
    if (context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE) {
        CurrentBinding binding(display_);
        if (MakeCurrent()) {
            // 先阻塞回收全部在途源资源，确保 GPU 不再引用它们，再拆自有资源。
            DrainPendingSources();
            glFinish();
            if (view_.id) glDeleteTextures(1, &view_.id);
            if (popup_.id) glDeleteTextures(1, &popup_.id);
            if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
            if (vertexArray_) glDeleteVertexArrays(1, &vertexArray_);
            if (program_) glDeleteProgram(program_);
        } else {
            OH_LOG_Print(LOG_APP, LOG_ERROR, kLogDomain, "GpuOsrPresenter",
                "[OHOS_GPU_OSR] make current failed during shutdown, EGL=%{public}x, pending=%{public}zu",
                eglGetError(), pending_.size());
            // 无法 current：GPU 资源交由 context 销毁回收，但缓冲区引用是 CPU
            // 侧计数，必须在这里释放，否则该缓冲区泄漏。
            for (auto& pending : pending_) {
                pending.buffer.reset();
                pending.fence = nullptr;  // sync 对象随 context 销毁失效
                pending.texture = 0;
                pending.image = EGL_NO_IMAGE_KHR;
            }
            pending_.clear();
        }
    }
    if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
    if (window_) OH_NativeWindow_DestroyNativeWindow(window_);
    window_ = nullptr;
    // EGL_DEFAULT_DISPLAY 可能被其他组件共用，不能 eglTerminate 它。
    display_ = EGL_NO_DISPLAY;
    view_ = {};
    popup_ = {};
    framebuffer_ = vertexArray_ = program_ = 0;
}

void ArkUICefGpuPresenter::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    ShutdownLocked();
}

} // namespace arkui_cef
