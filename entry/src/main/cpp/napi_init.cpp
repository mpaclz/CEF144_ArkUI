#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <atomic>
#include <hilog/log.h>
#include <string>
#include <arkui/native_interface.h>
#include <arkui/native_node_napi.h>

#include "JsThreadSafeCallback.h"
#include "napi/native_api.h"
#include "arkui_cef/ArkUICefBrowser.h"
#include "arkui_cef/ArkUICefApp.h"
#include "arkui_cef/ArkUICefManager.h"
#include "arkui_cef/ArkUICefRenderHandler.h"
#include "include/base/cef_callback.h"

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;
using arkui_cef::ArkUICefBrowser;
using arkui_cef::ArkUICefApp;
using arkui_cef::PaintCallback;
using EMBEDDED_WINDOW_ADAPTER::Node;

// HarmonyOS KeyType values (from @kit.ArkUI)
constexpr int32_t OH_KEYTYPE_DOWN = 0;
constexpr int32_t OH_KEYTYPE_UP   = 1;

// HarmonyOS KeyCode values (from @kit.InputKit)
// Letters A-Z: 2017+0..25 -> KeyCode 2017..2042
// Digits 0-9: 2000..2009
// Common keys
constexpr int32_t OH_KEYCODE_ENTER      = 2054;
constexpr int32_t OH_KEYCODE_BACKSPACE  = 2055;
constexpr int32_t OH_KEYCODE_DELETE     = 2056;
constexpr int32_t OH_KEYCODE_SPACE      = 2062;
constexpr int32_t OH_KEYCODE_TAB        = 2076;
constexpr int32_t OH_KEYCODE_ESCAPE     = 2077;
constexpr int32_t OH_KEYCODE_CTRL_LEFT  = 2072;
constexpr int32_t OH_KEYCODE_SHIFT_LEFT = 2071;
constexpr int32_t OH_KEYCODE_ALT_LEFT   = 2074;
constexpr int32_t OH_KEYCODE_CAPS_LOCK  = 2078;
constexpr int32_t OH_KEYCODE_PAGE_UP    = 2067;
constexpr int32_t OH_KEYCODE_PAGE_DOWN  = 2068;
constexpr int32_t OH_KEYCODE_HOME       = 2069;
constexpr int32_t OH_KEYCODE_END        = 2070;
constexpr int32_t OH_KEYCODE_DPAD_LEFT  = 2014;
constexpr int32_t OH_KEYCODE_DPAD_UP    = 2016;
constexpr int32_t OH_KEYCODE_DPAD_RIGHT = 2015;
constexpr int32_t OH_KEYCODE_DPAD_DOWN  = 2017;
constexpr int32_t OH_KEYCODE_F1  = 2084;
constexpr int32_t OH_KEYCODE_F2  = 2085;
constexpr int32_t OH_KEYCODE_F3  = 2086;
constexpr int32_t OH_KEYCODE_F4  = 2087;
constexpr int32_t OH_KEYCODE_F5  = 2088;
constexpr int32_t OH_KEYCODE_F6  = 2089;
constexpr int32_t OH_KEYCODE_F7  = 2090;
constexpr int32_t OH_KEYCODE_F8  = 2091;
constexpr int32_t OH_KEYCODE_F9  = 2092;
constexpr int32_t OH_KEYCODE_F10 = 2093;
constexpr int32_t OH_KEYCODE_F11 = 2094;
constexpr int32_t OH_KEYCODE_F12 = 2095;

