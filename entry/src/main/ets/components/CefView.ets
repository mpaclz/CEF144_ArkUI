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

  onCreatedCallback: CreatedCallback | undefined = undefined;
  onTitleChangeCallback: TitleChangeCallback | undefined = undefined;

  build() {
    Row() {
      ContentSlot(this.cefContent)
    }
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
      // 拦截 Tab 键和方向键，自行处理焦点移动
      if (event.keyCode === KeyCode.KEYCODE_TAB ||
        event.keyCode === KeyCode.KEYCODE_DPAD_UP ||
        event.keyCode === KeyCode.KEYCODE_DPAD_DOWN ||
        event.keyCode === KeyCode.KEYCODE_DPAD_LEFT ||
        event.keyCode === KeyCode.KEYCODE_DPAD_RIGHT) {
        // 自行处理自绘制空间内的焦点移动
        this.handleInternalFocusMove(event);
        // 返回 true 消费事件，阻止框架自动走焦
        return true;
      }
      // 非走焦按键不拦截，交由框架正常处理
      return false;
    })
    .onFocus(() => {
      this.interceptFocusKeys = true;
    })
  }

  handleInternalFocusMove(event: KeyEvent) {
    if (!this.isCreated()) {
      hilog.warn(DOMAIN, MODULE, "handleInternalFocusMove: browser not created, id: %{public}d", this.browserId);
      return;
    }
    hilog.info(DOMAIN, MODULE, "Focus control key, send it to CEF: keyCode=%{public}d, keyType=%{public}d",
      event.keyCode, event.type);
    arkui_cef.cefSendKeyEvent(this.browserId, event.keyCode, event.type, 0);
  }

  aboutToAppear(): void {
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
      (id) => { this.onCefCreated(id); },
      (title) => { this.onCefTitleChanged(title); },
      (next) => { this.onCefFocusTake(next); });
  }

  onCefCreated(id: number): void {
    hilog.info(DOMAIN, MODULE, "CEF created, browserId=%{public}d", id);
    this.browserId = id;
    this.cefCreating = false;
    // aboutToAppear 在 onAreaChange 之前触发，
    // 所以 aboutToAppear 中创建 CEF 时还不知道正确的大小。
    // 如果在 aboutToAppear 中创建 CEF，
    // 需要在完成创建后 resize 一次，让 CEF 变成正确的大小。
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
    this.interceptFocusKeys = false;
  }
}
