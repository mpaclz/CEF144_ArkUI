import { BrowserView } from '../components/BrowserView'
import { CefView } from '../components/CefView';
import { OsrCefView } from '../components/OsrCefView';

// ── Tab data source (registry + render source in one) ──────────────────
// The TabDef array both drives TabContent rendering (ForEach index == Tabs
// position) and carries each OSR view's identity token (uid). Adding or
// removing a tab touches only this array - position and identity share one
// source of truth, with no second place to keep in sync by hand (the old
// hard-coded myTabIndex indexes were removed after exactly that class of
// bug). uid conventions: OSR tabs are non-empty and unique across the table
// (also used as a focus-target id suffix, so ASCII only); non-OSR tabs use
// the empty string (outside the broadcast channel).
interface TabDef {
  kind: 'browser' | 'cef' | 'osr-cpu' | 'osr-gpu';
  label: string;
  url: string;
  uid: string;
}

@Entry
@Component
struct MultiTabPage {
  // ── The single source of truth ──
  static readonly TABS: TabDef[] = [
    { kind: 'browser', label: '多CEF实例', url: 'https://qq.com', uid: '' },
    { kind: 'browser', label: 'BrowserView', url: 'https://example.com/', uid: '' },
    { kind: 'cef', label: 'CefView', url: 'https://developer.huawei.com/consumer/cn/develop/', uid: '' },
    { kind: 'browser', label: 'WebRTC', url: 'https://webrtc.github.io/samples/', uid: '' },
    { kind: 'cef', label: 'Clipboard', url: 'file:///data/storage/el1/bundle/entry/resources/resfile/Clipboard-full.html', uid: '' },
    { kind: 'cef', label: 'PDF', url: 'https://www.orimi.com/pdf-test.pdf', uid: '' },
    { kind: 'osr-cpu', label: 'OSR显示测试',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_display_test.html',
      uid: 'osr-display' },
    { kind: 'osr-cpu', label: 'OSR输入测试',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_input_test.html',
      uid: 'osr-input' },
    { kind: 'osr-gpu', label: 'GPU OSR测试',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_test.html',
      uid: 'osr-gpu' },
    { kind: 'osr-gpu', label: 'ufotest-fix',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/ufo_fixed_test.html',
      uid: 'osr-ufofix' },
    { kind: 'osr-cpu', label: 'OSR拖放测试',
      url: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_drag_test.html',
      uid: 'osr-drag' },
  ];

  // Active tab position (Tabs' own coordinate system: drives switching
  // and the two-row header highlight).
  @State currentIndex: number = 0;
  // Active tab's identity token (mapped from TABS by onChange), broadcast
  // to every OSR view.
  @State activeUid: string = '';
  // TabsController driver: custom header chip clicks -> changeIndex.
  controller: TabsController = new TabsController();

  private isOsr(t: TabDef): boolean {
    return t.kind === 'osr-cpu' || t.kind === 'osr-gpu';
  }

  private isRow1(t: TabDef): boolean {
    return t.kind === 'browser' || t.kind === 'cef';
  }

  // ── One tab chip ──
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
      // ── Row 1: non-OSR (windowed) tabs ──
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

      // ── Row 2: OSR tabs (CPU OSR + GPU OSR) ──
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

      // ── Content area: ForEach data-driven; the array index is the Tabs position ──
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
                CefView({ viewUrl: t.url })
              }
              .width('100%')
              .height('100%')
            } else {
              // OSR (CPU + GPU share OsrCefView; GPU goes through the
              // native presenter).
              Column() {
                OsrCefView({
                  viewUrl: t.url,
                  gpuEnabled: t.kind === 'osr-gpu',
                  viewUid: t.uid,
                  activeUid: this.activeUid
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
