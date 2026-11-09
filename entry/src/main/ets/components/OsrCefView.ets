import arkui_cef from 'libarkui_cef.so';
import { image } from '@kit.ImageKit';
import { hilog } from "@kit.PerformanceAnalysisKit";
import { pointer } from '@kit.InputKit';
import { window, dragController } from "@kit.ArkUI";
import { common } from '@kit.AbilityKit';
import { unifiedDataChannel, uniformTypeDescriptor } from '@kit.ArkData';
import { fileIo, fileUri } from '@kit.CoreFileKit';

const DOMAIN = 0xFF01;
const MODULE = 'OsrCefView'

interface DragDataInfo {
  text: string;
  html: string;
  url: string;
  filePaths: string;
}

@Component
export struct OsrCefView {
  @State private browserId: number = -1;
  @State private cefCreating: boolean = false;
  @State private pixelMap: image.PixelMap | undefined = undefined;
  @State private pageTitle: string = '';
  @State private urlInput: string = '';
  // Double-buffered persistent PixelMaps. The Image component only reloads
  // when the object REFERENCE changes (in-place content updates are not
  // rendered — documented ArkUI behavior), and it keeps showing the previous
  // image while a new src loads. So each incremental frame writes the dirty
  // region into BOTH buffers (keeping their content identical) and swaps the
  // reference to the back buffer: the reload is seamless because the old
  // front image shows identical content. This replaces the earlier
  // destroy/recreate Image toggle, which flashed the black background during
  // every reload window.
  private pmA: image.PixelMap | undefined = undefined;
  private pmB: image.PixelMap | undefined = undefined;
  private showA: boolean = true;
  // Track if a PixelMap (re)creation is in progress — only used on the
  // low-frequency full-frame path (first frame / resize). Incremental
  // frames are synchronous and never need this guard.
  private pixelMapCreating: boolean = false;
  // Size (physical px) of the currently displayed PixelMap. An incoming
  // frame whose viewW/viewH differs takes the rebuild path.
  private pmWidth: number = 0;
  private pmHeight: number = 0;
  // Latest full-frame payload received while a rebuild is already in flight
  // (rapid resizes). Kept as pending and applied when the current build
  // finishes; older payloads are replaced.
  private pendingFullFrame: ArrayBuffer | undefined = undefined;
  private pendingFullFrameW: number = 0;
  private pendingFullFrameH: number = 0;
  // Track mouse button state for drag operations (scrollbar drag, text selection, etc.)
  // Tracks the LEFT button only (the only one with drag semantics).
  private mouseButtonPressed: boolean = false;
  private lastMouseX: number = 0;
  private lastMouseY: number = 0;
  // Double/triple-click tracking — ArkUI MouseEvent has no clickCount, but CEF
  // requires it in the mouse event for dblclick/triple-click recognition
  // (word/paragraph text selection). Windows defaults: 500ms, 4px slop.
  private lastClickTime: number = 0;
  private lastClickX: number = -1;
  private lastClickY: number = -1;
  private clickCount: number = 0;
  private static readonly DOUBLE_CLICK_TIME_MS: number = 500;
  private static readonly DOUBLE_CLICK_SLOP_PX: number = 4;
  // Touchpad pinch → page zoom (ctrl+wheel), like desktop Chrome's touchpad
  // pinch on macOS/Windows. Touchscreen multi-touch is forwarded to CEF
  // natively (see handleTouchEvent) — the renderer or the page itself
  // (e.g. maps) handles it, so the two input types intentionally use
  // different zoom engines (touchpad = page zoom with re-layout,
  // touchscreen = viewport zoom / page-handled).
  // Log-scale accumulation: Chromium's zoom presets are multiplicative, so a
  // log mapping makes pinch-in / pinch-out round trips symmetric (the same
  // ratio in and out emits exactly opposite step counts). One accumulated
  // step (≈15% pinch change) emits one ctrl+wheel tick.
  private pinchLastScale: number = 1.0;
  private pinchLogAccum: number = 0.0;
  private static readonly PINCH_ZOOM_STEP_LOG: number = 0.14; // ≈ Math.log(1.15): one zoom step per ~15% pinch change
  // Input Method Framework (IMF) — using C API via NAPI for cursor updates.
  // ArkTS inputMethodController was replaced because its async updateCursor()
  // triggered a second internal OnCursorUpdate that caused candidate window jumps.
  //
  // Design (reference QCefView):
  // - keyboardVisible: set by CEF OnVirtualKeyboardRequested — tracks whether
  //   the web page has a focused input field (like Qt's WA_InputMethodEnabled).
  // - IMF attach/detach: driven by window focus and keyboardVisible together:
  //   WINDOW_INACTIVE → detach (hide keyboard, preserve keyboardVisible)
  //   WINDOW_ACTIVE → if keyboardVisible, attach (show keyboard)
  //   OnVKR(true) → keyboardVisible=true, attach if window active
  //   OnVKR(false) → keyboardVisible=false, detach
  private imeAttached: boolean = false;
  private imeAttaching: boolean = false;
  // When true, key events are forwarded to CEF (user clicked on render area).
  // When false, key events go to the URL bar TextInput normally.
  private renderAreaActive: boolean = false;
  // Set by WINDOW_INACTIVE — when true, WINDOW_ACTIVE should restore CEF focus.
  private focusLostFlag: boolean = false;
  // Current render-pause state (CEF WasHidden). Transition-guarded so
  // repeated onVisibleAreaChange events are idempotent. Mirrors QCefView's
  // QEvent::Show/Hide handling — window FOCUS events deliberately do not
  // participate (an unfocused window may still be visible and interactive).
  private osrHidden: boolean = false;
  // Component position within the window (from globalPosition, in vp/CSS px)
  // and device scale factor — cached from onAreaChange for IME cursor computation.
  private componentGlobalX: number = 0;
  private componentGlobalY: number = 0;
  private cachedDsf: number = 1.0;
  // Cached window object — used to get windowRect (window's screen position)
  // via the synchronous getWindowProperties() API.
  private cachedWin: window.Window | null = null;
  // Last caret position in window-relative px (from C++ onImeCompositionRangeChanged).
  private lastCaretWinLeft: number = 0;
  private lastCaretWinTop: number = 0;
  private lastCaretWidth: number = 2;
  private lastCaretHeight: number = 24;
  // True after onOsrImeCursor has fired at least once.
  private hasCaretBounds: boolean = false;
  private lastWinRectLeft: number = 0;
  private lastWinRectTop: number = 0;
  // Last input position (touch or mouse down) in vp, relative to the render
  // area component. Used to compute the initial IME cursor position when
  // attaching the input method.
  private lastInputX: number = 0;
  private lastInputY: number = 0;
  // Window ID — passed to IMF C API via TextConfig.windowId in GetTextConfigFunc.
  private cachedWindowId: number = -1;
  // True when IMF was detached due to component becoming invisible (tab switch).
  // Used to re-attach when the component becomes visible again.
  private imeDetachedByVisibility: boolean = false;
  // Drag & drop state
  // True while a system drag (initiated by dragController.executeDrag) is in progress.
  private isDragInProgress: boolean = false;
  // Last drag operation from CEF UpdateDragCursor callback.
  // DRAG_OPERATION_NONE=0, COPY=1, LINK=2, MOVE=16
  private lastDragOperation: number = 0;
  // Last drag position (view-relative CSS px) from onDragMove/onDrop.
  // Used by executeDrag callback for DragSourceEndedAt coordinates.
  // Initialized to StartDragging position as fallback (for drags that end
  // outside the component without any onDragMove/onDrop firing).
  private lastDragMoveX: number = 0;
  private lastDragMoveY: number = 0;
  // Cached drag data from CEF StartDragging — used as fallback in onDrop
  // when ArkUI DragEvent.getData() returns empty (e.g. self-drag edge cases).
  private pendingDragText: string = '';
  private pendingDragHtml: string = '';
  private pendingDragUrl: string = '';
  // Newline-separated file paths for self-drag file drop-back
  private pendingDragFilePaths: string = '';
  // True when cefDragTargetDragEnter has been called for the current drag session.
  // If true, onDrop only needs to call DragOver + Drop (Enter already done).
  // If false (external drag, getData() not available until onDrop), onDrop calls Enter + Over + Drop.
  private dragEnterCalled: boolean = false;

  @State private viewWidth: number = 800;
  @State private viewHeight: number = 600;
  @State private scrollOffsetText: string = '';
  @State private selectedText: string = '';
  @State private keyboardVisible: boolean = false;
  @State private cursorTypeText: string = 'default';
  @State private dragPreviewText: string = '';

  @Prop viewUrl: string = '';
  // Maximum OSR frame rate (fps). The render pipeline now runs fully
  // synchronous (dirty-region writePixelsSync, no per-frame createPixelMap),
  // so the historical 10fps cap is no longer required to keep the JS thread
  // healthy. 20 is a safe default; profile and raise on capable devices.
  @Prop osrFrameRate: number = 60;

