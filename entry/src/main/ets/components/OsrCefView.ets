import arkui_cef from 'libarkui_cef.so';
import { image } from '@kit.ImageKit';
import { hilog } from "@kit.PerformanceAnalysisKit";

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

  @State private viewWidth: number = 800;
  @State private viewHeight: number = 600;
  @State private scrollOffsetText: string = '';
  @State private selectedText: string = '';
  @State private keyboardVisible: boolean = false;

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
      .flexGrow(1)
      .backgroundColor(Color.Black)
      .hitTestBehavior(HitTestMode.Block)
      .onTouch((event: TouchEvent) => this.handleTouchEvent(event))
      .onMouse((event: MouseEvent) => this.handleMouseEvent(event))
      .onAxisEvent((event: AxisEvent) => this.handleAxisEvent(event))
      .onKeyEvent((event: KeyEvent) => this.handleKeyEvent(event))
      .onAreaChange((_oldValue, newValue) => {
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
  }

  getBrowserId(): number {
    return this.browserId;
  }

  isCreated(): boolean {
    return this.browserId > 0;
  }

  destroy(): void {
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
      (text: string) => { this.onOsrTextSelection(text); });

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
    // TODO: integrate with HarmonyOS input method framework
    // For now, just log. A full implementation would call @kit.InputMethodKit to show/hide
  }

  onOsrTextSelection(text: string): void {
    hilog.info(DOMAIN, MODULE, "OSR text selection: len=%{public}d", text.length);
    this.selectedText = text;
  }

  // ==================== Touch Event (触摸屏) ====================
  handleTouchEvent(event: TouchEvent): void {
    if (!this.isCreated()) {
      return;
    }

    // Stop propagation so Tabs doesn't handle the event
    event.stopPropagation();

    let touch = event.touches[0];
    if (!touch) {
      return;
    }

    // DSF is set to device scale factor, so CEF GetViewRect returns CSS pixels.
    // ArkUI touch coordinates are in vp (CSS pixels), so no conversion needed.
    let x = Math.round(touch.x);
    let y = Math.round(touch.y);

    // CEF touch event types: TOUCHEVENT_RELEASED=0, TOUCHEVENT_PRESSED=1, TOUCHEVENT_MOVED=2, TOUCHEVENT_CANCELLED=3
    let touchType = 2; // default moved
    switch (event.type) {
      case TouchType.Down:
        touchType = 1; // pressed
        hilog.info(DOMAIN, MODULE, "Touch Down: x=%{public}d, y=%{public}d", x, y);
        // Send focus event on touch down
        arkui_cef.cefSendFocusEvent(this.browserId, true);
        // Send both touch event and mouse simulation for compatibility
        arkui_cef.cefSendTouchEvent(this.browserId, x, y, touchType, touch.id);
        arkui_cef.cefSendMouseMove(this.browserId, x, y, false);
        // Set EVENTFLAG_LEFT_MOUSE_BUTTON (0x10) on touch-down mouse click simulation
        arkui_cef.cefSendMouseClick(this.browserId, x, y, 0, false, 1, 0x10);
        break;
      case TouchType.Up:
        touchType = 0; // released
        hilog.info(DOMAIN, MODULE, "Touch Up: x=%{public}d, y=%{public}d", x, y);
        arkui_cef.cefSendTouchEvent(this.browserId, x, y, touchType, touch.id);
        arkui_cef.cefSendMouseClick(this.browserId, x, y, 0, true, 1, 0);
        break;
      case TouchType.Move:
        touchType = 2; // moved
        arkui_cef.cefSendTouchEvent(this.browserId, x, y, touchType, touch.id);
        // During touch drag, set EVENTFLAG_LEFT_MOUSE_BUTTON so CEF tracks drag state
        arkui_cef.cefSendMouseMove(this.browserId, x, y, false, 0x10);
        break;
      case TouchType.Cancel:
        touchType = 3; // cancelled
        arkui_cef.cefSendTouchEvent(this.browserId, x, y, touchType, touch.id);
        arkui_cef.cefSendMouseMove(this.browserId, x, y, true);
        arkui_cef.cefSendCaptureLost(this.browserId);
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

    // ArkUI AxisEvent: positive verticalAxis = scroll up, negative = scroll down.
    // CEF: positive deltaY = scroll DOWN. So negate.
    let rawDeltaY = -event.getVerticalAxisValue() * OsrCefView.WHEEL_SCALE;
    let rawDeltaX = -event.getHorizontalAxisValue() * OsrCefView.WHEEL_SCALE;

    // Accumulate small deltas to filter out inertia/momentum events
    this.accumulatedWheelY += rawDeltaY;
    this.accumulatedWheelX += rawDeltaX;

    // Only send when accumulated delta exceeds threshold — this filters out
    // the "small slide then big slide" two-stage behavior
    if (Math.abs(this.accumulatedWheelY) >= OsrCefView.WHEEL_ACCUM_THRESHOLD ||
        Math.abs(this.accumulatedWheelX) >= OsrCefView.WHEEL_ACCUM_THRESHOLD) {
      let deltaY = Math.round(this.accumulatedWheelY);
      let deltaX = Math.round(this.accumulatedWheelX);

      // Reset accumulator after sending
      this.accumulatedWheelY = 0;
      this.accumulatedWheelX = 0;

      hilog.info(DOMAIN, MODULE, "Axis: x=%{public}d, y=%{public}d, dX=%{public}d, dY=%{public}d",
        x, y, deltaX, deltaY);
      arkui_cef.cefSendMouseWheel(this.browserId, x, y, deltaX, deltaY);
    }
  }

  // ==================== Key Event (键盘) ====================
  handleKeyEvent(event: KeyEvent): void {
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
