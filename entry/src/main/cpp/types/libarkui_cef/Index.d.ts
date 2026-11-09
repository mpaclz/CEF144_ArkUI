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
