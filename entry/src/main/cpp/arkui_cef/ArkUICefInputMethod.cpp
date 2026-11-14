#include "ArkUICefInputMethod.h"
#include <hilog/log.h>
#include <cstring>

namespace {
constexpr unsigned int IME_LOG_DOMAIN = 0xFF00;
const char* TAG = "ArkUICefIM";

std::string utf16ToUtf8(const char16_t* text, size_t length) {
    if (!text || length == 0) return "";
    std::string result;
    result.reserve(length * 2);
    for (size_t i = 0; i < length; i++) {
        char16_t ch = text[i];
        if (ch < 0x80) {
            result += static_cast<char>(ch);
        } else if (ch < 0x800) {
            result += static_cast<char>(0xC0 | (ch >> 6));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        } else {
            result += static_cast<char>(0xE0 | (ch >> 12));
            result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        }
    }
    return result;
}
} // namespace

namespace arkui_cef {

ArkUICefInputMethod* ArkUICefInputMethod::instance_ = nullptr;

ArkUICefInputMethod::ArkUICefInputMethod() {
    instance_ = this;
}

ArkUICefInputMethod::~ArkUICefInputMethod() {
    releaseResources();
    if (instance_ == this) {
        instance_ = nullptr;
    }
}

void ArkUICefInputMethod::releaseResources() {
    if (input_method_proxy_) {
        InputMethod_ErrorCode res = OH_InputMethodController_Detach(input_method_proxy_);
        if (res != IME_ERR_OK) {
            OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
                "releaseResources: OH_InputMethodController_Detach failed: %{public}d", res);
        }
        input_method_proxy_ = nullptr;
    }

    if (text_editor_proxy_) {
        OH_TextEditorProxy_Destroy(text_editor_proxy_);
        text_editor_proxy_ = nullptr;
    }

    if (attach_options_) {
        OH_AttachOptions_Destroy(attach_options_);
        attach_options_ = nullptr;
    }

    if (cursor_info_) {
        OH_CursorInfo_Destroy(cursor_info_);
        cursor_info_ = nullptr;
    }

    is_attached_ = false;
}

