import { common, Want, StartOptions } from '@kit.AbilityKit';
import { window, NodeContent } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import arkui_cef from 'libarkui_cef.so';

const DOMAIN = 0xFF01;
const MODULE = 'DragOutSession';

export type DragOutMode = 'windowed' | 'osr';
export type DragOutState = 'idle' | 'tearing' | 'following' | 'settled';

/** 拖出触发信息（检测层产出，坐标全 px）。 */
export interface DragOutInfo {
  browserId: number;
  mode: DragOutMode;
  /** 按下点相对视图左上角（px），即新窗内光标偏移的基础值 */
  pressLocalX: number;
  pressLocalY: number;
  /** 按下点屏幕坐标（px） */
  pressScreenX: number;
  pressScreenY: number;
  /** 触发时光标屏幕坐标（px） */
  curScreenX: number;
  curScreenY: number;
  /** 视图尺寸（px） */
  viewW: number;
  viewH: number;
  /** 源窗口 id */
  sourceWindowId: number;
}

/** 源视图注册到会话的回调（abort 恢复 / settle 固化 / 关闭新窗迁回）。 */
export interface DragOutSourceCallbacks {
  /** 拉出失败/超时后恢复源视图显示（cefShow / released=false） */
  onRestore: () => void;
  /** 拉出成功 settle 后固化源视图占位态 */
  onSettled: () => void;
  /**
   * 关闭新主窗时把浏览器迁回本视图（迁回 = 反向迁移）。
   * windowed：tearOffContent 为新窗 TearOffPage 的 NodeContent，本视图
   * 反向调用 adoptCefView（旧=TearOff 内容，新=本视图内容）；
   * OSR：tearOffContent 恒为 undefined，本视图重绑回调并重新挂载渲染面。
   * 完成后必须恰好回调 done(ok) 一次。
   */
  onTakeBack: (tearOffContent: NodeContent | undefined, done: (ok: boolean) => void) => void;
}

/** TearOff 新窗侧注册的钩子（OSR 模式迁回前先解除新窗表面）。 */
export interface TearOffHooks {
  /** OSR 迁回前置：解除新窗 OsrCefView 的 surface（防误删源视图的新绑定） */
  releaseBrowser: () => void;
}

/** 源视图注册信息（windowed 需携带 NodeContent 供 adopt 使用）。 */
export interface DragOutSourceRegistration {
  browserId: number;
  mode: DragOutMode;
  context: common.UIAbilityContext;
  /** windowed 模式必填：源视图的 NodeContent（内含旧父 Stack） */
  nodeContent?: NodeContent;
  callbacks: DragOutSourceCallbacks;
}

/** 内部存储条目：key 用于防止迁移后旧视图的注销误删新视图的登记。 */
interface StoredSource {
  reg: DragOutSourceRegistration;
  key: number;
}

/** want.parameters 传递给 CefTearOffAbility 的参数（均为运行期数值兜底）。 */
export interface TearOffWantParams {
  instanceKey: string;
  sourceBrowserId: number;
  mode: DragOutMode;
  viewW: number;
  viewH: number;
  offsetX: number;
  offsetY: number;
}

/**
 * 拖出会话管理器（进程级单例，两个 UIAbility 实例同进程共享）。
 *
 * 状态机（设计 §3）：
 *   idle --startDragOut--> tearing --adopt 完成--> following --释放/失败--> settled -> idle
 *   tearing --超时/adopt 失败/startAbility 失败/新窗销毁--> abort（恢复源视图）-> idle
 *
 * 并发保护：全局同一时刻只允许一个非 idle 会话；源视图按 browserId 注册
 * （多 CefView/OsrCefView 实例并存时各自登记，触发时按 browserId 匹配）。
 */
