#ifndef ARKUI_CEF_BROWSER_H
#define ARKUI_CEF_BROWSER_H

#include "arkui/native_node.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "ArkUICefClient.h"
#include <string>
#include <functional>
#include <memory>
#include "ArkUICefGpuPresenter.h"

namespace EMBEDDED_WINDOW_ADAPTER {
struct Node {
  ArkUI_NodeType nodeType; // Type of the node. The value can be ARKUI_NODE_XCOMPONENT or other types supported in later versions.
  ArkUI_NodeHandle node; // ARKUI_NODE_XCOMPONENT type or other.
  ArkUI_NodeHandle container; // Must be of type ARKUI_NODE_STACK.
  char nodeOwner[8]; // Describes the body of the Node structure to be created, such as "QT," "CEF," "SDL2," "FLUT".
  void* nodePrivate; // Structure used for the definition of the nodeOwner body record itself.
};
}

namespace arkui_cef {

/**
 * @class ArkUICefBrowser
 * @brief 封装单个 CEF 浏览器实例，管理浏览器的创建、销毁、导航、显示/隐藏等操作。
 * @thread 所有公开方法需在 CEF UI 线程调用，或通过 CefPostTask 转发。
 */
class ArkUICefBrowser : public CefBaseRefCounted {
public:
    using LoadCallback = std::function<void(bool OK)>;
    using TitleCallback = std::function<void(const std::string& title)>;
    using CreateCallback = std::function<void(int64_t)>;
    using FocusTakeCallback = std::function<void(bool next)>;
    using CursorChangeCallback = std::function<void(int cursorType)>;
    using Node = EMBEDDED_WINDOW_ADAPTER::Node;
    
    /**
     * @brief 构造函数
     * @param id 浏览器唯一标识符
     * @param width 初始宽度（像素）
     * @param height 初始高度（像素）
     */
    ArkUICefBrowser(const int64_t id, int width, int height);
    
    /**
     * @brief 析构函数，确保浏览器正确关闭
     */
    ~ArkUICefBrowser();
    
    /**
     * @brief 创建 CEF 浏览器实例
     * @param client CEF 客户端回调处理对象
     * @param parentNode 父窗口节点指针
     * @param url 初始加载 URL
     * @thread 应在 CEF UI 线程调用
     */
    void CreateBrowser(CefRefPtr<ArkUICefClient>, Node* parentNode, const std::string& url);
    
    /**
     * @brief 关闭浏览器
     * @thread 应在 CEF UI 线程调用
     */
    void close();
    
    /**
     * @brief 加载指定 URL
     * @param url 目标 URL
     * @thread 应在 CEF UI 线程调用
     */
    void LoadUrl(const std::string& url);
    
    /**
     * @brief 后退到上一页
     * @thread 应在 CEF UI 线程调用
     */
    void GoBack();
    
    /**
     * @brief 前进到下一页
     * @thread 应在 CEF UI 线程调用
     */
    void GoForward();
    
    /**
     * @brief 刷新当前页面
     * @thread 应在 CEF UI 线程调用
     */
    void reload();
    
    /**
     * @brief 调整浏览器尺寸
     * @param width 目标宽度（像素）
     * @param height 目标高度（像素）
     * @thread 应在 CEF UI 线程调用
     */
    void resize(int width, int height);
    
    /**
     * @brief 显示浏览器（设置可见性为 VISIBLE）
     * @thread 应在 CEF UI 线程调用
     */
    void show();
    
    /**
     * @brief 隐藏浏览器（设置可见性为 HIDDEN）
     * @thread 应在 CEF UI 线程调用
     */
    void hide();

    /**
     * @brief 向浏览器发送按键事件
     * @param event CEF 按键事件结构体
     * @thread 应在 CEF UI 线程调用
     */
    void SendKeyEvent(const CefKeyEvent& event);

    /**
     * @brief 向浏览器发送鼠标点击事件 (OSR 模式)
     * @param x 鼠标 X 坐标（相对于视图左上角）
     * @param y 鼠标 Y 坐标（相对于视图左上角）
     * @param button 鼠标按钮 (0=左, 1=中, 2=右)
     * @param mouseUp true=释放, false=按下
     * @param clickCount 点击次数
     * @thread 应在 CEF UI 线程调用
     */
    void SendMouseClickEvent(int x, int y, int button, bool mouseUp, int clickCount, uint32_t modifiers = 0);