   aboutToAppear(): void {
    this.urlInput = this.viewUrl;
    if (this.browserId <= 0) {
      // Use physical pixels for CEF buffer size (OnPaint outputs physical pixels).
      // DSF is set to the actual device scale factor so CEF's GetViewRect returns
      // CSS pixels, making web content layout match non-OSR tabs.
      let dsf = this.getUIContext().vp2px(1);
      let wpx = Math.round(this.viewWidth * dsf);
      let hpx = Math.round(this.viewHeight * dsf);
      // Store physical-pixel dimensions for OnPaint buffer allocation
      this.viewWidth = wpx;
      this.viewHeight = hpx;
      hilog.info(DOMAIN, MODULE, "Begin to create OSR browser. dsf=%{public}f, size=%{public}d X %{public}d",
        dsf, wpx, hpx);
      this.createOsrCefView(this.viewUrl, wpx, hpx);
    }
  }

  aboutToDisappear(): void {
    this.destroy();
  }

  build() {
    Column() {
      // Toolbar
      Row() {
        Button('Back')
          .fontSize(12)
          .height(28)
          .enabled(this.isCreated())
          .onClick(() => this.goBack())

        Button('Forward')
          .fontSize(12)
          .height(28)
          .margin({ left: 4 })
          .enabled(this.isCreated())
          .onClick(() => this.goForward())

        Button('Reload')
          .fontSize(12)
          .height(28)
          .margin({ left: 4 })
          .enabled(this.isCreated())
          .onClick(() => this.reload())

        TextInput({ text: this.urlInput })
          .fontSize(12)
          .height(28)
          .layoutWeight(1)
          .margin({ left: 4 })
          .enabled(this.isCreated())
          .onChange((val: string) => { this.urlInput = val; })
          .onSubmit(() => this.loadUrl(this.urlInput))
          .onClick(() => {
            this.setRenderAreaActive(false);
          })

        Button('Go')
          .fontSize(12)
          .height(28)
          .margin({ left: 4 })
          .enabled(this.isCreated())
          .onClick(() => this.loadUrl(this.urlInput))
      }
      .width('100%')
      .height(36)
      .padding({ left: 4, right: 4 })
      .backgroundColor(Color.White)

      // Title bar
      if (this.pageTitle) {
        Text(this.pageTitle)
          .fontSize(10)
          .fontColor('#666666')
          .width('100%')
          .height(18)
          .padding({ left: 8, right: 8 })
          .backgroundColor('#F0F0F0')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }

      // Render area — all events are captured and forwarded to CEF
      Column() {
        if (this.pixelMap) {
          Image(this.pixelMap)
            .width('100%')
            .height('100%')
            .objectFit(ImageFit.Fill)
        } else {
          Column() {
            Text('OSR Loading...')
              .fontSize(16)
              .fontColor(Color.Gray)
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.Center)
          .backgroundColor(Color.Black)
        }
      }
      .width('100%')
      .layoutWeight(1)
      .backgroundColor(Color.Black)
      .hitTestBehavior(HitTestMode.Block)
      .focusable(true)
      .onTouch((event: TouchEvent) => this.handleTouchEvent(event))
      .onMouse((event: MouseEvent) => this.handleMouseEvent(event))
      .onAxisEvent((event: AxisEvent) => this.handleAxisEvent(event))
      .onHover((isHover: boolean, _event: HoverEvent) => {
        // Fires for mouse/touchpad only (touch never triggers hover). When the
        // pointer LEAVES the render area, notify CEF so the page clears its
        // hover state (e.g. hovered links stop highlighting). Skip during an
        // in-progress left-button drag or a system HTML5 drag — the press
        // stream must continue.
        if (!isHover && this.isCreated() && !this.mouseButtonPressed && !this.isDragInProgress) {
          arkui_cef.cefSendMouseMove(this.browserId, this.lastMouseX, this.lastMouseY, true);
        }
      })
      .gesture(
        PinchGesture({ fingers: 2, distance: 0.1 })
          .onActionStart((event: GestureEvent) => {
            if (event.source === SourceType.TouchScreen) {
              // Touchscreen multi-touch is forwarded to CEF natively in
              // handleTouchEvent — the renderer (or the page, e.g. maps)
              // handles the pinch itself. Adding ctrl+wheel here would
              // double-zoom.
              return;
            }
            // Touchpad pinch → ctrl+wheel page zoom (25%-500% with re-layout),
            // like desktop Chrome's touchpad pinch. Touchpad input never
            // enters the touch stream, so this cannot conflict with
            // page-handled multi-touch.
            this.pinchLastScale = event.scale;
            this.pinchLogAccum = 0.0;
          })
          .onActionUpdate((event: GestureEvent) => {
            if (event.source === SourceType.TouchScreen) {
              return;
            }
            if (event.scale <= 0) {
              return;
            }
            // Log-scale accumulation: zoom presets are multiplicative, so log
            // mapping makes pinch-in / pinch-out round trips symmetric.
            this.pinchLogAccum += Math.log(event.scale / this.pinchLastScale);
            this.pinchLastScale = event.scale;
            // Emit ONE ctrl+wheel zoom tick per accumulated ~15% change
            // (Chromium zooms one preset step per wheel event).
            while (Math.abs(this.pinchLogAccum) >= OsrCefView.PINCH_ZOOM_STEP_LOG) {
              let deltaY = this.pinchLogAccum > 0 ? 120 : -120;
              // Touchpad pinch is a mouse-category gesture (single fingerList
              // record per docs); guard against an empty list as insurance.
              let px = event.fingerList.length > 0 ? Math.round(event.fingerList[0].localX) : this.lastMouseX;
              let py = event.fingerList.length > 0 ? Math.round(event.fingerList[0].localY) : this.lastMouseY;
              arkui_cef.cefSendMouseWheel(this.browserId, px, py, 0, deltaY, 0x04);
              let sign = this.pinchLogAccum > 0 ? 1 : -1;
              this.pinchLogAccum -= sign * OsrCefView.PINCH_ZOOM_STEP_LOG;
            }
          })
          .onActionEnd(() => {
            this.pinchLastScale = 1.0;
            this.pinchLogAccum = 0.0;
          })
      )
      .onAreaChange((_oldValue: Area, newValue: Area) => {
        let wpx = Math.round(this.getUIContext().vp2px(newValue.width as number));
        let hpx = Math.round(this.getUIContext().vp2px(newValue.height as number));
        hilog.info(DOMAIN, MODULE, "onAreaChange: wpx=%{public}d, hpx=%{public}d", wpx, hpx);

        // Cache component position and DSF for IME cursor computation
        this.componentGlobalX = newValue.globalPosition.x as number;
        this.componentGlobalY = newValue.globalPosition.y as number;
        this.cachedDsf = this.getUIContext().vp2px(1);

        if (wpx > 0 && hpx > 0) {
          this.viewWidth = wpx;
          this.viewHeight = hpx;

          if (this.isCreated()) {
            // Set DSF to actual device scale factor for correct content sizing
            let dsf = this.getUIContext().vp2px(1);
        this.cachedDsf = dsf;
            arkui_cef.cefOsrSetScale(this.browserId, dsf);
            arkui_cef.cefOsrResize(this.browserId, this.viewWidth, this.viewHeight);
            // Pass component offset (window-relative, from globalPosition) for
            // cursor coordinate conversion. ArkTS adds windowRect.left/top
            // separately to get screen-absolute coordinates for IMF.
            arkui_cef.cefSetComponentOffset(this.browserId,
              Math.round(newValue.globalPosition.x as number),
              Math.round(newValue.globalPosition.y as number));
          }
        }
      })
      .onDragEnter((event: DragEvent) => {
        if (!this.isCreated()) return;
        let x = Math.round(event.getWindowX() - this.componentGlobalX);
        let y = Math.round(event.getWindowY() - this.componentGlobalY);
        // Self-drag: data is available in pendingDrag* (from CEF StartDragging).
        // External drag: getData() not available until onDrop — defer.
        if (this.isDragInProgress && (this.pendingDragText.length > 0 || this.pendingDragUrl.length > 0 ||
            this.pendingDragFilePaths.length > 0)) {
          hilog.info(DOMAIN, MODULE, "onDragEnter: self-drag, calling DragTargetDragEnter with pending data");
          arkui_cef.cefDragTargetDragEnter(this.browserId,
            this.pendingDragText, this.pendingDragHtml, this.pendingDragUrl,
            this.pendingDragFilePaths, x, y, 19);
          this.dragEnterCalled = true;
        } else {
          hilog.info(DOMAIN, MODULE, "onDragEnter: external drag, deferring to onDrop");
        }
        event.setResult(DragResult.DROP_ENABLED);
        event.dragBehavior = DragBehavior.MOVE;
      })
      .onDragMove((event: DragEvent) => {
        if (!this.isCreated()) return;
        let x = Math.round(event.getWindowX() - this.componentGlobalX);
        let y = Math.round(event.getWindowY() - this.componentGlobalY);
        // Track last drag position for DragSourceEndedAt (fix #1).
        this.lastDragMoveX = x;
        this.lastDragMoveY = y;
        if (this.dragEnterCalled) {
          arkui_cef.cefDragTargetDragOver(this.browserId, x, y, 19);
          // Use CEF's UpdateDragCursor feedback to enable/disable drop (fix #4).
          // lastDragOperation reflects CEF's response from the previous DragTargetDragOver.
          // For external drag (dragEnterCalled=false), always enable since CEF hasn't been notified yet.
          if (this.lastDragOperation === 0) {
            event.setResult(DragResult.DROP_DISABLED);
          } else {
            event.setResult(DragResult.DROP_ENABLED);
          }
        } else {
          event.setResult(DragResult.DROP_ENABLED);
        }
        event.dragBehavior = DragBehavior.MOVE;
      })
      .onDragLeave((event: DragEvent) => {
        if (!this.isCreated()) return;
        hilog.info(DOMAIN, MODULE, "onDragLeave: dragEnterCalled=%{public}s",
          this.dragEnterCalled ? "true" : "false");
        if (this.dragEnterCalled) {
          arkui_cef.cefDragTargetDragLeave(this.browserId);
          this.dragEnterCalled = false;
        }
      })
      .onDrop((event: DragEvent) => {
        if (!this.isCreated()) return;
        let x = Math.round(event.getWindowX() - this.componentGlobalX);
        let y = Math.round(event.getWindowY() - this.componentGlobalY);
        // Track final drag position for DragSourceEndedAt (fix #1).
        this.lastDragMoveX = x;
        this.lastDragMoveY = y;

        if (this.dragEnterCalled) {
          // Self-drag: DragTargetDragEnter was already called in onDragEnter.
          // Just call DragOver (final position) + Drop.
          hilog.info(DOMAIN, MODULE, "onDrop: self-drag (enter already called)");
          arkui_cef.cefDragTargetDragOver(this.browserId, x, y, 19);
          arkui_cef.cefDragTargetDrop(this.browserId, x, y);
        } else {
          // External drag: getData() is available now (only in onDrop).
          // Call Enter + Over + Drop in sequence.
          let data = this.extractDragData(event);
          hilog.info(DOMAIN, MODULE, "onDrop: external drag, text_len=%{public}d, url='%{public}s'",
            data.text.length, data.url);
          arkui_cef.cefDragTargetDragEnter(this.browserId, data.text, data.html, data.url, data.filePaths, x, y, 19);
          arkui_cef.cefDragTargetDragOver(this.browserId, x, y, 19);
          arkui_cef.cefDragTargetDrop(this.browserId, x, y);
        }

        // Clear state.
        this.dragEnterCalled = false;
        this.pendingDragText = '';
        this.pendingDragHtml = '';
        this.pendingDragUrl = '';
        this.pendingDragFilePaths = '';
        event.setResult(DragResult.DRAG_SUCCESSFUL);
        event.dragBehavior = DragBehavior.MOVE;
      })
      .allowDrop([
        uniformTypeDescriptor.UniformDataType.PLAIN_TEXT,
        uniformTypeDescriptor.UniformDataType.HTML,
        uniformTypeDescriptor.UniformDataType.HYPERLINK,
        uniformTypeDescriptor.UniformDataType.FILE,
        uniformTypeDescriptor.UniformDataType.IMAGE
      ])

      // Status bar — shows OSR state info
      if (this.isCreated()) {
        Row() {
          Text(`Scroll: ${this.scrollOffsetText || '0,0'}`)
            .fontSize(9)
            .fontColor('#999999')
            .layoutWeight(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })

          Text(`Cursor: ${this.cursorTypeText}`)
            .fontSize(9)
            .fontColor('#66AA66')
        }
        .width('100%')
        .height(16)
        .padding({ left: 8, right: 8 })
        .backgroundColor('#F5F5F5')
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
    .focusable(true)
    .onVisibleAreaChange([0.0, 0.01], (isVisible: boolean, currentRatio: number) => {
      // Pause/resume CEF rendering on full (in)visibility — tab switches and
      // scrolling off-screen. Same pattern as QCefView's QEvent::Show/Hide
      // → WasHidden().
      this.setOsrHidden(!isVisible || currentRatio === 0);
      if (!isVisible && currentRatio === 0) {
        if (this.imeAttached) {
          hilog.info(DOMAIN, MODULE, "onVisibleAreaChange: invisible, detaching IME");
          this.imeDetachedByVisibility = true;
          this.detachInputMethod();
        }
      } else if (isVisible && this.imeDetachedByVisibility) {
        hilog.info(DOMAIN, MODULE, "onVisibleAreaChange: visible again, kbVisible=%{public}s",
          this.keyboardVisible ? "true" : "false");
        this.imeDetachedByVisibility = false;
        if (this.keyboardVisible) {
          this.attachInputMethod();
        }
      }
    })
  }

  getBrowserId(): number {
    return this.browserId;
  }

  isCreated(): boolean {
    return this.browserId > 0;
  }

  // Pause/resume the CEF render pipeline (WasHidden). Called from visibility
  // transitions only — the osrHidden guard makes repeated events idempotent.
  private setOsrHidden(hidden: boolean): void {
    if (!this.isCreated() || this.osrHidden === hidden) {
      return;
    }
    this.osrHidden = hidden;
    hilog.info(DOMAIN, MODULE, "setOsrHidden: %{public}s", hidden ? "true" : "false");
    arkui_cef.cefWasHidden(this.browserId, hidden);
  }

  destroy(): void {
    this.detachInputMethod();
    if (this.isCreated()) {
      hilog.info(DOMAIN, MODULE, "Destroying OSR browser, id: %{public}d", this.browserId);
      // Release native pixelmap handles BEFORE the ArkTS PixelMaps below —
      // the handles must never outlive the memory they point to.
      arkui_cef.unregisterOsrPixelmaps(this.browserId);
      arkui_cef.destroyOsrCefView(this.browserId);
      this.browserId = -1;
    }
    // this.pixelMap aliases pmA/pmB — release the owners only.
    if (this.pmA && this.pmA !== this.pmB) {
      this.pmA.release();
    }
    if (this.pmB) {
      this.pmB.release();
    }
    this.pmA = undefined;
    this.pmB = undefined;
    this.pixelMap = undefined;
    this.showA = true;
    this.pmWidth = 0;
    this.pmHeight = 0;
    this.pendingFullFrame = undefined;
    this.pixelMapCreating = false;
  }

  goBack(): void {
    if (this.isCreated()) {
      arkui_cef.cefGoBack(this.browserId);
    }
  }

  goForward(): void {
    if (this.isCreated()) {
      arkui_cef.cefGoForward(this.browserId);
    }
  }

  reload(): void {
    if (this.isCreated()) {
      arkui_cef.cefReload(this.browserId);
    }
  }


  loadUrl(url: string): void {
    if (!this.isCreated()) {
      hilog.info(DOMAIN, MODULE, "loadUrl: browser not created yet");
      return;
    }
    if (url.length === 0) {
      return;
    }
    // Add https:// prefix if missing
    if (!url.startsWith('http://') && !url.startsWith('https://') && !url.startsWith('file://')) {
      url = 'https://' + url;
      this.urlInput = url;
    }
    hilog.info(DOMAIN, MODULE, "loadUrl: %{public}s", url);
    arkui_cef.cefLoadUrl(this.browserId, url);
  }

  sleep(delay: number): Promise<void> {
    return new Promise<void>((resolve) => setTimeout(resolve, delay));
  }

  async createOsrCefView(url: string, width: number, height: number): Promise<void> {
    if (this.cefCreating || this.isCreated()) {
      hilog.info(DOMAIN, MODULE, "OSR is creating or created. id: %{public}d", this.browserId);
      return;
    }

    while (!arkui_cef.isCefRunning()) {
      await this.sleep(20);
    }

    hilog.info(DOMAIN, MODULE, "Begin to create OSR cef view, url: %{public}s, size: %{public}d X %{public}d",
      url, width, height);
    this.cefCreating = true;

    let id = arkui_cef.createOsrCefView(url, width, height,
      (regionData: ArrayBuffer | undefined, x: number, y: number, w: number, h: number,
        fullFrame: boolean, viewW: number, viewH: number) => {
        this.onOsrPaint(regionData, x, y, w, h, fullFrame, viewW, viewH);
      },
      (id: number) => { this.onOsrCreated(id); },
      (title: string) => { this.onOsrTitleChanged(title); },
      (next: boolean) => { this.onOsrFocusTake(next); },
      (offset: string) => { this.onOsrScrollOffset(offset); },
      (show: boolean) => { this.onOsrVirtualKeyboard(show); },
      (text: string) => { this.onOsrTextSelection(text); },
      (cursorType: number) => { this.onOsrCursorChange(cursorType); },
      (left: number, top: number, w: number, h: number) => { this.onOsrImeCursor(left, top, w, h); },
      (text: string, html: string, durl: string, urlTitle: string, fileName: string, fileData: ArrayBuffer, x: number, y: number) => {
        this.onOsrStartDrag(text, html, durl, urlTitle, fileName, fileData, x, y);
      },
      (operation: number) => { this.onOsrUpdateDragCursor(operation); });

    hilog.info(DOMAIN, MODULE, "OSR CEF starts to create, id: %{public}d", id);
  }

  onOsrCreated(id: number): void {
    hilog.info(DOMAIN, MODULE, "OSR CEF created, browserId=%{public}d", id);
    this.browserId = id;
    this.cefCreating = false;
    // Set DSF to actual device scale factor so CEF GetViewRect returns CSS pixels.
    // This makes web content layout match non-OSR tabs (content not too small).
    // OnPaint buffer is still physical pixels (CSS * DSF).
    // Mouse/touch coordinates are sent in CSS pixels (vp) to match GetViewRect.
    let dsf = this.getUIContext().vp2px(1);
    this.cachedDsf = dsf;
    arkui_cef.cefOsrSetScale(this.browserId, dsf);
    arkui_cef.cefOsrResize(this.browserId, this.viewWidth, this.viewHeight);
    // Apply the requested frame rate (creation default is 60; this allows
    // callers to lower it via the osrFrameRate prop). SetWindowlessFrameRate
    // takes effect on the CEF UI thread; static pages emit no extra frames
    // regardless of the cap, so the value only affects pages with dynamic
    // content.
    if (this.osrFrameRate >= 1) {
      arkui_cef.cefSetFrameRate(this.browserId, this.osrFrameRate);
    }
    // Register window-level key event filter for non-text keys (digits, symbols, etc.)
    this.registerKeyFilter();
  }

  private async registerKeyFilter(): Promise<void> {
    try {
      let context: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      let win: window.Window = await window.getLastWindow(context);
      this.cachedWin = win;
      let windowId: number = win.getWindowProperties().id;
      this.cachedWindowId = windowId;
      hilog.info(DOMAIN, MODULE, "registerKeyFilter: windowId=%{public}d, browserId=%{public}d",
        windowId, this.browserId);
      arkui_cef.registerOsrKeyFilter(windowId, this.browserId);

      win.on('windowEvent', (event: window.WindowEventType) => {
        hilog.info(DOMAIN, MODULE, "windowEvent: %{public}d", event);
        if (event === window.WindowEventType.WINDOW_INACTIVE) {
          this.focusLostFlag = true;
          this.detachInputMethod();
          if (this.isCreated() && this.renderAreaActive) {
            arkui_cef.cefSendFocusEvent(this.browserId, false);
          }
          // Deliberately NOT pausing rendering here: WINDOW_INACTIVE is a
          // focus event, not a visibility event. On PC form factors an
          // unfocused window can still be fully visible (and must keep
          // rendering and accepting wheel/touchpad input) — mirrors
          // cefclient, where focus and WasHidden are decoupled. Only
          // component-level invisibility (onVisibleAreaChange, e.g. tab
          // switches) pauses the render pipeline.
        } else if (event === window.WindowEventType.WINDOW_ACTIVE) {
          if (this.focusLostFlag) {
            this.focusLostFlag = false;
            hilog.info(DOMAIN, MODULE, "WINDOW_ACTIVE: restoring focus, kbVisible=%{public}s",
              this.keyboardVisible ? "true" : "false");
            if (this.isCreated() && this.renderAreaActive) {
              arkui_cef.cefSendFocusEvent(this.browserId, true);
            }
            if (this.keyboardVisible) {
              this.attachInputMethod();
            }
          }
        }
      });

      // Refresh IME cursor when window position or size changes (e.g.,
      // floating window drag). Using C API (synchronous NotifyCursorUpdate),
      // no async second OnCursorUpdate issue. API 12+.
      win.on('windowRectChange', (data: window.RectChangeOptions) => {
        if (this.imeAttached) {
          hilog.info(DOMAIN, MODULE, "windowRectChange: left=%{public}d top=%{public}d reason=%{public}d",
            data.rect.left, data.rect.top, data.reason);
          this.refreshImeCursor();
        }
      });
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "registerKeyFilter failed: %{public}s", (e as Error).message);
    }
  }

