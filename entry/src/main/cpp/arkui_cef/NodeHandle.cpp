//
// Created on 2026/6/3.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#include "NodeHandle.h"

#include <arkui/native_interface.h>
#include <functional>
#include <hilog/log.h>

// Forward declaration — implementation lives in libadapter.so.
// Actual namespace is ohos::adapter::taskRunner (verified via nm on libadapter.so).
namespace ohos::adapter::taskRunner {
class MainThreadTaskRunner {
 public:
  MainThreadTaskRunner(MainThreadTaskRunner& other) = delete;
  void operator=(const MainThreadTaskRunner&) = delete;
  static MainThreadTaskRunner& GetInstance();
  static void PostTask(std::function<void()> task_func);
};
}  // namespace ohos::adapter::taskRunner

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;

ArkUI_NativeNodeAPI_1* NodeApi() {
    static ArkUI_NativeNodeAPI_1* api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
    return api;
}

void setNodeVisibility(ArkUI_NodeHandle handle, bool visible) {
    if (!handle || !NodeApi()) {
        return;
    }
    
    ArkUI_NumberValue visibilityValue[] = {{.i32 = visible ? ARKUI_VISIBILITY_VISIBLE : ARKUI_VISIBILITY_HIDDEN}};
    ArkUI_AttributeItem visibilityItem = {visibilityValue, 1};
    int err = NodeApi()->setAttribute(handle, NODE_VISIBILITY, &visibilityItem);
    if (err != ARKUI_ERROR_CODE_NO_ERROR) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "NodeHandle", 
            "setNodeVisibility: setAttribute NODE_VISIBILITY failed!");
    }
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "NodeHandle", 
        "setNodeVisibility: visible=%{public}d", visible);
}

void resizeNode(ArkUI_NodeHandle handle, int width, int height) {
    if (!handle || !NodeApi()) {
        return;
    }
    
    ArkUI_NumberValue widthValue[] = {{.f32 = static_cast<float>(width)}};
    ArkUI_AttributeItem widthItem = {widthValue, 1};
    int err = NodeApi()->setAttribute(handle, NODE_WIDTH, &widthItem);
    if (err != ARKUI_ERROR_CODE_NO_ERROR) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "NodeHandle", 
            "resizeNode: setAttribute NODE_WIDTH failed!");
    }
    
    ArkUI_NumberValue heightValue[] = {{.f32 = static_cast<float>(height)}};
    ArkUI_AttributeItem heightItem = {heightValue, 1};
    err = NodeApi()->setAttribute(handle, NODE_HEIGHT, &heightItem);
    if (err != ARKUI_ERROR_CODE_NO_ERROR) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "NodeHandle", 
            "resizeNode: setAttribute NODE_HEIGHT failed!");
    }
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "NodeHandle", 
        "resizeNode: width=%{public}d, height=%{public}d", width, height);
}

void releaseNode(ArkUI_NodeHandle handle) {
    if (!handle || !NodeApi()) {
        return;
    }
    
    NodeApi()->disposeNode(handle);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "NodeHandle", 
        "releaseNode: node disposed");
}
} // namespace

namespace arkui_cef {

void show(ArkUI_NodeHandle handle) {
    ohos::adapter::taskRunner::MainThreadTaskRunner::PostTask([handle]() {
        setNodeVisibility(handle, true);
    });
}

void hide(ArkUI_NodeHandle handle) {
    ohos::adapter::taskRunner::MainThreadTaskRunner::PostTask([handle]() {
        setNodeVisibility(handle, false);
    });
}

void resize(ArkUI_NodeHandle handle, int width, int height) {
    ohos::adapter::taskRunner::MainThreadTaskRunner::PostTask([handle, width, height]() {
        resizeNode(handle, width, height);
    });
}

void release(ArkUI_NodeHandle handle) {
    ohos::adapter::taskRunner::MainThreadTaskRunner::PostTask([handle]() {
        releaseNode(handle);
    });
}

} // namespace arkui_cef