// Windows virtual key codes
constexpr int32_t VK_BACK   = 0x08;
constexpr int32_t VK_TAB    = 0x09;
constexpr int32_t VK_RETURN = 0x0D;
constexpr int32_t VK_SHIFT  = 0x10;
constexpr int32_t VK_CONTROL= 0x11;
constexpr int32_t VK_MENU   = 0x12;  // Alt
constexpr int32_t VK_ESCAPE = 0x1B;
constexpr int32_t VK_SPACE  = 0x20;
constexpr int32_t VK_DELETE = 0x2E;
constexpr int32_t VK_LEFT   = 0x25;
constexpr int32_t VK_UP     = 0x26;
constexpr int32_t VK_RIGHT  = 0x27;
constexpr int32_t VK_DOWN   = 0x28;
constexpr int32_t VK_PRIOR  = 0x21;  // Page Up
constexpr int32_t VK_NEXT   = 0x22;  // Page Down
constexpr int32_t VK_HOME   = 0x23;
constexpr int32_t VK_END    = 0x24;
constexpr int32_t VK_CAPITAL= 0x14;  // Caps Lock
constexpr int32_t VK_F1  = 0x70;
constexpr int32_t VK_F2  = 0x71;
constexpr int32_t VK_F3  = 0x72;
constexpr int32_t VK_F4  = 0x73;
constexpr int32_t VK_F5  = 0x74;
constexpr int32_t VK_F6  = 0x75;
constexpr int32_t VK_F7  = 0x76;
constexpr int32_t VK_F8  = 0x77;
constexpr int32_t VK_F9  = 0x78;
constexpr int32_t VK_F10 = 0x79;
constexpr int32_t VK_F11 = 0x7A;
constexpr int32_t VK_F12 = 0x7B;

CefKeyEvent convertKeyEvent(int32_t keyCode, int32_t keyType, uint32_t modifiers) {
    // HarmonyOS KeyType -> CEF key event type
    // OH_KEYTYPE_DOWN=0, OH_KEYTYPE_UP=1
    // CEF: KEYEVENT_RAWKEYDOWN=0, KEYEVENT_KEYUP=1, KEYEVENT_CHAR=2
    cef_key_event_type_t cefType = KEYEVENT_RAWKEYDOWN;
    if (keyType == OH_KEYTYPE_UP) {
        cefType = KEYEVENT_KEYUP;
    } else if (keyType == 2) {
        // KEYEVENT_CHAR — ArkTS sends type=2 for character events
        cefType = KEYEVENT_CHAR;
    }

    // HarmonyOS KeyCode -> Windows virtual key code
    int32_t winVk = 0;
    // Character for KEYEVENT_CHAR (0 = no char event needed)
    char16_t character = 0;

    // Letters A-Z: HarmonyOS KeyCode 2018..2043 (A=2018, B=2019, ..., Z=2043)
    if (keyCode >= 2018 && keyCode <= 2043) {
        winVk = 'A' + (keyCode - 2018);
        // For CHAR events, use lowercase if no Shift modifier, uppercase if Shift
        if (modifiers & 0x2) { // EVENTFLAG_SHIFT_DOWN = 0x2
            character = static_cast<char16_t>('A' + (keyCode - 2018));
        } else {
            character = static_cast<char16_t>('a' + (keyCode - 2018));
        }
    } else if (keyCode >= 2000 && keyCode <= 2009) {
        // Digits 0-9: 0=2000, 1=2001, ... 9=2009
        winVk = '0' + (keyCode - 2000);
        character = static_cast<char16_t>('0' + (keyCode - 2000));
    } else {
        switch (keyCode) {
            case OH_KEYCODE_ENTER:      winVk = VK_RETURN; character = u'\r'; break;
            case OH_KEYCODE_BACKSPACE:  winVk = VK_BACK;   character = u'\b'; break;
            case OH_KEYCODE_DELETE:     winVk = VK_DELETE; break;
            case OH_KEYCODE_SPACE:      winVk = VK_SPACE;  character = u' '; break;
            case OH_KEYCODE_TAB:        winVk = VK_TAB;    character = u'\t'; break;
            case OH_KEYCODE_ESCAPE:     winVk = VK_ESCAPE; break;
            case OH_KEYCODE_CTRL_LEFT:  winVk = VK_CONTROL;break;
            case OH_KEYCODE_SHIFT_LEFT: winVk = VK_SHIFT;  break;
            case OH_KEYCODE_ALT_LEFT:   winVk = VK_MENU;   break;
            case OH_KEYCODE_CAPS_LOCK:  winVk = VK_CAPITAL;break;
            case OH_KEYCODE_PAGE_UP:    winVk = VK_PRIOR;  break;
            case OH_KEYCODE_PAGE_DOWN:  winVk = VK_NEXT;   break;
            case OH_KEYCODE_HOME:       winVk = VK_HOME;   break;
            case OH_KEYCODE_END:        winVk = VK_END;    break;
            case OH_KEYCODE_DPAD_LEFT:  winVk = VK_LEFT;   break;
            case OH_KEYCODE_DPAD_UP:    winVk = VK_UP;     break;
            case OH_KEYCODE_DPAD_RIGHT: winVk = VK_RIGHT;  break;
            case OH_KEYCODE_DPAD_DOWN:  winVk = VK_DOWN;   break;
            case OH_KEYCODE_F1:  winVk = VK_F1;  break;
            case OH_KEYCODE_F2:  winVk = VK_F2;  break;
            case OH_KEYCODE_F3:  winVk = VK_F3;  break;
            case OH_KEYCODE_F4:  winVk = VK_F4;  break;
            case OH_KEYCODE_F5:  winVk = VK_F5;  break;
            case OH_KEYCODE_F6:  winVk = VK_F6;  break;
            case OH_KEYCODE_F7:  winVk = VK_F7;  break;
            case OH_KEYCODE_F8:  winVk = VK_F8;  break;
            case OH_KEYCODE_F9:  winVk = VK_F9;  break;
            case OH_KEYCODE_F10: winVk = VK_F10; break;
            case OH_KEYCODE_F11: winVk = VK_F11; break;
            case OH_KEYCODE_F12: winVk = VK_F12; break;
            default: break;
        }
    }

    CefKeyEvent event = {};
    event.size = sizeof(CefKeyEvent);
    event.type = cefType;
    event.modifiers = modifiers;
    event.windows_key_code = winVk;
    event.native_key_code = keyCode;
    event.is_system_key = 0;
    event.character = character;
    event.unmodified_character = character;
    event.focus_on_editable_field = 1;
    return event;
}

