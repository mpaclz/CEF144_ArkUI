//
// Created on 2026/6/3.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#include "ArkUICefManager.h"

#include <hilog/log.h>

#include "ArkUICefApp.h"
#include "ArkUICefBrowser.h"
#include "ArkUICefClient.h"
#include "include/base/cef_callback.h"
#include <cerrno>

namespace arkui_cef {

namespace {
constexpr unsigned int LOG_PRINT_DOMAIN = 0xFF00;
int g_browserIdCounter = 0;

int allocateBrowserId() {
    return ++g_browserIdCounter;
}

void closeBrowser(CefRefPtr<ArkUICefBrowser> browser) {
    if (browser) {
        browser->close();
    }
}
}

ArkUICefManager& ArkUICefManager::getInstance() {
    static ArkUICefManager instance;
    return instance;
}

ArkUICefManager::ArkUICefManager() {
}

ArkUICefManager::~ArkUICefManager() {
    stop();
}

void ArkUICefManager::start() {
    cefThread_ = std::thread([this]() {
        CefThreadMain();
    });
}

void ArkUICefManager::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    initOk_ = false;
    
    std::unordered_map<int64_t, CefRefPtr<ArkUICefBrowser>> browsersToClose;
    browsersToClose = browsers_;
    
    for (auto& [id, browser] : browsersToClose) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&closeBrowser, browser)));
    }
    
    if (cefThread_.joinable()) {
        cefThread_.join();
    }
}

bool ArkUICefManager::isCefRunning() {
    std::lock_guard<std::mutex> lock(mutex_);
    return initOk_;
}

int64_t ArkUICefManager::createBrowser(Node* parentWindowHandle, const std::string& url,
                                       int width, int height, std::function<void(int64_t)> onCreated,
                                       std::function<void(const std::string&)> onTitleChange,
                                       std::function<void(bool)> onFocusTake) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initOk_) {
        return -1;
    }

    int64_t id = allocateBrowserId();
    CefRefPtr<ArkUICefBrowser> browser = new ArkUICefBrowser(id, width, height);
    browser->SetCreateCallback(onCreated);
    browser->SetTitleCallback(onTitleChange);
    browser->SetFocusTakeCallback(onFocusTake);
    CefRefPtr<ArkUICefClient> client = new ArkUICefClient(id, browser, this);
    CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::createBrowser, browser,
                                                            client, parentWindowHandle, url)));
    return id;
}

void ArkUICefManager::destroyBrowser(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initOk_) {
        return;
    }
    
    CefRefPtr<ArkUICefBrowser> browser;
    auto it = browsers_.find(id);
    if (it != browsers_.end()) {
        browser = it->second;
    }
    
    if (browser) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::close, browser)));
    }
}

CefRefPtr<ArkUICefBrowser> ArkUICefManager::getBrowser(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = browsers_.find(id);
    if (it == browsers_.end()) {
        OH_LOG_Print(LOG_APP, LOG_WARN, LOG_PRINT_DOMAIN, "ArkUICefManager", 
        "getBrowser: not found id=%{public}ld.", id);
        return nullptr;
    }
    return it->second;
}

bool ArkUICefManager::hasBrowser(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return browsers_.find(id) != browsers_.end();
}

void ArkUICefManager::onBrowserCreated(CefRefPtr<ArkUICefBrowser> browser) {
    std::lock_guard<std::mutex> lock(mutex_);
    browsers_.emplace(browser->getId(), browser);
}

void ArkUICefManager::onBrowserDestroyed(CefRefPtr<ArkUICefBrowser> browser) {
    std::lock_guard<std::mutex> lock(mutex_);
    browsers_.erase(browser->getId());
}

void ArkUICefManager::RegisterPopupTitleCallback(int cefBrowserId, std::function<void(const std::string&)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    popupTitleCallbacks_[cefBrowserId] = std::move(callback);
}

void ArkUICefManager::UnregisterPopupTitleCallback(int cefBrowserId) {
    std::lock_guard<std::mutex> lock(mutex_);
    popupTitleCallbacks_.erase(cefBrowserId);
}

void ArkUICefManager::NotifyPopupTitleChange(int cefBrowserId, const std::string& title) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = popupTitleCallbacks_.find(cefBrowserId);
    if (it != popupTitleCallbacks_.end()) {
        it->second(title);
    }
}

void ArkUICefManager::SetPopupBrowserCreatedCallback(std::function<void(int)> callback) {
    popupBrowserCreatedCallback_ = std::move(callback);
}

void ArkUICefManager::NotifyPopupBrowserCreated(int cefBrowserId) {
    if (popupBrowserCreatedCallback_) {
        popupBrowserCreatedCallback_(cefBrowserId);
        popupBrowserCreatedCallback_ = nullptr;
    }
}

void ArkUICefManager::CefThreadMain() {
    pthread_setname_np(pthread_self(), "CEF_Main");
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefManager", 
        "CefThreadMain: cef main thread starts.");
    CefMainArgs mainArgs;
    CefRefPtr<ArkUICefApp> app = new ArkUICefApp();
    
    CefSettings settings;
    ArkUICefApp::PopulateSettings(&settings);
    
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefManager", 
        "CefThreadMain: initialize begin.");
    bool ok = CefInitialize(mainArgs, settings, app, nullptr);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefManager", 
        "CefThreadMain: initialize done: %{public}d.", ok);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initOk_ = ok;
    }
    
    if (!ok) {
        return;
    }

    CefRunMessageLoop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        initOk_ = false;
    }
    CefShutdown();
}
} // namespace arkui_cef