bool ArkUICefInputMethod::attach(bool showKeyboard, int32_t windowId,
                                 double cursorLeft, double cursorTop,
                                 double cursorWidth, double cursorHeight,
                                 const ImeEventCallbacks& callbacks) {
    if (is_attached_) {
        OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
            "attach: already attached, releasing first");
        releaseResources();
    }

    window_id_ = windowId;
    cursor_left_ = cursorLeft;
    cursor_top_ = cursorTop;
    cursor_width_ = cursorWidth;
    cursor_height_ = cursorHeight;
    callbacks_ = callbacks;

    text_editor_proxy_ = OH_TextEditorProxy_Create();
    if (!text_editor_proxy_) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "attach: OH_TextEditorProxy_Create failed");
        return false;
    }

    InputMethod_ErrorCode err = IME_ERR_OK;

    err = OH_TextEditorProxy_SetGetTextConfigFunc(text_editor_proxy_, &ArkUICefInputMethod::GetTextConfigFunc);
    if (err != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "attach: SetGetTextConfigFunc failed: %{public}d", err);
        releaseResources();
        return false;
    }

    err = OH_TextEditorProxy_SetInsertTextFunc(text_editor_proxy_, &ArkUICefInputMethod::InsertTextFunc);
    if (err != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "attach: SetInsertTextFunc failed: %{public}d", err);
        releaseResources();
        return false;
    }

    err = OH_TextEditorProxy_SetDeleteBackwardFunc(text_editor_proxy_, &ArkUICefInputMethod::DeleteBackwardFunc);
    if (err != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "attach: SetDeleteBackwardFunc failed: %{public}d", err);
        releaseResources();
        return false;
    }

    err = OH_TextEditorProxy_SetSendEnterKeyFunc(text_editor_proxy_, &ArkUICefInputMethod::SendEnterKeyFunc);
    if (err != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "attach: SetSendEnterKeyFunc failed: %{public}d", err);
        releaseResources();
        return false;
    }

    err = OH_TextEditorProxy_SetSetPreviewTextFunc(text_editor_proxy_, &ArkUICefInputMethod::SetPreviewTextFunc);
    if (err != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "attach: SetSetPreviewTextFunc failed: %{public}d", err);
        releaseResources();
        return false;
    }

    err = OH_TextEditorProxy_SetFinishTextPreviewFunc(text_editor_proxy_, &ArkUICefInputMethod::FinishTextPreviewFunc);
    if (err != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "attach: SetFinishTextPreviewFunc failed: %{public}d", err);
        releaseResources();
        return false;
    }

    // Register stub callbacks — IMF requires ALL callbacks to be non-null
    OH_TextEditorProxy_SetDeleteForwardFunc(text_editor_proxy_, &ArkUICefInputMethod::DeleteForwardFunc);
    OH_TextEditorProxy_SetSendKeyboardStatusFunc(text_editor_proxy_, &ArkUICefInputMethod::SendKeyboardStatusFunc);
    OH_TextEditorProxy_SetMoveCursorFunc(text_editor_proxy_, &ArkUICefInputMethod::MoveCursorFunc);
    OH_TextEditorProxy_SetHandleSetSelectionFunc(text_editor_proxy_, &ArkUICefInputMethod::HandleSetSelectionFunc);
    OH_TextEditorProxy_SetHandleExtendActionFunc(text_editor_proxy_, &ArkUICefInputMethod::HandleExtendActionFunc);
    OH_TextEditorProxy_SetGetLeftTextOfCursorFunc(text_editor_proxy_, &ArkUICefInputMethod::GetLeftTextOfCursorFunc);
    OH_TextEditorProxy_SetGetRightTextOfCursorFunc(text_editor_proxy_, &ArkUICefInputMethod::GetRightTextOfCursorFunc);
    OH_TextEditorProxy_SetGetTextIndexAtCursorFunc(text_editor_proxy_, &ArkUICefInputMethod::GetTextIndexAtCursorFunc);
    OH_TextEditorProxy_SetReceivePrivateCommandFunc(text_editor_proxy_, &ArkUICefInputMethod::ReceivePrivateCommandFunc);

    attach_options_ = OH_AttachOptions_Create(showKeyboard);
    if (!attach_options_) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "attach: OH_AttachOptions_Create failed");
        releaseResources();
        return false;
    }

    InputMethod_ErrorCode res = OH_InputMethodController_Attach(
        text_editor_proxy_, attach_options_, &input_method_proxy_);
    if (res != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG,
            "attach: OH_InputMethodController_Attach failed: %{public}d", res);
        releaseResources();
        return false;
    }

    if (cursor_info_) {
        OH_CursorInfo_Destroy(cursor_info_);
    }
    cursor_info_ = OH_CursorInfo_Create(cursorLeft, cursorTop, cursorWidth, cursorHeight);
    if (cursor_info_) {
        InputMethod_ErrorCode cursorRes =
            OH_InputMethodProxy_NotifyCursorUpdate(input_method_proxy_, cursor_info_);
        if (cursorRes != IME_ERR_OK) {
            OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
                "attach: NotifyCursorUpdate failed: %{public}d", cursorRes);
        }
    }

    // Explicitly show keyboard — don't rely on proxy TextInput focus which
    // would create a competing ArkTS IMF client.
    InputMethod_ErrorCode showRes = OH_InputMethodProxy_ShowKeyboard(input_method_proxy_);
    if (showRes != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
            "attach: ShowKeyboard failed: %{public}d", showRes);
    }

    is_attached_ = true;
    return true;
}

void ArkUICefInputMethod::detach() {
    releaseResources();
}

bool ArkUICefInputMethod::showKeyboard() {
    if (!is_attached_ || !input_method_proxy_) {
        OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
            "showKeyboard: not attached (is_attached_=%{public}d proxy=%{public}d)", is_attached_, input_method_proxy_ != nullptr);
        return false;
    }
    InputMethod_ErrorCode res = OH_InputMethodProxy_ShowKeyboard(input_method_proxy_);
    if (res != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
            "showKeyboard: err=%{public}d", res);
    }
    return res == IME_ERR_OK;
}

bool ArkUICefInputMethod::hideKeyboard() {
    if (!is_attached_ || !input_method_proxy_) {
        OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
            "hideKeyboard: not attached (is_attached_=%{public}d proxy=%{public}d)", is_attached_, input_method_proxy_ != nullptr);
        return false;
    }
    InputMethod_ErrorCode res = OH_InputMethodProxy_HideKeyboard(input_method_proxy_);
    if (res != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
            "hideKeyboard: err=%{public}d", res);
    }
    return res == IME_ERR_OK;
}

