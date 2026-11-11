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
  // 拖出阈值（与设计 §3 一致，借鉴 Chromium kMinimumDragDistance）
  private static readonly DRAG_OUT_EDGE_PX: number = 24;
  private static readonly DRAG_OUT_MIN_DRAG_VP: number = 10;

  private cefContent: NodeContent = new NodeContent();
  @State private browserId: number = -1;
  @State private cefCreating: boolean = false;

  @State private viewWidth: number = -1;
  @State private viewHeight: number = -1;

  // 网页拖出后源视图占位态：NodeContent 中的节点保留（cefHide 隐藏），
  // adopt 成功后由 napi 侧移除；abort 时恢复显示。
  @State private released: boolean = false;

  @Prop viewUrl: string = '';
  private interceptFocusKeys: boolean = false;

  private dragOutEnabled: boolean = false;
  private dragOutWindow: window.Window | null = null;
  // session 源登记令牌（0=未登记）
  private sessionSourceKey: number = 0;
  // windowRectChange 订阅句柄（销毁时精准反注册，避免影响同窗其他订阅）
  private dragOutRectHandler: ((data: window.RectChangeOptions) => void) | undefined = undefined;
  // 检测矩形缓存：内容区原点（相对窗口左上，即标题栏偏移）与尺寸（px）
  private dragOutContentOffsetX: number = 0;
  private dragOutContentOffsetY: number = 0;
  private dragOutContentW: number = 0;
  private dragOutContentH: number = 0;

  onCreatedCallback: CreatedCallback | undefined = undefined;
  onTitleChangeCallback: TitleChangeCallback | undefined = undefined;
  private onDragOutCallback: DragOutCallback | undefined = undefined;

  build() {
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

  aboutToDisappear(): void {
    // 精准反注册 windowRectChange 订阅（P3-1）：组件销毁后窗口仍在，若不
    // off，dragOutRectHandler 闭包持有本实例（含 dragOutWindow 引用）造成
    // 泄漏 + 后续回调打到已销毁组件上。其余销毁语义（destroy 由外部显式
    // 调用）不变。
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
      // browser 已迁往新窗口宿主：旧父 Stack 已被 adoptCefView 移除销毁，
      // 这里只解除 session 登记并复位本地状态，不销毁 browser。
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
    // aboutToAppear 在 onAreaChange 之前触发，
    // 所以 aboutToAppear 中创建 CEF 时还不知道正确的大小。
    // 如果在 aboutToAppear 中创建 CEF，
    // 需要在完成创建后 resize 一次，让 CEF 变成正确的大小。
    arkui_cef.cefResize(this.browserId, this.viewWidth, this.viewHeight);
    if (this.onCreatedCallback) {
      this.onCreatedCallback(this);
    }
  }

  // ==================== 拖出（tear-off）支持 ====================

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
        // 窗口移动/缩放时同步检测矩形（含 drawableRect 与 windowRect 差值）；
        // 幂等：同窗口重复 enableDragOut 只订阅一次
        if (this.dragOutRectHandler === undefined) {
          this.dragOutRectHandler = (data: window.RectChangeOptions) => {
            this.refreshDragOutRectFromWindow(win);
          };
          win.on('windowRectChange', this.dragOutRectHandler);
        }
        // 登记源视图信息（供新窗 adopt 取 oldNodeContent、abort 恢复、关闭迁回）
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
      // 实测（2in1 自由窗）：windowRect.left/top 为屏幕绝对坐标，而
      // drawableRect.left/top 恒为 (0,0)（窗口相对），直接相减会得到
      // 负偏移（如 -515/-211），把边缘触发带整体错位到窗口中部甚至
      // 窗外。钳到 >= 0：本平台归零（drawable 尺寸 == 窗口尺寸），
      // 标题栏真实存在的窗口上差值为正、语义保持不变。
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
    // 终止网页内拖拽态 + 视觉移除（browser 保留，设计 §5.1 T2a）
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
          // 对反向迁移产生的新父 Stack 重新注册检测 + 刷新矩形 +
          // 重新登记源信息（幂等：windowRectChange 订阅不重复）
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
    this.interceptFocusKeys = false;
  }
}