struct CefCreatedCallbackContext {
    napi_threadsafe_function tsfn = nullptr;
};

struct CefCreatedCallbackData {
    int64_t browserId = -1;
};

// OSR paint data: holds a copy of the BGRA pixel buffer from CEF OnPaint
struct OsrPaintData {
    std::vector<uint8_t> pixels;
    int width;
    int height;
};

// Custom threadsafe function for OSR paint callbacks.
// JsThreadSafeCallback doesn't support ArrayBuffer, so we need a dedicated
// threadsafe function that creates an ArrayBuffer from the pixel data.
struct OsrPaintTsfn {
    napi_threadsafe_function tsfn = nullptr;
    // Frame-skipping: if a frame is still pending in the JS thread queue,
    // skip the next frame to prevent flooding and APP_INPUT_BLOCK.
    std::atomic<bool> framePending_{false};

    ~OsrPaintTsfn() {
        Release();
    }

    bool Init(napi_env env, napi_value jsFunction, const char* resourceName) {
        if (!env || !jsFunction) return false;

        napi_valuetype type;
        napi_status status = napi_typeof(env, jsFunction, &type);
        if (status != napi_ok || type != napi_function) return false;

        napi_value resource;
        status = napi_create_string_utf8(env, resourceName, NAPI_AUTO_LENGTH, &resource);
        if (status != napi_ok) return false;

        status = napi_create_threadsafe_function(
            env, jsFunction, nullptr, resource,
            0, 1, nullptr, nullptr, this,
            &OsrPaintTsfn::CallJs, &tsfn);
        return status == napi_ok && tsfn != nullptr;
    }

