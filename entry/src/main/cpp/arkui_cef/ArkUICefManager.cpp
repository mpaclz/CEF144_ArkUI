//
// Created on 2026/6/3.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#include "ArkUICefManager.h"

#include <hilog/log.h>

#include <vector>

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

void quitMessageLoop() {
    CefQuitMessageLoop();
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
    // Snapshot the browser list and release the lock BEFORE posting close
    // tasks and joining the CEF thread: the close chain OnBeforeClose ->
    // onBrowserDestroyed runs ON the CEF UI thread and also acquires
    // mutex_, so holding it across join() would deadlock.
    std::vector<CefRefPtr<ArkUICefBrowser>> browsersToClose;
    bool wasRunning = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        wasRunning = initOk_;
        initOk_ = false;
        shuttingDown_ = true;
        browsersToClose.reserve(browsers_.size());
        for (auto& [id, browser] : browsers_) {
            browsersToClose.push_back(browser);
        }
    }

    if (!wasRunning) {
        // CEF never initialized or already stopped — nothing to close.
        if (cefThread_.joinable()) {
            cefThread_.join();
        }
        return;
    }

    for (auto& browser : browsersToClose) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&closeBrowser, browser)));
    }

    // No browsers alive: no OnBeforeClose will ever fire, so nobody would
    // call CefQuitMessageLoop — quit the loop directly via a posted task.
    if (browsersToClose.empty()) {
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&quitMessageLoop)));
    }

    // The CEF thread exits when all browsers closed (last OnBeforeClose ->
    // onBrowserDestroyed sees shuttingDown_ && empty map -> CefQuitMessageLoop)
    // or the direct quit task above ran.
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

int64_t ArkUICefManager::createOsrBrowser(const std::string& url, int width, int height,
                                          PaintCallback onPaint,
                                          std::function<void(int64_t)> onCreated,
                                          std::function<void(const std::string&)> onTitleChange,
                                          std::function<void(bool)> onFocusTake,
                                          ScrollOffsetCallback onScrollOffset,
                                          VirtualKeyboardCallback onVirtualKeyboard,
                                          TextSelectionCallback onTextSelection,
                                          CursorChangeCallback onCursorChange,
                                          ImeCursorCallback onImeCursor,
                                          StartDragCallback onStartDrag,
                                          UpdateDragCursorCallback onUpdateDragCursor) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initOk_) {
        return -1;
    }

    int64_t id = allocateBrowserId();
    CefRefPtr<ArkUICefBrowser> browser = new ArkUICefBrowser(id, width, height);
    browser->SetUseOsr(true);
    browser->SetCreateCallback(onCreated);
    browser->SetTitleCallback(onTitleChange);
    browser->SetFocusTakeCallback(onFocusTake);

    CefRefPtr<ArkUICefRenderHandler> renderHandler = new ArkUICefRenderHandler(width, height);
    renderHandler->SetPaintCallback(onPaint);
    if (onScrollOffset) renderHandler->SetScrollOffsetCallback(onScrollOffset);
    if (onVirtualKeyboard) renderHandler->SetVirtualKeyboardCallback(onVirtualKeyboard);
    if (onTextSelection) renderHandler->SetTextSelectionCallback(onTextSelection);
    if (onImeCursor) renderHandler->SetImeCursorCallback(onImeCursor);
    if (onStartDrag) renderHandler->SetStartDragCallback(onStartDrag);
    if (onUpdateDragCursor) renderHandler->SetUpdateDragCursorCallback(onUpdateDragCursor);
    if (onCursorChange) browser->SetCursorChangeCallback(onCursorChange);

    CefRefPtr<ArkUICefClient> client = new ArkUICefClient(id, browser, this);
    client->SetUseOsr(true);
    client->SetRenderHandler(renderHandler);

    // OSR 模式不需要父窗口，传 nullptr
    CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(&ArkUICefBrowser::createBrowser, browser,
                                                            client, (Node*)nullptr, url)));
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

CefRefPtr<ArkUICefRenderHandler> ArkUICefManager::getRenderHandler(int64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = browsers_.find(id);
    if (it == browsers_.end()) {
        return nullptr;
    }
    CefRefPtr<ArkUICefClient> client = it->second->getClient();
    if (!client) {
        return nullptr;
    }
    return client->GetArkUICefRenderHandler();
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
    bool shouldQuit = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        browsers_.erase(browser->getId());
        // Quit the message loop only during a full shutdown (stop() requested)
        // AND only after the LAST browser closed. This callback runs on the
        // CEF UI thread, which is the thread CefQuitMessageLoop must be
        // called on.
        shouldQuit = shuttingDown_ && browsers_.empty();
    }
    if (shouldQuit) {
        CefQuitMessageLoop();
    }
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

    CefMainArgs mainArgs;
    CefRefPtr<ArkUICefApp> app = new ArkUICefApp();

    CefSettings settings;
    ArkUICefApp::PopulateSettings(&settings);

    bool ok = CefInitialize(mainArgs, settings, app, nullptr);
    OH_LOG_Print(LOG_APP, LOG_INFO, LOG_PRINT_DOMAIN, "ArkUICefManager",
        "CefThreadMain: CefInitialize %{public}s", ok ? "succeeded" : "failed");

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
