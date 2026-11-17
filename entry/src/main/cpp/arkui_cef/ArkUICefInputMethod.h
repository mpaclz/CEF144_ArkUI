#ifndef ARKUI_CEF_INPUT_METHOD_H
#define ARKUI_CEF_INPUT_METHOD_H

#include <functional>
#include <mutex>
#include <string>
#include "inputmethod/inputmethod_controller_capi.h"
#include "inputmethod/inputmethod_text_editor_proxy_capi.h"
#include "inputmethod/inputmethod_attach_options_capi.h"
#include "inputmethod/inputmethod_cursor_info_capi.h"
#include "inputmethod/inputmethod_text_config_capi.h"
#include "inputmethod/inputmethod_types_capi.h"
#include "inputmethod/inputmethod_inputmethod_proxy_capi.h"

namespace arkui_cef {

struct ImeEventCallbacks {
    std::function<void(const std::string& text)> onInsertText;
    std::function<void(int32_t length)> onDeleteBackward;
    std::function<void()> onSendEnterKey;
    std::function<void(const std::string& text)> onSetPreviewText;
    std::function<void()> onFinishTextPreview;
};

class ArkUICefInputMethod {
public:
    ArkUICefInputMethod();
    ~ArkUICefInputMethod();

    bool attach(bool ShowKeyboard, int32_t windowId,
                double cursorLeft, double cursorTop,
                double cursorWidth, double cursorHeight,
                const ImeEventCallbacks& callbacks);

    void detach();

    // Panel control WITHOUT session churn: the documented call order is
    // Attach → ShowKeyboard → HideKeyboard → Detach, i.e. the session is
    // DESIGNED to persist across panel show/hide cycles. Repeated
    // Attach/Detach raced ArkUI's own TextInput editor lifecycle at the
    // input method service (a late ArkUI teardown can land after our fresh
    // attach and kill it — panel disappears, input dead). Persistent
    // session + show/hide removes that race window entirely. Returns false
    // (IME_ERR_DETACHED) when the session died — the caller self-heals by
    // re-attaching.
    bool ShowKeyboard();
    bool HideKeyboard();

    // Reports the caret rectangle (screen-absolute physical px) to the input
    // method so it can place the candidate window. Called from the CEF UI
    // thread on every caret move while a session is attached; a no-op
    // otherwise. Safe to call from any thread: attach()/detach() run on the
    // JS thread, so the session handle is guarded by a mutex.
    static void NotifyCaretBounds(double left, double top,
                                  double width, double height);

    bool IsAttached() const { return is_attached_; }

private:
    bool UpdateCursor(double left, double top, double width, double height);
    static void GetTextConfigFunc(InputMethod_TextEditorProxy* proxy,
                                  InputMethod_TextConfig* config);
    static void InsertTextFunc(InputMethod_TextEditorProxy* proxy,
                               const char16_t* text, size_t length);
    static void DeleteBackwardFunc(InputMethod_TextEditorProxy* proxy,
                                   int32_t length);
    static void SendEnterKeyFunc(InputMethod_TextEditorProxy* proxy,
                                 InputMethod_EnterKeyType enterKeyType);
    static int SetPreviewTextFunc(InputMethod_TextEditorProxy* proxy,
                                   const char16_t* text, size_t length,
                                   int32_t start, int32_t end);
    static void FinishTextPreviewFunc(InputMethod_TextEditorProxy* proxy);
    // Stubs for required-but-unused callbacks
    static void DeleteForwardFunc(InputMethod_TextEditorProxy* proxy, int32_t length);
    static void SendKeyboardStatusFunc(InputMethod_TextEditorProxy* proxy, InputMethod_KeyboardStatus status);
    static void MoveCursorFunc(InputMethod_TextEditorProxy* proxy, InputMethod_Direction direction);
    static void HandleSetSelectionFunc(InputMethod_TextEditorProxy* proxy, int32_t start, int32_t end);
    static void HandleExtendActionFunc(InputMethod_TextEditorProxy* proxy, InputMethod_ExtendAction action);
    static void GetLeftTextOfCursorFunc(InputMethod_TextEditorProxy* proxy, int32_t number, char16_t text[], size_t* length);
    static void GetRightTextOfCursorFunc(InputMethod_TextEditorProxy* proxy, int32_t number, char16_t text[], size_t* length);
    static int32_t GetTextIndexAtCursorFunc(InputMethod_TextEditorProxy* proxy);
    static int ReceivePrivateCommandFunc(InputMethod_TextEditorProxy* proxy, InputMethod_PrivateCommand* privateCommand[], size_t size);

    void ReleaseResources();

    InputMethod_TextEditorProxy* text_editor_proxy_ = nullptr;
    InputMethod_AttachOptions* attach_options_ = nullptr;
    InputMethod_InputMethodProxy* input_method_proxy_ = nullptr;
    InputMethod_CursorInfo* cursor_info_ = nullptr;
    bool is_attached_ = false;

    int32_t window_id_ = -1;
    double cursor_left_ = 0;
    double cursor_top_ = 0;
    double cursor_width_ = 2;
    double cursor_height_ = 24;

    ImeEventCallbacks callbacks_;

    static ArkUICefInputMethod* instance_;
    // Latest caret reported by the render handler (screen-absolute px), kept
    // even while no session exists. Clicking an input field fires
    // SelectionBoundsChanged a few ms BEFORE the ArkTS attach() call, so the
    // first caret would otherwise be dropped by the is_attached_ gate — leaving
    // attach() with only the tap-point estimate, which is far off in an empty
    // field (tap in the middle vs caret pinned at the left edge). attach()
    // consumes this cache for its initial CursorInfo. Guarded by session_mutex_.
    static double cached_caret_left_;
    static double cached_caret_top_;
    static double cached_caret_width_;
    static double cached_caret_height_;
    static int64_t cached_caret_time_ms_;
    // Guards instance_ and the session handles across the JS thread
    // (attach/detach) and the CEF UI thread (caret reports). attach() holds it
    // across the blocking Attach IPC, so a caret report arriving in that
    // window waits a few ms instead of touching a half-built session.
    static std::mutex session_mutex_;
};

} // namespace arkui_cef

#endif // ARKUI_CEF_INPUT_METHOD_H
