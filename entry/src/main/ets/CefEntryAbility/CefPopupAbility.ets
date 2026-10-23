import arkui_cef from 'libarkui_cef.so';
import window from '@ohos.window';
import Want from '@ohos.app.ability.Want';
import AbilityConstant from '@ohos.app.ability.AbilityConstant';
import { Configuration } from '@ohos.app.ability.Configuration';
import { hilog } from "@kit.PerformanceAnalysisKit";

import { WebAbility } from 'web_engine';

const DOMAIN = 0x0000;
const MODULE = 'CefPopupAbility';

export default class CefPopupAbility extends WebAbility {
  private cefBrowserId: number = -1;

  onConfigurationUpdate(config: Configuration) {
    super.onConfigurationUpdate(config);
  }

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam) {
    super.onCreate(want, launchParam);

    arkui_cef.setPopupBrowserCreatedCallback((cefBrowserId: number) => {
      hilog.info(DOMAIN, MODULE, "Popup browser created: cefBrowserId=%{public}d", cefBrowserId);
      this.cefBrowserId = cefBrowserId;

      arkui_cef.registerPopupTitleCallback(cefBrowserId, (title: string) => {
        hilog.info(DOMAIN, MODULE, "Popup title changed: cefBrowserId=%{public}d, title=%{public}s", cefBrowserId, title);
        this.setWindowTitle(title);
      });
    });
  }

  async onPrepareToTerminateAsync(): Promise<boolean> {
    return await super.onPrepareToTerminateAsync();
  }

  async onDestroy(): Promise<void> {
    await super.onDestroy();

    arkui_cef.unregisterPopupTitleCallback(this.cefBrowserId);
  }

  onWindowStageCreate(windowStage: window.WindowStage) {
    super.onWindowStageCreate(windowStage);
  }

  onWindowStageDestroy() {
    super.onWindowStageDestroy();
  }

  onForeground() {
    super.onForeground();
  }

  onBackground() {
    super.onBackground();
  }

  protected isAppEntry(): boolean {
    return true;
  }

  protected setTitleBarVisible(window: window.Window) {
    window.setWindowDecorVisible(true);
  }
}