    void Call(const void* buf, int w, int h) {
        if (!tsfn) return;

        // Frame-skipping: if the previous frame hasn't been consumed by the JS thread yet,
        // skip this frame to avoid queue flooding and main-thread blocking.
        bool expected = false;
        if (!framePending_.compare_exchange_strong(expected, true)) {
            // Previous frame still pending — skip this one
            return;
        }

        size_t size = static_cast<size_t>(w) * h * 4;
        auto* data = new OsrPaintData{
            std::vector<uint8_t>(static_cast<const uint8_t*>(buf),
                                 static_cast<const uint8_t*>(buf) + size),
            w, h
        };
        napi_status status = napi_call_threadsafe_function(tsfn, data, napi_tsfn_nonblocking);
        if (status != napi_ok) {
            delete data;
            framePending_.store(false);
        }
    }

    void Release() {
        if (tsfn) {
            napi_release_threadsafe_function(tsfn, napi_tsfn_release);
            tsfn = nullptr;
        }
    }

    static void CallJs(napi_env env, napi_value jsCallback, void* context, void* data) {
        auto* paintData = static_cast<OsrPaintData*>(data);
        if (!paintData) return;
        if (!env || !jsCallback) {
            delete paintData;
            return;
        }

        napi_value arrayBuffer;
        void* bufferData = nullptr;
        napi_status status = napi_create_arraybuffer(env, paintData->pixels.size(), &bufferData, &arrayBuffer);
        if (status == napi_ok) {
            memcpy(bufferData, paintData->pixels.data(), paintData->pixels.size());

            napi_value widthVal;
            napi_create_int32(env, paintData->width, &widthVal);
            napi_value heightVal;
            napi_create_int32(env, paintData->height, &heightVal);

            napi_value argv[3] = { arrayBuffer, widthVal, heightVal };
            napi_value global;
            napi_get_global(env, &global);
            napi_value result;
            napi_call_function(env, global, jsCallback, 3, argv, &result);
        }
        delete paintData;

        // Clear the pending flag so the next frame can be sent
        auto* self = static_cast<OsrPaintTsfn*>(context);
        if (self) {
            self->framePending_.store(false);
        }
    }
};

