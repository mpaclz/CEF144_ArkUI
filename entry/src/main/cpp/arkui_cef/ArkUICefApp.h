#ifndef ARKUI_CEF_APP_H
#define ARKUI_CEF_APP_H

#include "include/cef_app.h"

namespace arkui_cef {
/**
 * @class ArkUICefApp
 * @brief CEF Application 类，实现 CefApp 和 CefBrowserProcessHandler 接口。
 * @brief 负责配置 CEF 全局设置和命令行参数，处理应用级事件。
 * @thread 在 CEF 主线程执行。
 */
class ArkUICefApp : public CefApp, public CefBrowserProcessHandler {
public:
    /** @brief 构造函数 */
    ArkUICefApp() = default;
    
    /** @brief 获取浏览器进程处理器 */
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
        return this;
    }
    
    /**
     * @brief 填充 CEF 全局设置
     * @param settings CefSettings 对象指针
     * @note 设置包括：no_sandbox、multi_threaded_message_loop、log_severity、windowless_rendering_enabled
     */
    static void PopulateSettings(CefSettings* settings);
    
    /**
     * @brief 命令行处理前回调，可修改命令行参数
     * @param process_type 进程类型（空字符串表示主进程）
     * @param command_line 命令行对象
     * @thread CEF 主线程
     */
    void OnBeforeCommandLineProcessing(
        const CefString& process_type,
        CefRefPtr<CefCommandLine> command_line) override;
    
    /**
     * @brief CEF 上下文初始化完成回调
     * @thread CEF 主线程
     * @note 此时可以安全地创建浏览器实例
     */
    void OnContextInitialized() override;
private:
    IMPLEMENT_REFCOUNTING(ArkUICefApp);
    DISALLOW_COPY_AND_ASSIGN(ArkUICefApp);
};
} // namespace arkui_cef
#endif // ARKUI_CEF_APP_H