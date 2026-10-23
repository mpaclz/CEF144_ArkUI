// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import arkui_cef from 'libarkui_cef.so';
import { WebAbilityStage } from 'web_engine';
import display from '@ohos.display';
import Want from '@ohos.app.ability.Want';

export default class MyAbilityStage extends WebAbilityStage {

  onCreate(): void {
    console.log('ZYX | AbilityStage.onCreate.');
    super.onCreate();
    setTimeout(() => { arkui_cef.startCef() }, 0);
  }

  /**
   * 返回ability的Key，如果已存在则直接拉起，否则创建新的ability。
   * @param want
   * @returns
   */
  onAcceptWant(want: Want): string {
    if (want.parameters?.instanceKey !== undefined) {
      return `${want.parameters?.instanceKey}`;
    }
    return "main_window";
  }
}
