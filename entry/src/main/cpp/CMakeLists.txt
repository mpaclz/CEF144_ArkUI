cmake_minimum_required(VERSION 3.13.0)
project(ArkUICef)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

include_directories(${NATIVERENDER_ROOT_PATH}
                    ${NATIVERENDER_ROOT_PATH}/include)

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

# CEF include and lib directories
set(CEF_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include/cef")
set(CEF_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs")
set(HAP_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../libs/arm64-v8a")

find_package(CEF REQUIRED)

# CEF 144: use pre-built libcef_dll_wrapper.a instead of building from source
# (the old libcef_dll/ source directory has been removed)
add_library(libcef_dll_wrapper STATIC IMPORTED)
set_target_properties(libcef_dll_wrapper PROPERTIES
    IMPORTED_LOCATION "${CEF_LIB_DIR}/libcef_dll_wrapper.a"
)

find_library(
    native-drawing-lib
    native_drawing
)

find_library(
    native-window-lib
    native_window
)

find_library(
    native-display-manager-lib
    native_display_manager
)

find_library(
    hilog-lib
    hilog_ndk.z
)

find_library(
    libace-lib
    ace_ndk.z
)

find_library(
    libnapi-lib
    ace_napi.z
)

add_library(arkui_cef_core SHARED
    arkui_cef/ArkUICefApp.cpp
    arkui_cef/ArkUICefClient.cpp
    arkui_cef/ArkUICefBrowser.cpp
    arkui_cef/ArkUICefRenderHandler.cpp
    arkui_cef/NodeHandle.cpp
    arkui_cef/ArkUICefManager.cpp)
target_include_directories(arkui_cef_core PUBLIC
    ${NATIVERENDER_ROOT_PATH}
    arkui_cef
    ${CEF_INCLUDE_DIR}
    ${CEF_INCLUDE_DIR}/chromium
    ${CEF_INCLUDE_DIR}/gen
    ${CEF_INCLUDE_DIR}/include
    ${CEF_INCLUDE_DIR}/include/wrapper
)
# CEF 144: libcef_dll_wrapper.a is built in Release mode (NDEBUG), so DCHECK_IS_ON()=false.
# Our code must match to avoid undefined symbol errors (e.g. RefCountedThreadSafeBase::~RefCountedThreadSafeBase).
target_compile_definitions(arkui_cef_core PUBLIC NDEBUG)
target_link_directories(arkui_cef_core PUBLIC ${HAP_LIB_DIR})

target_link_libraries(arkui_cef_core PUBLIC
    cef
    libcef_dll_wrapper
    ${HAP_LIB_DIR}/libadapter.so
    ${native-window-lib}
    ${native-display-manager-lib}
    ${native-drawing-lib}
    ${hilog-lib}
    ${libace-lib}
    ${libnapi-lib}
)

add_library(arkui_cef SHARED napi_init.cpp JsThreadSafeCallback.cpp)
target_include_directories(arkui_cef PUBLIC
    ${NATIVERENDER_ROOT_PATH}
    arkui_cef
    ${CEF_INCLUDE_DIR}
    ${CEF_INCLUDE_DIR}/chromium
    ${CEF_INCLUDE_DIR}/gen
    ${CEF_INCLUDE_DIR}/include
    ${CEF_INCLUDE_DIR}/include/wrapper
)
target_compile_definitions(arkui_cef PUBLIC NDEBUG)
target_link_libraries(arkui_cef PUBLIC
    arkui_cef_core
    libace_napi.z.so
)

# cefsimple 共享库：OHOS 子进程入口
add_library(cefsimple SHARED CefProcessEntry.cpp)

target_compile_definitions(cefsimple PRIVATE NDEBUG)

target_include_directories(cefsimple
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${CEF_INCLUDE_DIR}
        ${CEF_INCLUDE_DIR}/chromium
        ${CEF_INCLUDE_DIR}/gen
        ${CEF_INCLUDE_DIR}/include
        ${CEF_INCLUDE_DIR}/include/wrapper
)

target_link_libraries(cefsimple
    PRIVATE
        arkui_cef
        ${HAP_LIB_DIR}/libcef.so
)
