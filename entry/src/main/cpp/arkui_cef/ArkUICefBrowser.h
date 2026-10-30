#ifndef ARKUI_CEF_BROWSER_H
#define ARKUI_CEF_BROWSER_H

#include "arkui/native_node.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include <string>
#include <functional>

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

class ArkUICefClient;

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
    void createBrowser(CefRefPtr<ArkUICefClient>, Node* parentNode, const std::string& url);
    
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
    void loadUrl(const std::string& url);
    
    /**
     * @brief 后退到上一页
     * @thread 应在 CEF UI 线程调用
     */
    void goBack();
    
    /**
     * @brief 前进到下一页
     * @thread 应在 CEF UI 线程调用
     */
    void goForward();
    
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
    void sendKeyEvent(const CefKeyEvent& event);

    /**
     * @brief 向浏览器发送鼠标点击事件 (OSR 模式)
     * @param x 鼠标 X 坐标（相对于视图左上角）
     * @param y 鼠标 Y 坐标（相对于视图左上角）
     * @param button 鼠标按钮 (0=左, 1=中, 2=右)
     * @param mouseUp true=释放, false=按下
     * @param clickCount 点击次数
     * @thread 应在 CEF UI 线程调用
     */
    void sendMouseClickEvent(int x, int y, int button, bool mouseUp, int clickCount, uint32_t modifiers = 0);

    /**
     * @brief 向浏览器发送鼠标移动事件 (OSR 模式)
     * @param x 鼠标 X 坐标
     * @param y 鼠标 Y 坐标
     * @param mouseLeave true=鼠标离开视图
     * @thread 应在 CEF UI 线程调用
     */
    void sendMouseMoveEvent(int x, int y, bool mouseLeave, uint32_t modifiers = 0);

    /**
     * @brief 向浏览器发送鼠标滚轮事件 (OSR 模式)
     * @param x 鼠标 X 坐标
     * @param y 鼠标 Y 坐标
     * @param deltaX 水平滚动量
     * @param deltaY 垂直滚动量
     * @thread 应在 CEF UI 线程调用
     */
    void sendMouseWheelEvent(int x, int y, int deltaX, int deltaY, uint32_t modifiers = 0);

    /**
     * @brief 向浏览器发送焦点事件 (OSR 模式)
     * @param focused true=获得焦点, false=失去焦点
     */
    void sendFocusEvent(bool focused);

    /**
     * @brief 通知浏览器捕获丢失 (OSR 模式)
     */
    void sendCaptureLostEvent();

    /**
     * @brief 手动触发重绘 (OSR 模式)
     */
    void invalidate();

    /**
     * @brief 设置OSR窗口可见性 (OSR 模式)
     * @param visible true=可见, false=不可见
     */
    void setWindowlessVisibility(bool visible);

    /**
     * @brief 向浏览器发送触摸事件 (OSR 模式)
     * @param x 触摸 X 坐标
     * @param y 触摸 Y 坐标
     * @param touchType 触摸类型 (0=down, 1=moved, 2=up, 3=cancel)
     * @param pointerId 触摸点 ID
     */
    void sendTouchEvent(int x, int y, int touchType, int pointerId);

    void imeSetComposition(const std::string& text);
    void imeCommitText(const std::string& text);
    void imeCancelComposition();

    /**
     * @brief 获取浏览器唯一 ID
     * @return 浏览器 ID
     */
    int64_t getId() const { return id_; }
    
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

    /**
     * @brief 设置OSR设备缩放因子
     * @param factor 设备缩放因子（如2.0表示Retina屏幕）
     * @thread 应在 CEF UI 线程调用
     */
    void setOsrScale(float factor);

    void setComponentOffset(int x, int y);
    
    /**
     * @brief 内部回调：浏览器创建完成
     * @param browser CEF Browser 引用
     * @thread 由 ArkUICefClient 在 CEF UI 线程调用
     */
    void onBrowserCreated(CefRefPtr<CefBrowser> browser);
    
    /**
     * @brief 内部回调：浏览器关闭完成
     * @thread 由 ArkUICefClient 在 CEF UI 线程调用
     */
    void onBrowserClosed();
    
    IMPLEMENT_REFCOUNTING(ArkUICefBrowser);

private:
    int64_t id_;                    ///< 浏览器唯一标识符
    int width_;                     ///< 当前宽度
    int height_;                    ///< 当前高度
    bool browserCreated_;           ///< 浏览器是否已创建
    bool browserClosing_;           ///< 浏览器是否正在关闭
    
    CefRefPtr<CefBrowser> browser_; ///< CEF Browser 引用
    CefRefPtr<ArkUICefClient> client_; ///< 关联的 Client 引用
    
    CreateCallback createCallback_; ///< CEF实例创建回调
    LoadCallback loadCallback_;     ///< 页面加载回调
    TitleCallback titleCallback_;   ///< 标题变更回调
    FocusTakeCallback focusTakeCallback_; ///< 焦点离开回调
    CursorChangeCallback cursorChangeCallback_; ///< 光标变更回调

    bool use_osr_ = false;            ///< 是否使用离屏渲染模式
};
} // namespace arkui_cef
#endif // ARKUI_CEF_BROWSER_H
