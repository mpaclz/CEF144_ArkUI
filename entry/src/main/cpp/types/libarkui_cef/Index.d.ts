import { NodeContent } from "@kit.ArkUI";

export const startCef: () => void;
export const isCefRunning: () => boolean;
export const cefShow: (number) => void;
export const cefHide: (number) => void;
export const hasCef: (number) => boolean;
export const createCefView: (nodeContent: NodeContent, url: string,
                             width: number, height: number,
                             onCreated: (browserId: number) => void,
                             onTitleChange: (title: string) => void,
                             onFocusTake: (next: boolean) => void) => number;
export const destroyCefView: (browserId: number, nodeContent: NodeContent) => void;
export const cefResize: (browserId: number, width: number, height: number) => void;
export const cefGoBack: (browserId: number) => void;
export const cefGoForward: (browserId: number) => void;
export const cefReload: (browserId: number) => void;
export const cefLoadUrl: (browserId: number, url: string) => void;
export const registerPopupTitleCallback: (cefBrowserId: number, callback: (title: string) => void) => void;
export const unregisterPopupTitleCallback: (cefBrowserId: number) => void;
export const setPopupBrowserCreatedCallback: (callback: (cefBrowserId: number) => void) => void;
export const cefSendKeyEvent: (browserId: number, keyCode: number, keyType: number, modifiers: number, charCode?: number) => void;
export const createOsrCefView: (url: string, width: number, height: number,
                                 onPaint: (buffer: ArrayBuffer, width: number, height: number) => void,
                                 onCreated: (browserId: number) => void,
                                 onTitleChange: (title: string) => void,
                                 onFocusTake: (next: boolean) => void,
                                 onScrollOffset?: (offset: string) => void,
                                 onVirtualKeyboard?: (show: boolean) => void,
                                 onTextSelection?: (selectedText: string) => void,
                                 onCursorChange?: (cursorType: number) => void) => number;
export const destroyOsrCefView: (browserId: number) => void;
export const cefOsrResize: (browserId: number, width: number, height: number) => void;
export const cefOsrSetScale: (browserId: number, scaleFactor: number) => void;
export const cefSetComponentOffset: (browserId: number, offsetX: number, offsetY: number) => void;
export const cefSendMouseClick: (browserId: number, x: number, y: number, button: number, mouseUp: boolean, clickCount?: number, modifiers?: number) => void;
export const cefSendMouseMove: (browserId: number, x: number, y: number, mouseLeave: boolean, modifiers?: number) => void;
export const cefSendMouseWheel: (browserId: number, x: number, y: number, deltaX: number, deltaY: number) => void;
export const cefSendFocusEvent: (browserId: number, focused: boolean) => void;
export const cefSendCaptureLost: (browserId: number) => void;
export const cefInvalidate: (browserId: number) => void;
export const cefSetWindowlessVisibility: (browserId: number, visible: boolean) => void;
export const cefSendTouchEvent: (browserId: number, x: number, y: number, touchType: number, pointerId: number) => void;