struct OsrPaintCallbackHolder {
    std::shared_ptr<OsrPaintTsfn> tsfn;
    explicit OsrPaintCallbackHolder(std::shared_ptr<OsrPaintTsfn> t) : tsfn(std::move(t)) {}
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
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 4) {
        napi_throw_error(env, nullptr,
            "Expected 4-5 arguments: browserId, keyCode, keyType, modifiers, charCode?");
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

    // Optional 5th argument: Unicode char code from ArkUI KeyEvent.unicode
    // When provided (for CHAR events), this overrides the character derived from KeyCode
    int32_t charCode = 0;
    if (argc >= 5) {
        napi_get_value_int32(env, args[4], &charCode);
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefSendKeyEvent: browserId=%{public}ld, keyCode=%{public}d, keyType=%{public}d, charCode=%{public}d",
        browserId, keyCode, keyType, charCode);

    CefKeyEvent keyEvent = convertKeyEvent(keyCode, keyType, modifiers);

    // If ArkTS provided a Unicode char code, use it for the character field
    if (charCode > 0) {
        keyEvent.character = static_cast<char16_t>(charCode);
        keyEvent.unmodified_character = static_cast<char16_t>(charCode);
    }

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

static napi_value NAPI_Global_createOsrCefView(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "createOsrCefView called");

    size_t argc = 11;
    napi_value args[11];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 7) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "createOsrCefView: expected 7-11 arguments, got %{public}zu", argc);
        napi_throw_error(env, nullptr,
            "Expected 7-11 arguments: url, width, height, onPaint, onCreated, onTitleChange, onFocusTake [, onScrollOffset, onVirtualKeyboard, onTextSelection, onCursorChange]");
        return nullptr;
    }

    char url[1024] = {};
    size_t urlLen;
    napi_get_value_string_utf8(env, args[0], url, sizeof(url), &urlLen);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "createOsrCefView: url=%{public}s", url);

    int32_t width = 0;
    napi_get_value_int32(env, args[1], &width);

    int32_t height = 0;
    napi_get_value_int32(env, args[2], &height);

    // Create the OSR paint threadsafe function
    auto paintTsfn = std::make_shared<OsrPaintTsfn>();
    if (!paintTsfn->Init(env, args[3], "OsrPaintCallback")) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "napi",
            "createOsrCefView: failed to init paint threadsafe function");
        napi_throw_error(env, nullptr, "Failed to create paint callback");
        return nullptr;
    }

    auto jsCreatedCallback = makeJsThreadSafeCallback<int64_t>(env, args[4], "OsrCreatedCallback");
    auto jsTitleChangeCallback = makeJsThreadSafeCallback<std::string>(env, args[5], "OsrTitleChangeCallback");
    auto jsFocusTakeCallback = makeJsThreadSafeCallback<bool>(env, args[6], "OsrFocusTakeCallback");

    // Optional callbacks (args 7, 8, 9, 10)
    std::function<void(double, double)> scrollOffsetCb;
    std::function<void(bool)> virtualKeyboardCb;
    std::function<void(const std::string&)> textSelectionCb;
    std::function<void(int)> cursorChangeCb;

    if (argc >= 8) {
        auto jsScrollCallback = makeJsThreadSafeCallback<double>(env, args[7], "OsrScrollOffsetCallback");
        if (jsScrollCallback) {
            // We need to pass two doubles; use a pair wrapper
            // Actually JsThreadSafeCallback only supports single arg.
            // We'll encode as a string "x,y" and parse in JS.
            // But that's messy. Let's create a custom threadsafe function for this.
        }
    }

    // For scroll offset, virtual keyboard, and text selection we use custom TSFNs
    // Since JsThreadSafeCallback doesn't support double pairs, we'll use napi_create_double
    // Let's keep it simpler — use the JsThreadSafeCallback for single-arg callbacks
    // and encode scroll offset as a string.
    if (argc >= 8) {
        napi_valuetype vt;
        napi_typeof(env, args[7], &vt);
        if (vt == napi_function) {
            auto jsScrollCb = makeJsThreadSafeCallback<std::string>(env, args[7], "OsrScrollOffsetCallback");
            if (jsScrollCb) {
                scrollOffsetCb = [jsScrollCb](double x, double y) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%.2f,%.2f", x, y);
                    jsScrollCb->Call(std::string(buf));
                };
            }
        }
    }

    if (argc >= 9) {
        napi_valuetype vt;
        napi_typeof(env, args[8], &vt);
        if (vt == napi_function) {
            auto jsVkCb = makeJsThreadSafeCallback<bool>(env, args[8], "OsrVirtualKeyboardCallback");
            if (jsVkCb) {
                virtualKeyboardCb = [jsVkCb](bool show) {
                    jsVkCb->Call(show);
                };
            }
        }
    }

    if (argc >= 10) {
        napi_valuetype vt;
        napi_typeof(env, args[9], &vt);
        if (vt == napi_function) {
            auto jsTextCb = makeJsThreadSafeCallback<std::string>(env, args[9], "OsrTextSelectionCallback");
            if (jsTextCb) {
                textSelectionCb = [jsTextCb](const std::string& text) {
                    jsTextCb->Call(text);
                };
            }
        }
    }

    if (argc >= 11) {
        napi_valuetype vt;
        napi_typeof(env, args[10], &vt);
        if (vt == napi_function) {
            auto jsCursorCb = makeJsThreadSafeCallback<int32_t>(env, args[10], "OsrCursorChangeCallback");
            if (jsCursorCb) {
                cursorChangeCb = [jsCursorCb](int cursorType) {
                    jsCursorCb->Call(static_cast<int32_t>(cursorType));
                };
            }
        }
    }

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "createOsrCefView: width=%{public}d, height=%{public}d", width, height);

    // Wrap the paint threadsafe function in a PaintCallback
    auto paintHolder = std::make_shared<OsrPaintCallbackHolder>(paintTsfn);
    PaintCallback paintCallback = [paintHolder](const void* buffer, int w, int h) {
        paintHolder->tsfn->Call(buffer, w, h);
    };

    int64_t browserId = arkui_cef::ArkUICefManager::getInstance().createOsrBrowser(
        url, width, height, paintCallback,
        [jsCreatedCallback](int64_t id) {
            jsCreatedCallback->Call(id);
            jsCreatedCallback->Release();
        },
        [jsTitleChangeCallback](const std::string& title) {
            jsTitleChangeCallback->Call(title);
        },
        [jsFocusTakeCallback](bool next) {
            jsFocusTakeCallback->Call(next);
        },
        scrollOffsetCb,
        virtualKeyboardCb,
        textSelectionCb,
        cursorChangeCb);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "createOsrCefView: browserId=%{public}ld", browserId);

    napi_value result;
    napi_create_int64(env, browserId, &result);
    return result;
}

