import { NodeContent, window } from '@kit.ArkUI';
import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { getDragOutSession } from '../common/DragOutSession';
import { OsrCefView } from '../components/OsrCefView';
import arkui_cef from 'libarkui_cef.so';

const DOMAIN = 0xFF01;
const MODULE = 'TearOffPage';

/**
 * 拖出会话的目标页。windowed：ContentSlot(new NodeContent) 交给会话
 * adoptCefView（oldNodeContent 来自源 CefView 的 session 登记）；OSR：
 * OsrCefView(adoptBrowserId) 重绑回调 + surface 接管后由其通知会话。
 * onAppear → getLastWindow → session.notifyWindowReady(win)。
 */
@Entry
@Component
struct TearOffPage {
  @State mode: string = 'windowed';
  @State hasSession: boolean = false;
  @State browserId: number = -1;
  private newContent: NodeContent = new NodeContent();

  aboutToAppear(): void {
    let session = getDragOutSession();
    this.mode = session.getMode();
    this.browserId = session.getBrowserId();
    this.hasSession = this.browserId > 0;
    hilog.info(DOMAIN, MODULE, "aboutToAppear: mode=%{public}s, browserId=%{public}d",
      this.mode, this.browserId);
  }

  /**
   * 页面销毁（用户关新窗 / ability 终止）。关闭迁回已在
   * CefTearOffAbility.onPrepareToTerminateAsync 中完成（await 后才放行
   * 关闭）：browser 已回源视图（shouldKeepBrowserAlive）时不销毁；
   * 迁回失败/未迁回（如无源登记、另一会话占用）时销毁兜底，防止浏览器
   * 无宿主泄漏。OSR 分支同理由 OsrCefView.destroy 的门控处理。
   */
  aboutToDisappear(): void {
    if (this.mode === 'windowed' && this.browserId > 0 &&
        !getDragOutSession().shouldKeepBrowserAlive(this.browserId)) {
      hilog.info(DOMAIN, MODULE,
        "aboutToDisappear: browser not restored, destroying browser id=%{public}d",
        this.browserId);
      arkui_cef.destroyCefView(this.browserId, this.newContent);
    }
  }

  build() {
    Column() {
      if (this.hasSession && this.mode === 'windowed') {
        Stack() {
          ContentSlot(this.newContent)
        }
        .width('100%')
        .height('100%')
        // 窗口尺寸变化（用户拉伸自由窗）同步给 CEF 宿主窗口（P2-4），
        // 换算写法与 CefView.ets 的 onAreaChange 一致（vp → px）。
        .onAreaChange((_oldValue, newValue) => {
          let wpx = Math.round(this.getUIContext().vp2px(newValue.width as number));
          let hpx = Math.round(this.getUIContext().vp2px(newValue.height as number));
          if (this.browserId > 0 && wpx > 0 && hpx > 0) {
            hilog.info(DOMAIN, MODULE, "onAreaChange: resize browser=%{public}d to %{public}dx%{public}d",
              this.browserId, wpx, hpx);
            arkui_cef.cefResize(this.browserId, wpx, hpx);
          }
        })
      } else if (this.hasSession && this.mode === 'osr') {
        OsrCefView({
          adoptBrowserId: this.browserId,
          viewUrl: '',
          myTabIndex: -1,
          activeTabIndex: -1
        })
        .width('100%')
        .height('100%')
      } else {
        // 会话失效（超时/被 abort）：显示占位（ability 会随后被终止）
        Column() {
          Text('拖出会话已失效')
            .fontSize(16)
            .fontColor(Color.Gray)
        }
        .width('100%')
        .height('100%')
        .justifyContent(FlexAlign.Center)
      }
    }
    .width('100%')
    .height('100%')
    .onAppear(() => {
      this.onPageAppear();
    })
  }

  private onPageAppear(): void {
    let session = getDragOutSession();
    let ctx = this.getUIContext().getHostContext();
    if (ctx === undefined) {
      hilog.error(DOMAIN, MODULE, "onAppear: no host context");
      session.abort('TearOffPage no context');
      return;
    }
    window.getLastWindow(ctx as common.UIAbilityContext)
      .then((win: window.Window) => {
        hilog.info(DOMAIN, MODULE, "onAppear: window ready (id=%{public}d)",
          win.getWindowProperties().id);
        session.notifyWindowReady(win, this.mode === 'windowed' ? this.newContent : undefined);
      })
      .catch((err: BusinessError) => {
        hilog.error(DOMAIN, MODULE, "getLastWindow failed: %{public}d %{public}s",
          err.code, err.message);
        session.abort('TearOffPage getLastWindow failed');
      });
  }
}
