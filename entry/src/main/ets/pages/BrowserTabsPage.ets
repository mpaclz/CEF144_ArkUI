import { BrowserView } from '../components/BrowserView'
import { CefView } from '../components/CefView';
import { OsrCefView } from '../components/OsrCefView';
import { getDragOutSession, DragOutInfo } from '../common/DragOutSession';

// ── 标签数据源（注册表 + 渲染源合一）──────────────────────────────────────
// TabDef 数组既驱动 TabContent 渲染（ForEach 下标 = Tabs 位置），也承载每个
// OSR 视图的身份令牌（uid）。增删标签只改这个数组——位置与身份天然同源，
// 不存在需要人工同步的第二处（旧 myTabIndex 硬编码索引因此类 bug 已移除）。
// uid 约定：OSR 标签非空且全表唯一（同时用作 focus target id 后缀，须 ASCII）；
//          非 OSR 标签为空串（不参与广播通道）。
interface TabDef {
  kind: 'browser' | 'cef' | 'osr-cpu' | 'osr-gpu';
  label: string;
  url: string;
  uid: string;
  dragOut?: boolean;
}

@Entry
@Component
struct MultiTabPage {
  // ── 唯一数据源 ──
  static readonly TABS: TabDef[] = [
    { kind: 'browser', label: '多CEF实例', url: 'https://qq.com', uid: '' },
    { kind: 'browser', label: 'BrowserView', url: 'https://example.com/', uid: '' },
    { kind: 'cef', label: 'CefView', url: 'https://developer.huawei.com/consumer/cn/develop/', uid: '' },
    { kind: 'browser', label: 'WebRTC', url: 'https://webrtc.github.io/samples/', uid: '' },
    { kind: 'cef', label: 'Clipboard', url: 'file:///data/storage/el1/bundle/entry/resources/resfile/Clipboard-full.html', uid: '' },
    { kind: 'cef', label: 'PDF', url: 'https://www.orimi.com/pdf-test.pdf', uid: '' },
    { kind: 'osr-cpu', label: 'OSR显示测试',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_display_test.html',
      uid: 'osr-display', dragOut: true },
    { kind: 'osr-cpu', label: 'OSR输入测试',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_input_test.html',
      uid: 'osr-input', dragOut: true },
    { kind: 'osr-gpu', label: 'GPU OSR测试',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_test.html',
      uid: 'osr-gpu' },
    { kind: 'osr-gpu', label: 'ufotest-fix',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/ufo_fixed_test.html',
      uid: 'osr-ufofix' },
    { kind: 'osr-cpu', label: 'OSR拖放测试',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_drag_test.html',
      uid: 'osr-drag', dragOut: true },
  ];

  // 当前激活标签位置（Tabs 自身坐标系：驱动切换、两行头部高亮）。
  @State currentIndex: number = 0;
  // 当前激活标签的身份令牌（由 onChange 从 TABS 换算），广播给全部 OSR 视图。
  @State activeUid: string = '';
  // TabsController 驱动器：自定义标签头点击 → changeIndex。
  controller: TabsController = new TabsController();

  private isOsr(t: TabDef): boolean {
    return t.kind === 'osr-cpu' || t.kind === 'osr-gpu';
  }

  private isRow1(t: TabDef): boolean {
    return t.kind === 'browser' || t.kind === 'cef';
  }

  // ── 单个标签按钮 ──
  @Builder
  TabChip(t: TabDef, index: number) {
    Text(t.label)
      .fontSize(15)
      .fontColor(this.currentIndex === index ? Color.White : Color.Black)
      .textAlign(TextAlign.Center)
      .height(38)
      .padding({ left: 14, right: 14 })
      .constraintSize({ minWidth: 64 })
      .borderRadius(8)
      .backgroundColor(this.currentIndex === index ? '#007DFF' : '#F1F3F5')
      .onClick(() => {
        this.controller.changeIndex(index);
      })
  }

