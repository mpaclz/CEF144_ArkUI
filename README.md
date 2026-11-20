# ArkUI-CEF144

CEF 144 鸿蒙 (OHOS)  demo 工程，构建时依赖以下框架产物。（后续用 `$FW` 指代框架根目录，用 `$DEMO` 指代工程根目录。）

| # | 来源（$FW/src/）                                                                                                                                                          | 拷贝到（$DEMO/）                                         | 说明                |
| - | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------- | ----------------- |
| 1 | `out/musl_64/libcef.so`                                                                                                                                               | `entry/libs/arm64-v8a/`                             | CEF 主库            |
| 2 | `out/musl_64/libadapter.so`                                                                                                                                           | `entry/libs/arm64-v8a/`                             | OHOS 适配层          |
| 3 | `ohos_sdk/openharmony/native/llvm/lib/aarch64-linux-ohos/libc++_shared.so`                                                                                            | `entry/libs/arm64-v8a/`                             | C++ 运行时           |
| 4 | `out/musl_64/` 下 `resources.pak` `chrome_100_percent.pak` `chrome_200_percent.pak` `icudtl.dat` `snapshot_blob.bin` `v8_context_snapshot.bin` `locales/` `resources/` | `web_engine/src/main/resources/resfile/`            | CEF 运行资源          |
| 5 | `cef/include/**`（390 个）                                                                                                                                               | `entry/src/main/cpp/include/cef/include/`           | CEF 公共头           |
| 6 | `out/musl_64/gen/cef/include/*.h`（7 个生成头）                                                                                                                             | `entry/src/main/cpp/include/cef/include/`           |                   |
| 7 | `net/base/net_error_list.h`                                                                                                                                           | `entry/src/main/cpp/include/cef/chromium/net/base/` |                   |
| 8 | `out/musl_64/obj/cef/libcef_dll_wrapper.a`                                                                                                                            | `entry/src/main/cpp/libs/`                          | thin archive，仅索引  |
| 9 | `out/musl_64/obj/cef/libcef_dll_wrapper/*.o`                                                                                                                          | `entry/src/main/cpp/libs/libcef_dll_wrapper/`       | thin archive，代码实体 |

## 注意事项

1. wrapper 的 .o 可被本工程 CMake 直接链接，依赖框架 `cef/BUILD.gn` 的 `libcef_dll_wrapper_no_lto` config（OHOS 下 `-fno-lto`，产出真 ELF 而非 ThinLTO bitcode）。

