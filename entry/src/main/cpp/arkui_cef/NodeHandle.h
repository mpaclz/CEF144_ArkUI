//
// Created on 2026/6/3.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#ifndef OPENQTCEF_NODEHANDLE_H
#define OPENQTCEF_NODEHANDLE_H

#include <arkui/native_node.h>
#include <arkui/native_type.h>
#include <functional>

namespace arkui_cef {

/**
 * @brief 任意线程安全的通用任务投递（经 libadapter.so 的主线程 runner 执行）
 * @param task 将在 ArkUI 主线程执行的任务（拷贝持有，执行完自动释放）
 * @note 供 napi 层跨线程完成回调使用：所有 ArkUI 节点操作必须回主线程做
 */
void PostToMainThread(std::function<void()> task);

/**
 * @brief 设置节点宽高（异步投递到主线程后立即返回）
 * @param handle ArkUI 节点句柄
 * @param width 目标宽度（像素）
 * @param height 目标高度（像素）
 * @note 任务通过 uv_queue_work 投递到 ArkUI 主线程执行，函数立即返回不等待
 */
void resize(ArkUI_NodeHandle handle, int width, int height);

/**
 * @brief 显示节点（设置可见性为 VISIBLE，异步投递后立即返回）
 * @param handle ArkUI 节点句柄
 * @note 任务通过 uv_queue_work 投递到 ArkUI 主线程执行，函数立即返回不等待
 */
void show(ArkUI_NodeHandle handle);

/**
 * @brief 隐藏节点（设置可见性为 HIDDEN，异步投递后立即返回）
 * @param handle ArkUI 节点句柄
 * @note 任务通过 uv_queue_work 投递到 ArkUI 主线程执行，函数立即返回不等待
 */
void hide(ArkUI_NodeHandle handle);

/**
 * @brief 释放节点资源（异步投递到主线程后立即返回）
 * @param handle ArkUI 节点句柄
 * @note 任务通过 uv_queue_work 投递到 ArkUI 主线程执行，函数立即返回不等待
 */
void release(ArkUI_NodeHandle handle);
} // namespace arkui_cef
#endif //OPENQTCEF_NODEHANDLE_H
