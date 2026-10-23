#ifndef ARKUI_CEF_CLIENT_H
#define ARKUI_CEF_CLIENT_H

#include "include/cef_client.h"
#include "include/cef_browser.h"
#include "include/cef_download_handler.h"
#include "include/cef_focus_handler.h"
#include "include/cef_keyboard_handler.h"
#include "include/cef_permission_handler.h"
#include <functional>

namespace arkui_cef {
class ArkUICefBrowser;

/**
 * @class ArkUICefDelegate
 * @brief 浏览器生命周期回调接口
 * @note 由 ArkUICefManager 实现，用于接收浏览器创建/销毁事件
 */
class ArkUICefDelegate {
public:
    /**
     * @brief 浏览器创建完成回调
     * @param browser 新创建的浏览器实例
     */
    virtual void onBrowserCreated(CefRefPtr<ArkUICefBrowser> browser) = 0;
    
    /**
     * @brief 浏览器销毁完成回调
     * @param browser 被销毁的浏览器实例
     */
    virtual void onBrowserDestroyed(CefRefPtr<ArkUICefBrowser> browser) = 0;
};

/**
 * @class ArkUICefClient
 * @brief CEF 客户端处理类，实现 CefClient、CefDisplayHandler、CefLifeSpanHandler、CefLoadHandler 接口。
 * @brief 处理标题变更、弹窗拦截、加载状态、生命周期等浏览器事件。
 * @thread 所有回调在 CEF UI 线程执行。
 */
class ArkUICefClient : public CefClient,
                       public CefDisplayHandler,
                       public CefLifeSpanHandler,
                       public CefLoadHandler,
                       public CefDownloadHandler,
                       public CefPermissionHandler,
                       public CefFocusHandler,
                       public CefKeyboardHandler {
public:
    /** @brief 页面加载状态回调，参数表示是否加载成功 */
    using LoadCallback = std::function<void(bool OK)>;
    /** @brief 页面标题变更回调，参数为新标题 */
    using TitleCallback = std::function<void(const std::string& title)>;
    /** @brief 焦点离开回调，参数为是否向下一个组件移动 */
    using FocusTakeCallback = std::function<void(bool next)>;
    
    /**
     * @brief 构造函数
     * @param id 关联的浏览器 ID
     * @param browser ArkUICefBrowser 弱引用
     * @param delegate 生命周期回调委托者（通常为 ArkUICefManager）
     */
    ArkUICefClient(int64_t id, CefRefPtr<ArkUICefBrowser> browser, ArkUICefDelegate* delegate) : 
        id_(id), browser_(browser), delegate_(delegate) {}
    
    /** @brief 析构函数 */
    ~ArkUICefClient() = default;
    
    /** @brief 获取 DisplayHandler 实例 */
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }
    /** @brief 获取 LifeSpanHandler 实例 */
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
    /** @brief 获取 LoadHandler 实例 */
    CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
    /** @brief 获取 DownloadHandler 实例 */
    CefRefPtr<CefDownloadHandler> GetDownloadHandler() override { return this; }
    /** @brief 获取 KeyboardHandler 实例 */
    CefRefPtr<CefKeyboardHandler> GetKeyboardHandler() override { return this; }
    /** @brief 获取 PermissionHandler 实例 */
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    /** @brief 获取 FocusHandler 实例 */
    CefRefPtr<CefFocusHandler> GetFocusHandler() override { return this; }
    
    /**
     * @brief 页面标题变更回调
     * @param browser CEF Browser 引用
     * @param title 新的页面标题
     * @thread CEF UI 线程
     */
    void OnTitleChange(CefRefPtr<CefBrowser> browser,
                               const CefString& title) override;
    