static napi_value NAPI_Global_cefOsrResize(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "cefOsrResize called");

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

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefOsrResize %{public}ld, %{public}d X %{public}d", browserId, width, height);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::resize, browser, width, height)));
    }
    return nullptr;
}

static napi_value NAPI_Global_destroyOsrCefView(napi_env env, napi_callback_info info) {
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi", "destroyOsrCefView called");

    size_t argc = 1;
    napi_value args[1];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId;
    napi_get_value_int64(env, args[0], &browserId);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "destroyOsrCefView id: %{public}ld", browserId);

    arkui_cef::ArkUICefManager::getInstance().destroyBrowser(browserId);
    return nullptr;
}

static napi_value NAPI_Global_cefSendMouseClick(napi_env env, napi_callback_info info) {
    size_t argc = 7;
    napi_value args[7] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 5) {
        napi_throw_error(env, nullptr,
            "Expected 5-7 arguments: browserId, x, y, button, mouseUp [, clickCount] [, modifiers]");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t x = 0;
    napi_get_value_int32(env, args[1], &x);
    int32_t y = 0;
    napi_get_value_int32(env, args[2], &y);
    int32_t button = 0;
    napi_get_value_int32(env, args[3], &button);
    bool mouseUp = false;
    napi_get_value_bool(env, args[4], &mouseUp);
    int32_t clickCount = 1;
    if (argc >= 6) {
        napi_get_value_int32(env, args[5], &clickCount);
    }
    uint32_t modifiers = 0;
    if (argc >= 7) {
        napi_get_value_uint32(env, args[6], &modifiers);
    }

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendMouseClickEvent, browser, x, y, button, mouseUp, clickCount, modifiers)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendMouseMove(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 4) {
        napi_throw_error(env, nullptr,
            "Expected 4-5 arguments: browserId, x, y, mouseLeave, modifiers");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t x = 0;
    napi_get_value_int32(env, args[1], &x);
    int32_t y = 0;
    napi_get_value_int32(env, args[2], &y);
    bool mouseLeave = false;
    napi_get_value_bool(env, args[3], &mouseLeave);
    uint32_t modifiers = 0;
    if (argc >= 5) {
        napi_get_value_uint32(env, args[4], &modifiers);
    }

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendMouseMoveEvent, browser, x, y, mouseLeave, modifiers)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendMouseWheel(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 5) {
        napi_throw_error(env, nullptr,
            "Expected 5 arguments: browserId, x, y, deltaX, deltaY");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t x = 0;
    napi_get_value_int32(env, args[1], &x);
    int32_t y = 0;
    napi_get_value_int32(env, args[2], &y);
    int32_t deltaX = 0;
    napi_get_value_int32(env, args[3], &deltaX);
    int32_t deltaY = 0;
    napi_get_value_int32(env, args[4], &deltaY);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendMouseWheelEvent, browser, x, y, deltaX, deltaY)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefOsrSetScale(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, scaleFactor");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    double scaleFactor = 1.0;
    napi_get_value_double(env, args[1], &scaleFactor);

    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "napi",
        "cefOsrSetScale: browserId=%{public}ld, scale=%{public}f", browserId, scaleFactor);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::setOsrScale, browser, static_cast<float>(scaleFactor))));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSetComponentOffset(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 3) {
        napi_throw_error(env, nullptr, "Expected 3 arguments: browserId, offsetX, offsetY");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    int32_t offsetX = 0;
    napi_get_value_int32(env, args[1], &offsetX);

    int32_t offsetY = 0;
    napi_get_value_int32(env, args[2], &offsetY);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::setComponentOffset, browser, offsetX, offsetY)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendFocusEvent(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, focused");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    bool focused = false;
    napi_get_value_bool(env, args[1], &focused);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendFocusEvent, browser, focused)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendCaptureLost(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendCaptureLostEvent, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefInvalidate(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 1) {
        napi_throw_error(env, nullptr, "Expected 1 argument: browserId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::invalidate, browser)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSetWindowlessVisibility(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 2) {
        napi_throw_error(env, nullptr, "Expected 2 arguments: browserId, visible");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    bool visible = false;
    napi_get_value_bool(env, args[1], &visible);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::setWindowlessVisibility, browser, visible)));
    }
    return nullptr;
}

