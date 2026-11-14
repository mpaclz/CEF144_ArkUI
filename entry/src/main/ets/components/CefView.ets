import arkui_cef from 'libarkui_cef.so';
import { NodeContent, window } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { hilog } from "@kit.PerformanceAnalysisKit";
import { BusinessError } from '@kit.BasicServicesKit';
import { getDragOutSession, DragOutInfo, DragOutSourceRegistration } from '../common/DragOutSession';
import { KeyCode } from '@kit.InputKit';

const DOMAIN = 0xFF01;
const MODULE = 'CefView'

type CreatedCallback = (cefView: CefView) => void;
type TitleChangeCallback = (title: string) => void;
type DragOutCallback = (info: DragOutInfo) => void;

@Component
export struct CefView {
  // Drag-out threshold (design §3; mirrors Chromium kMinimumDragDistance).
  private static readonly DRAG_OUT_EDGE_PX: number = 24;
  private static readonly DRAG_OUT_MIN_DRAG_VP: number = 10;

  private cefContent: NodeContent = new NodeContent();
  @State private browserId: number = -1;
  @State private cefCreating: boolean = false;

  @State private viewWidth: number = -1;
  @State private viewHeight: number = -1;

  // Source-view placeholder after a drag-out: the nodes inside NodeContent
  // stay (hidden via cefHide); the napi side removes them on adopt success,
  // and abort restores visibility.
  @State private released: boolean = false;

  @Prop viewUrl: string = '';
  private interceptFocusKeys: boolean = false;
  // Landing spots when TAB focus flows out of the web boundary
  // (OnTakeFocus -> the host moves focus). next=true (Tab leaving the page
  // end) -> focusNextId; next=false (Shift+Tab leaving the page start) ->
  // focusPrevId. The container wires them (e.g. BrowserView points the
  // backward landing at its own toolbar's last button id for one-key
  // reach). Unwired directions fall to this view's lazy boundary anchors
  // (focusPreAnchorKey/focusPostAnchorKey, tabIndex(-1): addressable by
  // requestFocus but not part of Tab traversal); the next Tab/Shift+Tab
  // then continues from the boundary via native framework traversal -
  // forward wraps into the TabBar, backward reaches the stop before this
  // view. Same boundary semantics as the OSR view (OsrCefView).
  focusNextId: string = '';
  focusPrevId: string = '';
  // Lazy boundary anchor ids (unique per instance). They live OUTSIDE the
  // Stack (so onKeyPreIme interception is not triggered) and use
  // tabIndex(-1) (not a traversal stop; the wrap-around landing remains
  // the page itself).
  private static uidCounter: number = 0;
  private uid: number = 0;
  private focusPreAnchorKey: string = 'cefAnchorPre';
  private focusPostAnchorKey: string = 'cefAnchorPost';

  private dragOutEnabled: boolean = false;
  private dragOutWindow: window.Window | null = null;
  // Session source registration token (0 = not registered).
  private sessionSourceKey: number = 0;
  // windowRectChange subscription handle (unregistered precisely on
  // dispose so other subscriptions of the same window are untouched).
  private dragOutRectHandler: ((data: window.RectChangeOptions) => void) | undefined = undefined;
  // Detection-rect cache: content-area origin (window-relative, i.e. the
  // title-bar offset) and size (px).
  private dragOutContentOffsetX: number = 0;
  private dragOutContentOffsetY: number = 0;
  private dragOutContentW: number = 0;
  private dragOutContentH: number = 0;

  onCreatedCallback: CreatedCallback | undefined = undefined;
  onTitleChangeCallback: TitleChangeCallback | undefined = undefined;
  private onDragOutCallback: DragOutCallback | undefined = undefined;