    /**
     * @brief 弹窗前回调，拦截弹窗并在当前 frame 加载
     * @param browser CEF Browser 引用
     * @param frame 触发弹窗的 frame
     * @param target_url 弹窗目标 URL
     * @param target_frame_name 目标 frame 名称
     * @param target_disposition 窗口打开方式
     * @param user_gesture 是否用户手势触发
     * @param popupFeatures 弹窗特性
     * @param windowInfo 窗口信息（输出）
     * @param client 客户端（输出）
     * @param settings 浏览器设置（输出）
     * @param extra_info 额外信息（输出）
     * @param no_javascript_access 是否禁止 JavaScript 访问（输出）
     * @return true=拦截弹窗（阻止打开新窗口）；false=允许弹窗
     * @thread CEF UI 线程
     */
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                    int popup_id,
                                    const CefString &target_url, const CefString &target_frame_name,
                                    CefLifeSpanHandler::WindowOpenDisposition target_disposition, bool user_gesture,
                                    const CefPopupFeatures &popupFeatures, CefWindowInfo &windowInfo,
                                    CefRefPtr<CefClient> &client, CefBrowserSettings &settings,
                                    CefRefPtr<CefDictionaryValue> &extra_info, bool *no_javascript_access) override;
    
    /**
     * @brief 浏览器创建完成回调
     * @param browser CEF Browser 引用
     * @thread CEF UI 线程
     */
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    
    /**
     * @brief 处理浏览器关闭请求
     * @param browser CEF Browser 引用
     * @return false=允许关闭；true=阻止关闭
     * @thread CEF UI 线程
     */
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    
    /**
     * @brief 浏览器关闭前回调
     * @param browser CEF Browser 引用
     * @thread CEF UI 线程
     */
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;
    
    /**
     * @brief 页面加载错误回调
     * @param browser CEF Browser 引用
     * @param frame 加载失败的 frame
     * @param errorCode 错误码
     * @param errorText 错误描述
     * @param failedUrl 加载失败的 URL
     * @thread CEF UI 线程
     * @note 错误码为 ERR_ABORTED 时直接返回，否则显示错误页面
     */
    void OnLoadError(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             ErrorCode errorCode,
                             const CefString& errorText,
                             const CefString& failedUrl) override;
    
    /**
     * @brief 页面加载完成回调
     * @param browser CEF Browser 引用
     * @param frame 加载完成的 frame
     * @param httpStatusCode HTTP 状态码
     * @thread CEF UI 线程
     * @note 状态码 200-399 视为加载成功
     */
    void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           int httpStatusCode) override;

    // CefDownloadHandler methods
    /**
     * @brief 下载前检查是否允许下载
     * @param browser CEF Browser 引用
     * @param url 下载目标 URL
     * @param request_method 请求方法（GET、POST 等）
     * @return true=允许下载，false=拒绝下载
     * @thread CEF UI 线程
     */
    bool CanDownload(CefRefPtr<CefBrowser> browser,
                            const CefString& url,
                            const CefString& request_method) override;

    /**
     * @brief 下载开始前回调，设置下载路径
     * @param browser CEF Browser 引用
     * @param download_item 下载项信息
     * @param suggested_name 建议的文件名
     * @param callback 异步回调，用于继续或取消下载
     * @return true=自行处理下载（需调用callback），false=使用默认处理
     * @thread CEF UI 线程
     */
    bool OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefDownloadItem> download_item,
                                 const CefString& suggested_name,
                                 CefRefPtr<CefBeforeDownloadCallback> callback) override;

    /**
     * @brief 下载状态更新回调（进度、完成、取消等）
     * @param browser CEF Browser 引用
     * @param download_item 下载项信息
     * @param callback 用于取消/暂停/恢复下载
     * @thread CEF UI 线程
     */
    void OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefDownloadItem> download_item,
                                  CefRefPtr<CefDownloadItemCallback> callback) override;

    // CefKeyboardHandler methods
    /**
     * @brief 键盘事件预处理回调
     * @param browser CEF Browser 引用
     * @param event 键盘事件信息
     * @param os_event 操作系统事件句柄
     * @param is_keyboard_shortcut 输出参数，标记是否为快捷键
     * @return true=已处理事件，false=继续传递
     * @thread CEF UI 线程
     */
    bool OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                              const CefKeyEvent& event,
                              CefEventHandle os_event,
                              bool* is_keyboard_shortcut) override;

    /**
     * @brief 键盘事件后处理回调（在渲染器和JS处理后）
     * @param browser CEF Browser 引用
     * @param event 键盘事件信息
     * @param os_event 操作系统事件句柄
     * @return true=已处理事件，false=继续传递
     * @thread CEF UI 线程
     */
    bool OnKeyEvent(CefRefPtr<CefBrowser> browser,
                           const CefKeyEvent& event,
                           CefEventHandle os_event) override;

    // CefPermissionHandler methods
    /**
     * @brief 权限请求提示回调
     * @param browser CEF Browser 引用
     * @param prompt_id 权限请求唯一标识
     * @param requesting_origin 请求权限的源
     * @param requested_permissions 请求的权限类型组合（cef_permission_request_types_t）
     * @param callback 异步回调，用于继续或取消请求
     * @return true=自行处理权限请求；false=使用默认处理（Alloy 模式下为 IGNORE）
     * @thread CEF UI 线程
     * @note 目前自动授予剪贴板读取权限，其余权限使用默认处理
     */
    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                       uint64_t prompt_id,
                                       const CefString& requesting_origin,
                                       uint32_t requested_permissions,
                                       CefRefPtr<CefPermissionPromptCallback> callback) override;

    // CefFocusHandler methods
    /**
     * @brief 焦点即将离开浏览器时回调
     * @param browser CEF Browser 引用
     * @param next true=焦点向下一个组件移动，false=向前一个组件移动
     * @thread CEF UI 线程
     */
    void OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next) override;
    
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
     * @brief 设置焦点离开回调
     * @param callback 焦点离开回调，参数为是否向下一个组件移动
     */
    void SetFocusTakeCallback(FocusTakeCallback callback) { focusTakeCallback_ = std::move(callback); }
    
    IMPLEMENT_REFCOUNTING(ArkUICefClient);

private:
    int64_t id_;                                            ///< 关联的浏览器 ID
    int64_t arkui_cef_id_ = -1;
    CefRefPtr<ArkUICefBrowser> browser_;                   ///< ArkUICefBrowser 弱引用
    ArkUICefDelegate* delegate_;                           ///< 生命周期回调委托者
    bool is_closing_;                                      ///< 是否正在关闭
    
    LoadCallback loadCallback_;                            ///< 页面加载回调
    TitleCallback titleCallback_;                          ///< 标题变更回调
    FocusTakeCallback focusTakeCallback_;                  ///< 焦点离开回调
};
} // namespace arkui_cef
#endif // ARKUI_CEF_CLIENT_H