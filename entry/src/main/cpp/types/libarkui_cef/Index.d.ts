import { NodeContent } from "@kit.ArkUI";
import { image } from "@kit.ImageKit";

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
                                 onPaint: (regionData: ArrayBuffer | undefined, x: number, y: number, w: number, h: number,
                                           fullFrame: boolean, viewW: number, viewH: number) => void,
                                 onCreated: (browserId: number) => void,
                                 onTitleChange: (title: string) => void,
                                 onFocusTake: (next: boolean) => void,
                                 onScrollOffset?: (offset: string) => void,
                                 onVirtualKeyboard?: (show: boolean) => void,
                                 onTextSelection?: (selectedText: string) => void,
                                 onCursorChange?: (cursorType: number) => void,
                                  onImeCursor?: (left: number, top: number, width: number, height: number) => void,
                                  onStartDrag?: (text: string, html: string, url: string, urlTitle: string, fileName: string, fileData: ArrayBuffer, x: number, y: number) => void,
                                 onUpdateDragCursor?: (operation: number) => void) => number;
export const destroyOsrCefView: (browserId: number) => void;
export const registerOsrPixelmaps: (browserId: number, pmA: image.PixelMap, pmB: image.PixelMap) => void;
export const unregisterOsrPixelmaps: (browserId: number) => void;
export const attachOsrSurface: (browserId: number, surfaceId: string, widthPx: number, heightPx: number, dsf: number) => boolean;
export const updateOsrSurfaceGeometry: (browserId: number, widthPx: number, heightPx: number) => void;
export const detachOsrSurface: (browserId: number) => void;
export const cefOsrResize: (browserId: number, width: number, height: number) => void;
export const cefOsrSetScale: (browserId: number, scaleFactor: number) => void;
export const cefSetComponentOffset: (browserId: number, offsetX: number, offsetY: number) => void;
export const cefSetFrameRate: (browserId: number, frameRate: number) => void;
export const cefExecuteJavaScript: (browserId: number, code: string) => void;
export const cefSendMouseClick: (browserId: number, x: number, y: number, button: number, mouseUp: boolean, clickCount?: number, modifiers?: number) => void;
export const cefSendMouseMove: (browserId: number, x: number, y: number, mouseLeave: boolean, modifiers?: number) => void;
export const cefSendMouseWheel: (browserId: number, x: number, y: number, deltaX: number, deltaY: number, modifiers?: number) => void;
export const cefSendFocusEvent: (browserId: number, focused: boolean) => void;
export const cefSendCaptureLost: (browserId: number) => void;
export const cefInvalidate: (browserId: number) => void;
export const cefWasHidden: (browserId: number, hidden: boolean) => void;
export const cefSendTouchEvent: (browserId: number, x: number, y: number, touchType: number, pointerId: number) => void;
export const cefImeSetComposition: (browserId: number, text: string) => void;
export const cefImeCommitText: (browserId: number, text: string) => void;
export const cefImeCancelComposition: (browserId: number) => void;
export const registerOsrKeyFilter: (windowId: number, browserId: number) => void;
export const setRenderAreaActive: (browserId: number, active: boolean) => void;
export const cefImeCAttach: (
  windowId: number,
  cursorLeft: number, cursorTop: number, cursorWidth: number, cursorHeight: number,
  onInsertText: (text: string) => void,
  onDeleteLeft: (length: number) => void,
  onSendEnterKey: () => void,
  onSetPreviewText: (text: string) => void,
  onFinishTextPreview: () => void
) => boolean;
export const cefImeCUpdateCursor: (left: number, top: number, width: number, height: number) => boolean;
export const cefImeCDetach: () => void;
export const cefImeCShowKeyboard: () => boolean;
export const cefImeCHideKeyboard: () => boolean;
export const cefDragTargetDragEnter: (browserId: number, text: string, html: string, url: string, filePaths: string, x: number, y: number, allowedOps: number) => void;
export const cefDragTargetDragOver: (browserId: number, x: number, y: number, allowedOps: number) => void;
export const cefDragTargetDragLeave: (browserId: number) => void;
export const cefDragTargetDrop: (browserId: number, x: number, y: number) => void;
export const cefDragSourceEndedAt: (browserId: number, x: number, y: number, op: number) => void;
export const cefDragSourceSystemDragEnded: (browserId: number) => void;
// 拖出检测（windowed）：对 createCefView 的父 Stack 注册 NODE_ON_MOUSE 观察。
// winLeft/winTop 为内容区原点相对窗口左上角的偏移（= drawableRect.left/top -
// windowRect.left/top，2in1 自由窗即标题栏高度），winW/winH 为内容区尺寸，均 px。
// 坐标回调参数全 px。
export const cefWatchDragOut: (browserId: number, edgePx: number, minDragPx: number,
  winLeft: number, winTop: number, winW: number, winH: number, viewW: number, viewH: number,
  onDragOut: (browserId: number, pressLocalX: number, pressLocalY: number,
    pressScreenX: number, pressScreenY: number, curScreenX: number, curScreenY: number) => void) => boolean;
export const cefUpdateDragOutRect: (browserId: number, winLeft: number, winTop: number,
  winW: number, winH: number, viewW: number, viewH: number) => void;
export const cefUnwatchDragOut: (browserId: number) => void;
// windowed 迁移：把 browser 宿主窗口迁到 newNodeContent 下的新父 Stack，成功后
// 移除并销毁 oldNodeContent 中的旧父 Stack。同步阻塞（≤3s 等待 CEF UI 线程）。
export const adoptCefView: (browserId: number, oldNodeContent: NodeContent, newNodeContent: NodeContent,
  width: number, height: number, onAdopted: (ok: boolean) => void) => void;
// OSR 迁移：把 createOsrCefView 的各回调（无 url/onCreated）重绑到新窗口视图，
// 不创建/销毁 browser。Set* 在 CEF UI 线程执行，避免与回调调用竞争。
export const adoptOsrCefView: (browserId: number,
  onPaint: (regionData: ArrayBuffer | undefined, x: number, y: number, w: number, h: number,
    fullFrame: boolean, viewW: number, viewH: number) => void,
  onTitleChange: (title: string) => void,
  onFocusTake: (next: boolean) => void,
  onScrollOffset?: (offset: string) => void,
  onVirtualKeyboard?: (show: boolean) => void,
  onTextSelection?: (selectedText: string) => void,
  onCursorChange?: (cursorType: number) => void,
  onImeCursor?: (left: number, top: number, width: number, height: number) => void,
  onStartDrag?: (text: string, html: string, url: string, urlTitle: string, fileName: string, fileData: ArrayBuffer, x: number, y: number) => void,
  onUpdateDragCursor?: (operation: number) => void) => boolean;