export class DragOutSessionManager {
  // 必须严格大于 napi 侧 reparent 的 3s 上限（cef_ohos_reparent_browser_host
  // 阻塞等待 CEF UI 线程最多 3s）：若同为 3s，会话超时 abort（terminate 新窗）
  // 可能与 reparent 恰好完成撞车，产生双 3s 边界竞争（adopt 完成回调晚于
  // abort 到达）。5s 给迁移留出明确的先后窗口。
  private static readonly LAUNCH_TIMEOUT_MS: number = 5000;
  /** 关闭迁回（反向迁移）超时：须大于 napi reparent 3s 上限 + 页面销毁余量。 */
  private static readonly RESTORE_TIMEOUT_MS: number = 6000;

  private state: DragOutState = 'idle';
  private sources: Map<number, StoredSource> = new Map();
  private sourceKeyCounter: number = 0;

  // 当前会话参数
  private browserId: number = -1;
  private mode: DragOutMode = 'windowed';
  private viewW: number = 0;
  private viewH: number = 0;
  /** 光标相对新窗口左上角偏移（px，含标题栏补偿），startMoving 直接使用 */
  private followOffsetX: number = 0;
  private followOffsetY: number = 0;
  private newWindow: window.Window | undefined = undefined;
  private osrAdopted: boolean = false;
  private tearOffContext: common.UIAbilityContext | undefined = undefined;
  private timeoutId: number = -1;
  // 源窗口矩形跟踪（源窗 windowRectChange 回调更新，落点判定/诊断用）
  private sourceWindowRect: window.Rect | undefined = undefined;
  // windowed 迁回所需：新窗 TearOffPage 的 NodeContent（notifyWindowReady
  // 登记；reset() 有意保留，供 settled 之后的关闭迁回使用）。
  private tearOffNodeContent: NodeContent | undefined = undefined;
  // OSR 迁回前置钩子：TearOff 实例注册（adopt 模式 aboutToAppear 登记）。
  private tearOffHooks: TearOffHooks | undefined = undefined;
  // "browser 存活在别处（源视图），新窗关闭时不得销毁" 的集合：
  // abort 恢复（browser 回源视图）与迁回成功（takeBack）时加入；源视图
  // 注销登记时移除。取代旧的 lastSettledBrowserId 门控。
  private keptAliveBrowsers: Set<number> = new Set();

  getState(): DragOutState {
    return this.state;
  }

  getBrowserId(): number {
    return this.browserId;
  }

  getMode(): DragOutMode {
    return this.mode;
  }

  /**
   * 源视图（CefView/OsrCefView）登记；可重复调用覆盖同 browserId 项。
   * 返回登记令牌 key，供 unregisterSource 防误删（迁移后同 browserId
   * 由新视图重新登记，旧视图销毁时只注销自己那次登记）。
   */
  registerSource(reg: DragOutSourceRegistration): number {
    let key = ++this.sourceKeyCounter;
    this.sources.set(reg.browserId, { reg: reg, key: key });
    hilog.info(DOMAIN, MODULE, "registerSource: browserId=%{public}d mode=%{public}s key=%{public}d",
      reg.browserId, reg.mode, key);
    return key;
  }

  unregisterSource(browserId: number, key: number): void {
    let stored = this.sources.get(browserId);
    if (stored !== undefined && stored.key === key) {
      this.sources.delete(browserId);
      this.keptAliveBrowsers.delete(browserId);
      hilog.info(DOMAIN, MODULE, "unregisterSource: browserId=%{public}d key=%{public}d", browserId, key);
    }
  }

  /** TearOff 实例（OSR adopt 模式）登记迁回前置钩子。 */
  setTearOffHooks(hooks: TearOffHooks): void {
    this.tearOffHooks = hooks;
  }

  /** 源窗 windowRectChange 时更新（仅记录，检测矩形由视图自行转发 napi）。 */
  updateSourceWindowRect(rect: window.Rect): void {
    this.sourceWindowRect = rect;
  }

  /** CefTearOffAbility onCreate 时登记自身 context（abort 时 terminateSelf 用）。 */
  setTearOffContext(context: common.UIAbilityContext): void {
    this.tearOffContext = context;
  }

