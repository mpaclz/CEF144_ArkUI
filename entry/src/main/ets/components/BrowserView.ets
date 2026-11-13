import { CefView } from './CefView'
import { hilog } from '@kit.PerformanceAnalysisKit';
import { getDragOutSession, DragOutInfo } from '../common/DragOutSession'

const DOMAIN = 0xFF01;
const MODULE = 'BrowserView'

@Component
export struct BrowserView {
  private static DEFAULT_HINT: string = '此处显示 CEF';
  @Prop defaultUrl: string = '';

  @State private cefView: CefView | undefined = undefined;
  @State private hint: string = BrowserView.DEFAULT_HINT;

  // 实例唯一后缀：工具栏末按钮的 focus id 必须全页唯一（多实例场景），
  // 供 CefView 在 Shift+Tab 流出网页边界时（OnTakeFocus next=false）定向
  // 移焦（一键直达工具栏末按钮——网页视图走焦顺序上的前一个停靠点）。
  // 正向（next=true）不接线：落到 CefView 的惰性边界锚点，下一次 Tab 由
  // 框架遍历环绕进 TabBar，与 OSR 视图的流转语义一致。
  private static uidCounter: number = 0;
  private uid: number = 0;
  private toolbarLastKey: string = '';

  aboutToAppear(): void {
    BrowserView.uidCounter += 1;
    this.uid = BrowserView.uidCounter;
    this.toolbarLastKey = 'bvToolbarLast_' + this.uid;
  }

  build() {
    Column() {
      Row() {
        Button('后退', { type: ButtonType.Normal, stateEffect: true })
          .fontSize(10)
          .height(22)
          .fontColor(Color.White)
          .backgroundColor('#409EFF')
          .onClick(() => {
            this.cefView?.goBack();
          })
        Blank().width(6)
        Button('前进', { type: ButtonType.Normal, stateEffect: true })
          .fontSize(10)
          .height(22)
          .fontColor(Color.White)
          .backgroundColor('#409EFF')
          .onClick(() => {
            this.cefView?.goForward();
          })
        Blank().width(6)
        Button('刷新', { type: ButtonType.Normal, stateEffect: true })
          .fontSize(10)
          .height(22)
          .fontColor(Color.White)
          .backgroundColor('#409EFF')
          .onClick(() => {
            this.cefView?.reload();
          })
        Blank().width(6)
        Button('隐藏', { type: ButtonType.Normal, stateEffect: true })
          .fontSize(10)
          .height(22)
          .fontColor(Color.White)
          .backgroundColor('#67C23A')
          .onClick(() => {
            this.cefView?.hide();
            if (this.cefView?.isCreated()) {
              this.hint = 'CEF 已隐藏'
            } else {
              this.hint = 'CEF 未创建'
            }
          })
        Blank().width(6)
        Button('显示', { type: ButtonType.Normal, stateEffect: true })
          .fontSize(10)
          .height(22)
          .fontColor(Color.White)
          .backgroundColor('#67C23A')
          .onClick(() => {
            this.cefView?.show();
            if (this.cefView?.isCreated()) {
              this.hint = 'CEF 已显示'
            } else {
              this.hint = 'CEF 未创建'
            }
          })
        Blank().width(6)
        Button('创建', { type: ButtonType.Normal, stateEffect: true })
          .fontSize(10)
          .height(22)
          .fontColor(Color.White)
          .backgroundColor('#409EFF')
          .onClick(() => {
            if (!this.cefView?.isCreated()) {
              this.cefView?.loadUrl(this.defaultUrl);
              this.hint = BrowserView.DEFAULT_HINT;
            }
          })
        Blank().width(6)
        Button('销毁', { type: ButtonType.Normal, stateEffect: true })
          .fontSize(10)
          .height(22)
          .fontColor(Color.White)
          .backgroundColor('#F56C6C')
          .onClick(() => {
            if (!this.cefView?.isCreated()) {
              this.hint = 'CEF 未创建'
            } else {
              this.cefView?.destroy();
              this.hint = 'CEF 已销毁'
            }
          })
        Blank().width(6)
        Button('重复创建', { type: ButtonType.Normal, stateEffect: true })
          .fontSize(10)
          .height(22)
          .fontColor(Color.White)
          .backgroundColor('#F56C6C')
          .id(this.toolbarLastKey)
          .onClick(async () => {
            hilog.info(DOMAIN, MODULE, "Start testing.");
            for (let i: number = 0; i < 50; i++) {
              this.cefView?.destroy();
              this.hint = `CEF 已销毁（第${i}次）`
              await this.cefView?.sleep(150);
              this.cefView?.loadUrl(this.defaultUrl);
              this.hint = `CEF 已创建（第${i}次）`
              await this.cefView?.sleep(300);
            }
            hilog.info(DOMAIN, MODULE, "Finish testing.");
          })
      }
      .width('100%')
      .height('5%')
      .alignItems(VerticalAlign.Center)
      Row() {
        Stack() {
          Text(this.hint).fontSize(32).fontColor(Color.White).textAlign(TextAlign.Center)
            .width('100%').height('100%')
            .backgroundColor(Color.Gray)
          CefView({viewUrl: this.defaultUrl,
            focusPrevId: this.toolbarLastKey,
            onCreatedCallback: (view) => {
            hilog.info(DOMAIN, MODULE, "CefView created, id: %{public}d", view.getBrowserId())
            this.cefView = view;
            // 拖出支持：触发后视图自行完成 capture-lost/hide/占位，
            // 会话负责拉起新窗（adopt/跟随/恢复均由 session 驱动）
            view.enableDragOut((info: DragOutInfo) => {
              getDragOutSession().startDragOut(info);
            });
          }}).width('100%').height('100%')
        }

      }
      .width('100%')
      .height('95%')
    }
    .height('100%')
    .width('100%')
  }
}
