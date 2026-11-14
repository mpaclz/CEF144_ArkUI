// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <hilog/log.h>

#include "arkui_cef/ArkUICefApp.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"

// Entry point function for secondary processes (renderer, GPU, utility, etc.).
// On OHOS the subprocess is a shared library (libcefsimple.so): the CEF
// main process forks, dlopens it and calls CefMain().
extern "C" {
int __attribute__((visibility("default"))) CefMain(int argc, char* argv[]) {
    CefMainArgs main_args(argc, argv);

    // Parse the command line to determine the process type.
    CefRefPtr<CefCommandLine> command_line = CefCommandLine::CreateCommandLine();
    command_line->InitFromArgv(argc, argv);

    std::string process_type = command_line->GetSwitchValue("type");

    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "CefMain",
        "CefMain: subprocess entry, process_type=%{public}s", process_type.c_str());

    int result = CefExecuteProcess(main_args, nullptr, nullptr);
    OH_LOG_Print(LOG_APP, LOG_INFO, 0xFF00, "CefMain",
        "CefMain: CefExecuteProcess returned %{public}d", result);
    
    return result;
}
}
