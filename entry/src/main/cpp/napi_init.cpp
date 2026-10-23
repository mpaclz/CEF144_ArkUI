#include <cstdint>
#include <hilog/log.h>
#include <string>
#include <arkui/native_interface.h>
#include <arkui/native_node_napi.h>

#include "JsThreadSafeCallback.h"
#include "napi/native_api.h"
#include "arkui_cef/ArkUICefBrowser.h"
#include "arkui_cef/ArkUICefApp.h"
#include "arkui_cef/ArkUICefManager.h"
#include "include/base/cef_callback.h"

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;
using arkui_cef::ArkUICefBrowser;
using arkui_cef::ArkUICefApp;
using EMBEDDED_WINDOW_ADAPTER::Node;

// HarmonyOS KeyType values (from @kit.ArkUI)
constexpr int32_t OH_KEYTYPE_DOWN = 0;
constexpr int32_t OH_KEYTYPE_UP   = 1;

// HarmonyOS KeyCode values (from @kit.InputKit)
constexpr int32_t OH_KEYCODE_TAB        = 2076;
constexpr int32_t OH_KEYCODE_DPAD_LEFT  = 2014;
constexpr int32_t OH_KEYCODE_DPAD_UP    = 2016;
constexpr int32_t OH_KEYCODE_DPAD_RIGHT = 2015;
constexpr int32_t OH_KEYCODE_DPAD_DOWN  = 2017;

// Windows virtual key codes
constexpr int32_t VK_TAB   = 0x09;
constexpr int32_t VK_LEFT  = 0x25;
constexpr int32_t VK_UP    = 0x26;
constexpr int32_t VK_RIGHT = 0x27;
constexpr int32_t VK_DOWN  = 0x28;

CefKeyEvent convertKeyEvent(int32_t keyCode, int32_t keyType, uint32_t modifiers) {
    // HarmonyOS KeyType -> CEF key event type
    cef_key_event_type_t cefType = KEYEVENT_RAWKEYDOWN;
    if (keyType == OH_KEYTYPE_UP) {
        cefType = KEYEVENT_KEYUP;
    }

    // HarmonyOS KeyCode -> Windows virtual key code
    int32_t winVk = 0;
    switch (keyCode) {
        case OH_KEYCODE_TAB:        winVk = VK_TAB;   break;
        case OH_KEYCODE_DPAD_LEFT:  winVk = VK_LEFT;  break;
        case OH_KEYCODE_DPAD_UP:    winVk = VK_UP;    break;
        case OH_KEYCODE_DPAD_RIGHT: winVk = VK_RIGHT; break;
        case OH_KEYCODE_DPAD_DOWN:  winVk = VK_DOWN;  break;
        default: break;
    }

    CefKeyEvent event = {};
    event.size = sizeof(CefKeyEvent);
    event.type = cefType;
    event.modifiers = modifiers;
    event.windows_key_code = winVk;
    event.native_key_code = keyCode;
    event.is_system_key = 0;
    event.character = 0;
    event.unmodified_character = 0;
    event.focus_on_editable_field = 1;
    return event;
}

struct CefCreatedCallbackContext {
    napi_threadsafe_function tsfn = nullptr;
};

struct CefCreatedCallbackData {
    int64_t browserId = -1;
};

Node* createParentNode() {
    ArkUI_NativeNodeAPI_1* api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
    if (!api) {
        return nullptr;
    }
    
    ArkUI_NodeHandle stack = api->createNode(ARKUI_NODE_STACK);
    if (!stack) {
        return nullptr;
    }
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "parentStack: %{public}p", stack);
    Node* node = new Node{};
    node->container = stack;
    node->nodeType = ARKUI_NODE_XCOMPONENT;
    
    return node;
}

void destroyNode(Node* node) {
    if (!node) {
        return;
    }
    
    ArkUI_NativeNodeAPI_1* api = reinterpret_cast<ArkUI_NativeNodeAPI_1*>(
        OH_ArkUI_QueryModuleInterfaceByName(ARKUI_NATIVE_NODE, "ArkUI_NativeNodeAPI_1"));
    if (api) {
        if (node->container) {
        api->disposeNode(node->container);
        }
        if (node->node) {
            api->disposeNode(node->node);
        }
    }
    delete node;
}
} // namespace

static napi_value NAPI_Global_startCef(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "startCef called");
    
    arkui_cef::ArkUICefManager::getInstance().start();
    return nullptr;
}

static napi_value NAPI_Global_isCefRunning(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "isCefRunning called");
    
    bool ok = arkui_cef::ArkUICefManager::getInstance().isCefRunning();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "isCefRunning: %{public}d", ok);
    napi_value result;
    napi_get_boolean(env, ok, &result);
    return result;
}