  /**
   * 拖出触发入口（源视图完成 releaseView/releaseBrowser 后调用）。
   * 防重入：非 idle 直接拒绝。
   */
  startDragOut(info: DragOutInfo): void {
    if (this.state !== 'idle') {
      hilog.warn(DOMAIN, MODULE,
        "startDragOut rejected: session busy, state=%{public}s, active=%{public}d",
        this.state, this.browserId);
      // 拒绝时源视图已被隐藏（检测触发先 releaseView 再进本方法），必须
      // 恢复其显示，否则该视图永久停留占位态（P3-2）。
      let stored = this.sources.get(info.browserId);
      if (stored !== undefined) {
        hilog.info(DOMAIN, MODULE,
          "startDragOut: restoring hidden source view, browserId=%{public}d", info.browserId);
        stored.reg.callbacks.onRestore();
      }
      return;
    }
    let stored = this.sources.get(info.browserId);
    let source: DragOutSourceRegistration | undefined = stored ? stored.reg : undefined;
    if (!source || source.mode !== info.mode) {
      hilog.error(DOMAIN, MODULE,
        "startDragOut: no matching source registration, browserId=%{public}d, mode=%{public}s",
        info.browserId, info.mode);
      return;
    }

    this.browserId = info.browserId;
    this.mode = info.mode;
    this.viewW = info.viewW;
    this.viewH = info.viewH;
    this.osrAdopted = false;
    this.newWindow = undefined;
    this.state = 'tearing';
    // 新会话开始：清掉上一轮的迁回资源（旧 NodeContent/钩子已随旧窗销毁）
    this.tearOffNodeContent = undefined;
    this.tearOffHooks = undefined;
    hilog.info(DOMAIN, MODULE,
      "startDragOut: browserId=%{public}d mode=%{public}s view=%{public}dx%{public}d cur=(%{public}d,%{public}d)",
      info.browserId, info.mode, info.viewW, info.viewH, info.curScreenX, info.curScreenY);

    this.timeoutId = setTimeout(() => {
      this.timeoutId = -1;
      this.abort('launch timeout');
    }, DragOutSessionManager.LAUNCH_TIMEOUT_MS);

    this.launchTearOffWindow(source, info);
  }

  /**
   * 组装 want + StartOptions 拉起新主窗（设计 §5.1 T2b）。
   * 标题栏补偿：StartOptions.windowLeft/Top 定位的是含装饰的窗口矩形，
   * startMoving(offsetX, offsetY) 的语义是"预期鼠标位置相对窗口左上角的
   * 偏移"（文档已核实，px 非负），因此偏移 = 按下点(视图内) + 内容区在
   * 窗口内的原点偏移（drawableRect - windowRect，即标题栏/边框尺寸）。
   */
  private async launchTearOffWindow(source: DragOutSourceRegistration, info: DragOutInfo): Promise<void> {
    try {
      let win = await window.getLastWindow(source.context);
      let props = win.getWindowProperties();
      let winRect = props.windowRect;
      let drawable = props.drawableRect;
      // 内容区原点在窗口内的偏移（>=0：标题栏高度、边框宽度）
      let contentOffX = drawable.left - winRect.left;
      let contentOffY = drawable.top - winRect.top;
      // 窗口装饰超出内容区的尺寸（边框/标题栏），用于新窗总尺寸
      let decorW = winRect.width - drawable.width;
      let decorH = winRect.height - drawable.height;
      // 光标相对新窗口左上角（含标题栏）的偏移
      let offsetXWin = Math.round(info.pressLocalX) + contentOffX;
      let offsetYWin = Math.round(info.pressLocalY) + contentOffY;
      if (offsetXWin < 0) {
        offsetXWin = 0;
      }
      if (offsetYWin < 0) {
        offsetYWin = 0;
      }
      this.followOffsetX = offsetXWin;
      this.followOffsetY = offsetYWin;

      let want: Want = {
        bundleName: source.context.abilityInfo.bundleName,
        abilityName: 'CefTearOffAbility',
        parameters: {
          'instanceKey': `tearoff_${info.browserId}_${Date.now()}`,
          'sourceBrowserId': info.browserId,
          'mode': info.mode,
          'viewW': info.viewW,
          'viewH': info.viewH,
          'offsetX': offsetXWin,
          'offsetY': offsetYWin
        }
      };
      let options: StartOptions = {
        windowLeft: Math.round(info.curScreenX) - offsetXWin,
        windowTop: Math.round(info.curScreenY) - offsetYWin,
        windowWidth: Math.round(info.viewW) + decorW,
        windowHeight: Math.round(info.viewH) + decorH
      };
      hilog.info(DOMAIN, MODULE,
        "startAbility: window=(%{public}d,%{public}d,%{public}d,%{public}d) offset=(%{public}d,%{public}d)",
        options.windowLeft, options.windowTop, options.windowWidth, options.windowHeight,
        offsetXWin, offsetYWin);
      await source.context.startAbility(want, options);
    } catch (e) {
      let err = e as BusinessError;
      hilog.error(DOMAIN, MODULE, "startAbility failed: %{public}d %{public}s",
        err.code, err.message);
      this.abort('startAbility failed');
    }
  }