    /**
     * @brief 向浏览器发送鼠标移动事件 (OSR 模式)
     * @param x 鼠标 X 坐标
     * @param y 鼠标 Y 坐标
     * @param mouseLeave true=鼠标离开视图
     * @thread 应在 CEF UI 线程调用
     */
    void SendMouseMoveEvent(int x, int y, bool mouseLeave, uint32_t modifiers = 0);

    /**
     * @brief 向浏览器发送鼠标滚轮事件 (OSR 模式)
     * @param x 鼠标 X 坐标
     * @param y 鼠标 Y 坐标
     * @param deltaX 水平滚动量
     * @param deltaY 垂直滚动量
     * @thread 应在 CEF UI 线程调用
     */
    void SendMouseWheelEvent(int x, int y, int deltaX, int deltaY, uint32_t modifiers = 0);

    /**
     * @brief 向浏览器发送焦点事件 (OSR 模式)
     * @param focused true=获得焦点, false=失去焦点
     */
    void SendFocusEvent(bool focused);

    /**
     * @brief 通知浏览器捕获丢失 (OSR 模式)
     */
    void SendCaptureLostEvent();

    /**
     * @brief 手动触发重绘 (OSR 模式)
     */
    void invalidate();

    /**
     * @brief 通知浏览器可见性变化 (OSR 模式)
     * @param hidden true=不可见（挂起布局与 OnPaint），false=恢复渲染
     * @thread 应在 CEF UI 线程调用
     */
    void WasHidden(bool hidden);

    /**
     * @brief 设置OSR窗口可见性 (OSR 模式)
     *
     * 通过 CefBrowserHost::WasHidden 将应用侧的标签页可见状态通知 CEF：
     * 隐藏后 CEF 停止 layout 与 OnPaint/帧采集，renderer 侧
     * document.visibilityState 变为 hidden，定时器节流、rAF 停发。
     * browser 尚未创建时仅记录状态，OnBrowserCreated 时补调。
     * GPU OSR 标签的 tabActive 通道使用本接口（内部复用 WasHidden 的
     * kick-start 恢复路径）。
     *
     * @param visible true=可见, false=不可见
     */
    void SetWindowlessVisibility(bool visible);

    /**
     * @brief 向浏览器发送触摸事件 (OSR 模式)
     * @param x 触摸 X 坐标
     * @param y 触摸 Y 坐标
     * @param touchType 触摸类型，与 cef_touch_event_type_t 枚举一致：
     *                 0=RELEASED(抬起) 1=PRESSED(按下) 2=MOVED(移动) 3=CANCELLED(取消)。
     *                 注意与 ArkUI TouchType(DOWN=0/UP=1/MOVE=2/CANCEL=3) 不同，
     *                 ArkTS 侧需自行映射（参考 OsrCefView.handleTouchEvent）。
     * @param pointerId 触摸点 ID
     */
    void SendTouchEvent(int x, int y, int touchType, int pointerId);

    void ImeSetComposition(const std::string& text);
    void ImeCommitText(const std::string& text);
    void ImeCancelComposition();

    void DragTargetDragEnter(const std::string& text, const std::string& html,
                             const std::string& url, const std::string& filePaths,
                             int x, int y, int allowedOps);
    void DragTargetDragOver(int x, int y, int allowedOps);
    void DragTargetDragLeave();
    void DragTargetDrop(int x, int y);
    void DragSourceEndedAt(int x, int y, int op);
    void DragSourceSystemDragEnded();

    /**
     * @brief 获取浏览器唯一 ID
     * @return 浏览器 ID
     */
    int64_t GetId() const { return id_; }

    /** @brief 获取关联的 Client（OSR 渲染链路查询用，可能为空） */
    CefRefPtr<ArkUICefClient> GetClient() const { return client_; }

    /** @brief 获取 CEF Browser（页面执行 JS 等用，创建前可能为空） */
    CefRefPtr<CefBrowser> GetCefBrowser() const { return browser_; }
    
