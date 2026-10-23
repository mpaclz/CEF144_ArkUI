// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <hilog/log.h>

#include "arkui_cef/ArkUICefApp.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"

// Entry point function for secondary processes (renderer, GPU, utility, etc.).
// OHOS 上子进程是动态库 (libcefsimple.so)，由 CEF 主进程 fork 后 dlopen 加载并调用 CefMain()。
extern "C" {
int __attribute__((visibility("default"))) CefMain(int argc, char* argv[]) {
    CefMainArgs main_args(argc, argv);

    // 解析命令行判断进程类型
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