static napi_value NAPI_Global_createCefView(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "createCefView called");
    
    size_t argc = 7;
    napi_value args[7];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 7) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "createCefView: expected 7 arguments, got %{public}zu", argc);
        napi_throw_error(env, nullptr, "Expected 7 arguments: nodeContent, url, width, height, onCreated, onTitleChange, onFocusTake");
        return nullptr;
    }
    
    ArkUI_NodeContentHandle contentHandle = nullptr;
    OH_ArkUI_GetNodeContentFromNapiValue(env, args[0], &contentHandle);
    if (contentHandle == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: getNodeContentHandleFromNapiValue failed");
        napi_throw_error(env, nullptr, "Failed to get NodeContent handle");
        return nullptr;
    }
    
    char url[1024] = {};
    size_t urlLen;
    napi_get_value_string_utf8(env, args[1], url, sizeof(url), &urlLen);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", 
        "createCefView: url=%{public}s", url);
    
    int32_t width = 0;
    napi_get_value_int32(env, args[2], &width);
    
    int32_t height = 0;
    napi_get_value_int32(env, args[3], &height);
    
    auto jsCreatedCallback = makeJsThreadSafeCallback<int64_t>(env, args[4], "CefCreatedCallback");
    auto jsTitleChangeCallback = makeJsThreadSafeCallback<std::string>(env, args[5], "CefTitleChangeCallback");
    auto jsFocusTakeCallback = makeJsThreadSafeCallback<bool>(env, args[6], "CefFocusTakeCallback");
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", 
        "createCefView: width=%{public}d, height=%{public}d", width, height);
    
    Node* parentNode = createParentNode();
    if (parentNode == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: createStackNode failed");
        napi_throw_error(env, nullptr, "Failed to create Stack node");
        return nullptr;
    }
    
    int32_t err = OH_ArkUI_NodeContent_AddNode(contentHandle, parentNode->container);
    if (err != ARKUI_ERROR_CODE_NO_ERROR) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: addStackToNodeContent failed");
        delete parentNode;
        napi_throw_error(env, nullptr, "Failed to add Stack to NodeContent");
        return nullptr;
    }
    
    err = OH_ArkUI_NodeContent_SetUserData(contentHandle, parentNode);
    if (err != ARKUI_ERROR_CODE_NO_ERROR) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: setUserData failed");
        delete parentNode;
        napi_throw_error(env, nullptr, "Failed to add user data to NodeContent");
        return nullptr;
    }
    
    int64_t browserId = arkui_cef::ArkUICefManager::getInstance().createBrowser(
       parentNode, url, width, height,
       [jsCreatedCallback](int64_t id) {
        jsCreatedCallback->Call(id);
        jsCreatedCallback->Release();
       },
       [jsTitleChangeCallback](const std::string& title) {
        jsTitleChangeCallback->Call(title);
       },
       [jsFocusTakeCallback](bool next) {
        jsFocusTakeCallback->Call(next);
       });
    
    napi_value result;
    napi_create_int64(env, browserId, &result);
    return result;
}

static napi_value NAPI_Global_destroyCefView(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "destroyCefView called");
    
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "destroyCefView id: %{public}ld",
                 browserId);
    
    ArkUI_NodeContentHandle contentHandle = nullptr;
    OH_ArkUI_GetNodeContentFromNapiValue(env, args[1], &contentHandle);
    if (contentHandle == nullptr) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: getNodeContentHandleFromNapiValue failed");
        napi_throw_error(env, nullptr, "Failed to get NodeContent handle");
        return nullptr;
    }
    
    arkui_cef::ArkUICefManager::getInstance().destroyBrowser(browserId);
    
    Node* parentNode = static_cast<Node*>(OH_ArkUI_NodeContent_GetUserData(contentHandle));
    if (parentNode) {
        int32_t err = OH_ArkUI_NodeContent_RemoveNode(contentHandle, parentNode->container);
        if (err != ARKUI_ERROR_CODE_NO_ERROR) {
            OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", 
            "createCefView: removeNode failed");
        }
        destroyNode(parentNode);
    }
    
    return nullptr;
}

static napi_value NAPI_Global_cefResize(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefRisize called");
    
    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 3) {
        napi_throw_error(env, nullptr, "Expected 3 arguments: browserId, width, height");
        return nullptr;
    }
    
    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    
    int32_t width = 0;
    napi_get_value_int32(env, args[1], &width);
    
    int32_t height = 0;
    napi_get_value_int32(env, args[2], &height);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefRisize %{public}ld, %{public}d X %{public}d",
                 browserId, width, height);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::resize, browser, width, height)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefGoBack(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefGoBack %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::goBack, browser)));
    }
    
    return nullptr;
}

static napi_value NAPI_Global_cefGoForward(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefGoForward %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::goForward, browser)));
    }
    
    return nullptr;
}

static napi_value NAPI_Global_cefReload(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefReload %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::reload, browser)));
    }
    
    return nullptr;
}

