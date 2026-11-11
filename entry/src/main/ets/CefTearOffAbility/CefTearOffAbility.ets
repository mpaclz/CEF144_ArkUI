import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { window } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { getDragOutSession } from '../common/DragOutSession';

const DOMAIN = 0xFF01;
const MODULE = 'CefTearOffAbility';

/**
 * 拖出形成的新主窗 UIAbility（launchType "specified" + instanceKey 每次唯一，
 * 由 MyAbilityStage.onAcceptWant 透传 key，每次拉出都是新实例）。
 * 独立简单实现（不继承 web_engine 的 WebAbility）：
 * onCreate 解析 want 参数（诊断用，会话数据以进程内 DragOutSession 为准）
 * 并登记 context；onWindowStageCreate 加载 TearOffPage；onWindowStageDestroy
 * 通知会话（adopt 未完成时 abort 恢复源视图）。
 */
export default class CefTearOffAbility extends UIAbility {
  private browserId: number = -1;

  onCreate(want: Want, _launchParam: AbilityConstant.LaunchParam): void {
    let params = want.parameters;
    let browserId = Number(params?.sourceBrowserId);
    let mode = String(params?.mode);
    let viewW = Number(params?.viewW);
    let viewH = Number(params?.viewH);
    this.browserId = Number.isNaN(browserId) ? -1 : browserId;
    hilog.info(DOMAIN, MODULE,
      "onCreate: browserId=%{public}d, mode=%{public}s, view=%{public}dx%{public}d",
      this.browserId, mode,
      Number.isNaN(viewW) ? 0 : viewW, Number.isNaN(viewH) ? 0 : viewH);
    getDragOutSession().setTearOffContext(this.context);
  }

  onWindowStageCreate(windowStage: window.WindowStage): void {
    hilog.info(DOMAIN, MODULE, "onWindowStageCreate");
    windowStage.loadContent('pages/TearOffPage', (err) => {
      if (err.code) {
        hilog.error(DOMAIN, MODULE, "loadContent failed: %{public}s", JSON.stringify(err));
        return;
      }
      hilog.info(DOMAIN, MODULE, "loadContent ok");
      // 自由窗装饰（标题栏），参照 CefPopupAbility.setTitleBarVisible
      try {
        let win = windowStage.getMainWindowSync();
        win.setWindowDecorVisible(true);
      } catch (e) {
        hilog.error(DOMAIN, MODULE, "setWindowDecorVisible failed: %{public}s", (e as Error).message);
      }
    });
  }

  onWindowStageDestroy(): void {
    hilog.info(DOMAIN, MODULE, "onWindowStageDestroy");
    getDragOutSession().onTearOffWindowDestroyed();
  }

  onForeground(): void {
    hilog.info(DOMAIN, MODULE, "onForeground");
  }

  onBackground(): void {
    hilog.info(DOMAIN, MODULE, "onBackground");
  }

  /**
   * 用户点 × 关闭新窗（PC/2in1）。文档语义（已核实）：返回 true = 取消
   * 本次关闭流程，返回 false = 继续正常关闭——此前返回 true 导致窗口
   * 永远关不掉。正确姿势：放行关闭前先 await 迁回（把浏览器迁回源窗口、
   * 恢复原布局显示），然后 return false。
   */
  async onPrepareToTerminateAsync(): Promise<boolean> {
    hilog.info(DOMAIN, MODULE, "onPrepareToTerminateAsync: browserId=%{public}d", this.browserId);
    let restored = await getDragOutSession().handleTearOffWindowClose(this.browserId);
    hilog.info(DOMAIN, MODULE,
      "onPrepareToTerminateAsync: restore result=%{public}s", restored ? "true" : "false");
    return false;
  }
}