  build() {
    Column() {
      // Lazy boundary anchors (the web view's focus-flow boundary).
      // tabIndex(-1): addressable by requestFocus, but Tab traversal never
      // stops on them - a normal focus cycle never notices them. Absolute
      // positioning takes them out of the layout flow (no 1px line height,
      // so the native CEF view is not pushed down into a gap). When TAB
      // flows out of the page on an unwired side, focus moves onto the
      // anchor (the web view as a whole holds focus; the keyboard is back
      // with the host) and the next Tab/Shift+Tab continues traversal from
      // the boundary.
      Text('')
        .focusable(true)
        .tabIndex(-1)
        .id(this.focusPreAnchorKey)
        .width(1)
        .height(1)
        .opacity(0)
        .position({ x: 0, y: 0 })
        .hitTestBehavior(HitTestMode.None)

      Stack() {
        ContentSlot(this.cefContent)
        if (this.released) {
          Column() {
            Text('网页已拖出至新窗口')
              .fontSize(14)
              .fontColor(Color.White)
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.Center)
          .backgroundColor('#666666')
        }
      }
      .width('100%')
      .height('100%')
      .onAreaChange((_oldValue, newValue) => {
      hilog.info(DOMAIN, MODULE, `onAreaChange: width=${newValue.width}, height=${newValue.height}`);
      let wpx = Math.round(this.getUIContext().vp2px(newValue.width as number));
      let hpx = Math.round(this.getUIContext().vp2px(newValue.height as number));
      hilog.info(DOMAIN, MODULE, "onAreaChange: wpx=%{public}d, hpx=%{public}d", wpx, hpx);
      this.viewWidth = wpx;
      this.viewHeight = hpx;

      if (this.viewWidth <= 0 || this.viewHeight <=0) {
        return;
      }
      if (this.isCreated()) {
        hilog.info(DOMAIN, MODULE, "Begin to resize %{public}d : %{public}d X %{public}d.",
          this.browserId, this.viewWidth, this.viewHeight);
        arkui_cef.cefResize(this.browserId, this.viewWidth, this.viewHeight);
      }
      if (this.dragOutEnabled) {
        this.syncDragOutRect();
      }
    })
    .onKeyPreIme((event: KeyEvent) => {
      if (!this.interceptFocusKeys) {
        return false;
      }
      // Intercept Tab and arrow keys; handle focus movement ourselves.
      if (event.keyCode === KeyCode.KEYCODE_TAB ||
        event.keyCode === KeyCode.KEYCODE_DPAD_UP ||
        event.keyCode === KeyCode.KEYCODE_DPAD_DOWN ||
        event.keyCode === KeyCode.KEYCODE_DPAD_LEFT ||
        event.keyCode === KeyCode.KEYCODE_DPAD_RIGHT) {
        // Handle focus movement inside our self-drawn space.
        this.handleInternalFocusMove(event);
        // Return true to consume the event and stop framework auto-focus.
        return true;
      }
      // Do not intercept non-focus keys; let the framework handle them.
      return false;
    })
    .onFocus(() => {
      this.interceptFocusKeys = true;
    })
    .onBlur(() => {
      // Focus left this view's subtree (including the embedded native CEF
      // node): stop intercepting focus keys and let key events return to
      // ArkUI native focus traversal. In-page focus flow is maintained by
      // CEF itself; onFocus re-arms interception when focus re-enters.
      this.interceptFocusKeys = false;
    })

      // Lazy boundary anchor (same as above, the other side of the web
      // view's focus boundary; absolute, takes no layout space).
      Text('')
        .focusable(true)
        .tabIndex(-1)
        .id(this.focusPostAnchorKey)
        .width(1)
        .height(1)
        .opacity(0)
        .position({ x: 0, y: 0 })
        .hitTestBehavior(HitTestMode.None)
    }
    .width('100%')
    .height('100%')
  }

  handleInternalFocusMove(event: KeyEvent) {
    if (!this.isCreated()) {
      hilog.warn(DOMAIN, MODULE, "handleInternalFocusMove: browser not created, id: %{public}d", this.browserId);
      return;
    }
    // Modifier state must reach CEF too: Blink decides forward vs backward
    // focus by Shift (DefaultTabEventHandler: shiftKey -> kBackward); losing
    // the modifier would make SHIFT+TAB circulate forward only.
    let mods = 0;
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
    hilog.info(DOMAIN, MODULE, "Focus control key, send it to CEF: keyCode=%{public}d, keyType=%{public}d, mods=%{public}d",
      event.keyCode, event.type, mods);
    arkui_cef.cefSendKeyEvent(this.browserId, event.keyCode, event.type, mods);
  }

