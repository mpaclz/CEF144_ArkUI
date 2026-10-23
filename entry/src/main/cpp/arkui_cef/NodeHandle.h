//
// Created on 2026/6/3.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#ifndef OPENQTCEF_NODEHANDLE_H
#define OPENQTCEF_NODEHANDLE_H

#include <arkui/native_node.h>
#include <arkui/native_type.h>

namespace arkui_cef {

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
