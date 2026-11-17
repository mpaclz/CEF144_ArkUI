import arkui_cef from 'libarkui_cef.so';
import { NodeContent } from "@kit.ArkUI";
import { hilog } from "@kit.PerformanceAnalysisKit";
import { common } from '@kit.AbilityKit';
import { KeyCode } from '@kit.InputKit';

const DOMAIN = 0xFF01;
const MODULE = 'CefView'

type CreatedCallback = (cefView: CefView) => void;
type TitleChangeCallback = (title: string) => void;

@Component
export struct CefView {
  private cefContent: NodeContent = new NodeContent();
  @State private browserId: number = -1;
  @State private cefCreating: boolean = false;

  @State private viewWidth: number = -1;
  @State private viewHeight: number = -1;

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

  onCreatedCallback: CreatedCallback | undefined = undefined;
  onTitleChangeCallback: TitleChangeCallback | undefined = undefined;

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
    if (this.isCreated()) {
      arkui_cef.destroyCefView(this.browserId, this.cefContent);
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