  /**
   * 新窗页面 onAppear 时上报窗口引用；windowed 同时传新 NodeContent，
   * 由会话驱动 adoptCefView。OSR 由 OsrCefView surface attach 后调
   * onAdoptResult(true)。
   */
  notifyWindowReady(newWindow: window.Window, newNodeContent?: NodeContent): void {
    this.newWindow = newWindow;
    if (this.state !== 'tearing') {
      hilog.warn(DOMAIN, MODULE, "notifyWindowReady: state=%{public}s (ignore)", this.state);
      return;
    }
    if (this.mode === 'windowed') {
      // 登记新窗 NodeContent：正向 adopt 用；关闭迁回（反向 adopt）也要用
      this.tearOffNodeContent = newNodeContent;
      let stored = this.sources.get(this.browserId);
      let oldContent = stored ? stored.reg.nodeContent : undefined;
      if (!oldContent || newNodeContent === undefined) {
        hilog.error(DOMAIN, MODULE, "notifyWindowReady: missing node content, browserId=%{public}d",
          this.browserId);
        this.abort('missing node content');
        return;
      }
      hilog.info(DOMAIN, MODULE, "adopt begin: browserId=%{public}d size=%{public}dx%{public}d",
        this.browserId, this.viewW, this.viewH);
      arkui_cef.adoptCefView(this.browserId, oldContent, newNodeContent, this.viewW, this.viewH,
        (ok: boolean) => {
          this.onAdoptResult(ok);
        });
    } else if (this.osrAdopted) {
      // OSR：adopt 已完成、新窗引用刚就绪 → 开始跟随
      this.beginFollow();
    }
  }

  /** adopt 结果回调（windowed 由 adoptCefView 触发，OSR 由 surface attach 触发）。 */
  onAdoptResult(ok: boolean): void {
    if (this.state !== 'tearing') {
      hilog.warn(DOMAIN, MODULE, "onAdoptResult: state=%{public}s (ignore)", this.state);
      return;
    }
    if (!ok) {
      this.abort('adopt failed');
      return;
    }
    if (this.newWindow !== undefined) {
      this.beginFollow();
    } else {
      // 新窗引用尚未就绪（OSR surface attach 早于页面 onAppear），
      // notifyWindowReady 到达后补启跟随。
      this.osrAdopted = true;
      hilog.info(DOMAIN, MODULE, "onAdoptResult: ok, waiting for window ref");
    }
  }

  /** adopt 成功后系统级跟随（startMoving 释放即结束；失败则原地收尾）。 */
  private beginFollow(): void {
    this.clearLaunchTimer();
    this.state = 'following';
    let win = this.newWindow;
    if (win === undefined) {
      this.abort('no window to follow');
      return;
    }
    hilog.info(DOMAIN, MODULE, "beginFollow: offset=(%{public}d,%{public}d)",
      this.followOffsetX, this.followOffsetY);
    win.startMoving(Math.round(this.followOffsetX), Math.round(this.followOffsetY))
      .then(() => {
        hilog.info(DOMAIN, MODULE, "follow ended (release)");
        this.finishDrag();
      })
      .catch((err: BusinessError) => {
        // FallbackFollow：窗口已由 StartOptions 定位在光标处；事件流无法
        // 跨窗续接（源窗事件停发），保持当前位置直接收尾等待用户释放。
        hilog.error(DOMAIN, MODULE, "startMoving failed: %{public}d %{public}s",
          err.code, err.message);
        this.finishDrag();
      });
  }