  aboutToAppear(): void {
    CefView.uidCounter += 1;
    this.uid = CefView.uidCounter;
    this.focusPreAnchorKey = 'cefAnchorPre_' + this.uid;
    this.focusPostAnchorKey = 'cefAnchorPost_' + this.uid;
    if (this.browserId <= 0) {
      hilog.info(DOMAIN, MODULE, "Begin to create browser.");
      this.createCefView(this.cefContent, this.viewUrl, this.viewWidth, this.viewHeight)
        .then((id) => {
          hilog.info(DOMAIN, MODULE, "CEF starts to create, id: %{public}d", id);
        })
    }
  }

  aboutToDisappear(): void {
    // Precisely unregister the windowRectChange subscription (P3-1): the
    // window outlives the component; without off(), the dragOutRectHandler
    // closure would keep this instance alive (including its dragOutWindow
    // reference) - a leak - and later callbacks would hit a disposed
    // component. Other dispose semantics (destroy is called explicitly by
    // the outside) are unchanged.
    if (this.dragOutWindow !== null && this.dragOutRectHandler !== undefined) {
      this.dragOutWindow.off('windowRectChange', this.dragOutRectHandler);
    }
  }

  url(url: string): CefView {
    this.viewUrl = url;
    return this;
  }

  getBrowserId(): number {
    return this.browserId;
  }

  isCreated(): boolean {
    return this.browserId > 0;
  }

  hide(): void {
    if (this.isCreated()) {
      arkui_cef.cefHide(this.browserId);
    } else {
      hilog.info(DOMAIN, MODULE, "hide: Browser not created, id: %{public}d", this.browserId);
    }
  }

  show(): void {
    if (this.isCreated()) {
      arkui_cef.cefShow(this.browserId);
    } else {
      hilog.info(DOMAIN, MODULE, "show: Browser not created, id: %{public}d", this.browserId);
    }
  }

  loadUrl(url: string): void {
    if (this.cefCreating) {
      hilog.info(DOMAIN, MODULE, "CEF is creating, can not load URL: %{public}s", url);
      return;
    }
    if (this.browserId <= 0) {
      this.viewUrl = url;
      this.createCefView(this.cefContent, this.viewUrl, this.viewWidth, this.viewHeight)
        .then((id) => {
          hilog.info(DOMAIN, MODULE, "CEF starts to create, id: %{public}d", id);
        })
    } else {
      arkui_cef.cefLoadUrl(this.browserId, url);
    }
  }

  goBack(): void {
    if (this.isCreated()) {
      arkui_cef.cefGoBack(this.browserId);
    } else {
      hilog.info(DOMAIN, MODULE, "goBack: Browser not created, id: %{public}d", this.browserId);
    }
  }

  goForward(): void {
    if (this.isCreated()) {
      arkui_cef.cefGoForward(this.browserId);
    } else {
      hilog.info(DOMAIN, MODULE, "goForward: Browser not created, id: %{public}d", this.browserId);
    }
  }

  reload(): void {
    if (this.isCreated()) {
      arkui_cef.cefReload(this.browserId);
    } else {
      hilog.info(DOMAIN, MODULE, "reload: Browser not created, id: %{public}d", this.browserId);
    }
  }

  destroy(): void {
    if (this.released) {
      // The browser has moved to the new-window host: the old parent Stack
      // was already removed and destroyed by adoptCefView; here we only
      // unregister the session and reset local state, not the browser.
      hilog.info(DOMAIN, MODULE, "destroy: released, skip browser teardown, id: %{public}d", this.browserId);
      getDragOutSession().unregisterSource(this.browserId, this.sessionSourceKey);
      this.sessionSourceKey = 0;
      this.browserId = -1;
      return;
    }
    if (this.isCreated()) {
      arkui_cef.destroyCefView(this.browserId, this.cefContent);
      getDragOutSession().unregisterSource(this.browserId, this.sessionSourceKey);
      this.sessionSourceKey = 0;
      this.browserId = -1;
    } else {
      hilog.info(DOMAIN, MODULE, "destroy: Browser not created, id: %{public}d", this.browserId);
    }
  }

  sleep(delay: number): Promise<void> {
    return new Promise<void>((resolve) => setTimeout(resolve, delay));
  }

