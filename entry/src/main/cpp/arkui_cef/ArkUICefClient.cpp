#include "ArkUICefClient.h"

#include "ArkUICefBrowser.h"
#include "ArkUICefManager.h"
#include "cef_app.h"
#include "include/wrapper/cef_helpers.h"
#include "include/cef_parser.h"
#include "include/cef_path_util.h"
#include <hilog/log.h>
#include <sstream>

namespace {
    constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;
}
namespace arkui_cef {
void ArkUICefClient::OnTitleChange(CefRefPtr<CefBrowser> browser,
                                   const CefString& title) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient", 
        "OnTitleChange: title=%{public}s, browser_id=%{public}d, arkui_cef_id_=%{public}d",
        title.ToString().c_str(), browser->GetIdentifier(), arkui_cef_id_);
    
    if (browser->GetIdentifier() != arkui_cef_id_) {
        // 弹窗浏览器的标题变更，通过 ArkUICefManager 全局回调通知到弹窗窗口
        ArkUICefManager::getInstance().NotifyPopupTitleChange(
            browser->GetIdentifier(), title.ToString());
    } else if (titleCallback_) {
        titleCallback_(title.ToString());
    }
}

bool ArkUICefClient::OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                                   int popup_id,
                                   const CefString &target_url, const CefString &target_frame_name,
                                   CefLifeSpanHandler::WindowOpenDisposition target_disposition, bool user_gesture,
                                   const CefPopupFeatures &popupFeatures, CefWindowInfo &windowInfo,
                                   CefRefPtr<CefClient> &client, CefBrowserSettings &settings,
                                   CefRefPtr<CefDictionaryValue> &extra_info, bool *no_javascript_access) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient",
        "OnBeforePopup: target_url=%{public}s, target_disposition=%{public}d", target_url.ToString().c_str(), target_disposition);
    // Load the target URL in the current frame instead of opening a new popup window
    frame->LoadURL(target_url);
    return true;
    
    // Open a new popup window
    // return false;
}

void ArkUICefClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient", 
        "OnAfterCreated: browser_id=%{public}d, id_=%{public}d",
        browser->GetIdentifier(), id_);
    
    if (arkui_cef_id_ < 0) {
        arkui_cef_id_ = browser->GetIdentifier();
        browser_->onBrowserCreated(browser);
        delegate_->onBrowserCreated(browser_);
    } else {
        ArkUICefManager::getInstance().NotifyPopupBrowserCreated(browser->GetIdentifier());
    }
}

bool ArkUICefClient::DoClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient", 
        "DoClose: browser_id=%{public}d", browser->GetIdentifier());
    
    return false;
}

void ArkUICefClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient", 
        "OnBeforeClose: browser_id=%{public}d", browser->GetIdentifier());
    
    browser_->onBrowserClosed();
    delegate_->onBrowserDestroyed(browser_);
}

void ArkUICefClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 ErrorCode errorCode,
                                 const CefString& errorText,
                                 const CefString& failedUrl) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "ArkUICefClient", 
        "OnLoadError: browser_id=%{public}d, errorCode=%{public}d, url=%{public}s, text=%{public}s",
        browser->GetIdentifier(), errorCode, failedUrl.ToString().c_str(), errorText.ToString().c_str());
    
    if (errorCode == ERR_ABORTED) {
        return;
    }
    
    std::stringstream ss;
    ss << "<html><body bgcolor=\"white\">"
       << "<h2>Failed to load URL "
       << std::string(failedUrl) << " with error " << std::string(errorText) << " (" << errorCode
       << ").</h2></body></html>";
    
    frame->LoadURL("data:text/html;base64," + CefURIEncode(CefBase64Encode(ss.str().data(), ss.str().size()), false).ToString());
}

void ArkUICefClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame,
                               int httpStatusCode) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient", 
        "OnLoadEnd: browser_id=%{public}d, url=%{public}s, status=%{public}d",
        browser->GetIdentifier(), frame->GetURL().ToString().c_str(), httpStatusCode);
    
    if (loadCallback_) {
        loadCallback_(httpStatusCode >= 200 && httpStatusCode < 400);
    }
}

