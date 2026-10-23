#include "ArkUICefApp.h"

#include "ArkUICefManager.h"
#include <hilog/log.h>

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;

void CefThreadMain() {
    
}
}
namespace arkui_cef {
void ArkUICefApp::PopulateSettings(CefSettings* settings) {
    if (settings == nullptr) {
        return;
    }
    
    settings->no_sandbox = true;
    settings->multi_threaded_message_loop = false;
    settings->log_severity = LOGSEVERITY_INFO;
    settings->windowless_rendering_enabled = false;
}

void ArkUICefApp::OnBeforeCommandLineProcessing(
    const CefString& process_type,
    CefRefPtr<CefCommandLine> command_line) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefApp", 
        "OnBeforeCommandLineProcessing: process_type=%{public}s", 
        process_type.ToString().c_str());
    
    if (process_type.empty()) {
        command_line->AppendSwitch("enable-media-stream");
        command_line->AppendSwitch("use-mock-keychain");
        command_line->AppendSwitchWithValue("use-gl", "egl");
        command_line->AppendSwitchWithValue("enable-features", "UseOzonePlatform");
        command_line->AppendSwitchWithValue("ozone-platform", "ohos");
        command_line->AppendSwitchWithValue("v", "0");
        command_line->AppendSwitchWithValue("ozone-dump-file", "/data/storage/el2/base/cache/");
        command_line->AppendSwitchWithValue("log-file", "/data/storage/el2/base/cache/test.log");
        command_line->AppendSwitchWithValue("bundle-installation-dir", "/data/storage/el1/bundle/entry/resources/resfile/");
        command_line->AppendSwitchWithValue("user-data-dir", "/data/storage/el2/base/files/");
        command_line->AppendSwitchWithValue("force-renderer-accessibility", "basic");
        command_line->AppendSwitchWithValue("lang", "zh-CN");
        command_line->AppendSwitchWithValue("js-flags", "--jitless");
        command_line->AppendSwitchWithValue("remote-debugging-port", "9222");
        command_line->AppendSwitchWithValue("remote-allow-origins", "*");
        command_line->AppendSwitch("enable-speech-input");
        command_line->AppendSwitch("enable-logging");
        command_line->AppendSwitch("in-process-gpu");
        command_line->AppendSwitch("no-zygote");
        command_line->AppendSwitch("log-net-log");
        command_line->AppendSwitch("enable-print-preview");
    }
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefApp",
        "OnBeforeCommandLineProcessing: %{public}s", command_line->GetCommandLineString().ToString().c_str());
}

void ArkUICefApp::OnContextInitialized() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefApp", 
        "OnContextInitialized");
}
} // namespace arkui_cef