  async createCefView(nodeContent: NodeContent, url: string, width: number, height: number): Promise<number> {
    if (this.cefCreating || this.isCreated()) {
      hilog.info(DOMAIN, MODULE, "CEF is creating or created. id: %{public}d", this.browserId);
      return this.browserId;
    }

    while (!arkui_cef.isCefRunning()) {
      await this.sleep(20);
    }
    hilog.info(DOMAIN, MODULE, "Begin to create cef view, url: %{public}s, size: %{public}d X %{public}d",
      url, width, height);
    this.cefCreating = true;
    return arkui_cef.createCefView(nodeContent, url, width, height,
      (id: number) => { this.onCefCreated(id); },
      (title: string) => { this.onCefTitleChanged(title); },
      (next: boolean) => { this.onCefFocusTake(next); });
  }

  onCefCreated(id: number): void {
    hilog.info(DOMAIN, MODULE, "CEF created, browserId=%{public}d", id);
    this.browserId = id;
    this.cefCreating = false;
    // aboutToAppear fires before onAreaChange, so the correct size is not
    // yet known when CEF is created in aboutToAppear. If CEF is created
    // there, resize once after creation so CEF reaches the right size.
    arkui_cef.cefResize(this.browserId, this.viewWidth, this.viewHeight);
    if (this.onCreatedCallback) {
      this.onCreatedCallback(this);
    }
  }

  // ==================== Tear-off (drag-out) support ====================

  /**
   * 启用拖出检测（外部在 browser 创建后调用，如 BrowserView 的
   * onCreatedCallback）。触发时本组件先做 releaseView（capture lost +
   * cefHide + released 占位），再把 DragOutInfo 交给 cb（cb 通常一行接入
   * DragOutSession.startDragOut）。同时向 session 登记本视图的源信息
   * （NodeContent/恢复回调），供新窗 adopt 与 abort 恢复使用。
   */
  enableDragOut(cb: DragOutCallback): void {
    this.onDragOutCallback = cb;
    this.dragOutEnabled = true;
    if (!this.isCreated()) {
      hilog.warn(DOMAIN, MODULE, "enableDragOut: browser not created yet, id: %{public}d", this.browserId);
    }
    this.setupDragOutWatch();
  }

  private setupDragOutWatch(): void {
    if (!this.isCreated()) {
      return;
    }
    let ctx = this.getUIContext().getHostContext();
    if (ctx === undefined) {
      hilog.error(DOMAIN, MODULE, "enableDragOut: no host context");
      return;
    }
    window.getLastWindow(ctx as common.UIAbilityContext)
      .then((win: window.Window) => {
        this.dragOutWindow = win;
        this.refreshDragOutRectFromWindow(win);
        let minDragPx = Math.round(CefView.DRAG_OUT_MIN_DRAG_VP * this.getUIContext().vp2px(1));
        let ok = arkui_cef.cefWatchDragOut(this.browserId, CefView.DRAG_OUT_EDGE_PX, minDragPx,
          this.dragOutContentOffsetX, this.dragOutContentOffsetY,
          this.dragOutContentW, this.dragOutContentH,
          this.viewWidth > 0 ? this.viewWidth : 0, this.viewHeight > 0 ? this.viewHeight : 0,
          (browserId: number, pressLocalX: number, pressLocalY: number,
            pressScreenX: number, pressScreenY: number, curScreenX: number, curScreenY: number) => {
            this.onNativeDragOut(browserId, pressLocalX, pressLocalY,
              pressScreenX, pressScreenY, curScreenX, curScreenY);
          });
        hilog.info(DOMAIN, MODULE, "cefWatchDragOut: ok=%{public}s, id=%{public}d",
          ok ? "true" : "false", this.browserId);
        // Keep the detection rect in sync on window move/resize (including
        // the drawableRect vs windowRect delta); idempotent: repeated
        // enableDragOut on the same window subscribes only once.
        if (this.dragOutRectHandler === undefined) {
          this.dragOutRectHandler = (data: window.RectChangeOptions) => {
            this.refreshDragOutRectFromWindow(win);
          };
          win.on('windowRectChange', this.dragOutRectHandler);
        }
        // Register source-view info (for the new window's adopt to fetch
        // oldNodeContent, abort restore, and close-migration home).
        let registration: DragOutSourceRegistration = {
          browserId: this.browserId,
          mode: 'windowed',
          context: ctx as common.UIAbilityContext,
          nodeContent: this.cefContent,
          callbacks: {
            onRestore: () => { this.restoreReleasedView(); },
            onSettled: () => {
              hilog.info(DOMAIN, MODULE, "drag-out settled, browserId=%{public}d", this.browserId);
            },
            onTakeBack: (tearOffContent: NodeContent | undefined, done: (ok: boolean) => void) => {
              this.takeBackFromTearOff(tearOffContent, done);
            }
          }
        };
        this.registerSessionSourceWithKey(registration);
      })
      .catch((err: BusinessError) => {
        hilog.error(DOMAIN, MODULE, "enableDragOut: getLastWindow failed: %{public}s", err.message);
      });
  }