bool ArkUICefInputMethod::updateCursor(double left, double top, double width, double height) {
    if (!is_attached_ || !input_method_proxy_) {
        OH_LOG_Print(LOG_APP, LOG_WARN, IME_LOG_DOMAIN, TAG,
            "updateCursor: not attached (is_attached_=%{public}d proxy=%{public}d)", is_attached_, input_method_proxy_ != nullptr);
        return false;
    }

    cursor_left_ = left;
    cursor_top_ = top;
    cursor_width_ = width;
    cursor_height_ = height;

    if (cursor_info_) {
        OH_CursorInfo_Destroy(cursor_info_);
    }
    cursor_info_ = OH_CursorInfo_Create(left, top, width, height);
    if (!cursor_info_) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG, "updateCursor: OH_CursorInfo_Create failed");
        return false;
    }

    InputMethod_ErrorCode res =
        OH_InputMethodProxy_NotifyCursorUpdate(input_method_proxy_, cursor_info_);
    if (res != IME_ERR_OK) {
        OH_LOG_Print(LOG_APP, LOG_ERROR, IME_LOG_DOMAIN, TAG,
            "updateCursor: NotifyCursorUpdate failed: %{public}d (left=%{public}.0f top=%{public}.0f)",
            res, left, top);
        return false;
    }

    return true;
}

// ==================== Static callback implementations ====================

void ArkUICefInputMethod::GetTextConfigFunc(InputMethod_TextEditorProxy* proxy,
                                            InputMethod_TextConfig* config) {
    if (!proxy || !config || !instance_) return;

    OH_TextConfig_SetInputType(config, IME_TEXT_INPUT_TYPE_TEXT);
    OH_TextConfig_SetEnterKeyType(config, IME_ENTER_KEY_GO);
    OH_TextConfig_SetWindowId(config, instance_->window_id_);
    OH_TextConfig_SetPreviewTextSupport(config, false);
}

void ArkUICefInputMethod::InsertTextFunc(InputMethod_TextEditorProxy* proxy,
                                         const char16_t* text, size_t length) {
    if (!proxy || !instance_) return;
    if (instance_->callbacks_.onInsertText) {
        instance_->callbacks_.onInsertText(utf16ToUtf8(text, length));
    }
}

void ArkUICefInputMethod::DeleteBackwardFunc(InputMethod_TextEditorProxy* proxy,
                                             int32_t length) {
    if (!proxy || !instance_) return;
    if (instance_->callbacks_.onDeleteBackward) {
        instance_->callbacks_.onDeleteBackward(length);
    }
}

void ArkUICefInputMethod::SendEnterKeyFunc(InputMethod_TextEditorProxy* proxy,
                                           InputMethod_EnterKeyType enterKeyType) {
    if (!proxy || !instance_) return;
    if (instance_->callbacks_.onSendEnterKey) {
        instance_->callbacks_.onSendEnterKey();
    }
}

int ArkUICefInputMethod::SetPreviewTextFunc(InputMethod_TextEditorProxy* proxy,
                                             const char16_t* text, size_t length,
                                             int32_t start, int32_t end) {
    if (!proxy || !instance_) return static_cast<int>(IME_ERR_NULL_POINTER);
    if (instance_->callbacks_.onSetPreviewText) {
        instance_->callbacks_.onSetPreviewText(utf16ToUtf8(text, length));
    }
    return static_cast<int>(IME_ERR_OK);
}

void ArkUICefInputMethod::FinishTextPreviewFunc(InputMethod_TextEditorProxy* proxy) {
    if (!proxy || !instance_) return;
    if (instance_->callbacks_.onFinishTextPreview) {
        instance_->callbacks_.onFinishTextPreview();
    }
}

// Stub implementations for required-but-unused callbacks
void ArkUICefInputMethod::DeleteForwardFunc(InputMethod_TextEditorProxy* proxy, int32_t length) {}
void ArkUICefInputMethod::SendKeyboardStatusFunc(InputMethod_TextEditorProxy* proxy, InputMethod_KeyboardStatus status) {}
void ArkUICefInputMethod::MoveCursorFunc(InputMethod_TextEditorProxy* proxy, InputMethod_Direction direction) {}
void ArkUICefInputMethod::HandleSetSelectionFunc(InputMethod_TextEditorProxy* proxy, int32_t start, int32_t end) {}
void ArkUICefInputMethod::HandleExtendActionFunc(InputMethod_TextEditorProxy* proxy, InputMethod_ExtendAction action) {}
void ArkUICefInputMethod::GetLeftTextOfCursorFunc(InputMethod_TextEditorProxy* proxy, int32_t number, char16_t text[], size_t* length) {
    if (length) *length = 0;
}
void ArkUICefInputMethod::GetRightTextOfCursorFunc(InputMethod_TextEditorProxy* proxy, int32_t number, char16_t text[], size_t* length) {
    if (length) *length = 0;
}
int32_t ArkUICefInputMethod::GetTextIndexAtCursorFunc(InputMethod_TextEditorProxy* proxy) {
    return 0;
}
int ArkUICefInputMethod::ReceivePrivateCommandFunc(InputMethod_TextEditorProxy* proxy, InputMethod_PrivateCommand* privateCommand[], size_t size) {
    return static_cast<int>(IME_ERR_OK);
}

} // namespace arkui_cef