  /** 跟随结束（释放）→ Settled：固化源视图占位态并复位会话。 */
  private finishDrag(): void {
    if (this.state === 'idle') {
      return;
    }
    hilog.info(DOMAIN, MODULE, "settled: browserId=%{public}d", this.browserId);
    this.state = 'settled';
    let stored = this.sources.get(this.browserId);
    if (stored) {
      stored.reg.callbacks.onSettled();
    }
    this.reset();
  }

  /** 中止会话：恢复源视图 + 终止新窗 + 复位。仅在非 idle 时有效。 */
  abort(reason: string, terminateWindow: boolean = true): void {
    if (this.state === 'idle') {
      return;
    }
    hilog.warn(DOMAIN, MODULE, "abort: %{public}s (browserId=%{public}d, state=%{public}s)",
      reason, this.browserId, this.state);
    // abort 路径：browser 回源视图存活，标记"保活"——新窗关闭时不得销毁。
    this.keptAliveBrowsers.add(this.browserId);
    let stored = this.sources.get(this.browserId);
    if (stored) {
      stored.reg.callbacks.onRestore();
    }
    if (terminateWindow) {
      let ctx = this.tearOffContext;
      if (ctx) {
        ctx.terminateSelf().catch((err: BusinessError) => {
          hilog.error(DOMAIN, MODULE, "terminateSelf failed: %{public}d %{public}s",
            err.code, err.message);
        });
      }
    }
    this.reset();
  }

  /** CefTearOffAbility.onWindowStageDestroy 回调。 */
  onTearOffWindowDestroyed(): void {
    if (this.state === 'tearing') {
      // adopt 未完成时新窗被销毁 → 恢复源视图
      this.abort('window destroyed before adopt');
    } else {
      // following/settled 阶段：用户关闭新窗或跟随已结束，仅复位
      hilog.info(DOMAIN, MODULE, "tear-off window destroyed in state=%{public}s", this.state);
      this.reset();
    }
  }

  /**
   * 销毁门控（TearOffPage / OSR adopt 实例关闭时）：browser 是否存活在
   * 源视图（abort 恢复或迁回成功）——true 时新窗关闭不得销毁 browser。
   */
  shouldKeepBrowserAlive(browserId: number): boolean {
    return this.keptAliveBrowsers.has(browserId);
  }

  /**
   * 关闭新主窗时的迁回编排（CefTearOffAbility.onPrepareToTerminateAsync
   * 在返回 false 放行关闭前 await 本方法）：
   * - adopt 进行中（tearing）→ abort 恢复源视图（不再 terminateSelf，
   *   关闭由 onPrepareToTerminateAsync 返回 false 放行）；
   * - 跟随中（following）→ 先落定（finishDrag）再迁回；
   * - settled/idle → 直接迁回（源登记缺失或迁回失败时返回 false，
   *   TearOffPage 关闭将销毁 browser 兜底）。
   * 返回 true = 迁回成功（源视图已恢复显示）。
   */
  async handleTearOffWindowClose(browserId: number): Promise<boolean> {
    if (browserId <= 0) {
      return false;
    }
    if (this.state !== 'idle' && this.browserId !== browserId) {
      // 另一个拖出会话正在进行：并发 reparent 有风险，不迁回——
      // browser 由 TearOffPage 的销毁门控走 destroy 兜底。
      hilog.warn(DOMAIN, MODULE,
        "handleTearOffWindowClose: session busy with browserId=%{public}d, skip restore for %{public}d",
        this.browserId, browserId);
      return false;
    }
    if (this.state === 'tearing') {
      this.abort('tear-off window closed during tearing', false);
      return false;
    }
    if (this.state === 'following') {
      this.finishDrag();
    }
    return await this.restoreToSource(browserId);
  }