  /** 覆盖式登记（重复 enableDragOut 时更新令牌）。 */
  private registerSessionSourceWithKey(registration: DragOutSourceRegistration): void {
    this.sessionSourceKey = getDragOutSession().registerSource(registration);
  }

  /** 从窗口属性刷新检测矩形缓存并同步给 napi。 */
  private refreshDragOutRectFromWindow(win: window.Window): void {
    try {
      let props = win.getWindowProperties();
      let drawable = props.drawableRect;
      let winRect = props.windowRect;
      // Measured (2-in-1 free windows): windowRect.left/top are absolute
      // screen coordinates while drawableRect.left/top are always (0,0)
      // (window-relative), so a direct subtraction yields negative offsets
      // (e.g. -515/-211) that misplace the whole edge trigger band into the
      // middle of the window or outside it. Clamp to >= 0: on this platform
      // it zeroes out (drawable size == window size); on windows with a
      // real title bar the delta is positive and the semantics hold.
      this.dragOutContentOffsetX = Math.max(0, drawable.left - winRect.left);
      this.dragOutContentOffsetY = Math.max(0, drawable.top - winRect.top);
      this.dragOutContentW = drawable.width;
      this.dragOutContentH = drawable.height;
      getDragOutSession().updateSourceWindowRect(winRect);
      hilog.info(DOMAIN, MODULE,
        "dragOut rect: window=(%{public}d,%{public}d,%{public}d,%{public}d), " +
        "drawable=(%{public}d,%{public}d,%{public}d,%{public}d), offset=(%{public}d,%{public}d)",
        winRect.left, winRect.top, winRect.width, winRect.height,
        drawable.left, drawable.top, drawable.width, drawable.height,
        this.dragOutContentOffsetX, this.dragOutContentOffsetY);
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "refreshDragOutRect failed: %{public}s", (e as Error).message);
    }
    this.syncDragOutRect();
  }

  private syncDragOutRect(): void {
    if (this.dragOutEnabled && this.isCreated()) {
      arkui_cef.cefUpdateDragOutRect(this.browserId,
        this.dragOutContentOffsetX, this.dragOutContentOffsetY,
        this.dragOutContentW, this.dragOutContentH,
        this.viewWidth > 0 ? this.viewWidth : 0, this.viewHeight > 0 ? this.viewHeight : 0);
    }
  }

  /** napi 拖出检测触发（参数全 px）。 */
  private onNativeDragOut(browserId: number, pressLocalX: number, pressLocalY: number,
    pressScreenX: number, pressScreenY: number, curScreenX: number, curScreenY: number): void {
    if (!this.isCreated() || this.released) {
      return;
    }
    hilog.info(DOMAIN, MODULE,
      "onDragOut: browserId=%{public}d, press=(%{public}d,%{public}d), cur=(%{public}d,%{public}d)",
      browserId, pressLocalX, pressLocalY, curScreenX, curScreenY);
    // End the in-page drag state + visual removal (browser kept, design
    // §5.1 T2a).
    arkui_cef.cefSendCaptureLost(this.browserId);
    arkui_cef.cefHide(this.browserId);
    this.released = true;
    let sourceWindowId = -1;
    if (this.dragOutWindow !== null) {
      try {
        sourceWindowId = this.dragOutWindow.getWindowProperties().id;
      } catch (e) {
        hilog.error(DOMAIN, MODULE, "getWindowProperties failed: %{public}s", (e as Error).message);
      }
    }
    let info: DragOutInfo = {
      browserId: this.browserId,
      mode: 'windowed',
      pressLocalX: pressLocalX,
      pressLocalY: pressLocalY,
      pressScreenX: pressScreenX,
      pressScreenY: pressScreenY,
      curScreenX: curScreenX,
      curScreenY: curScreenY,
      viewW: this.viewWidth > 0 ? this.viewWidth : 0,
      viewH: this.viewHeight > 0 ? this.viewHeight : 0,
      sourceWindowId: sourceWindowId
    };
    let cb = this.onDragOutCallback;
    if (cb) {
      cb(info);
    }
  }

  /** 会话 abort 时恢复源视图显示。 */
  private restoreReleasedView(): void {
    if (!this.released) {
      return;
    }
    hilog.info(DOMAIN, MODULE, "restoreReleasedView: browserId=%{public}d", this.browserId);
    this.released = false;
    if (this.isCreated()) {
      arkui_cef.cefShow(this.browserId);
    }
  }

  /**
   * 关闭新窗时迁回（反向 adopt）：旧内容 = TearOff 页的 NodeContent，
   * 新内容 = 本视图的 cefContent。napi 会在源内容下新建父 Stack、工作
   * 线程调 CEF reparent、完成后移除并销毁 TearOff 侧父 Stack。成功后
   * 恢复显示并重新对"新父 Stack"注册拖出检测（正向迁移时已注销）。
   */
  private takeBackFromTearOff(tearOffContent: NodeContent | undefined, done: (ok: boolean) => void): void {
    if (!this.isCreated() || !this.released || tearOffContent === undefined || tearOffContent === null) {
      hilog.warn(DOMAIN, MODULE,
        "takeBackFromTearOff: invalid state, id=%{public}d, released=%{public}s",
        this.browserId, this.released ? "true" : "false");
      done(false);
      return;
    }
    let w = this.viewWidth > 0 ? this.viewWidth : 0;
    let h = this.viewHeight > 0 ? this.viewHeight : 0;
    hilog.info(DOMAIN, MODULE,
      "takeBackFromTearOff: browserId=%{public}d, size=%{public}dx%{public}d", this.browserId, w, h);
    arkui_cef.adoptCefView(this.browserId, tearOffContent, this.cefContent, w, h,
      (ok: boolean) => {
        if (ok) {
          this.released = false;
          if (this.isCreated()) {
            arkui_cef.cefShow(this.browserId);
          }
          // Re-register detection on the new parent Stack produced by the
          // reverse migration + refresh the rect + re-register source info
          // (idempotent: no duplicate windowRectChange subscription).
          this.setupDragOutWatch();
        }
        done(ok);
      });
  }

  onCefTitleChanged(title: string): void {
    hilog.info(DOMAIN, MODULE, "CEF title changed: %{public}s", title);
    if (this.onTitleChangeCallback) {
      this.onTitleChangeCallback(title);
    }
  }

  onCefFocusTake(next: boolean): void {
    hilog.info(DOMAIN, MODULE, "CEF focus taken, next=%{public}s", next ? "true" : "false");
    // TAB circulated to the page boundary (end/start): CEF asks the host
    // to move focus out of the web view (QCefView's onCefWindowLostTabFocus
    // -> nextInFocusChain equivalent). Focus MUST be moved actively: the
    // native XComponent is a trap for the focus cycle - an unconsumed TAB
    // gets dispatched to the XComponent and into the renderer, and the
    // ArkUI focus system cannot move focus out of a native node (nor can
    // we copy the OSR "stay on the stop" approach).
    this.interceptFocusKeys = false;
    let wired = next ? this.focusNextId : this.focusPrevId;
    let anchor = next ? this.focusPostAnchorKey : this.focusPreAnchorKey;
    try {
      // Enter the focus-armed state: the user's very next Tab/Shift+Tab
      // moves focus directly (otherwise the first key only activates the
      // focus state without moving).
      this.getUIContext().getFocusController().activate(true);
      if (wired.length > 0) {
        // Wired (e.g. BrowserView's backward landing on the toolbar's last
        // button): one keypress lands there directly.
        this.getUIContext().getFocusController().requestFocus(wired);
        return;
      }
      // Unwired: focus falls to this view's lazy boundary anchor (the web
      // view as a whole holds focus; the keyboard is back with the host).
      // The anchor's tabIndex(-1) keeps it out of traversal and its
      // outside-the-Stack placement avoids interception; the next
      // Tab/Shift+Tab continues from the boundary via native framework
      // traversal - forward wraps into the TabBar, backward reaches the
      // stop before this view.
      this.getUIContext().getFocusController().requestFocus(anchor);
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "focus exit failed (wired=%{public}s, anchor=%{public}s): %{public}s",
        wired, anchor, (e as Error).message);
    }
  }
}