static napi_value NAPI_Global_cefSendTouchEvent(napi_env env, napi_callback_info info) {
    size_t argc = 5;
    napi_value args[5] = { nullptr };
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);

    if (argc < 5) {
        napi_throw_error(env, nullptr,
            "Expected 5 arguments: browserId, x, y, touchType, pointerId");
        return nullptr;
    }

    int64_t browserId = 0;
    napi_get_value_int64(env, args[0], &browserId);
    int32_t x = 0;
    napi_get_value_int32(env, args[1], &x);
    int32_t y = 0;
    napi_get_value_int32(env, args[2], &y);
    int32_t touchType = 0;
    napi_get_value_int32(env, args[3], &touchType);
    int32_t pointerId = 0;
    napi_get_value_int32(env, args[4], &pointerId);

    auto browser = arkui_cef::ArkUICefManager::getInstance().getBrowser(browserId);
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(
            base::BindOnce(&ArkUICefBrowser::sendTouchEvent, browser, x, y, touchType, pointerId)));
    }
    return nullptr;
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
        {"createOsrCefView", nullptr, NAPI_Global_createOsrCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"destroyOsrCefView", nullptr, NAPI_Global_destroyOsrCefView, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefOsrResize", nullptr, NAPI_Global_cefOsrResize, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefOsrSetScale", nullptr, NAPI_Global_cefOsrSetScale, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSetComponentOffset", nullptr, NAPI_Global_cefSetComponentOffset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendMouseClick", nullptr, NAPI_Global_cefSendMouseClick, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendMouseMove", nullptr, NAPI_Global_cefSendMouseMove, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendMouseWheel", nullptr, NAPI_Global_cefSendMouseWheel, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendFocusEvent", nullptr, NAPI_Global_cefSendFocusEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendCaptureLost", nullptr, NAPI_Global_cefSendCaptureLost, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefInvalidate", nullptr, NAPI_Global_cefInvalidate, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSetWindowlessVisibility", nullptr, NAPI_Global_cefSetWindowlessVisibility, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"cefSendTouchEvent", nullptr, NAPI_Global_cefSendTouchEvent, nullptr, nullptr, nullptr, napi_default, nullptr},
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