  /**
   * 反向迁移：把浏览器从新窗迁回源视图。
   * OSR 必须先解除新窗表面（tearOffHooks，无条件删除式 detach 会误删
   * 源视图随后的新绑定），再让源视图重绑回调 + 重新挂载渲染面。
   * windowed 直接反向调用 adoptCefView（napi 复用，无需新接口）。
   */
  private restoreToSource(browserId: number): Promise<boolean> {
    let stored = this.sources.get(browserId);
    if (!stored) {
      hilog.warn(DOMAIN, MODULE,
        "restoreToSource: no source registration, browserId=%{public}d", browserId);
      return new Promise<boolean>((resolve) => { resolve(false); });
    }
    if (stored.reg.mode === 'osr' && this.tearOffHooks !== undefined) {
      this.tearOffHooks.releaseBrowser();
    }
    let tearOffContent = this.tearOffNodeContent;
    if (stored.reg.mode === 'windowed' && tearOffContent === undefined) {
      hilog.warn(DOMAIN, MODULE,
        "restoreToSource: no tearOff node content, browserId=%{public}d", browserId);
      return new Promise<boolean>((resolve) => { resolve(false); });
    }
    // 闭包外的非空收窄无法穿透进 Promise 闭包（ArkTS 视捕获的 let 可能
    // 被重赋值）——提取为非可选局部后再捕获。
    let takeBack = stored.reg.callbacks.onTakeBack;
    let session = this;
    return new Promise<boolean>((resolve) => {
      let finished = false;
      let timer = setTimeout(() => {
        if (!finished) {
          finished = true;
          hilog.error(DOMAIN, MODULE,
            "restoreToSource: timeout, browserId=%{public}d", browserId);
          session.tearOffNodeContent = undefined;
          session.tearOffHooks = undefined;
          resolve(false);
        }
      }, DragOutSessionManager.RESTORE_TIMEOUT_MS);
      takeBack(tearOffContent, (ok: boolean) => {
        if (finished) {
          return;
        }
        finished = true;
        clearTimeout(timer);
        if (ok) {
          session.keptAliveBrowsers.add(browserId);
          hilog.info(DOMAIN, MODULE,
            "restoreToSource: ok, browser back at source, browserId=%{public}d", browserId);
        } else {
          hilog.error(DOMAIN, MODULE,
            "restoreToSource: failed, browserId=%{public}d", browserId);
        }
        session.tearOffNodeContent = undefined;
        session.tearOffHooks = undefined;
        resolve(ok);
      });
    });
  }

  private clearLaunchTimer(): void {
    if (this.timeoutId >= 0) {
      clearTimeout(this.timeoutId);
      this.timeoutId = -1;
    }
  }

  private reset(): void {
    this.clearLaunchTimer();
    this.state = 'idle';
    this.browserId = -1;
    this.mode = 'windowed';
    this.viewW = 0;
    this.viewH = 0;
    this.followOffsetX = 0;
    this.followOffsetY = 0;
    this.newWindow = undefined;
    this.osrAdopted = false;
    // keptAliveBrowsers / tearOffNodeContent / tearOffHooks 有意保留：
    // reset（settled）后关闭新窗仍需它们做迁回与销毁门控；下一次
    // startDragOut / 迁回完成时清理。
    // tearOffContext 不清理：新窗 ability 的 context 在其销毁后仍可安全调用
    // terminateSelf（幂等失败）；下一次会话会重新 setTearOffContext。
  }
}

// 模块级单例：同进程两个 UIAbility（源窗 + CefTearOffAbility）import 同一
// 模块路径，天然共享同一实例。
let gDragOutSession: DragOutSessionManager | undefined = undefined;

export function getDragOutSession(): DragOutSessionManager {
  if (gDragOutSession === undefined) {
    gDragOutSession = new DragOutSessionManager();
  }
  return gDragOutSession;
}
