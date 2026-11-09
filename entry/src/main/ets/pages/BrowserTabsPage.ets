import { BrowserView } from '../components/BrowserView'
import { CefView } from '../components/CefView';
import { OsrCefView } from '../components/OsrCefView';

@Entry
@Component
struct MultiTabPage {
  // Index of the currently displayed tab, synced into the OSR views to drive
  // their keyboard ownership (key filter target + IMF session). Tabs.onChange
  // is used instead of relying solely on visibility edges:
  // onVisibleAreaChange does not fire the FIRST time a TabContent's component
  // tree gets attached (non-mounted nodes are not measured), so the first
  // switch to an OSR tab would leave the window key filter inactive.
  @State osrActiveTabIndex: number = -1;

  build() {
    RelativeContainer() {
      Tabs() {
        TabContent() {
          Row() {
            Column() {
              BrowserView({defaultUrl: 'https://qq.com'})
            }
            .width('49%')
            .border({ width: 1, color: '#888888', radius: 12 })
            .backgroundColor(Color.Gray)

            Blank().width('2%')

            Column() {
              BrowserView({defaultUrl: 'https://bing.com'})
            }
            .width('49%')
            .border({ width: 1, color: '#888888', radius: 12 })
            .backgroundColor(Color.Gray)
          }
        }.tabBar(
          SubTabBarStyle.of('多CEF实例')
        )
        TabContent() {
          Column() {
            BrowserView({defaultUrl: 'https://example.com/'})
          }
        }.tabBar(
          SubTabBarStyle.of('BrowserView')
        )
        TabContent() {
          Column() {
            CefView({viewUrl: 'https://developer.huawei.com/consumer/cn/develop/'})
          }
        }.tabBar(
          SubTabBarStyle.of('CefView')
        )
        TabContent() {
          Column() {
            BrowserView({defaultUrl: 'https://webrtc.github.io/samples/'})
          }
        }.tabBar(
          SubTabBarStyle.of('WebRTC')
        )
        TabContent() {
          Column() {
            CefView({viewUrl: 'file:///data/storage/el1/bundle/entry/resources/resfile/Clipboard-full.html'})
          }
        }.tabBar(
          SubTabBarStyle.of('Clipboard')
        )
        TabContent() {
          Column() {
            CefView({viewUrl: 'https://www.orimi.com/pdf-test.pdf'})
          }
        }.tabBar(
          SubTabBarStyle.of('PDF')
        )
        TabContent() {
          Column() {
            OsrCefView({
              viewUrl: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_display_test.html',
              myTabIndex: 6,
              activeTabIndex: this.osrActiveTabIndex
            })
          }
          .width('100%')
          .height('100%')
          .focusable(true)
        }.tabBar(
          SubTabBarStyle.of('OSR显示测试')
        )
        TabContent() {
          Column() {
            OsrCefView({
              viewUrl: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_input_test.html',
              myTabIndex: 7,
              activeTabIndex: this.osrActiveTabIndex
            })
          }
          .width('100%')
          .height('100%')
          .focusable(true)
        }.tabBar(
          SubTabBarStyle.of('OSR输入测试')
        )
        TabContent() {
          Column() {
            OsrCefView({
              viewUrl: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_drag_test.html',
              myTabIndex: 8,
              activeTabIndex: this.osrActiveTabIndex
            })
          }
          .width('100%')
          .height('100%')
          .focusable(true)
        }.tabBar(
          SubTabBarStyle.of('OSR拖放测试')
        )
      }
      .barBackgroundColor(Color.White)
      .animationDuration(300)
      .onChange((index: number) => {
        this.osrActiveTabIndex = index;
      })
    }
    .height('100%')
    .width('100%')
    .onAppear(() => {
      // Tabs.onChange does not fire for the initial page (default index 0).
      this.osrActiveTabIndex = 0;
    })
  }
}