static napi_value NAPI_Global_cefHide(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefHide %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::hide, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefShow(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefShow %{public}ld", browserId);
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::show, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefLoadUrl(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, url");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    char url[1024] = {};
    size_t urlLen;
    napi_get_value_string_utf8(env, args[1], url, sizeof(url), &urlLen);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefLoadUrl %{public}ld, url=%{public}s", 
                 browserId, url);
    
    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::loadUrl, browser, std::string(url))));
    }

    return nullptr;
}

static napi_value NAPI_Global_cefSendKeyEvent(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 4) {
        napi_throw_error(env, nullptr,
            "Expected 4 arguments: browserId, keyCode, keyType, modifiers");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t keyCode = 0;
    napi_get_value_int32(env, args[1], &keyCode);

    int32_t keyType = 0;
    napi_get_value_int32(env, args[2], &keyType);

    uint32_t modifiers = 0;
    napi_get_value_uint32(env, args[3], &modifiers);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefSendKeyEvent: browserId=%{public}ld, keyCode=%{public}d, keyType=%{public}d",
        browserId, keyCode, keyType);

    CefKeyEvent keyEvent = convertKeyEvent(keyCode, keyType, modifiers);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendKeyEvent, browser, keyEvent)));
    }

    return nullptr;
}

static napi_value NAPI_Global_hasCef(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    
    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }
    
    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "hasCef %{public}ld", browserId);
    bool exists = arkui_cef::ArkUICefManager::getInstance().hasBrowser(browserId);
    
    napi_value result;
    napi_get_boolean(env, exists, &result);
    return result;
}

static napi_value NAPI_Global_registerPopupTitleCallback(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: cefBrowserId, callback");
        return nullptr;
    }

    int32_t cefBrowserId = 0;
    napi_get_value_int32(env, args[0], &cefBrowserId);

    auto jsCallback = makeJsThreadSafeCallback<std::string>(env, args[1], "PopupTitleCallback");

    if (jsCallback) {
        arkui_cef::ArkUICefManager::getInstance().RegisterPopupTitleCallback(
            cefBrowserId,
            [jsCallback](const std::string& title) {
                jsCallback->Call(title);
            });
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
            "registerPopupTitleCallback: registered for cefBrowserId=%{public}d", cefBrowserId);
    } else {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "registerPopupTitleCallback: failed to create callback");
    }

    return nullptr;
}

static napi_value NAPI_Global_unregisterPopupTitleCallback(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: cefBrowserId");
        return nullptr;
    }

    int32_t cefBrowserId = 0;
    napi_get_value_int32(env, args[0], &cefBrowserId);

    arkui_cef::ArkUICefManager::getInstance().UnregisterPopupTitleCallback(cefBrowserId);
    return nullptr;
}

static napi_value NAPI_Global_setPopupBrowserCreatedCallback(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "setPopupBrowserCreatedCallback called");

    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: callback");
        return nullptr;
    }

    auto jsPopupBrowserCreatedCallback = makeJsThreadSafeCallback<int>(env, args[0], "PopupBrowserCreatedCallback");

    if (jsPopupBrowserCreatedCallback) {
        arkui_cef::ArkUICefManager::getInstance().SetPopupBrowserCreatedCallback(
            [jsPopupBrowserCreatedCallback](int cefBrowserId) {
                jsPopupBrowserCreatedCallback->Call(cefBrowserId);
            });
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "setPopupBrowserCreatedCallback: registered");
    } else {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi", "setPopupBrowserCreatedCallback: failed to create callback");
    }

    return nullptr;
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "Init called");
    
    napi_property_descriptor desc[] = {
        {"startCef", nullptr, NAPI_Global_startCef, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"isCefRunning", nullptr, NAPI_Global_isCefRunning, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createCefView", nullptr, NAPI_Global_createCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyCefView", nullptr, NAPI_Global_destroyCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefResize", nullptr, NAPI_Global_cefResize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefGoBack", nullptr, NAPI_Global_cefGoBack, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefGoForward", nullptr, NAPI_Global_cefGoForward, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefReload", nullptr, NAPI_Global_cefReload, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefHide", nullptr, NAPI_Global_cefHide, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefShow", nullptr, NAPI_Global_cefShow, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefLoadUrl", nullptr, NAPI_Global_cefLoadUrl, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"hasCef", nullptr, NAPI_Global_hasCef, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"registerPopupTitleCallback", nullptr, NAPI_Global_registerPopupTitleCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"unregisterPopupTitleCallback", nullptr, NAPI_Global_unregisterPopupTitleCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"setPopupBrowserCreatedCallback", nullptr, NAPI_Global_setPopupBrowserCreatedCallback, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendKeyEvent", nullptr, NAPI_Global_cefSendKeyEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "Init done");
    return exports;
}
EXTERN_C_END

static napi_module demoModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "arkui_cef",
    .nm_priv = ((void*)0),
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterArkui_cefModule(void)
{
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", 
        "RegisterArkui_cefModule called");
    napi_module_register(&demoModule);
}
