import arkui_cef from 'libarkui_cef.so';
import { image } from '@kit.ImageKit';
import { hilog } from "@kit.PerformanceAnalysisKit";
import { pointer } from '@kit.InputKit';
import { inputMethod } from '@kit.IMEKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { window } from "@kit.ArkUI";
import { common } from '@kit.AbilityKit';

const DOMAIN = 0xFF01;
const MODULE = 'OsrCefView'

@Component
export struct OsrCefView {
  @State private browserId: number = -1;
  @State private cefCreating: boolean = false;
  @State private pixelMap: image.PixelMap | undefined = undefined;
  @State private pageTitle: string = '';
  @State private urlInput: string = '';
  // Track if a PixelMap creation is in progress to avoid flooding the main thread
  private pixelMapCreating: boolean = false;
  // Track mouse button state for drag operations (scrollbar drag, text selection, etc.)
  private mouseButtonPressed: boolean = false;
  private lastMouseX: number = 0;
  private lastMouseY: number = 0;
  // Track if mouse button was pressed via single-touch simulation
  private touchMouseSimulated: boolean = false;
  // Pinch gesture tracking for touchpad zoom
  private pinchLastScale: number = 1.0;
  // Input Method Framework (IMF) controller for OSR text input
  private inputMethodController: inputMethod.InputMethodController = inputMethod.getController();
  private imeAttached: boolean = false;
  private imeAttaching: boolean = false;
  private imeSubscribed: boolean = false;
  // When true, key events are forwarded to CEF (user clicked on render area).
  // When false, key events go to the URL bar TextInput normally.
  private renderAreaActive: boolean = false;
  // Hidden TextInput used as IME panel trigger. When focused via
  // FocusController.requestFocus('ime_proxy_input'), ArkUI's internal
  // component focus mechanism shows the IME panel — this works reliably
  // even after window focus loss/recovery, unlike showTextInput() which
  // may hang. Text input itself flows through InputMethodController callbacks.
  private imeProxyFocused: boolean = false;
  // Set by WINDOW_INACTIVE — when true, next touch/mouse-down on render area
  // forces CEF blur+focus to re-trigger OnVirtualKeyboardRequested.
  private focusLostFlag: boolean = false;
  // Set by WINDOW_ACTIVE when keyboard was visible before focus loss.
  private pendingImeShow: boolean = false;
  // Counter for IME changeSelection workaround (from IMFAdapter)
  private imeCount: number = 0;

  @State private viewWidth: number = 800;
  @State private viewHeight: number = 600;
  @State private scrollOffsetText: string = '';
  @State private selectedText: string = '';
  @State private keyboardVisible: boolean = false;
  @State private cursorTypeText: string = 'default';

