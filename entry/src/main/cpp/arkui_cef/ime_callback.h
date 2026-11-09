#ifndef ARKUI_CEF_IME_CALLBACK_H
#define ARKUI_CEF_IME_CALLBACK_H

#include <functional>
#include <string>

namespace arkui_cef {

struct ImeCursorInfo {
    double left;
    double top;
    double width;
    double height;
};

struct ImeCallback {
    std::function<void(bool show)> onRequestVirtualKeyboard;
    std::function<void(const ImeCursorInfo& cursorInfo,
                       int selectionStart, int selectionEnd)> onImeCursorChanged;
};

} // namespace arkui_cef

#endif // ARKUI_CEF_IME_CALLBACK_H
