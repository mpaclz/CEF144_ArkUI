import { WebProxy } from "../interface/WebProxy";
import { AbilityConstant, common, UIAbility, Want } from "@kit.AbilityKit";
import Inject from '../common/InjectModule';
import { window } from "@kit.ArkUI";
import LogUtil from "../utils/LogUtil";
import { BusinessError } from '@kit.BasicServicesKit';
import { ChangeEventType, NativeContext } from "../interface/CommonInterface";
import JsBindingUtils from "../utils/JsBindingUtils";
import { ContextType } from "../common/Constants";
import { Configuration } from '@ohos.app.ability.Configuration';
import { AbilityManager } from "../common/AbilityManager";
import { DisplayAdapter } from "../adapter/DisplayAdapter";
import { NativeThemeAdapter } from "../adapter/NativeThemeAdapter";
import { PermissionManagerAdapter } from "../adapter/PermissionManagerAdapter";
import AbilityIdController from "../utils/AbilityIdController";
import { ContextAdapter } from "../adapter/ContextAdapter";
import LogMethod from "../common/LogDecorator";

const TAG: string = 'WebAbilityHost';

export class WebAbilityHost extends UIAbility implements WebProxy {

  protected abilityId: string = '';
  protected mainWindowName: string = '';
  protected windowStage: window.WindowStage | undefined = undefined;
  protected originWindowId: number = -1;
  protected nativeContext: NativeContext =
    JsBindingUtils.getNativeContext(ContextType.kMainProcess);

  protected abilityManager: AbilityManager | undefined = undefined;
  protected contextAdapter: ContextAdapter | undefined = undefined;
  protected displayAdapter : DisplayAdapter | undefined = undefined;
  protected nativeThemeAdapter: NativeThemeAdapter | undefined = undefined;

  @LogMethod
  onConfigurationUpdate(newConfig: Configuration) {
    this.displayAdapter?.onSystemFontSizeChange(newConfig.fontSizeScale);
    this.nativeThemeAdapter?.setSystemNativeTheme(newConfig.colorMode);
  }

  @LogMethod
  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    this.abilityManager = Inject.get(AbilityManager);
    this.contextAdapter = Inject.get(ContextAdapter);
    this.nativeThemeAdapter = Inject.get(NativeThemeAdapter);
    this.displayAdapter = Inject.get(DisplayAdapter);
    Inject.get(PermissionManagerAdapter).initPermissions();

    this.abilityId = AbilityIdController.acquireUniqueId();

    this.nativeContext.OnAbilityCreated(this.abilityId);
  }

  onWindowStageCreate(windowStage: window.WindowStage) {
    this.windowStage = windowStage;
    let mainWindow = this.getWindow();
    if (!mainWindow) {
      LogUtil.error(TAG, `${this.abilityId} can not get main window.`);
    } else {
      try {
        let windowProp = mainWindow.getWindowProperties()
        this.originWindowId = windowProp.id;
        if (windowProp.name) {
          this.mainWindowName = windowProp.name;
        }
      } catch (error) {
        LogUtil.error(TAG, `${this.abilityId} can not get main window property.`);
      }

      try {
        mainWindow.on("windowEvent", (type) => {
          this.onWindowEvent(type);
        })
      } catch (e) {
        LogUtil.error(TAG, `${this.abilityId} listen to windowEvent failed: ${e.message}`);
      }

      this.abilityManager?.onWindowCreated(mainWindow, this);
    }

    this.abilityManager?.addProxy(this);
    LogUtil.error(TAG, `WebAbilityHost`);
  }

  @LogMethod
  onDestroy(): void | Promise<void> {
    this.nativeContext.OnAbilityDestroyed(this.abilityId)
  }

  @LogMethod
  onWindowEvent(type: window.WindowEventType) {
    LogUtil.warn(TAG, `${this.abilityId} window event: ${JSON.stringify(type)}`);
    if (type == window.WindowEventType.WINDOW_ACTIVE) {
      this.contextAdapter?.setActiveWindow(this.abilityId);
    }
    this.nativeContext.OnWindowEvent(this.abilityId, type.valueOf());
  }

  // WebProxy
  getWindowStage(): window.WindowStage | undefined {
    return this.windowStage;
  }

  getWindow(): window.Window | undefined {
    try {
      return this.windowStage?.getMainWindowSync();
    } catch (error) {
      LogUtil.error(TAG, `Get main window ${this.abilityId} Object error: ${error.msg}. Call stack ${error.stack}`);
    }
    return undefined;
  }

  getWindowContext(): common.Context {
    return this.context;
  }

  getWindowId(): string {
    return this.abilityId;
  }

  closeWindow() {}

  setWindowTitle(title: string) {}

  @LogMethod
  createSubWindow(name: string): Promise<window.Window> {
    return new Promise<window.Window>((resolve, reject) => {
      if (!this.windowStage) {
        reject(`${this.abilityId} WindowStage is undefined.`);
        return;
      }
      this.windowStage.createSubWindow(name)
        .then((data: window.Window) => {
          resolve(data);
        }).catch((error: BusinessError) => {
        reject(error);
      })
    });
  }

  getOriginWindowId(): number {
    return this.originWindowId;
  }

  getIsQtChild(): boolean {
    return true;
  }

  isUIAbility(): boolean {
    return true;
  }

  updateAvoidAreaStatus(isFullScreen: boolean) {}

  getAbilityId(): string {
    return this.abilityId;
  }

  setDeviceModeChangeEvent(deviceModeChangeEvent: ChangeEventType | undefined) {

  }
}
