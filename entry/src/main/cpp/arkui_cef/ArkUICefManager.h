//
// Created on 2026/6/3.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#ifndef OPENQTCEF_ARKUICEFMANAGER_H
#define OPENQTCEF_ARKUICEFMANAGER_H

#include <atomic>
#include <condition_variable>
#include <unordered_map>
#include <mutex>
#include <thread>

#include "ArkUICefBrowser.h"
#include "ArkUICefClient.h"
#include "ArkUICefRenderHandler.h"
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"

namespace arkui_cef {

using EMBEDDED_WINDOW_ADAPTER::Node;

/**
 * @class ArkUICefManager
 * @brief CEF 框架全局管理器，单例模式。负责 CEF 生命周期管理和所有浏览器实例的注册与查找。
 * @thread 自身在主线程创建，CEF 相关操作在独立的 cefThread_ 中执行（CEF UI 线程）。
 */
class ArkUICefManager : public ArkUICefDelegate {
public:
    /**
     * @brief 获取单例实例
     * @return ArkUICefManager& 单例引用
     */
    static ArkUICefManager& getInstance();
    
    /**
     * @brief 启动 CEF 框架，创建独立的 CEF 工作线程
     * @note 调用后 CEF 将在后台线程初始化，完成前 isCefRunning() 返回 false
     */
    void start();
    
    /**
     * @brief 停止 CEF 框架，关闭所有浏览器并等待线程结束
     * @note 同步阻塞直到 CEF 线程完全退出
     */
    void stop();
    
    /**
     * @brief 查询 CEF 框架是否已就绪
     * @return true=CEF 已初始化完成，可以创建浏览器；false=未就绪或已停止
     */
    bool isCefRunning();
    
    /**
     * @brief 创建浏览器实例
     * @param parentWindowHandle 父窗口节点指针（ArkUI NodeHandle）, 
     *        CEF 实例不应该持有这个指针，该指针的生命周期由外部框架管理。
     * @param url 初始加载 URL
     * @param width 初始宽度（像素）
     * @param height 初始高度（像素）
     * @return 浏览器 ID（>0 成功，-1 失败）
     * @note 若 CEF 未就绪返回 -1。浏览器创建是异步的，返回 ID 时浏览器可能尚未创建完成。
     */
    int64_t createBrowser(Node* parentWindowHandle, const std::string& url,
                          int width, int height, std::function<void(int64_t)> onCreated,
                          std::function<void(const std::string&)> onTitleChange,
                          std::function<void(bool)> onFocusTake);

    /**
     * @brief 创建离屏渲染 (OSR) 浏览器实例
     * @param url 初始加载 URL
     * @param width 初始宽度（像素）
     * @param height 初始高度（像素）
     * @param onPaint 像素数据回调 (buffer 为 BGRA 格式)
     * @param onCreated 浏览器创建完成回调
     * @param onTitleChange 标题变更回调
     * @param onFocusTake 焦点离开回调
     * @return 浏览器 ID（>0 成功，-1 失败）
     */
    int64_t createOsrBrowser(const std::string& url, int width, int height,
                             PaintCallback onPaint,
                             std::function<void(int64_t)> onCreated,
                             std::function<void(const std::string&)> onTitleChange,
                             std::function<void(bool)> onFocusTake,
                             ScrollOffsetCallback onScrollOffset = nullptr,
                             VirtualKeyboardCallback onVirtualKeyboard = nullptr,
                             TextSelectionCallback onTextSelection = nullptr,
                             CursorChangeCallback onCursorChange = nullptr);
    
    /**
     * @brief 销毁指定浏览器
     * @param id 浏览器 ID
     * @note 销毁是异步的，实际关闭在 CEF UI 线程完成
     */
    void destroyBrowser(int64_t id);
    
    /**
     * @brief 获取浏览器引用
     * @param id 浏览器 ID
     * @return 浏览器引用，若不存在返回 nullptr
     */
    CefRefPtr<ArkUICefBrowser> getBrowser(int64_t id);
    
    /**
     * @brief 检查浏览器是否存在
     * @param id 浏览器 ID
     * @return true=存在，false=不存在
     */
    bool hasBrowser(int64_t id);

    /**
     * @brief 为指定弹窗浏览器注册标题变更回调
     * @param cefBrowserId CEF 浏览器 ID
     * @param callback 标题变更回调
     */
    void RegisterPopupTitleCallback(int cefBrowserId, std::function<void(const std::string&)> callback);

    /**
     * @brief 移除指定弹窗浏览器的标题变更回调
     * @param cefBrowserId CEF 浏览器 ID
     */
    void UnregisterPopupTitleCallback(int cefBrowserId);
    
    /**
     * @brief 通知弹窗标题变更
     * @param cefBrowserId CEF 浏览器 ID
     * @param title 新标题
     */
    void NotifyPopupTitleChange(int cefBrowserId, const std::string& title);

    /**
     * @brief 设置弹窗浏览器创建回调
     * @param callback 回调，参数为 CEF 浏览器 ID
     * @note 弹窗浏览器的 OnAfterCreated 触发时调用，用于注册该浏览器的标题回调
     */
    void SetPopupBrowserCreatedCallback(std::function<void(int)> callback);

    /**
     * @brief 通知弹窗浏览器创建
     * @param cefBrowserId CEF 浏览器 ID
     */
    void NotifyPopupBrowserCreated(int cefBrowserId);
    
    // ArkUICefDelegate 接口实现
    /**
     * @brief 浏览器创建完成回调
     * @param browser 新创建的浏览器
     */
    void onBrowserCreated(CefRefPtr<ArkUICefBrowser> browser) override;
    
    /**
     * @brief 浏览器销毁完成回调
     * @param browser 被销毁的浏览器
     */
    void onBrowserDestroyed(CefRefPtr<ArkUICefBrowser> browser) override;
    
private:
    /** @brief 构造函数（私有，单例模式） */
    ArkUICefManager();
    
    /** @brief 析构函数 */
    ~ArkUICefManager();
    
    /**
     * @brief CEF 主线程入口函数
     * @note 在独立线程中执行，负责 CEF 初始化、消息循环和关闭
     */
    void CefThreadMain();
    
    std::thread cefThread_;                                      ///< CEF 主工作线程
    
    std::mutex mutex_;                                           ///< 保护 browsers_ 和 initOk_ 的锁
    std::unordered_map<int64_t, CefRefPtr<ArkUICefBrowser>> browsers_; ///< 浏览器注册表
    
    bool initOk_ = false;                                        ///< CEF 是否已初始化完成

    std::unordered_map<int, std::function<void(const std::string&)>> popupTitleCallbacks_; ///< cefBrowserId → 标题回调
    std::function<void(int)> popupBrowserCreatedCallback_;                                ///< 弹窗浏览器创建回调
};
} // namespace arkui_cef
#endif //OPENQTCEF_ARKUICEFMANAGER_H
