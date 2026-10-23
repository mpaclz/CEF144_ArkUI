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
export const cefSendKeyEvent: (browserId: number, keyCode: number, keyType: number, modifiers: number) => void;