bool ArkUICefClient::CanDownload(CefRefPtr<CefBrowser> browser,
                                 const CefString& url,
                                 const CefString& request_method) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient",
        "CanDownload: url=%{public}s, method=%{public}s",
        url.ToString().c_str(), request_method.ToString().c_str());
    return true;
}

bool ArkUICefClient::OnBeforeDownload(CefRefPtr<CefBrowser> browser,
                                      CefRefPtr<CefDownloadItem> download_item,
                                      const CefString& suggested_name,
                                      CefRefPtr<CefBeforeDownloadCallback> callback) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient",
        "OnBeforeDownload: suggested_name=%{public}s, id=%{public}u",
        suggested_name.ToString().c_str(), download_item->GetId());

    CefString download_dir;
    if (CefGetPath(PK_DIR_RESOURCES, download_dir) && !download_dir.empty()) {
        std::string download_path = download_dir.ToString() + "/" + suggested_name.ToString();
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient",
            "OnBeforeDownload: download_path=%{public}s", download_path.c_str());
        callback->Continue(CefString(download_path), true);
    } else {
        OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_PRINT_DOMAIN, "ArkUICefClient",
            "OnBeforeDownload: failed to get download directory");
    }
    return true;
}

void ArkUICefClient::OnDownloadUpdated(CefRefPtr<CefBrowser> browser,
                                       CefRefPtr<CefDownloadItem> download_item,
                                       CefRefPtr<CefDownloadItemCallback> callback) {}

bool ArkUICefClient::OnPreKeyEvent(CefRefPtr<CefBrowser> browser,
                                   const CefKeyEvent& event,
                                   CefEventHandle os_event,
                                   bool* is_keyboard_shortcut) {
    CEF_REQUIRE_UI_THREAD();

    if (!browser) {
        return false;
    }

    // F12 (0x7B): 打开 DevTools
    if (event.type == KEYEVENT_RAWKEYDOWN) {
        switch (event.windows_key_code) {
        case 0x7B:
            if (browser->GetHost()) {
                CefWindowInfo windowInfo;
                CefBrowserSettings settings;
                browser->GetHost()->ShowDevTools(windowInfo, this, settings,
                                                 CefPoint());
            }
            return true;
        default:
            break;
        }
    }

    // Ctrl + 快捷键 demo
    if (event.modifiers & EVENTFLAG_CONTROL_DOWN) {
        if (event.type == KEYEVENT_KEYUP) {
            switch (event.windows_key_code) {
            case '1':
                if (browser->CanGoBack()) {
                    browser->GoBack();
                }
                return true;
            case '2':
                if (browser->CanGoForward()) {
                    browser->GoForward();
                }
                return true;
            case '3':
                browser->Reload();
                return true;
            case '4':
                browser->ReloadIgnoreCache();
                return true;
            case '5':
                browser->StopLoad();
                return true;
            default:
                break;
            }
        }
    }

    return false;
}

bool ArkUICefClient::OnKeyEvent(CefRefPtr<CefBrowser> browser,
                                const CefKeyEvent& event,
                                CefEventHandle os_event) {
    return false;
}

bool ArkUICefClient::OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                            uint64_t prompt_id,
                                            const CefString& requesting_origin,
                                            uint32_t requested_permissions,
                                            CefRefPtr<CefPermissionPromptCallback> callback) {
    CEF_REQUIRE_UI_THREAD();

    if (requested_permissions & CEF_PERMISSION_TYPE_CLIPBOARD) {
        OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient",
            "OnShowPermissionPrompt: granting clipboard permission, origin=%{public}s, prompt_id=%{public}llu",
            requesting_origin.ToString().c_str(), prompt_id);
        callback->Continue(CEF_PERMISSION_RESULT_ACCEPT);
        return true;
    }

    return false;
}

void ArkUICefClient::OnTakeFocus(CefRefPtr<CefBrowser> browser, bool next) {
    CEF_REQUIRE_UI_THREAD();
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefClient",
        "OnTakeFocus: browser_id=%{public}d, next=%{public}d",
        browser->GetIdentifier(), next);

    if (focusTakeCallback_) {
        focusTakeCallback_(next);
    }
}
} // namespace arkui_cef