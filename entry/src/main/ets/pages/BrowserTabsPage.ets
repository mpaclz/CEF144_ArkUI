import { BrowserView } from '../components/BrowserView'
import { CefView } from '../components/CefView';
import { OsrCefView } from '../components/OsrCefView';

@Entry
@Component
struct MultiTabPage {
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
            OsrCefView({viewUrl: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_test.html'})
          }
        }.tabBar(
          SubTabBarStyle.of('OSR离屏渲染')
        )
        TabContent() {
          Column() {
            OsrCefView({viewUrl: 'file:///data/storage/el1/bundle/entry/resources/resfile/osr_input_test.html'})
          }
        }.tabBar(
          SubTabBarStyle.of('OSR输入测试')
        )
      }
      .barBackgroundColor(Color.White)
      .animationDuration(300)
    }
    .height('100%')
    .width('100%')
  }
}
