// Copyright (c) 2024 Huawei Device Co., Ltd. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import { injectable } from 'inversify';
import { ConfigData } from './Constants';
import { WebProxy } from '../interface/WebProxy';
import { window } from '@kit.ArkUI';
import LogMethod from './LogDecorator';
import LogUtil from '../utils/LogUtil';

AppStorage.setOrCreate('abilityProxyList', []);

const TAG = 'AbilityManager'

@injectable()
export class AbilityManager {
  private appStorage: WebProxy[];
  private widgetOwners: Map<string, window.Window>;
  private windowOwners: Map<window.Window, WebProxy>;

  constructor() {
    let appStorageLink: SubscribedAbstractProperty<WebProxy[]>
      = AppStorage.link('abilityProxyList');
    this.appStorage = appStorageLink.get();
    this.widgetOwners = new Map<string, window.Window>();
    this.windowOwners = new Map<window.Window, WebProxy>();
  }

  private paramNormalization(proxyId: string | number): string {
    if (typeof proxyId === 'number') {
      return ConfigData.WINDOW_PREFIX + proxyId;
    }
    return proxyId;
  }

  public addProxy(proxy: WebProxy) {
    for (let i = 0; i < this.appStorage.length; i++) {
      if (proxy.getWindowId() === this.appStorage[i].getWindowId()) {
        return;
      }
    }
    this.appStorage.push(proxy);
  }

  public removeProxy(proxyId: string | number) {
    let id = this.paramNormalization(proxyId);
    for (let i = 0; i < this.appStorage.length; i++) {
      if (id === this.appStorage[i].getWindowId()) {
        this.appStorage.splice(i, 1);
        break;
      }
    }
  }

  public getProxy(proxyId: string | number): WebProxy | undefined {
    let id = this.paramNormalization(proxyId);
    for (let i = 0; i < this.appStorage.length; i++) {
      if (id === this.appStorage[i].getWindowId()) {
        return this.appStorage[i];
      }
    }
    let window = this.getWidgetWindow(proxyId);
    if (window) {
      return this.getWindowWebProxy(window);
    }
    return undefined;
  }

  public getProxyDefault(): WebProxy | undefined {
    if (this.isEmpty()) {
      return undefined;
    }
    return this.appStorage[0];
  }

  public isEmpty(): boolean {
    return this.appStorage.length === 0;
  }

  public getProxyCount(): number {
    return this.appStorage.length;
  }

  public getProxyList(): WebProxy[] {
    return this.appStorage;
  }
  
  public getWindowIdByOriginWindow(originWindowId: number): string {
    for (let i = 0; i < this.appStorage.length; i++) {
      if (originWindowId === this.appStorage[i].getOriginWindowId()) {
        return this.appStorage[i].getWindowId();
      }
    }
    return "";
  }

  public getOriginWindowId(windowId: string): number {
    for (let i = 0; i < this.appStorage.length; i++) {
      if (windowId === this.appStorage[i].getWindowId()) {
        return this.appStorage[i].getOriginWindowId();
      }
    }
    return -1;
  }

  @LogMethod
  public onWidgetCreated(widgetId: number, windowName: string) {
    try {
      const windowObj = window.findWindow(windowName);
      let wId = this.paramNormalization(widgetId);
      this.widgetOwners[wId] = windowObj;
    } catch (error) {
      LogUtil.error(TAG, `Widget ${widgetId}(${windowName}) get window failed: ${error.code} ${error.msg}`);
    }
  }

  @LogMethod
  public onWidgetDestroyed(widgetId: number) {
    let wId = this.paramNormalization(widgetId);
    this.widgetOwners.delete(wId);
  }

  public getWidgetWindow(widgetId: string | number): window.Window | undefined {
    let wId = this.paramNormalization(widgetId);
    return this.widgetOwners[wId];
  }

  public onWindowCreated(windowClass: window.Window, proxy: WebProxy) {
    this.windowOwners.set(windowClass, proxy);
  }

  public onWindowClosed(windowClass: window.Window) {
    this.windowOwners.delete(windowClass);
  }

  public getWindowWebProxy(windowClass: window.Window): WebProxy | undefined {
    return this.windowOwners.get(windowClass);
  }
};