  onOsrPaint(regionData: ArrayBuffer | undefined, x: number, y: number, w: number, h: number,
             fullFrame: boolean, viewW: number, viewH: number): void {
    if (fullFrame) {
      // First frame or resize: the PixelMap must be (re)created at the new
      // size. Low-frequency path. Always carries pixel data.
      if (regionData === undefined) {
        return;
      }
      this.rebuildPixelMap(regionData, viewW, viewH);
      return;
    }

    if (this.pmWidth !== viewW || this.pmHeight !== viewH) {
      // Incremental frame at a new view size while the rebuild is still in
      // flight (or the PixelMap is not ready yet). The region payload is NOT
      // a full frame, so it can't be used to rebuild — drop it and ask CEF
      // for a full repaint. The next frame arrives as fullFrame and rebuilds
      // with complete content (one frame of latency at most).
      if (this.isCreated()) {
        arkui_cef.cefInvalidate(this.browserId);
      }
      return;
    }

    let pmFront = this.showA ? this.pmA : this.pmB;
    let pmBack = this.showA ? this.pmB : this.pmA;
    if (!pmFront || !pmBack || w <= 0 || h <= 0) {
      return;
    }

    if (regionData === undefined) {
      // Native fast path: the NAPI layer already wrote the dirty region into
      // BOTH persistent PixelMaps (they stay identical) — just swap the
      // displayed reference. The Image component reloads and keeps showing
      // the old (identical) image until the new src is ready, so there is no
      // black flash window.
      this.pixelMap = pmBack;
      this.showA = !this.showA;
      return;
    }

    // Fallback path: pixel data arrived via ArrayBuffer (native write path
    // unavailable or failed) — write it into both buffers ourselves.
    // regionData holds compact BGRA rows (w * 4 bytes per row),
    // so stride = w * 4 and offset = 0.
    try {
      let area: image.PositionArea = {
        pixels: regionData,
        offset: 0,
        stride: w * 4,
        region: { size: { width: w, height: h }, x: x, y: y }
      };
      pmFront.writePixelsSync(area);
      pmBack.writePixelsSync(area);
      // Swap the object reference — the Image component reloads and keeps
      // showing the old (identical) image until the new src is ready, so
      // there is no black flash window.
      this.pixelMap = pmBack;
      this.showA = !this.showA;
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "writePixelsSync failed: %{public}s", (e as Error).message);
    }
  }

  private rebuildPixelMap(frameData: ArrayBuffer, viewW: number, viewH: number): void {
    if (this.pixelMapCreating) {
      // A rebuild is already in flight — keep only the latest payload; it
      // will be applied when the current creation completes.
      this.pendingFullFrame = frameData;
      this.pendingFullFrameW = viewW;
      this.pendingFullFrameH = viewH;
      return;
    }
    this.pixelMapCreating = true;
    try {
      // CEF outputs BGRA; createPixelMap defaults srcPixelFormat to
      // BGRA_8888 when unspecified, matching directly. editable must be
      // true — the incremental path writes dirty regions into the
      // persistent buffers via writePixelsSync, which fails on
      // non-editable PixelMaps (documented behavior).
      let opts: image.InitializationOptions = {
        size: { width: viewW, height: viewH },
        pixelFormat: image.PixelMapFormat.BGRA_8888,
        editable: true,
      };
      image.createPixelMap(frameData, opts).then((pm: image.PixelMap) => {
        // Build the second buffer from the same full-frame payload so both
        // buffers start with identical content.
        image.createPixelMap(frameData, opts).then((pm2: image.PixelMap) => {
          let oldA = this.pmA;
          let oldB = this.pmB;
          this.pmA = pm;
          this.pmB = pm2;
          this.showA = true;
          this.pixelMap = this.pmA;
          this.pmWidth = viewW;
          this.pmHeight = viewH;
          // Register the new pair for native dirty-region writes BEFORE
          // retiring the old ArkTS PixelMaps — the NAPI registry must never
          // hold handles into released memory.
          if (this.isCreated()) {
            arkui_cef.registerOsrPixelmaps(this.browserId, pm, pm2);
          }
          if (oldA && oldA !== oldB) {
            this.retirePixelMap(oldA);
          }
          if (oldB) {
            this.retirePixelMap(oldB);
          }
          this.pixelMapCreating = false;
          // Diagnostic: compare the PixelMap size against the render area
          // size (viewWidth/viewHeight track onAreaChange). A persistent
          // mismatch means the viewport is systematically off — the bottom
          // of the page would look cut off.
          hilog.info(DOMAIN, MODULE,
            "rebuildPixelMap: pm=%{public}dx%{public}d, area=%{public}dx%{public}d",
            viewW, viewH, this.viewWidth, this.viewHeight);

          // Apply a full-frame payload that arrived while we were building.
          if (this.pendingFullFrame !== undefined) {
            let data = this.pendingFullFrame;
            let pw = this.pendingFullFrameW;
            let ph = this.pendingFullFrameH;
            this.pendingFullFrame = undefined;
            this.rebuildPixelMap(data, pw, ph);
          }
        }).catch((err: Error) => {
          hilog.error(DOMAIN, MODULE, "createPixelMap(2nd) failed: %{public}s", err.message);
          this.finishRebuildFailed(pm, viewW, viewH);
        });
      }).catch((err: Error) => {
        hilog.error(DOMAIN, MODULE, "createPixelMap failed: %{public}s", err.message);
        this.finishRebuildFailed(undefined, viewW, viewH);
      });
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "rebuildPixelMap error: %{public}s", (e as Error).message);
      this.finishRebuildFailed(undefined, viewW, viewH);
    }
  }

  private retirePixelMap(pm: image.PixelMap): void {
    // Deferred release. After swapping the Image src, the component keeps
    // rendering the OLD image while the new one loads — that old image is
    // this just-replaced PixelMap. Releasing it immediately makes that
    // "keep showing old image" path occasionally read a released object,
    // which shows as a black frame during rapid resizes. 200ms comfortably
    // covers the reload, and the memory is freed shortly after.
    setTimeout(() => {
      pm.release();
    }, 200);
  }

  private finishRebuildFailed(pm: image.PixelMap | undefined, viewW: number, viewH: number): void {
    // Rebuild failed: fall back to a single-buffer mode if we at least got
    // the first PixelMap (the double-buffer path requires both; with only
    // one buffer the incremental path can't swap references, so keep the
    // single PixelMap displayed statically and request full frames).
    if (pm) {
      let oldA = this.pmA;
      let oldB = this.pmB;
      this.pmA = pm;
      this.pmB = pm;
      this.showA = true;
      this.pixelMap = pm;
      this.pmWidth = viewW;
      this.pmHeight = viewH;
      if (oldA && oldA !== pm) {
        this.retirePixelMap(oldA);
      }
      if (oldB && oldB !== pm) {
        this.retirePixelMap(oldB);
      }
    }
    this.pixelMapCreating = false;
    this.pendingFullFrame = undefined;
  }

  onOsrTitleChanged(title: string): void {
    hilog.info(DOMAIN, MODULE, "OSR CEF title changed: %{public}s", title);
    this.pageTitle = title;
  }

  onOsrFocusTake(next: boolean): void {
    hilog.info(DOMAIN, MODULE, "OSR CEF focus taken, next=%{public}s", next ? "true" : "false");
  }

  onOsrScrollOffset(offset: string): void {
    hilog.info(DOMAIN, MODULE, "OSR scroll offset: %{public}s", offset);
    this.scrollOffsetText = offset;
  }

  onOsrVirtualKeyboard(show: boolean): void {
    hilog.info(DOMAIN, MODULE, "OSR virtual keyboard: %{public}s", show ? "show" : "hide");
    this.keyboardVisible = show;
    if (show) {
      if (!this.imeAttached) {
        this.attachInputMethod();
      }
    } else {
      this.detachInputMethod();
    }
  }

  onOsrTextSelection(text: string): void {
    hilog.info(DOMAIN, MODULE, "OSR text selection: len=%{public}d", text.length);
    this.selectedText = text;
  }

  onOsrCursorChange(cursorType: number): void {
    // CEF cef_cursor_type_t enum (sequential from 0):
    // CT_POINTER=0 CT_CROSS=1 CT_HAND=2 CT_IBEAM=3 CT_WAIT=4 CT_HELP=5
    // CT_EASTRESIZE=6 CT_NORTHRESIZE=7 CT_NORTHEASTRESIZE=8 CT_NORTHWESTRESIZE=9
    // CT_SOUTHRESIZE=10 CT_SOUTHEASTRESIZE=11 CT_SOUTHWESTRESIZE=12 CT_WESTRESIZE=13
    // CT_NORTHSOUTHRESIZE=14 CT_EASTWESTRESIZE=15
    // CT_NORTHEASTSOUTHWESTRESIZE=16 CT_NORTHWESTSOUTHEASTRESIZE=17
    // CT_COLUMNRESIZE=18 CT_ROWRESIZE=19
    // CT_MOVE=29 CT_VERTICALTEXT=30 CT_CELL=31 CT_CONTEXTMENU=32
    // CT_COPY=36 CT_NONE=37 CT_NOTALLOWED=38 CT_ZOOMIN=39 CT_ZOOMOUT=40
    // CT_GRAB=41 CT_GRABBING=42
    let pStyle: pointer.PointerStyle = pointer.PointerStyle.DEFAULT;
    let label: string = 'default';
    switch (cursorType) {
      case 0:  pStyle = pointer.PointerStyle.DEFAULT; label = 'pointer'; break;
      case 1:  pStyle = pointer.PointerStyle.CROSS; label = 'cross'; break;
      case 2:  pStyle = pointer.PointerStyle.HAND_POINTING; label = 'hand'; break;
      case 3:  pStyle = pointer.PointerStyle.TEXT_CURSOR; label = 'text'; break;
      case 4:  pStyle = pointer.PointerStyle.LOADING; label = 'wait'; break;
      case 5:  pStyle = pointer.PointerStyle.HELP; label = 'help'; break;
      case 6:  pStyle = pointer.PointerStyle.EAST; label = 'e-resize'; break;
      case 7:  pStyle = pointer.PointerStyle.NORTH; label = 'n-resize'; break;
      case 8:  pStyle = pointer.PointerStyle.NORTH_EAST_SOUTH_WEST; label = 'ne-resize'; break;
      case 9:  pStyle = pointer.PointerStyle.NORTH_WEST_SOUTH_EAST; label = 'nw-resize'; break;
      case 10: pStyle = pointer.PointerStyle.SOUTH; label = 's-resize'; break;
      case 11: pStyle = pointer.PointerStyle.NORTH_WEST_SOUTH_EAST; label = 'se-resize'; break;
      case 12: pStyle = pointer.PointerStyle.NORTH_EAST_SOUTH_WEST; label = 'sw-resize'; break;
      case 13: pStyle = pointer.PointerStyle.WEST; label = 'w-resize'; break;
      case 14: pStyle = pointer.PointerStyle.NORTH_SOUTH; label = 'ns-resize'; break;
      case 15: pStyle = pointer.PointerStyle.WEST_EAST; label = 'ew-resize'; break;
      case 16: pStyle = pointer.PointerStyle.NORTH_EAST_SOUTH_WEST; label = 'nesw-resize'; break;
      case 17: pStyle = pointer.PointerStyle.NORTH_WEST_SOUTH_EAST; label = 'nwse-resize'; break;
      case 18: pStyle = pointer.PointerStyle.WEST_EAST; label = 'col-resize'; break;
      case 19: pStyle = pointer.PointerStyle.NORTH_SOUTH; label = 'row-resize'; break;
      case 29: pStyle = pointer.PointerStyle.MOVE; label = 'move'; break;
      case 30: pStyle = pointer.PointerStyle.HORIZONTAL_TEXT_CURSOR; label = 'vtext'; break;
      case 31: pStyle = pointer.PointerStyle.CURSOR_CROSS; label = 'cell'; break;
      case 32: pStyle = pointer.PointerStyle.HELP; label = 'context*'; break;
      case 36: pStyle = pointer.PointerStyle.CURSOR_COPY; label = 'copy'; break;
      case 37: pStyle = pointer.PointerStyle.DEFAULT; label = 'none*'; break;
      case 38: pStyle = pointer.PointerStyle.CURSOR_FORBID; label = 'forbid'; break;
      case 39: pStyle = pointer.PointerStyle.ZOOM_IN; label = 'zoomin'; break;
      case 40: pStyle = pointer.PointerStyle.ZOOM_OUT; label = 'zoomout'; break;
      case 41: pStyle = pointer.PointerStyle.HAND_OPEN; label = 'grab'; break;
      case 42: pStyle = pointer.PointerStyle.HAND_GRABBING; label = 'grabbing'; break;
      default: pStyle = pointer.PointerStyle.DEFAULT; label = 'type=' + cursorType; break;
    }
    this.cursorTypeText = label;
    hilog.info(DOMAIN, MODULE, "OSR cursor change: type=%{public}d -> %{public}s", cursorType, label);

    try {
      let cursorController = this.getUIContext().getCursorController();
      if (cursorType === 0) {
        cursorController.restoreDefault();
      } else {
        cursorController.setCursor(pStyle);
      }
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "setCursor failed: %{public}s", (e as Error).message);
    }
  }

  onOsrImeCursor(left: number, top: number, width: number, height: number): void {
    this.lastCaretWinLeft = left;
    this.lastCaretWinTop = top;
    this.lastCaretWidth = width;
    this.lastCaretHeight = height;
    this.hasCaretBounds = true;

    // Screen-absolute px = window-relative px + windowRect.left/top.
    // C API OH_InputMethodProxy_NotifyCursorUpdate expects screen-absolute.
    let winRect = this.getWindowRect();
    let screenLeft = left + winRect.left;
    let screenTop = top + winRect.top;
    hilog.info(DOMAIN, MODULE,
      "onOsrImeCursor: winRel=%{public}d,%{public}d winRect=%{public}d,%{public}d screen=%{public}d,%{public}d",
      left, top, winRect.left, winRect.top, screenLeft, screenTop);

    if (!this.imeAttached) {
      hilog.info(DOMAIN, MODULE, "onOsrImeCursor: skipped (IME not attached)");
      return;
    }

    arkui_cef.cefImeCUpdateCursor(screenLeft, screenTop, width, height);
  }

  private getWindowRect(): window.Rect {
    if (this.cachedWin) {
      try {
        return this.cachedWin.getWindowProperties().windowRect;
      } catch (e) {
        hilog.error(DOMAIN, MODULE, "getWindowRect failed: %{public}s", (e as Error).message);
      }
    }
    let defaultRect: window.Rect = { left: 0, top: 0, width: 0, height: 0 };
    return defaultRect;
  }

  private applyImeCursor(left: number, top: number, width: number, height: number): void {
    let ok = arkui_cef.cefImeCUpdateCursor(left, top, width, height);
    hilog.info(DOMAIN, MODULE, "applyImeCursor: left=%{public}d top=%{public}d ok=%{public}s",
      left, top, ok ? "true" : "false");
  }

  // ==================== Drag & Drop ====================

  onOsrStartDrag(text: string, html: string, durl: string, urlTitle: string,
                  fileName: string, fileData: ArrayBuffer, x: number, y: number): void {
    hilog.info(DOMAIN, MODULE,
      "onOsrStartDrag: text='%{public}s', url='%{public}s', fileName='%{public}s', fileDataSize=%{public}d, x=%{public}d, y=%{public}d",
      text.substring(0, 50), durl, fileName, fileData.byteLength, x, y);
    if (this.isDragInProgress) {
      hilog.info(DOMAIN, MODULE, "onOsrStartDrag: drag already in progress, skipping");
      return;
    }
    this.isDragInProgress = true;
    // Reset per-drag state: stale lastDragOperation from the previous drag
    // would corrupt the first DROP_ENABLED/DROP_DISABLED decision (#5).
    this.lastDragOperation = 0;

    // Cache full drag data for self-drag target (onDragEnter uses this to call
    // cefDragTargetDragEnter immediately, and onDrop uses it for the final drop).
    this.pendingDragText = text;
    this.pendingDragHtml = html;
    this.pendingDragUrl = durl;
    this.pendingDragFilePaths = '';
    // Initialize last drag position to start position as fallback.
    this.lastDragMoveX = x;
    this.lastDragMoveY = y;

    // Build UnifiedData with ALL non-empty records, matching CEF's non-OSR
    // adapter behavior (HandlePlainTextRecord/HandleUdsHtmlRecord/etc. each
    // add their data type conditionally). The number badge is suppressed via
    // previewOptions.numberBadge = false so multiple records don't show "+N".
    // Records carry exactly what the web page declared (FragmentText/Html/Url) —
    // data fidelity is preserved; the test page's dragstart sets text/plain
    // for links to match Chromium's default link-drag behavior.
    let unifiedData = new unifiedDataChannel.UnifiedData();
    if (text.length > 0) {
      let plainText = new unifiedDataChannel.PlainText();
      plainText.textContent = text;
      unifiedData.addRecord(plainText);
    }
    if (html.length > 0) {
      let htmlRecord = new unifiedDataChannel.HTML();
      htmlRecord.htmlContent = html;
      if (text.length > 0) {
        htmlRecord.plainContent = text;
      }
      unifiedData.addRecord(htmlRecord);
    }
    if (durl.length > 0) {
      let link = new unifiedDataChannel.Hyperlink();
      link.url = durl;
      link.description = urlTitle.length > 0 ? urlTitle : durl;
      unifiedData.addRecord(link);
    }

    // If CEF provided file contents (e.g. image dragged from <img>),
    // write to a temp file and add a File/Image UDMF record so external
    // apps (file manager, image viewer) can receive the drop.
    if (fileName.length > 0 && fileData.byteLength > 0) {
      try {
        let context = this.getUIContext().getHostContext() as common.UIAbilityContext;
        let cacheDir = context.cacheDir;
        // Sanitize fileName: strip path separators to prevent path traversal
        // (fileName originates from web content and is not trustworthy).
        let safeName = fileName.replace(/[/\\]/g, '_');
        // Use a dedicated dragdrop sub-directory, cleared before each drag-out
        // (mirrors the non-OSR adapter's DeletePathRecursively(dragdrop) behavior).
        let dragDir = `${cacheDir}/dragdrop`;
        if (fileIo.accessSync(dragDir)) {
          fileIo.rmdirSync(dragDir);
        }
        fileIo.mkdirSync(dragDir);
        let tempPath = `${dragDir}/${safeName}`;
        let file = fileIo.openSync(tempPath, fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC);
        fileIo.writeSync(file.fd, fileData);
        fileIo.closeSync(file);
        let fileUriStr = fileUri.getUriFromPath(tempPath);
        hilog.info(DOMAIN, MODULE, "onOsrStartDrag: wrote temp file %{public}s, uri=%{public}s", tempPath, fileUriStr);

        // Cache file path for self-drag drop-back (cefDragTargetDragEnter → AddFile)
        this.pendingDragFilePaths = tempPath;

        // Determine if it's an image by extension
        let isImage = /\.(png|jpg|jpeg|gif|bmp|webp|svg)$/i.test(safeName);
        if (isImage) {
          let image = new unifiedDataChannel.Image();
          image.imageUri = fileUriStr;
          unifiedData.addRecord(image);
        } else {
          let fileRecord = new unifiedDataChannel.File();
          fileRecord.uri = fileUriStr;
          unifiedData.addRecord(fileRecord);
        }
      } catch (e) {
        hilog.error(DOMAIN, MODULE, "onOsrStartDrag: write temp file failed: %{public}s", (e as Error).message);
      }
    }

    let previewOptions: DragPreviewOptions = { numberBadge: false };
    let dragInfo: dragController.DragInfo = {
      pointerId: 0,
      data: unifiedData,
      previewOptions: previewOptions
    };

    // Set drag preview text for the @Builder (match actual dragged content)
    this.dragPreviewText = fileName.length > 0 ? fileName.substring(0, 30) :
      (text.length > 0 ? text.substring(0, 30) :
        (durl.length > 0 ? durl.substring(0, 30) : 'Drag'));

    try {
      this.getUIContext().getDragController().executeDrag(
        () => { this.dragPreviewBuilder(); },
        dragInfo,
        (err, dragEventParam) => {
          this.isDragInProgress = false;
          this.pendingDragText = '';
          this.pendingDragHtml = '';
          this.pendingDragUrl = '';
          this.pendingDragFilePaths = '';
          this.dragEnterCalled = false;
          // The release that ended the drag is consumed by the drag system —
          // CEF never receives a matching mouse-up. Reset the button-state
          // flag so subsequent plain moves don't carry
          // EVENTFLAG_LEFT_MOUSE_BUTTON and trigger spurious drags.
          this.mouseButtonPressed = false;
          if (err) {
            hilog.error(DOMAIN, MODULE, "executeDrag error: %{public}s", err.message ?? '');
            arkui_cef.cefDragSourceEndedAt(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0);
            arkui_cef.cefDragSourceSystemDragEnded(this.browserId);
            // Synthetic mouse-up: the physical release was consumed by the drag
            // system, so CEF never saw it — without this the renderer keeps the
            // mouse-pressed state (CSS :active stays set, element stays dimmed).
            arkui_cef.cefSendMouseClick(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0, true, 1, 0);
            return;
          }
          let result = DragResult.DRAG_FAILED;
          let behavior = DragBehavior.COPY;
          if (dragEventParam && dragEventParam.event) {
            result = dragEventParam.event.getResult();
            behavior = dragEventParam.event.dragBehavior;
          }
          let cefOp = 0;
          if (result === DragResult.DRAG_SUCCESSFUL) {
            if (behavior === DragBehavior.MOVE) {
              cefOp = 16;
            } else {
              cefOp = 1;
            }
          }
          hilog.info(DOMAIN, MODULE,
            "executeDrag done: result=%{public}d, behavior=%{public}d, cefOp=%{public}d, pos=%{public}d,%{public}d",
            result, behavior, cefOp, this.lastDragMoveX, this.lastDragMoveY);
          arkui_cef.cefDragSourceEndedAt(this.browserId, this.lastDragMoveX, this.lastDragMoveY, cefOp);
          arkui_cef.cefDragSourceSystemDragEnded(this.browserId);
          // Synthetic mouse-up (see comment in err path above). Blink suppresses
          // click-after-drag, so this won't fire a click on the source element.
          arkui_cef.cefSendMouseClick(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0, true, 1, 0);
        }
      );
    } catch (e) {
      this.isDragInProgress = false;
      this.pendingDragText = '';
      this.pendingDragHtml = '';
      this.pendingDragUrl = '';
      this.pendingDragFilePaths = '';
      this.mouseButtonPressed = false;
      hilog.error(DOMAIN, MODULE, "executeDrag exception: %{public}s", (e as Error).message);
      arkui_cef.cefDragSourceEndedAt(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0);
      arkui_cef.cefDragSourceSystemDragEnded(this.browserId);
      arkui_cef.cefSendMouseClick(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0, true, 1, 0);
    }
  }

  onOsrUpdateDragCursor(operation: number): void {
    this.lastDragOperation = operation;
    hilog.info(DOMAIN, MODULE, "onOsrUpdateDragCursor: operation=%{public}d", operation);
  }

  private extractDragData(event: DragEvent): DragDataInfo {
    let text = '';
    let html = '';
    let url = '';
    let filePaths = '';
    try {
      let data = event.getData();
      if (data) {
        let records = data.getRecords();
        for (let i = 0; i < records.length; i++) {
          let record = records[i];
          let rtype = record.getType();
          if (rtype === uniformTypeDescriptor.UniformDataType.PLAIN_TEXT) {
            let pt = record as unifiedDataChannel.PlainText;
            text = pt.textContent;
          } else if (rtype === uniformTypeDescriptor.UniformDataType.HTML) {
            let hr = record as unifiedDataChannel.HTML;
            html = hr.htmlContent;
            if (text.length === 0 && hr.plainContent) {
              text = hr.plainContent;
            }
          } else if (rtype === uniformTypeDescriptor.UniformDataType.HYPERLINK) {
            let hl = record as unifiedDataChannel.Hyperlink;
            url = hl.url;
          } else if (rtype === uniformTypeDescriptor.UniformDataType.FILE) {
            let file = record as unifiedDataChannel.File;
            if (file.uri) {
              let fu = new fileUri.FileUri(file.uri);
              if (filePaths.length > 0) filePaths += '\n';
              filePaths += fu.path;
            }
          } else if (rtype === uniformTypeDescriptor.UniformDataType.IMAGE) {
            let img = record as unifiedDataChannel.Image;
            if (img.imageUri) {
              let fu = new fileUri.FileUri(img.imageUri);
              if (filePaths.length > 0) filePaths += '\n';
              filePaths += fu.path;
            }
          }
        }
      }
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "extractDragData error: %{public}s", (e as Error).message);
    }
    hilog.info(DOMAIN, MODULE, "extractDragData: text_len=%{public}d, html_len=%{public}d, url='%{public}s', filePaths_len=%{public}d",
      text.length, html.length, url, filePaths.length);
    let result: DragDataInfo = { text: text, html: html, url: url, filePaths: filePaths };
    return result;
  }

  @Builder
  dragPreviewBuilder() {
    Text(this.dragPreviewText)
      .fontSize(12)
      .fontColor('#333333')
      .padding({ left: 8, right: 8, top: 4, bottom: 4 })
      .backgroundColor('#f0f0f0')
      .border({ width: 1, color: '#cccccc', radius: 4 })
      .maxLines(1)
      .textOverflow({ overflow: TextOverflow.Ellipsis })
      .constraintSize({ maxWidth: 200 })
  }

  // ==================== Touch Event (触摸屏) ====================
  handleTouchEvent(event: TouchEvent): void {
    if (!this.isCreated()) {
      return;
    }

    event.stopPropagation();

    // ArkUI fires onTouch for ALL pointer input (mouse, touchpad, touchscreen).
    // Routing (verified from docs):
    // - Touchscreen: SourceType.TouchScreen — forwarded to CEF as touch events.
    // - Touchpad: single-finger acts as mouse (SourceType.Mouse), two-finger
    //   swipe/pinch arrive via onAxis/PinchGesture — never as touch events here.
    // - A mouse left-press is ALSO converted to a touch event (SourceType.Mouse)
    //   and must be filtered out, otherwise CEF enables touch scrolling and the
    //   page scrolls when the user clicks and drags with a mouse.
    if (event.source === SourceType.Mouse) {
      return;
    }

    // CEF touch event types: TOUCHEVENT_RELEASED=0, TOUCHEVENT_PRESSED=1, TOUCHEVENT_MOVED=2, TOUCHEVENT_CANCELLED=3
    let touchType = 2;
    switch (event.type) {
      case TouchType.Down: touchType = 1; break;
      case TouchType.Up: touchType = 0; break;
      case TouchType.Move: touchType = 2; break;
      case TouchType.Cancel: touchType = 3; break;
      default: break;
    }

    // Forward touch events to CEF — ALL fingers, native semantics. Pages that
    // handle multi-touch themselves (maps) receive their pinch directly, like
    // desktop Chrome with a touchscreen. NO multi-touch takeover here: the
    // previous takeover (cancel touches + ArkTS pinch → ctrl+wheel) corrupted
    // the touch state on pages like maps (input dead until reload) — the
    // synthetic touchcancel broke Chromium's active-touch tracking.
    // Down/Up/Cancel are per-point events: forward only the points that
    // triggered this event. Cancel is per-point (e.g. a stylus interrupt
    // cancels only the finger's stream). Move: send all active touches.
    let touchList: TouchObject[] = event.touches;
    if (event.type === TouchType.Down || event.type === TouchType.Up ||
        event.type === TouchType.Cancel) {
      touchList = event.changedTouches;
    }
    for (let i = 0; i < touchList.length; i++) {
      let touch = touchList[i];
      arkui_cef.cefSendTouchEvent(this.browserId,
        Math.round(touch.x), Math.round(touch.y), touchType, touch.id);
    }

    // Touch input is forwarded as touch events ONLY (loop above) — native
    // Chromium touch semantics apply: tap = click (synthesized by the touch
    // gesture pipeline), long-press = word selection, drag = scroll.
    // Do NOT simulate mouse events for touchscreen input: a synthesized mouse
    // press-drag is treated by the renderer as an immediate text-selection
    // drag, overriding the native long-press-to-select behavior.
    // HTML5 drag is mouse-initiated in Chromium — test drag & drop with the
    // mouse.
    let touch0 = event.touches[0];
    if (touch0 && event.type === TouchType.Down && event.touches.length === 1) {
      // Focus bookkeeping only (single-finger Down) — no mouse events.
      this.lastInputX = touch0.x;
      this.lastInputY = touch0.y;
      this.setRenderAreaActive(true);
      this.handleImeFocus();
      arkui_cef.cefSendFocusEvent(this.browserId, true);
    }
  }

  // ==================== Mouse Event (鼠标移动/点击/触摸板单指) ====================
  // Touchpad single-finger input also arrives here as SourceType.Mouse
  // (verified from docs) — tap=Press/Release, swipe=Move, so touchpad is
  // fully covered by this handler plus handleAxisEvent (two-finger scroll).

  // EVENTFLAG_* helpers
  private buttonFlag(cefButton: number): number {
    if (cefButton === 1) {
      return 0x20; // EVENTFLAG_MIDDLE_MOUSE_BUTTON
    }
    if (cefButton === 2) {
      return 0x40; // EVENTFLAG_RIGHT_MOUSE_BUTTON
    }
    return 0x10;   // EVENTFLAG_LEFT_MOUSE_BUTTON
  }

  private buildMouseModifiers(event: MouseEvent, buttonMods: number): number {
    // Shift/Ctrl/Alt via BaseEvent.getModifierKeyState (API 12+); guarded
    // because the method is optional on older SDKs.
    let mods = buttonMods;
    if (event.getModifierKeyState) {
      try {
        if (event.getModifierKeyState(['Shift'])) {
          mods |= 0x02; // EVENTFLAG_SHIFT_DOWN
        }
        if (event.getModifierKeyState(['Ctrl'])) {
          mods |= 0x04; // EVENTFLAG_CONTROL_DOWN
        }
        if (event.getModifierKeyState(['Alt'])) {
          mods |= 0x08; // EVENTFLAG_ALT_DOWN
        }
      } catch (e) {
      }
    }
    return mods;
  }

  private heldButtonMods(event: MouseEvent): number {
    // Prefer the system-reported button state (MouseEvent.pressedButtons,
    // API 15+): authoritative for multi-button holds and self-correcting if
    // a Release was missed (e.g. the button was released outside this
    // component) — the next Move reads the true state.
    let pb = event.pressedButtons;
    if (pb !== undefined) {
      let mods = 0;
      for (let i = 0; i < pb.length; i++) {
        if (pb[i] === MouseButton.Left) {
          mods |= 0x10;
        } else if (pb[i] === MouseButton.Middle) {
          mods |= 0x20;
        } else if (pb[i] === MouseButton.Right) {
          mods |= 0x40;
        }
      }
      return mods;
    }
    // Fallback (pressedButtons unavailable): left-only tracking
    return this.mouseButtonPressed ? 0x10 : 0;
  }

  handleMouseEvent(event: MouseEvent): void {
    if (!this.isCreated()) {
      return;
    }

    // Stop propagation so Tabs doesn't handle the event (prevents tab switching on scroll)
    event.stopPropagation();

    // DSF is set to device scale factor, so CEF GetViewRect returns CSS pixels.
    // ArkUI mouse coordinates are in vp (CSS pixels), so no conversion needed.
    let x = Math.round(event.x);
    let y = Math.round(event.y);

    // ArkUI MouseButton: Left=1, Right=2, Middle=4, Back=8, Forward=16, None=0
    // CEF cef_mouse_button_type_t: MBT_LEFT=0, MBT_MIDDLE=1, MBT_RIGHT=2
    let cefButton = 0; // default left
    if (event.button === MouseButton.Left) {
      cefButton = 0;
    } else if (event.button === MouseButton.Middle) {
      cefButton = 1;
    } else if (event.button === MouseButton.Right) {
      cefButton = 2;
    }

    // ArkUI MouseAction: Press=1, Release=2, Move=3
    switch (event.action) {
      case MouseAction.Press: {
        hilog.info(DOMAIN, MODULE, "Mouse Press: x=%{public}d, y=%{public}d, btn=%{public}d", x, y, cefButton);
        this.lastInputX = event.x;
        this.lastInputY = event.y;
        this.setRenderAreaActive(true);
        this.handleImeFocus();
        arkui_cef.cefSendFocusEvent(this.browserId, true);

        // Click count (LEFT only): double-click selects a word, triple-click
        // selects a paragraph — CEF recognizes these from click_count.
        let clickCount = 1;
        if (cefButton === 0) {
          let now = Date.now();
          if (now - this.lastClickTime <= OsrCefView.DOUBLE_CLICK_TIME_MS &&
              Math.abs(x - this.lastClickX) <= OsrCefView.DOUBLE_CLICK_SLOP_PX &&
              Math.abs(y - this.lastClickY) <= OsrCefView.DOUBLE_CLICK_SLOP_PX &&
              this.clickCount > 0) {
            // Triple-click wraps back to 1 (4th click = new single click)
            this.clickCount = this.clickCount >= 3 ? 1 : this.clickCount + 1;
          } else {
            this.clickCount = 1;
          }
          this.lastClickTime = now;
          this.lastClickX = x;
          this.lastClickY = y;
          clickCount = this.clickCount;
          this.mouseButtonPressed = true;
        }

        // Modifiers: the pressed button's own flag + Shift/Ctrl/Alt
        let pressMods = this.buildMouseModifiers(event, this.buttonFlag(cefButton));
        arkui_cef.cefSendMouseClick(this.browserId, x, y, cefButton, false, clickCount, pressMods);
        this.lastMouseX = x;
        this.lastMouseY = y;
        break;
      }
      case MouseAction.Release:
        hilog.info(DOMAIN, MODULE, "Mouse Release: x=%{public}d, y=%{public}d, btn=%{public}d", x, y, cefButton);
        arkui_cef.cefSendMouseClick(this.browserId, x, y, cefButton, true, 1, 0);
        if (cefButton === 0) {
          // Only LEFT clears the flag — releasing RIGHT/MIDDLE while LEFT is
          // held must not break an in-progress left-drag.
          this.mouseButtonPressed = false;
        }
        break;
      case MouseAction.Move: {
        // Forward move events including during a button-drag. Modifiers come
        // from the system-reported pressedButtons — accurate for multi-button
        // holds and self-correcting after a missed Release.
        let moveMods = this.buildMouseModifiers(event, this.heldButtonMods(event));
        if ((moveMods & 0x10) !== 0) {
          hilog.info(DOMAIN, MODULE, "Mouse Drag: x=%{public}d, y=%{public}d", x, y);
        }
        arkui_cef.cefSendMouseMove(this.browserId, x, y, false, moveMods);
        this.lastMouseX = x;
        this.lastMouseY = y;
        break;
      }
      default:
        break;
    }
  }

  // ==================== Axis Event (鼠标滚轮 / 触摸板双指滚动) ====================

  // Wheel translation: axis values are forwarded event-by-event with a
  // per-source gain — NO threshold batching (the old batching caused the
  // "two-stage scroll": sub-threshold deltas waited, then jumped). The gain
  // is an EMPIRICAL calibration factor (ArkUI's axis units are documented
  // inconsistently: vp vs angle vs pixel per source), NOT a magic number —
  // tune each source independently.
  // Defaults keep the previously accepted feel (×3 was tuned historically;
  // the old batching did not change the total amount, only its delivery).
  // Sign note: docs say touchpad swipe-up = positive and wheel-up = negative
  // (opposite conventions), but the unified negation below matches the
  // accepted feel on this device — if either input ever feels inverted,
  // flip that source's sign below.
  private static readonly TOUCHPAD_WHEEL_GAIN: number = 3;
  private static readonly MOUSE_WHEEL_GAIN: number = 3;

  handleAxisEvent(event: AxisEvent): void {
    if (!this.isCreated()) {
      return;
    }

    let x = Math.round(event.x);
    let y = Math.round(event.y);
    let vAxis = event.getVerticalAxisValue();
    let hAxis = event.getHorizontalAxisValue();

    // Ctrl+wheel zoom: detect Ctrl modifier and send with EVENTFLAG_CONTROL_DOWN
    let isCtrlPressed = false;
    if (event.getModifierKeyState) {
      try {
        isCtrlPressed = event.getModifierKeyState(['Ctrl']);
      } catch (e) {
      }
    }
    let modifiers = isCtrlPressed ? 0x04 : 0;

    if (vAxis === 0 && hAxis === 0) {
      return;
    }

    // Distinguish the input source (documented use of sourceTool on axis
    // events): TOUCHPAD = two-finger swipe, MOUSE = wheel rotation.
    let gain = OsrCefView.MOUSE_WHEEL_GAIN;
    if (event.sourceTool === SourceTool.TOUCHPAD) {
      gain = OsrCefView.TOUCHPAD_WHEEL_GAIN;
    }

    // Direct pass-through: every axis event becomes one wheel event with the
    // same total delta — Chromium coalesces rapid events itself.
    let deltaY = Math.round(-vAxis * gain);
    let deltaX = Math.round(-hAxis * gain);
    if (deltaY === 0 && deltaX === 0) {
      // Sub-rounding remainder (< 0.5 units) — skip.
      return;
    }
    arkui_cef.cefSendMouseWheel(this.browserId, x, y, deltaX, deltaY, modifiers);
  }

  // ==================== Input Method Framework (IME) ====================

  private handleImeFocus(): void {
    // Only attach if CEF has a focused input field (keyboardVisible=true).
    // If not, wait for CEF's OnVirtualKeyboardRequested to drive the attach.
    if (!this.imeAttached && this.keyboardVisible) {
      this.attachInputMethod();
    }
  }

  private setRenderAreaActive(active: boolean): void {
    this.renderAreaActive = active;
    arkui_cef.setRenderAreaActive(active);
    if (!active) {
      if (this.isCreated()) {
        arkui_cef.cefSendFocusEvent(this.browserId, false);
      }
    }
  }

  private attachInputMethod(): void {
    if (this.imeAttached || this.imeAttaching) {
      return;
    }
    this.imeAttaching = true;

    // Compute initial cursor position (screen-absolute px).
    let winRect = this.getWindowRect();
    this.lastWinRectLeft = winRect.left;
    this.lastWinRectTop = winRect.top;
    let cursorLeft = Math.round((this.lastInputX + this.componentGlobalX) * this.cachedDsf) + winRect.left;
    let cursorTop = Math.round((this.lastInputY + this.componentGlobalY) * this.cachedDsf) + winRect.top;
    let cursorHeight = Math.round(20 * this.cachedDsf);
    hilog.info(DOMAIN, MODULE,
      "attachInputMethod: screen_px left=%{public}d top=%{public}d winRect=%{public}d,%{public}d",
      cursorLeft, cursorTop, winRect.left, winRect.top);

    let ok = arkui_cef.cefImeCAttach(
      this.cachedWindowId,
      cursorLeft, cursorTop, 2, cursorHeight,
      (text: string) => {
        if (this.isCreated() && this.renderAreaActive) {
          hilog.info(DOMAIN, MODULE, "IME insertText: '%{public}s'", text);
          arkui_cef.cefImeCommitText(this.browserId, text);
        }
      },
      (length: number) => {
        if (this.isCreated() && this.renderAreaActive) {
          hilog.info(DOMAIN, MODULE, "IME deleteLeft: %{public}d", length);
          for (let i = 0; i < length; i++) {
            arkui_cef.cefSendKeyEvent(this.browserId, 2055, 0, 0);
            arkui_cef.cefSendKeyEvent(this.browserId, 2055, 1, 0);
          }
        }
      },
      () => {
        if (this.isCreated() && this.renderAreaActive) {
          hilog.info(DOMAIN, MODULE, "IME sendEnterKey");
          arkui_cef.cefSendKeyEvent(this.browserId, 2054, 0, 0);
          arkui_cef.cefSendKeyEvent(this.browserId, 2054, 2, 0, 13);
          arkui_cef.cefSendKeyEvent(this.browserId, 2054, 1, 0);
        }
      },
      (text: string) => {
        if (this.isCreated() && this.renderAreaActive) {
          hilog.info(DOMAIN, MODULE, "IME setPreviewText: '%{public}s'", text);
          arkui_cef.cefImeSetComposition(this.browserId, text);
        }
      },
      () => {
        if (this.isCreated() && this.renderAreaActive) {
          hilog.info(DOMAIN, MODULE, "IME finishTextPreview");
          arkui_cef.cefImeCancelComposition(this.browserId);
        }
      }
    );

    this.imeAttaching = false;
    if (ok) {
      this.imeAttached = true;
      hilog.info(DOMAIN, MODULE, "IME attached (C API) successfully");
      this.refreshImeCursor();
    } else {
      hilog.error(DOMAIN, MODULE, "IME attach (C API) failed");
    }
  }

  private refreshImeCursor(): void {
    if (!this.imeAttached || !this.hasCaretBounds) {
      return;
    }
    let winRect = this.getWindowRect();
    this.lastWinRectLeft = winRect.left;
    this.lastWinRectTop = winRect.top;
    let screenLeft = this.lastCaretWinLeft + winRect.left;
    let screenTop = this.lastCaretWinTop + winRect.top;
    hilog.info(DOMAIN, MODULE,
      "refreshImeCursor: winRel=%{public}d,%{public}d winRect=%{public}d,%{public}d screen=%{public}d,%{public}d",
      this.lastCaretWinLeft, this.lastCaretWinTop, winRect.left, winRect.top, screenLeft, screenTop);
    this.applyImeCursor(screenLeft, screenTop, this.lastCaretWidth, this.lastCaretHeight);
  }

  private detachInputMethod(): void {
    if (!this.imeAttached) {
      return;
    }
    this.imeAttached = false;
    arkui_cef.cefImeCDetach();
    hilog.info(DOMAIN, MODULE, "IME detached (C API)");
  }

}
