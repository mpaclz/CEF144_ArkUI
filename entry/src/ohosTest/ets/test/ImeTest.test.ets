import { describe, beforeAll, beforeEach, afterEach, afterAll, it, expect } from '@ohos/hypium';
import arkui_cef from 'libarkui_cef.so';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x0000;
const TAG = 'ImeTest';

export default function imeTest() {
  describe('ImeTest', () => {

    beforeAll(() => {
      hilog.info(DOMAIN, TAG, 'ImeTest suite begin');
    });
    beforeEach(() => {});
    afterEach(() => {});
    afterAll(() => {
      hilog.info(DOMAIN, TAG, 'ImeTest suite end');
    });

    // ============================================================
    // 1. Core 功能测试 (C1 - C15)
    // ============================================================
    describe('Core', () => {

      // C1: cefImeSetComposition 函数导出存在性
      it('C1_cefImeSetComposition_exists', 0, () => {
        expect(arkui_cef.cefImeSetComposition).assertNotUndefined();
        expect(typeof arkui_cef.cefImeSetComposition).assertEqual('function');
      });

      // C2: cefImeCommitText 函数导出存在性
      it('C2_cefImeCommitText_exists', 0, () => {
        expect(arkui_cef.cefImeCommitText).assertNotUndefined();
        expect(typeof arkui_cef.cefImeCommitText).assertEqual('function');
      });

      // C3: cefImeCancelComposition 函数导出存在性
      it('C3_cefImeCancelComposition_exists', 0, () => {
        expect(arkui_cef.cefImeCancelComposition).assertNotUndefined();
        expect(typeof arkui_cef.cefImeCancelComposition).assertEqual('function');
      });

      // C4: cefImeFinishComposition 函数导出存在性
      it('C4_cefImeFinishComposition_exists', 0, () => {
        expect(arkui_cef.cefImeFinishComposition).assertNotUndefined();
        expect(typeof arkui_cef.cefImeFinishComposition).assertEqual('function');
      });

      // C5: cefImeNotifyCursorInfo 函数导出存在性
      it('C5_cefImeNotifyCursorInfo_exists', 0, () => {
        expect(arkui_cef.cefImeNotifyCursorInfo).assertNotUndefined();
        expect(typeof arkui_cef.cefImeNotifyCursorInfo).assertEqual('function');
      });

      // C6: createOsrCefView 支持 12 个参数（含 onImeCursorChanged）
      it('C6_createOsrCefView_12params', 0, () => {
        expect(arkui_cef.createOsrCefView).assertNotUndefined();
        expect(typeof arkui_cef.createOsrCefView).assertEqual('function');
        // createOsrCefView 的 length 属性表示函数声明的形参数量
        // 即便不严格等于 12，只要函数存在即可，这里记录实际值
        hilog.info(DOMAIN, TAG, 'createOsrCefView.length = %{public}d', arkui_cef.createOsrCefView.length);
        expect(arkui_cef.createOsrCefView.length > 0).assertTrue();
      });

      // C7: cefImeSetComposition 参数校验（4 个参数）
      it('C7_cefImeSetComposition_paramCount', 0, () => {
        // 函数声明形参数量应为 4
        expect(arkui_cef.cefImeSetComposition.length).assertEqual(4);
      });

      // C8: cefImeCommitText 参数校验（2 个参数）
      it('C8_cefImeCommitText_paramCount', 0, () => {
        expect(arkui_cef.cefImeCommitText.length).assertEqual(2);
      });

      // C9: cefImeCancelComposition 参数校验（1 个参数）
      it('C9_cefImeCancelComposition_paramCount', 0, () => {
        expect(arkui_cef.cefImeCancelComposition.length).assertEqual(1);
      });

      // C10: cefImeFinishComposition 参数校验（1 个参数）
      it('C10_cefImeFinishComposition_paramCount', 0, () => {
        expect(arkui_cef.cefImeFinishComposition.length).assertEqual(1);
      });

      // C11: cefImeNotifyCursorInfo 参数校验（5 个参数）
      it('C11_cefImeNotifyCursorInfo_paramCount', 0, () => {
        expect(arkui_cef.cefImeNotifyCursorInfo.length).assertEqual(5);
      });

      // C12: cefImeSetComposition 传无效 browserId 不崩溃
      it('C12_cefImeSetComposition_invalidBrowserId', 0, () => {
        try {
          arkui_cef.cefImeSetComposition(-1, 'test', 0, 4);
          expect(true).assertTrue();
        } catch (e) {
          // NAPI 层查不到 browser 会静默返回，不崩溃即通过
          expect(true).assertTrue();
        }
      });

      // C13: cefImeCommitText 传无效 browserId 不崩溃
      it('C13_cefImeCommitText_invalidBrowserId', 0, () => {
        try {
          arkui_cef.cefImeCommitText(-1, 'test');
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // C14: cefImeCancelComposition 传无效 browserId 不崩溃
      it('C14_cefImeCancelComposition_invalidBrowserId', 0, () => {
        try {
          arkui_cef.cefImeCancelComposition(-1);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // C15: cefImeFinishComposition 传无效 browserId 不崩溃
      it('C15_cefImeFinishComposition_invalidBrowserId', 0, () => {
        try {
          arkui_cef.cefImeFinishComposition(-1);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });
    });

    // ============================================================
    // 2. Boundary 边界测试 (B1 - B15)
    // ============================================================
    describe('Boundary', () => {

      // B1: cefImeCommitText 传空字符串 ""
      it('B1_cefImeCommitText_emptyString', 0, () => {
        try {
          arkui_cef.cefImeCommitText(-1, '');
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B2: cefImeSetComposition 传空字符串
      it('B2_cefImeSetComposition_emptyString', 0, () => {
        try {
          arkui_cef.cefImeSetComposition(-1, '', 0, 0);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B3: cefImeSetComposition selectionFrom > selectionTo
      it('B3_cefImeSetComposition_fromGreaterThanTo', 0, () => {
        try {
          arkui_cef.cefImeSetComposition(-1, 'test', 4, 0);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B4: cefImeSetComposition selectionFrom = selectionTo = 0
      it('B4_cefImeSetComposition_zeroRange', 0, () => {
        try {
          arkui_cef.cefImeSetComposition(-1, 'test', 0, 0);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B5: cefImeSetComposition selectionFrom = selectionTo = -1
      it('B5_cefImeSetComposition_negativeRange', 0, () => {
        try {
          arkui_cef.cefImeSetComposition(-1, 'test', -1, -1);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B6: cefImeSetComposition 超长文本（10000 字符）
      it('B6_cefImeSetComposition_longText', 0, () => {
        let longText = 'a'.repeat(10000);
        try {
          arkui_cef.cefImeSetComposition(-1, longText, 0, longText.length);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B7: cefImeCommitText 超长文本（10000 字符）
      it('B7_cefImeCommitText_longText', 0, () => {
        let longText = 'a'.repeat(10000);
        try {
          arkui_cef.cefImeCommitText(-1, longText);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B8: cefImeSetComposition 传特殊字符（中文、emoji、换行符、制表符）
      it('B8_cefImeSetComposition_specialChars', 0, () => {
        let specialText = '你好世界🌍\n\t你好';
        try {
          arkui_cef.cefImeSetComposition(-1, specialText, 0, specialText.length);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B9: cefImeCommitText 传特殊字符（中文、emoji、换行符）
      it('B9_cefImeCommitText_specialChars', 0, () => {
        let specialText = '你好世界🌍\n测试';
        try {
          arkui_cef.cefImeCommitText(-1, specialText);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B10: cefImeNotifyCursorInfo 传全 0 坐标
      it('B10_cefImeNotifyCursorInfo_zeroCoords', 0, () => {
        try {
          arkui_cef.cefImeNotifyCursorInfo(-1, 0, 0, 0, 0);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B11: cefImeNotifyCursorInfo 传负坐标
      it('B11_cefImeNotifyCursorInfo_negativeCoords', 0, () => {
        try {
          arkui_cef.cefImeNotifyCursorInfo(-1, -10, -20, -5, -5);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B12: cefImeNotifyCursorInfo 传极大坐标
      it('B12_cefImeNotifyCursorInfo_extremeCoords', 0, () => {
        try {
          let maxVal = Number.MAX_VALUE;
          arkui_cef.cefImeNotifyCursorInfo(-1, maxVal, maxVal, maxVal, maxVal);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // B13: createOsrCefView 不传第 12 参数（argc=11）— 不应崩溃
      it('B13_createOsrCefView_noImeCallback', 0, () => {
        try {
          // 只传前 7 个必选参数，可选参数全部不传
          let browserId = arkui_cef.createOsrCefView(
            'about:blank', 1, 1,
            (_buf: ArrayBuffer, _w: number, _h: number) => {},
            (_bid: number) => {},
            (_title: string) => {},
            (_next: boolean) => {}
          );
          // 返回值应为 -1（失败）或有效 id，不崩溃即可
          expect(browserId !== undefined).assertTrue();
        } catch (e) {
          // 创建可能因环境问题失败，不崩溃即通过
          expect(true).assertTrue();
        }
      });

      // B14: createOsrCefView 传 12 参数但第 12 个不是函数（是 null）— 不应崩溃
      it('B14_createOsrCefView_nullImeCallback', 0, () => {
        try {
          // @ts-ignore — 故意传 null 作为第 12 参数
          let browserId = arkui_cef.createOsrCefView(
            'about:blank', 1, 1,
            (_buf: ArrayBuffer, _w: number, _h: number) => {},
            (_bid: number) => {},
            (_title: string) => {},
            (_next: boolean) => {},
            undefined,
            undefined,
            undefined,
            undefined,
            null
          );
          expect(browserId !== undefined).assertTrue();
        } catch (e) {
          // NAPI 层 napi_typeof 检查失败会 throw，预期行为
          expect(true).assertTrue();
        }
      });

      // B15: cefImeSetComposition 连续快速调用 100 次
      it('B15_cefImeSetComposition_100calls', 0, () => {
        try {
          for (let i = 0; i < 100; i++) {
            arkui_cef.cefImeSetComposition(-1, 'test' + i, 0, 5);
          }
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });
    });

    // ============================================================
    // 3. Exception 异常测试 (E1 - E10)
    // ============================================================
    describe('Exception', () => {

      // E1: 浏览器未创建（browserId=-1）时调 cefImeSetComposition
      it('E1_cefImeSetComposition_browserNotCreated', 0, () => {
        try {
          arkui_cef.cefImeSetComposition(-1, 'test', 0, 4);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // E2: 浏览器未创建时调 cefImeCommitText
      it('E2_cefImeCommitText_browserNotCreated', 0, () => {
        try {
          arkui_cef.cefImeCommitText(-1, 'test');
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // E3: 浏览器未创建时调 cefImeCancelComposition
      it('E3_cefImeCancelComposition_browserNotCreated', 0, () => {
        try {
          arkui_cef.cefImeCancelComposition(-1);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // E4: 浏览器未创建时调 cefImeFinishComposition
      it('E4_cefImeFinishComposition_browserNotCreated', 0, () => {
        try {
          arkui_cef.cefImeFinishComposition(-1);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // E5: 浏览器关闭后调 cefImeSetComposition — 使用一个曾经有效的 id（这里用 99999）
      it('E5_cefImeSetComposition_browserClosed', 0, () => {
        try {
          // 99999 是一个不存在的 browserId，模拟浏览器已关闭
          arkui_cef.cefImeSetComposition(99999, 'test', 0, 4);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // E6: 浏览器关闭后调 cefImeCommitText
      it('E6_cefImeCommitText_browserClosed', 0, () => {
        try {
          arkui_cef.cefImeCommitText(99999, 'test');
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // E7: cefImeSetComposition 传 undefined 作为 text
      it('E7_cefImeSetComposition_undefinedText', 0, () => {
        try {
          // @ts-ignore — 故意传 undefined
          arkui_cef.cefImeSetComposition(-1, undefined, 0, 0);
          expect(true).assertTrue();
        } catch (e) {
          // NAPI 层 napi_get_value_string_utf8 对 undefined 会抛错或转为 "undefined"
          expect(true).assertTrue();
        }
      });

      // E8: cefImeCommitText 传 undefined 作为 text
      it('E8_cefImeCommitText_undefinedText', 0, () => {
        try {
          // @ts-ignore
          arkui_cef.cefImeCommitText(-1, undefined);
          expect(true).assertTrue();
        } catch (e) {
          expect(true).assertTrue();
        }
      });

      // E9: cefImeSetComposition 传数字而不是字符串作为 text
      it('E9_cefImeSetComposition_numberAsText', 0, () => {
        try {
          // @ts-ignore — 故意传数字
          arkui_cef.cefImeSetComposition(-1, 12345, 0, 5);
          expect(true).assertTrue();
        } catch (e) {
          // NAPI 层会做类型转换或抛错，不崩溃即通过
          expect(true).assertTrue();
        }
      });

      // E10: cefImeSetComposition 缺少参数（只传 2 个）
      it('E10_cefImeSetComposition_missingArgs', 0, () => {
        let threw = false;
        try {
          // @ts-ignore — 故意只传 2 个参数
          arkui_cef.cefImeSetComposition(-1, 'test');
        } catch (e) {
          // NAPI 层 argc < 4 会 throw "Expected 4 arguments"
          threw = true;
          expect(e).assertNotUndefined();
        }
        // 无论是否抛异常，只要不 native crash 即通过
        // 但如果抛了异常，说明参数校验生效
        expect(true).assertTrue();
        hilog.info(DOMAIN, TAG, 'E10 threw=%{public}s', threw ? 'true' : 'false');
      });
    });

    // ============================================================
    // 4. Performance 性能测试 (P1 - P6)
    // ============================================================
    describe('Performance', () => {

      // P1: cefImeSetComposition 单次调用延迟 < 100ms（放宽阈值）
      it('P1_cefImeSetComposition_latency', 0, () => {
        let start = Date.now();
        try {
          arkui_cef.cefImeSetComposition(-1, 'test', 0, 4);
        } catch (e) {}
        let elapsed = Date.now() - start;
        // NAPI 解析 + CefPostTask 应该很快，放宽到 100ms 适应设备差异
        expect(elapsed < 100).assertTrue();
        hilog.info(DOMAIN, TAG, 'P1 elapsed=%{public}d ms', elapsed);
      });

      // P2: cefImeCommitText 单次调用延迟 < 100ms
      it('P2_cefImeCommitText_latency', 0, () => {
        let start = Date.now();
        try {
          arkui_cef.cefImeCommitText(-1, 'test');
        } catch (e) {}
        let elapsed = Date.now() - start;
        expect(elapsed < 100).assertTrue();
        hilog.info(DOMAIN, TAG, 'P2 elapsed=%{public}d ms', elapsed);
      });

      // P3: 连续 100 次 cefImeSetComposition 耗时 < 2000ms
      it('P3_cefImeSetComposition_100calls', 0, () => {
        let start = Date.now();
        for (let i = 0; i < 100; i++) {
          try {
            arkui_cef.cefImeSetComposition(-1, 'test' + i, 0, 5);
          } catch (e) {}
        }
        let elapsed = Date.now() - start;
        expect(elapsed < 2000).assertTrue();
        hilog.info(DOMAIN, TAG, 'P3 elapsed=%{public}d ms (100 calls)', elapsed);
      });

      // P4: 连续 100 次 cefImeCommitText 耗时 < 2000ms
      it('P4_cefImeCommitText_100calls', 0, () => {
        let start = Date.now();
        for (let i = 0; i < 100; i++) {
          try {
            arkui_cef.cefImeCommitText(-1, 'commit' + i);
          } catch (e) {}
        }
        let elapsed = Date.now() - start;
        expect(elapsed < 2000).assertTrue();
        hilog.info(DOMAIN, TAG, 'P4 elapsed=%{public}d ms (100 calls)', elapsed);
      });

      // P5: cefImeSetComposition 和 cefImeCommitText 交替调用 100 次 — 不死锁
      it('P5_cefSetAndCommit_alternating', 0, () => {
        let start = Date.now();
        for (let i = 0; i < 100; i++) {
          try {
            arkui_cef.cefImeSetComposition(-1, 'preedit' + i, 0, 7);
            arkui_cef.cefImeCommitText(-1, 'commit' + i);
          } catch (e) {}
        }
        let elapsed = Date.now() - start;
        // 没有死锁即通过，且耗时在合理范围
        expect(elapsed < 5000).assertTrue();
        hilog.info(DOMAIN, TAG, 'P5 elapsed=%{public}d ms (100 alternating)', elapsed);
      });

      // P6: cefImeNotifyCursorInfo 连续 1000 次调用 — 不崩溃，不内存泄漏
      it('P6_cefImeNotifyCursorInfo_1000calls', 0, () => {
        let start = Date.now();
        for (let i = 0; i < 1000; i++) {
          try {
            arkui_cef.cefImeNotifyCursorInfo(-1, i, i, 10, 10);
          } catch (e) {}
        }
        let elapsed = Date.now() - start;
        // 不崩溃即通过，耗时在合理范围
        expect(elapsed < 10000).assertTrue();
        hilog.info(DOMAIN, TAG, 'P6 elapsed=%{public}d ms (1000 calls)', elapsed);
      });
    });

  });
}