  build() {
    Column() {
      // ── 第一行：非 OSR（windowed）标签 ──
      Row({ space: 6 }) {
        ForEach(MultiTabPage.TABS, (t: TabDef, index: number) => {
          if (this.isRow1(t)) {
            this.TabChip(t, index)
          }
        }, (t: TabDef, index: number) => 'chip1_' + index + '_' + t.uid)
      }
      .width('100%')
      .height(46)
      .padding({ left: 8, right: 8 })
      .alignItems(VerticalAlign.Center)
      .backgroundColor(Color.White)

      // ── 第二行：OSR 标签（CPU OSR + GPU OSR）──
      Row({ space: 6 }) {
        ForEach(MultiTabPage.TABS, (t: TabDef, index: number) => {
          if (this.isOsr(t)) {
            this.TabChip(t, index)
          }
        }, (t: TabDef, index: number) => 'chip2_' + index + '_' + t.uid)
      }
      .width('100%')
      .height(46)
      .padding({ left: 8, right: 8 })
      .alignItems(VerticalAlign.Center)
      .backgroundColor(Color.White)
      .border({ width: { bottom: 1 }, color: '#D0D5DD' })

      // ── 内容区：ForEach 数据驱动，下标即 Tabs 位置 ──
      Tabs({ controller: this.controller, index: this.currentIndex }) {
        ForEach(MultiTabPage.TABS, (t: TabDef, index: number) => {
          TabContent() {
            if (t.kind === 'browser') {
              if (t.label === '多CEF实例') {
                Row() {
                  Column() {
                    BrowserView({ defaultUrl: 'https://qq.com' })
                  }
                  .width('49%')
                  .border({ width: 1, color: '#888888', radius: 12 })
                  .backgroundColor(Color.Gray)

                  Blank().width('2%')

                  Column() {
                    BrowserView({ defaultUrl: 'https://bing.com' })
                  }
                  .width('49%')
                  .border({ width: 1, color: '#888888', radius: 12 })
                  .backgroundColor(Color.Gray)
                }
              } else {
                Column() {
                  BrowserView({ defaultUrl: t.url })
                }
                .width('100%')
                .height('100%')
              }
            } else if (t.kind === 'cef') {
              Column() {
                if (t.label === 'CefView') {
                  CefView({
                    viewUrl: t.url,
                    onCreatedCallback: (view: CefView) => {
                      // 拖出支持：BrowserView 标签页同款接线
                      view.enableDragOut((info: DragOutInfo) => {
                        getDragOutSession().startDragOut(info);
                      });
                    }
                  })
                } else {
                  CefView({ viewUrl: t.url })
                }
              }
              .width('100%')
              .height('100%')
            } else {
              // OSR（CPU + GPU 共用 OsrCefView；GPU 走 native presenter）
              Column() {
                OsrCefView({
                  viewUrl: t.url,
                  gpuEnabled: t.kind === 'osr-gpu',
                  viewUid: t.uid,
                  activeUid: this.activeUid,
                  onDragOutCallback: t.dragOut === true ? (info: DragOutInfo) => {
                    getDragOutSession().startDragOut(info);
                  } : undefined
                })
              }
              .width('100%')
              .height('100%')
              .focusable(true)
            }
          }
        }, (t: TabDef, index: number) => 'tab_' + index + '_' + t.uid)
      }
      .barHeight(0)
      .barWidth(0)
      .animationDuration(300)
      .onChange((index: number) => {
        this.currentIndex = index;
        this.activeUid = MultiTabPage.TABS[index]?.uid ?? '';
      })
      .layoutWeight(1)
    }
    .height('100%')
    .width('100%')
    .onAppear(() => {
      // Tabs.onChange does not fire for the initial page (default index 0).
      this.currentIndex = 0;
      this.activeUid = MultiTabPage.TABS[0].uid;
    })
  }
}
