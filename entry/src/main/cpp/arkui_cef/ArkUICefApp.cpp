#include "ArkUICefApp.h"

#include <string>

#include "ArkUICefManager.h"
#include <hilog/log.h>

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;
}
namespace arkui_cef {
void ArkUICefApp::PopulateSettings(CefSettings* settings) {
    if (settings == nullptr) {
        return;
    }
    
    settings->no_sandbox = true;
    settings->multi_threaded_message_loop = false;
    settings->log_severity = LOGSEVERITY_INFO;
    settings->windowless_rendering_enabled = true;

    // Step 1 (out-of-process GPU): point libcef at the subprocess entry
    // library so Chromium launches GPU/renderer children with argv[0] set
    // to this absolute path. The OHOS adapter then dlopens exactly this
    // file (web_entry.cc RunOtherProcessType) instead of falling back to a
    // bare soname lookup.
    //
    // The demo bundles all CEF/adapter libs under the HAP install dir
    // (entry/libs/arm64-v8a -> /data/storage/el1/bundle/libs/arm64/),
    // so a constant path is sufficient here.
    const std::string subprocess =
        "/data/storage/el1/bundle/libs/arm64/libcefsimple.so";
    CefString(&settings->browser_subprocess_path).FromString(subprocess);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefApp",
        "browser_subprocess_path=%{public}s", subprocess.c_str());
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
        command_line->AppendSwitchWithValue("remote-debugging-port", "9333");
        command_line->AppendSwitchWithValue("remote-allow-origins", "*");
        command_line->AppendSwitch("enable-speech-input");
        command_line->AppendSwitch("enable-logging");
        // Step 1 (out-of-process GPU): --in-process-gpu removed so Chromium
        // launches a real GPU child process via OH_Ability_CreateNativeChildProcess.
        // NOTE: this breaks GPU OSR (the native-buffer registry is a
        // process-local singleton; its lease id is meaningless once the GPU
        // runs in another process). Windowed browsers are unaffected; OSR is
        // restored by Step 2 (cross-process Parcel buffer handoff).
        command_line->AppendSwitch("no-zygote");
        command_line->AppendSwitch("log-net-log");
        command_line->AppendSwitch("enable-print-preview");

        // OSR GPU delivery, declared here so the app owns the policy instead
        // of the engine. Note that libcef does not read these two switches --
        // they are cefclient-only (cef/tests/shared/common/client_switches.cc).
        // What actually turns the path on:
        //   - windowless rendering: CefSettings.windowless_rendering_enabled
        //     in PopulateSettings() above
        //   - shared-texture frames: CefWindowInfo.shared_texture_enabled, set
        //     per browser in ArkUICefBrowser.cpp
        // Kept as an explicit declaration of intent for this app.
        command_line->AppendSwitch("off-screen-rendering-enabled");
        command_line->AppendSwitch("shared-texture-enabled");
    }
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefApp",
        "OnBeforeCommandLineProcessing: %{public}s", command_line->GetCommandLineString().ToString().c_str());
}

void ArkUICefApp::OnContextInitialized() {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefApp", 
        "OnContextInitialized");
}
} // namespace arkui_cef