  @Prop viewUrl: string = '';

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
          .flexGrow(1)
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
        // Hidden TextInput — IME panel trigger.
        // When focused via FocusController.requestFocus('ime_proxy_input'),
        // ArkUI shows the IME panel through its internal component focus
        // mechanism. hitTestBehavior(None) ensures it never intercepts
        // touch/mouse events — all input goes to the render area handlers.
        TextInput({ text: '' })
          .opacity(0)
          .width(1)
          .height(1)
          .hitTestBehavior(HitTestMode.None)
          .key('ime_proxy_input')
      }
      .width('100%')
      .flexGrow(1)
      .backgroundColor(Color.Black)
      .hitTestBehavior(HitTestMode.Block)
      .focusable(true)
      .onTouch((event: TouchEvent) => this.handleTouchEvent(event))
      .onMouse((event: MouseEvent) => this.handleMouseEvent(event))
      .onAxisEvent((event: AxisEvent) => this.handleAxisEvent(event))
      .gesture(
        PinchGesture({ fingers: 2, distance: 0.1 })
          .onActionStart((event: GestureEvent) => {
            // Skip touchscreen — its multi-touch is handled by onTouch → CEF touch events.
            // Only handle non-touchscreen (touchpad/mouse) pinch for page zoom.
            if (event.source === SourceType.TouchScreen) {
              return;
            }
            this.pinchLastScale = event.scale;
          })
          .onActionUpdate((event: GestureEvent) => {
            if (event.source === SourceType.TouchScreen) {
              return;
            }
            let scaleDelta = event.scale - this.pinchLastScale;
            if (Math.abs(scaleDelta) > 0.01) {
              // Chromium: Ctrl + wheel deltaY > 0 = zoom in, < 0 = zoom out
              // Fingers expand → scale > 1 → deltaY > 0 → zoom in
              let deltaY = Math.round(scaleDelta * 300);
              let x = Math.round(event.fingerList[0].localX);
              let y = Math.round(event.fingerList[0].localY);
              arkui_cef.cefSendMouseWheel(this.browserId, x, y, 0, deltaY, 0x04);
              this.pinchLastScale = event.scale;
            }
          })
          .onActionEnd(() => {
            this.pinchLastScale = 1.0;
          })
      )
      .onAreaChange((_oldValue: Area, newValue: Area) => {
        let wpx = Math.round(this.getUIContext().vp2px(newValue.width as number));
        let hpx = Math.round(this.getUIContext().vp2px(newValue.height as number));
        hilog.info(DOMAIN, MODULE, "onAreaChange: wpx=%{public}d, hpx=%{public}d", wpx, hpx);

        if (wpx > 0 && hpx > 0) {
          this.viewWidth = wpx;
          this.viewHeight = hpx;

          if (this.isCreated()) {
            // Set DSF to actual device scale factor for correct content sizing
            let dsf = this.getUIContext().vp2px(1);
            arkui_cef.cefOsrSetScale(this.browserId, dsf);
            arkui_cef.cefOsrResize(this.browserId, this.viewWidth, this.viewHeight);
            // Pass component offset for GetScreenPoint (tooltip positioning)
            arkui_cef.cefSetComponentOffset(this.browserId,
              Math.round(newValue.globalPosition.x as number),
              Math.round(newValue.globalPosition.y as number));
          }
        }
      })

      // Status bar — shows OSR state info
      if (this.isCreated()) {
        Row() {
          Text(`Scroll: ${this.scrollOffsetText || '0,0'}`)
            .fontSize(9)
            .fontColor('#999999')
            .flexGrow(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })

          if (this.selectedText.length > 0) {
            Text(`Sel: ${this.selectedText.substring(0, 30)}`)
              .fontSize(9)
              .fontColor('#6666AA')
              .flexGrow(1)
              .maxLines(1)
              .textOverflow({ overflow: TextOverflow.Ellipsis })
          }

          if (this.keyboardVisible) {
            Text('KB: ON')
              .fontSize(9)
              .fontColor('#AA6666')
          }

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
  }

  getBrowserId(): number {
    return this.browserId;
  }

  isCreated(): boolean {
    return this.browserId > 0;
  }

  destroy(): void {
    this.blurProxyTextInput();
    this.pendingImeShow = false;
    this.detachInputMethod();
    if (this.isCreated()) {
      hilog.info(DOMAIN, MODULE, "Destroying OSR browser, id=%{public}d", this.browserId);
      arkui_cef.destroyOsrCefView(this.browserId);
      this.browserId = -1;
    }
    if (this.pixelMap) {
      this.pixelMap.release();
      this.pixelMap = undefined;
    }
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
      (buffer: ArrayBuffer, w: number, h: number) => { this.onOsrPaint(buffer, w, h); },
      (id: number) => { this.onOsrCreated(id); },
      (title: string) => { this.onOsrTitleChanged(title); },
      (next: boolean) => { this.onOsrFocusTake(next); },
      (offset: string) => { this.onOsrScrollOffset(offset); },
      (show: boolean) => { this.onOsrVirtualKeyboard(show); },
      (text: string) => { this.onOsrTextSelection(text); },
      (cursorType: number) => { this.onOsrCursorChange(cursorType); });

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
    arkui_cef.cefOsrSetScale(this.browserId, dsf);
    arkui_cef.cefOsrResize(this.browserId, this.viewWidth, this.viewHeight);
    // Register window-level key event filter for non-text keys (digits, symbols, etc.)
    this.registerKeyFilter();
  }

  private async registerKeyFilter(): Promise<void> {
    try {
      let context: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      let win: window.Window = await window.getLastWindow(context);
      let windowId: number = win.getWindowProperties().id;
      hilog.info(DOMAIN, MODULE, "registerKeyFilter: windowId=%{public}d, browserId=%{public}d",
        windowId, this.browserId);
      arkui_cef.registerOsrKeyFilter(windowId, this.browserId);

      win.on('windowEvent', (event: window.WindowEventType) => {
        hilog.info(DOMAIN, MODULE, "windowEvent: %{public}d", event);
        if (event === window.WindowEventType.WINDOW_INACTIVE) {
          // Window lost focus — system auto-detaches IMF and hides IME panel.
          // Reset imeAttached so attachInputMethod() will re-attach on next
          // onOsrVirtualKeyboard(true). Also notify CEF of blur.
          this.focusLostFlag = true;
          this.imeProxyFocused = false;
          this.imeAttached = false;
          if (this.isCreated() && this.renderAreaActive) {
            arkui_cef.cefSendFocusEvent(this.browserId, false);
          }
        } else if (event === window.WindowEventType.WINDOW_ACTIVE) {
          // Window regained focus. If keyboard was visible before focus loss,
          // focus the proxy TextInput to re-trigger the IME panel through
          // ArkUI's component focus mechanism. This is needed because
          // inputMethodController.showTextInput() may not work after re-focus.
          if (this.focusLostFlag && this.keyboardVisible) {
            this.focusLostFlag = false;
            this.focusProxyTextInput();
            hilog.info(DOMAIN, MODULE, "WINDOW_ACTIVE: focused proxy TextInput for IME re-trigger");
          }
        }
      });
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "registerKeyFilter failed: %{public}s", (e as Error).message);
    }
  }

  onOsrPaint(buffer: ArrayBuffer, width: number, height: number): void {
    // Skip if a previous PixelMap creation is still in progress.
    // This prevents flooding the main thread with createPixelMap tasks
    // which was the root cause of APP_INPUT_BLOCK freezes.
    if (this.pixelMapCreating) {
      return;
    }
    this.pixelMapCreating = true;
    try {
      // CEF outputs BGRA. ArkUI createPixelMap with BGRA_8888 format matches directly.
      let opts: image.InitializationOptions = {
        size: { width: width, height: height },
        pixelFormat: image.PixelMapFormat.BGRA_8888,
        editable: false,
      };
      image.createPixelMap(buffer, opts).then((pm: image.PixelMap) => {
        let oldPm = this.pixelMap;
        this.pixelMap = pm;
        if (oldPm) {
          oldPm.release();
        }
        this.pixelMapCreating = false;
      }).catch((err: Error) => {
        hilog.error(DOMAIN, MODULE, "createPixelMap failed: %{public}s", err.message);
        this.pixelMapCreating = false;
      });
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "onOsrPaint error: %{public}s", (e as Error).message);
      this.pixelMapCreating = false;
    }
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
      this.attachInputMethod();
    } else {
      this.blurProxyTextInput();
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

  // ==================== Touch Event (触摸屏) ====================
  handleTouchEvent(event: TouchEvent): void {
    if (!this.isCreated()) {
      return;
    }

    event.stopPropagation();

    // CEF touch event types: TOUCHEVENT_RELEASED=0, TOUCHEVENT_PRESSED=1, TOUCHEVENT_MOVED=2, TOUCHEVENT_CANCELLED=3
    let touchType = 2;
    switch (event.type) {
      case TouchType.Down: touchType = 1; break;
      case TouchType.Up: touchType = 0; break;
      case TouchType.Move: touchType = 2; break;
      case TouchType.Cancel: touchType = 3; break;
      default: break;
    }

    let isMultiTouch = event.touches.length > 1;

    // When transitioning from single-touch to multi-touch:
    // release the mouse button first, otherwise CEF stays in mouse-drag
    // state and stops forwarding touch events to the page.
    if (isMultiTouch && this.touchMouseSimulated && event.type === TouchType.Down) {
      let t0 = event.touches[0];
      arkui_cef.cefSendMouseClick(this.browserId,
        Math.round(t0.x), Math.round(t0.y), 0, true, 1, 0);
      this.touchMouseSimulated = false;
    }

    // Forward touch events to CEF (always, for all touch points)
    // Down/Up: only send changed touches; Move/Cancel: send all touches
    let touchList: TouchObject[] = event.touches;
    if (event.type === TouchType.Down || event.type === TouchType.Up) {
      touchList = event.changedTouches;
    }
    for (let i = 0; i < touchList.length; i++) {
      let touch = touchList[i];
      arkui_cef.cefSendTouchEvent(this.browserId,
        Math.round(touch.x), Math.round(touch.y), touchType, touch.id);
    }

    // Mouse simulation: only for single-touch (1 finger).
    // Multi-touch must not send mouse events — CEF would enter mouse-drag
    // state and stop forwarding touch events to the page's JavaScript.
    if (isMultiTouch) {
      return;
    }

    let touch0 = event.touches[0];
    if (!touch0) {
      return;
    }
    let x = Math.round(touch0.x);
    let y = Math.round(touch0.y);

    switch (event.type) {
      case TouchType.Down:
        this.setRenderAreaActive(true);
        this.handleImeFocus();
        arkui_cef.cefSendFocusEvent(this.browserId, true);
        arkui_cef.cefSendMouseMove(this.browserId, x, y, false);
        arkui_cef.cefSendMouseClick(this.browserId, x, y, 0, false, 1, 0x10);
        this.touchMouseSimulated = true;
        break;
      case TouchType.Up:
        arkui_cef.cefSendMouseClick(this.browserId, x, y, 0, true, 1, 0);
        this.touchMouseSimulated = false;
        break;
      case TouchType.Move:
        arkui_cef.cefSendMouseMove(this.browserId, x, y, false, 0x10);
        break;
      case TouchType.Cancel:
        arkui_cef.cefSendMouseMove(this.browserId, x, y, true);
        arkui_cef.cefSendCaptureLost(this.browserId);
        this.touchMouseSimulated = false;
        break;
      default:
        break;
    }
  }

  // ==================== Mouse Event (鼠标移动/点击) ====================
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
      case MouseAction.Press:
        hilog.info(DOMAIN, MODULE, "Mouse Press: x=%{public}d, y=%{public}d, btn=%{public}d", x, y, cefButton);
        this.setRenderAreaActive(true);
        this.handleImeFocus();
        arkui_cef.cefSendFocusEvent(this.browserId, true);
        // Set EVENTFLAG_LEFT_MOUSE_BUTTON (0x10) on mouse down so CEF scrollbar
        // drag and text selection can start. CEF requires the button flag in
        // modifiers even for click events.
        let pressMods = 0x10; // EVENTFLAG_LEFT_MOUSE_BUTTON
        if (cefButton === 1) {
          pressMods = 0x20; // EVENTFLAG_MIDDLE_MOUSE_BUTTON
        } else if (cefButton === 2) {
          pressMods = 0x40; // EVENTFLAG_RIGHT_MOUSE_BUTTON
        }
        arkui_cef.cefSendMouseClick(this.browserId, x, y, cefButton, false, 1, pressMods);
        this.mouseButtonPressed = true;
        this.lastMouseX = x;
        this.lastMouseY = y;
        break;
      case MouseAction.Release:
        hilog.info(DOMAIN, MODULE, "Mouse Release: x=%{public}d, y=%{public}d, btn=%{public}d", x, y, cefButton);
        arkui_cef.cefSendMouseClick(this.browserId, x, y, cefButton, true, 1, 0);
        this.mouseButtonPressed = false;
        break;
      case MouseAction.Move:
        // Forward move events including during drag (mouseButtonPressed=true).
        // CEF's SendMouseMoveEvent does NOT inherit button state from previous
        // SendMouseClickEvent — we must set modifiers explicitly for drag to work.
        // EVENTFLAG_LEFT_MOUSE_BUTTON = 1 << 4 = 0x10 (NOT 0x01 which is CAPS_LOCK)
        let moveMods = 0;
        if (this.mouseButtonPressed) {
          moveMods = 0x10; // EVENTFLAG_LEFT_MOUSE_BUTTON
          hilog.info(DOMAIN, MODULE, "Mouse Drag: x=%{public}d, y=%{public}d", x, y);
        }
        arkui_cef.cefSendMouseMove(this.browserId, x, y, false, moveMods);
        this.lastMouseX = x;
        this.lastMouseY = y;
        break;
      default:
        break;
    }
  }

  // ==================== Axis Event (鼠标滚轮) ====================

  // Accumulate small axis deltas to avoid sending too many tiny scroll events.
  // ArkUI fires many axis events per single wheel notch (momentum/inertia events).
  // We accumulate until we have a meaningful delta, then send one event to CEF.
  private accumulatedWheelX: number = 0;
  private accumulatedWheelY: number = 0;
  // Threshold: accumulate until delta exceeds this, then send. Reduces "two-stage" scroll.
  private static readonly WHEEL_ACCUM_THRESHOLD: number = 20;
  // Scale: ArkUI axis value per notch is variable; we scale to CEF's ~120/notch expectation.
  // Lower value = less scroll per notch. User reports too much scroll, so reduce from 12 to 3.
  private static readonly WHEEL_SCALE: number = 3;

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

    let rawDeltaY = -vAxis * OsrCefView.WHEEL_SCALE;
    let rawDeltaX = -hAxis * OsrCefView.WHEEL_SCALE;
    this.accumulatedWheelY += rawDeltaY;
    this.accumulatedWheelX += rawDeltaX;

    if (Math.abs(this.accumulatedWheelY) >= OsrCefView.WHEEL_ACCUM_THRESHOLD ||
        Math.abs(this.accumulatedWheelX) >= OsrCefView.WHEEL_ACCUM_THRESHOLD) {
      let deltaY = Math.round(this.accumulatedWheelY);
      let deltaX = Math.round(this.accumulatedWheelX);
      this.accumulatedWheelY = 0;
      this.accumulatedWheelX = 0;
      arkui_cef.cefSendMouseWheel(this.browserId, x, y, deltaX, deltaY, modifiers);
    }
  }

  // ==================== Input Method Framework (IME) ====================

  private handleImeFocus(): void {
    // If IME panel needs to be re-shown after window re-focus, focus the
    // proxy TextInput to trigger the IME panel through ArkUI's component
    // focus mechanism. This is more reliable than showTextInput() after
    // window re-focus.
    if (this.pendingImeShow) {
      this.pendingImeShow = false;
      hilog.info(DOMAIN, MODULE, "Focusing proxy TextInput for IME panel");
      this.focusProxyTextInput();
      if (!this.imeAttached) {
        this.attachInputMethod();
      }
    }
  }

  private setRenderAreaActive(active: boolean): void {
    this.renderAreaActive = active;
    arkui_cef.setRenderAreaActive(active);
    if (!active) {
      this.blurProxyTextInput();
      this.pendingImeShow = false;
      if (this.isCreated()) {
        arkui_cef.cefSendFocusEvent(this.browserId, false);
      }
    }
  }

  private focusProxyTextInput(): void {
    this.imeProxyFocused = true;
    try {
      this.getUIContext().getFocusController().requestFocus('ime_proxy_input');
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "focusProxyTextInput failed: %{public}s", (e as Error).message);
    }
  }

  private blurProxyTextInput(): void {
    this.imeProxyFocused = false;
  }

  private subscribeImeEvents(): void {
    // off() first to prevent duplicate callbacks — on() is cumulative
    this.inputMethodController.off('insertText');
    this.inputMethodController.off('setPreviewText');
    this.inputMethodController.off('finishTextPreview');
    this.inputMethodController.off('deleteLeft');
    this.inputMethodController.off('sendFunctionKey');

    this.inputMethodController.on('insertText', (text: string) => {
      if (this.isCreated() && this.renderAreaActive) {
        hilog.info(DOMAIN, MODULE, "IME insertText: '%{public}s'", text);
        arkui_cef.cefImeCommitText(this.browserId, text);
        this.inputMethodController.changeSelection("" + this.imeCount++, 0, 0);
      }
    });
    this.inputMethodController.on('setPreviewText', (text: string) => {
      if (this.isCreated() && this.renderAreaActive) {
        hilog.info(DOMAIN, MODULE, "IME setPreviewText: '%{public}s'", text);
        arkui_cef.cefImeSetComposition(this.browserId, text);
      }
    });
    this.inputMethodController.on('finishTextPreview', () => {
      if (this.isCreated() && this.renderAreaActive) {
        hilog.info(DOMAIN, MODULE, "IME finishTextPreview");
        arkui_cef.cefImeCancelComposition(this.browserId);
      }
    });
    this.inputMethodController.on('deleteLeft', (length: number) => {
      if (this.isCreated() && this.renderAreaActive) {
        hilog.info(DOMAIN, MODULE, "IME deleteLeft: %{public}d", length);
        for (let i = 0; i < length; i++) {
          arkui_cef.cefSendKeyEvent(this.browserId, 2055, 0, 0);
          arkui_cef.cefSendKeyEvent(this.browserId, 2055, 1, 0);
        }
      }
    });
    this.inputMethodController.on('sendFunctionKey', () => {
      if (this.isCreated() && this.renderAreaActive) {
        hilog.info(DOMAIN, MODULE, "IME sendFunctionKey (Enter)");
        arkui_cef.cefSendKeyEvent(this.browserId, 2054, 0, 0);
        arkui_cef.cefSendKeyEvent(this.browserId, 2054, 2, 0, 13);
        arkui_cef.cefSendKeyEvent(this.browserId, 2054, 1, 0);
      }
    });
  }

  private attachInputMethod(): void {
    if (this.imeAttached || this.imeAttaching) {
      return;
    }
    this.imeAttaching = true;
    let textConfig: inputMethod.TextConfig = {
      inputAttribute: {
        textInputType: inputMethod.TextInputType.TEXT,
        enterKeyType: inputMethod.EnterKeyType.GO
      }
    };
    // Try attach directly first. If the IMF is in a clean (detached) state,
    // this succeeds without flicker. If it fails (partially attached state),
    // fall back to detach→attach to ensure clean state.
    this.inputMethodController.attach(true, textConfig).then(() => {
      this.imeAttached = true;
      this.imeAttaching = false;
      hilog.info(DOMAIN, MODULE, "IME attached successfully");
      this.subscribeImeEvents();
    }).catch((err: BusinessError) => {
      hilog.info(DOMAIN, MODULE, "IME direct attach failed (%{public}d), trying detach→attach", err.code);
      this.inputMethodController.detach().then(() => {
        return this.inputMethodController.attach(true, textConfig);
      }).then(() => {
        this.imeAttached = true;
        this.imeAttaching = false;
        hilog.info(DOMAIN, MODULE, "IME attached successfully (after detach)");
        this.subscribeImeEvents();
      }).catch((err2: BusinessError) => {
        this.imeAttaching = false;
        hilog.error(DOMAIN, MODULE, "IME attach chain failed: code=%{public}d msg=%{public}s",
          err2.code, err2.message);
        this.subscribeImeEvents();
      });
    });
  }

  private detachInputMethod(): void {
    if (!this.imeAttached) {
      return;
    }
    this.imeAttached = false;
    this.inputMethodController.off('insertText');
    this.inputMethodController.off('setPreviewText');
    this.inputMethodController.off('finishTextPreview');
    this.inputMethodController.off('deleteLeft');
    this.inputMethodController.off('sendFunctionKey');
    this.inputMethodController.detach().then(() => {
      hilog.info(DOMAIN, MODULE, "IME detached successfully");
    }).catch((err: BusinessError) => {
      hilog.error(DOMAIN, MODULE, "IME detach failed: code=%{public}d msg=%{public}s",
        err.code, err.message);
    });
  }

  // ==================== Key Event (键盘) ====================
  handleKeyEvent(event: KeyEvent): void {
    hilog.info(DOMAIN, MODULE, "handleKeyEvent ENTER: keyCode=%{public}d, type=%{public}d, keyText=%{public}s",
      event.keyCode, event.type, event.keyText);
    if (!this.isCreated()) {
      return;
    }

    // Stop propagation so the event doesn't trigger Tab navigation
    event.stopPropagation();

    // ArkUI KeyType: Down=0, Up=1
    // CEF expects: KEYEVENT_RAWKEYDOWN (0) for down, KEYEVENT_KEYUP (1) for up
    // Additionally, for character input CEF needs KEYEVENT_CHAR (2) after RAWKEYDOWN.
    let keyType = (event.type === KeyType.Down) ? 0 : 1;

    // ArkUI KeyEvent.unicode provides the Unicode code point of the key (API 14+).
    // This is far more reliable than mapping KeyCode → character in C++.
    // For printable keys (unicode 0x21-0x7E), we pass it to CEF as the char code.
    let unicode = event.unicode ?? 0;

    // Build CEF modifiers from ArkUI key event state.
    // ArkUI KeyEvent has metaKey (0 or 1 for Windows/Super key only).
    // We also need to check keyCode for Shift/Ctrl/Alt state.
    let cefMods = 0;
    if (event.metaKey === 1) {
      cefMods |= 0x08; // EVENTFLAG_ALT_DOWN (map metaKey to Alt for CEF)
    }

    hilog.info(DOMAIN, MODULE,
      "KeyEvent: type=%{public}d, keyCode=%{public}d, keyText=%{public}s, metaKey=%{public}d, unicode=%{public}d, cefMods=%{public}d",
      keyType, event.keyCode, event.keyText, event.metaKey, unicode, cefMods);

    // Ensure CEF has focus before sending key events
    arkui_cef.cefSendFocusEvent(this.browserId, true);

    // Send the raw key down/up event (without char code)
    arkui_cef.cefSendKeyEvent(this.browserId, event.keyCode, keyType, cefMods);

    // On key down, also send a CHAR event for printable characters.
    // CEF uses KEYEVENT_CHAR to deliver text to input fields.
    if (event.type === KeyType.Down) {
      let kc = event.keyCode;
      let charCode = 0;

      // Try unicode first (most reliable, API 14+)
      if (unicode >= 0x20 && unicode <= 0x7E) {
        charCode = unicode;
      }

      // If unicode is 0, try keyText (single char keys like "a", "B", "5")
      if (charCode === 0 && event.keyText.length === 1) {
        let ch = event.keyText.charCodeAt(0);
        if (ch >= 0x20 && ch <= 0x7E) {
          charCode = ch;
        }
      }

      // Known printable keys that C++ convertKeyEvent handles from KeyCode
      let isPrintableFromKeyCode =
        kc === 2062 ||  // Space
        kc === 2054 ||  // Enter (\r)
        kc === 2055 ||  // Backspace (\b)
        kc === 2076;    // Tab (\t)

      if (charCode > 0) {
        // Send CHAR event with explicit Unicode char code
        hilog.info(DOMAIN, MODULE, "Sending CHAR: charCode=%{public}d ('%{public}c')", charCode, charCode);
        arkui_cef.cefSendKeyEvent(this.browserId, kc, 2, cefMods, charCode);
      } else if (isPrintableFromKeyCode) {
        // Space/Enter/Backspace/Tab — C++ will set character from KeyCode
        hilog.info(DOMAIN, MODULE, "Sending CHAR from KeyCode: kc=%{public}d", kc);
        arkui_cef.cefSendKeyEvent(this.browserId, kc, 2, cefMods);
      } else if (kc >= 2018 && kc <= 2043) {
        // Letters A-Z: if unicode and keyText both failed, fall back to C++ KeyCode mapping
        hilog.info(DOMAIN, MODULE, "Sending CHAR letter from KeyCode: kc=%{public}d", kc);
        arkui_cef.cefSendKeyEvent(this.browserId, kc, 2, cefMods);
      } else if (kc >= 2000 && kc <= 2009) {
        // Digits 0-9: fall back to C++ KeyCode mapping
        hilog.info(DOMAIN, MODULE, "Sending CHAR digit from KeyCode: kc=%{public}d", kc);
        arkui_cef.cefSendKeyEvent(this.browserId, kc, 2, cefMods);
      }
    }
  }
}