    /**
     * @brief 获取底层窗口句柄
     * @return 窗口节点指针
     * @thread 应在 CEF UI 线程调用
     */
    Node* getWindowHandle() const;
    
    /**
     * @brief 设置页面加载状态回调
     * @param callback 加载完成回调，参数表示是否加载成功
     */
    void SetLoadCallback(LoadCallback callback) { loadCallback_ = std::move(callback); }
    
    /**
     * @brief 设置页面标题变更回调
     * @param callback 标题变更回调，参数为新标题
     */
    void SetTitleCallback(TitleCallback callback) { titleCallback_ = std::move(callback); }

    /**
     * @brief 设置CEF实例创建回调
     * @param callback CEF实例创建完成后回调，参数为browser id
     */
    void SetCreateCallback(CreateCallback callback) { createCallback_ = std::move(callback); }

    /**
     * @brief 设置焦点离开回调
     * @param callback 焦点离开回调，参数为是否向下一个组件移动
     */
    void SetFocusTakeCallback(FocusTakeCallback callback) { focusTakeCallback_ = std::move(callback); }

    void SetCursorChangeCallback(CursorChangeCallback callback) { cursorChangeCallback_ = std::move(callback); }

    /** @brief 设置是否使用离屏渲染模式 */
    void SetUseOsr(bool use_osr) { use_osr_ = use_osr; }
    /** @brief 是否使用离屏渲染模式 */
    bool IsOsr() const { return use_osr_; }
    /** @brief 是否 windowed（非 OSR）模式 */
    bool IsWindowed() const { return !use_osr_; }

    void SetGpuPresenter(std::shared_ptr<ArkUICefGpuPresenter> presenter) {
        gpuPresenter_ = std::move(presenter);
    }
    void StopGpuPresentation() {
        if (gpuPresenter_) gpuPresenter_->Shutdown();
    }

    /**
     * @brief 设置OSR设备缩放因子
     * @param factor 设备缩放因子（如2.0表示Retina屏幕）
     * @thread 应在 CEF UI 线程调用
     */
    void SetOsrScale(float factor);

    void SetComponentOffset(int x, int y);

    void SetWindowOrigin(int x, int y);

    /**
     * @brief 设置离屏渲染最大帧率（OSR 模式）
     * @param frameRate 帧率值（fps），最小 1
     * @thread 应在 CEF UI 线程调用
     */
    void SetFrameRate(int frameRate);
    
    /**
     * @brief 内部回调：浏览器创建完成
     * @param browser CEF Browser 引用
     * @thread 由 ArkUICefClient 在 CEF UI 线程调用
     */
    void OnBrowserCreated(CefRefPtr<CefBrowser> browser);
    
    /**
     * @brief 内部回调：浏览器关闭完成
     * @thread 由 ArkUICefClient 在 CEF UI 线程调用
     */
    void OnBrowserClosed();
    
    IMPLEMENT_REFCOUNTING(ArkUICefBrowser);

private:
    void ApplyOsrPopupOffset(int& x, int& y);

    int64_t id_;                    ///< Unique browser identifier
    int width_;                     ///< Current width
    int height_;                    ///< Current height
    bool browserCreated_;           ///< Whether the CEF browser has been created
    bool browserClosing_;           ///< Whether the browser is closing
    
    CefRefPtr<CefBrowser> browser_; ///< CEF browser reference
    CefRefPtr<ArkUICefClient> client_; ///< Associated client reference
    
    CreateCallback createCallback_; ///< Browser creation callback
    LoadCallback loadCallback_;     ///< Page load callback
    TitleCallback titleCallback_;   ///< Title change callback
    FocusTakeCallback focusTakeCallback_; ///< Focus-taken callback
    CursorChangeCallback cursorChangeCallback_; ///< Cursor change callback

    bool use_osr_ = false;            ///< Whether off-screen rendering is enabled

    std::shared_ptr<ArkUICefGpuPresenter> gpuPresenter_;

    bool wasHidden_ = false;          ///< Hidden state last reported by the app (stashed until the browser exists)
    bool visibilityPending_ = false;  ///< WasHidden must be replayed after browser creation
};
} // namespace arkui_cef
#endif // ARKUI_CEF_BROWSER_H
