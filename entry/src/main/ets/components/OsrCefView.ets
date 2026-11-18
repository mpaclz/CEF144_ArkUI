import arkui_cef from 'libarkui_cef.so';
import { image } from '@kit.ImageKit';
import { hilog } from "@kit.PerformanceAnalysisKit";
import { pointer } from '@kit.InputKit';
import { window, dragController } from "@kit.ArkUI";
import { common } from '@kit.AbilityKit';
import { unifiedDataChannel, uniformTypeDescriptor } from '@kit.ArkData';
import { fileIo, fileUri } from '@kit.CoreFileKit';

const DOMAIN = 0xFF01;
const MODULE = 'OsrCefView'

interface DragDataInfo {
  text: string;
  html: string;
  url: string;
  filePaths: string;
}

// Each GPU OSR instance needs a unique XComponent id so multiple
// instances on one page do not collide.
let g_gpuComponentSeq = 0;

// XComponent(SURFACE) lifecycle controller. The onSurface* overrides are
// XComponentController virtuals (API 12+) that fire only when libraryname is
// NOT set; the callbacks are injected so the controller can route events
// back into the owning OsrCefView instance.
class OsrSurfaceController extends XComponentController {
  private onCreated: (surfaceId: string) => void = (surfaceId: string) => {};
  private onChanged: (surfaceId: string, w: number, h: number) => void =
    (surfaceId: string, w: number, h: number) => {};
  private onDestroyed: (surfaceId: string) => void = (surfaceId: string) => {};

  constructor(onCreated: (surfaceId: string) => void,
              onChanged: (surfaceId: string, w: number, h: number) => void,
              onDestroyed: (surfaceId: string) => void) {
    super();
    this.onCreated = onCreated;
    this.onChanged = onChanged;
    this.onDestroyed = onDestroyed;
  }

  onSurfaceCreated(surfaceId: string): void {
    this.onCreated(surfaceId);
  }

  onSurfaceChanged(surfaceId: string, rect: SurfaceRect): void {
    this.onChanged(surfaceId, rect.surfaceWidth, rect.surfaceHeight);
  }

  onSurfaceDestroyed(surfaceId: string): void {
    this.onDestroyed(surfaceId);
  }
}

@Component
export struct OsrCefView {
  @State private browserId: number = -1;
  @State private cefCreating: boolean = false;
  @State private pixelMap: image.PixelMap | undefined = undefined;
  @State private pageTitle: string = '';
  @State private urlInput: string = '';
  // Double-buffered persistent PixelMaps. The Image component only reloads
  // when the object REFERENCE changes (in-place content updates are not
  // rendered — documented ArkUI behavior), and it keeps showing the previous
  // image while a new src loads. So each incremental frame writes the dirty
  // region into BOTH buffers (keeping their content identical) and swaps the
  // reference to the back buffer: the reload is seamless because the old
  // front image shows identical content. This replaces the earlier
  // destroy/recreate Image toggle, which flashed the black background during
  // every reload window.
  private pmA: image.PixelMap | undefined = undefined;
  private pmB: image.PixelMap | undefined = undefined;
  private showA: boolean = true;
  // Track if a PixelMap (re)creation is in progress — only used on the
  // low-frequency full-frame path (first frame / resize). Incremental
  // frames are synchronous and never need this guard.
  private pixelMapCreating: boolean = false;
  // Size (physical px) of the currently displayed PixelMap. An incoming
  // frame whose viewW/viewH differs takes the rebuild path.
  private pmWidth: number = 0;
  private pmHeight: number = 0;
  // Latest full-frame payload received while a rebuild is already in flight
  // (rapid resizes). Kept as pending and applied when the current build
  // finishes; older payloads are replaced.
  private pendingFullFrame: ArrayBuffer | undefined = undefined;
  private pendingFullFrameW: number = 0;
  private pendingFullFrameH: number = 0;
  // Track mouse button state for drag operations (scrollbar drag, text selection, etc.)
  // Tracks the LEFT button only (the only one with drag semantics).
  private mouseButtonPressed: boolean = false;
  private lastMouseX: number = 0;
  private lastMouseY: number = 0;
  // Double/triple-click tracking — ArkUI MouseEvent has no clickCount, but CEF
  // requires it in the mouse event for dblclick/triple-click recognition
  // (word/paragraph text selection). Windows defaults: 500ms, 4px slop.
  private lastClickTime: number = 0;
  private lastClickX: number = -1;
  private lastClickY: number = -1;
  private clickCount: number = 0;
  private static readonly DOUBLE_CLICK_TIME_MS: number = 500;
  private static readonly DOUBLE_CLICK_SLOP_PX: number = 4;
  // Touchpad pinch → page zoom (ctrl+wheel), like desktop Chrome's touchpad
  // pinch on macOS/Windows. Touchscreen multi-touch is forwarded to CEF
  // natively (see handleTouchEvent) — the renderer or the page itself
  // (e.g. maps) handles it, so the two input types intentionally use
  // different zoom engines (touchpad = page zoom with re-layout,
  // touchscreen = viewport zoom / page-handled).
  // Log-scale accumulation: Chromium's zoom presets are multiplicative, so a
  // log mapping makes pinch-in / pinch-out round trips symmetric (the same
  // ratio in and out emits exactly opposite step counts). One accumulated
  // step (≈15% pinch change) emits one ctrl+wheel tick.
  private pinchLastScale: number = 1.0;
  private pinchLogAccum: number = 0.0;
  private static readonly PINCH_ZOOM_STEP_LOG: number = 0.14; // ≈ Math.log(1.15): one zoom step per ~15% pinch change
  // Input Method Framework (IMF) — using C API via NAPI for cursor updates.
  // ArkTS inputMethodController was replaced because its async updateCursor()
  // triggered a second internal OnCursorUpdate that caused candidate window jumps.
  //
  // Design (reference QCefView):
  // - keyboardVisible: set by CEF OnVirtualKeyboardRequested — tracks whether
  //   the web page has a focused input field (like Qt's WA_InputMethodEnabled).
  // - IMF attach/detach: driven by window focus and keyboardVisible together:
  //   WINDOW_INACTIVE → detach (hide keyboard, preserve keyboardVisible)
  //   WINDOW_ACTIVE → if keyboardVisible, attach (show keyboard)
  //   OnVKR(true) → keyboardVisible=true, attach if window active
  //   OnVKR(false) → keyboardVisible=false, detach
  private imeAttached: boolean = false;
  private imeAttaching: boolean = false;
  // When true, key events are forwarded to CEF (user clicked on render area).
  // When false, key events go to the URL bar TextInput normally.
  private renderAreaActive: boolean = false;
  // Set by WINDOW_INACTIVE — when true, WINDOW_ACTIVE should restore CEF focus.
  private focusLostFlag: boolean = false;
  // True while the toolbar URL TextInput holds ArkTS focus. Drives the
  // conditional stopEditing in stopUrlEditing().
  private urlInputFocused: boolean = false;
  // Controller for the URL TextInput — used to exit its edit state when
  // the user clicks the web render area.
  private urlInputController: TextInputController = new TextInputController();

  // Current render-pause state (CEF WasHidden). Transition-guarded so
  // repeated onVisibleAreaChange events are idempotent. Mirrors QCefView's
  // QEvent::Show/Hide handling — window FOCUS events deliberately do not
  // participate (an unfocused window may still be visible and interactive).
  private osrHidden: boolean = false;

  // ---- XComponent direct display (NativeWindow damage path) ----
  // Primary display mode: dirty regions are written straight into window
  // buffers by the NAPI layer (vsync-aligned buffer queue, damage-aware
  // upload). Falls back to the legacy PixelMap/Image path when attach
  // fails. surfaceAttached is the non-reactive mirror of the NAPI registry
  // state; surfaceReady drives the loading overlay.
  private xcController: XComponentController | null = null;
  private surfaceAttached: boolean = false;
  // Surface info waiting for the browser to be created (either the surface
  // or the browser may come up first).
  private pendingSurfaceId: string | undefined = undefined;
  private pendingSurfaceW: number = 0;
  private pendingSurfaceH: number = 0;
  @State private useSurfaceMode: boolean = true;
  @State private surfaceReady: boolean = false;
  // Component position within the window (from globalPosition, in vp/CSS px)
  // and device scale factor — cached from onAreaChange for IME cursor computation.
  private componentGlobalX: number = 0;
  private componentGlobalY: number = 0;
  private cachedDsf: number = 1.0;
  // Cached window object — used to get windowRect (window's screen position)
  // via the synchronous getWindowProperties() API.
  private cachedWin: window.Window | null = null;
  // Last input position (touch or mouse down) in vp, relative to the render
  // area component. Used to compute the initial IME cursor position when
  // attaching the input method.
  private lastInputX: number = 0;
  private lastInputY: number = 0;
  // Window ID — passed to IMF C API via TextConfig.windowId in GetTextConfigFunc.
  private cachedWindowId: number = -1;
  // True while this view is the visible tab and owns keyboard routing
  // (window-level key filter target + IMF session). Edge-tracked in
  // setTabActive.
  private tabActive: boolean = false;
  // Latest visibility state (level-tracked, unlike the tabActive edge): the
  // first visibility edge can fire BEFORE the browser is created, so
  // onOsrCreated re-checks it to take keyboard ownership for the initial
  // visible tab.
  private visibleNow: boolean = false;
  // Drag & drop state
  // True while a system drag (initiated by dragController.executeDrag) is in progress.
  private isDragInProgress: boolean = false;
  // Last drag operation from CEF UpdateDragCursor callback.
  // DRAG_OPERATION_NONE=0, COPY=1, LINK=2, MOVE=16
  private lastDragOperation: number = 0;
  // Last drag position (view-relative CSS px) from onDragMove/onDrop.
  // Used by executeDrag callback for DragSourceEndedAt coordinates.
  // Initialized to StartDragging position as fallback (for drags that end
  // outside the component without any onDragMove/onDrop firing).
  private lastDragMoveX: number = 0;
  private lastDragMoveY: number = 0;
  // Cached drag data from CEF StartDragging — used as fallback in onDrop
  // when ArkUI DragEvent.getData() returns empty (e.g. self-drag edge cases).
  private pendingDragText: string = '';
  private pendingDragHtml: string = '';
  private pendingDragUrl: string = '';
  // Newline-separated file paths for self-drag file drop-back
  private pendingDragFilePaths: string = '';
  // True when cefDragTargetDragEnter has been called for the current drag session.
  // If true, onDrop only needs to call DragOver + Drop (Enter already done).
  // If false (external drag, getData() not available until onDrop), onDrop calls Enter + Over + Drop.
  private dragEnterCalled: boolean = false;

  @State private viewWidth: number = 800;
  @State private viewHeight: number = 600;
  @State private scrollOffsetText: string = '';
  @State private selectedText: string = '';
  @State private keyboardVisible: boolean = false;
  @State private cursorTypeText: string = 'default';
  @State private dragPreviewText: string = '';

  @Prop viewUrl: string = '';
  // Maximum OSR frame rate (fps). The render pipeline now runs fully
  // synchronous (dirty-region writePixelsSync, no per-frame createPixelMap),
  // so the historical 10fps cap is no longer required to keep the JS thread
  // healthy. 20 is a safe default; profile and raise on capable devices.
  @Prop osrFrameRate: number = 60;
  // Identity token assigned by the hosting page (plain construction
  // parameter, not reactive). Non-empty only for views living in a Tabs
  // page; the page broadcasts the active token via activeUid and each view
  // decides for itself whether the broadcast refers to it. Empty string =
  // not in a Tabs context (tear-off window): the broadcast channel is
  // disabled and visibility follows onVisibleAreaChange only.
  viewUid: string = '';
  // Unique focus target id for the render area. Focus is requested here on
  // activation so the ArkTS onKeyEvent fallback channel (below) receives
  // key events without requiring a prior click on the render area.
  private renderAreaKey: string = 'osrRender';
  // Focus target id on the invisible anchor leaf — the only focusable node
  // in the render area (containers can't be focused, XComponent isn't
  // focusable).
  private focusAnchorKey: string = 'osrAnchor';
  // Focus-exit targets for TAB key flow across the web-view boundary (see
  // moveFocusOutOfWeb): when the web content tabs past its last (or before
  // its first) focusable element, CEF calls OnTakeFocus and the host must
  // move ArkUI focus out of the web view (QCefView: onCefWindowLostTabFocus
  // → nextInFocusChain/previousInFocusChain).
  // Backward default: the toolbar's last button ('Go') — the render area's
  // traversal-previous, an addressable stop → one-press exit.
  // Forward default: there is NO addressable stop after the render area
  // (the traversal wraps into the built-in TabBar, whose items carry no
  // requestFocus-able id), so the focus token is left ON the render-area
  // stop itself with the keyboard already released — visible as the
  // whole-view focus ring; the NEXT Tab continues the framework traversal
  // (wrap → TabBar active item → toolbar …). The containing page can
  // override either direction by passing explicit ids.
  focusNextId: string = '';
  focusPrevId: string = '';
  // Id of the last toolbar button ('Go') — default backward exit target.
  private toolbarLastKey: string = 'osrToolbarLast';
  // Suppresses the render-area blur deactivation while focus is being moved
  // INSIDE the render area (re-anchoring between the Column and the anchor
  // leaf). requestFocus takes effect in the current frame, so a same-tick
  // reset is sufficient.
  private focusMoveGuard: boolean = false;
  // True right after a web→native focus exit (moveFocusOutOfWeb). Suppresses
  // the per-keydown focus re-anchor (its setTimeout may still be pending and
  // would pull focus back onto the render area, undoing the exit) and the
  // web re-activation while the exit's traversal is settling. Cleared
  // deterministically when focus leaves the render area (the traversal
  // proceeded), on tab (re)activation, or by the safety timeout.
  private webExitArmed: boolean = false;
  // Global counter for window instances without a tab index (tear-off windows).
  private static nextWindowUid: number = 1;
  // Currently active identity token, synced from the page's Tabs.onChange
  // (the page maps the tab index to a token via its tab table). @Watch
  // routes activation: onChange fires reliably on EVERY switch including the
  // first one — unlike the visibility edge, which does not fire when a
  // TabContent's component tree is attached for the first time. A single
  // watch drives BOTH the keyboard ownership and the GPU visibility channel
  // (they share one event source; the old numeric-index comparison and the
  // separate gpuTabActive prop are gone).
  @Prop @Watch('onActiveUidChanged') activeUid: string = '';
  @Prop gpuEnabled: boolean = false;
  // Per-instance XComponent id; the SurfaceId route sets no libraryname=adapter.
  private gpuComponentId: string = 'osr_gpu_' + (++g_gpuComponentSeq);
  private disposed: boolean = false;
  // surfaceReady reuses the CPU-mode @State above (GPU callbacks write,
  // build reads; both modes share it).
  private surfaceSizeReady: boolean = false;
  private layoutSizeReady: boolean = false;
  private gpuSurfaceId: string = '';
  private creationGeneration: number = 0;
  private pendingBrowserId: number = -1;
  private gpuController: XComponentController = new OsrSurfaceController(
    (id: string) => this.onGpuSurfaceCreated(id),
    (id: string, w: number, h: number) => this.onGpuSurfaceChanged(id, w, h),
    (id: string) => this.onGpuSurfaceDestroyed(id));

  // Unified "is this view the one the user is looking at" decision for the
  // Tabs broadcast channel. Empty viewUid = no Tabs context.
  private isMyTurn(): boolean {
    return this.viewUid !== '' && this.viewUid === this.activeUid;
  }

  private onActiveUidChanged(): void {
    const active = this.isMyTurn();
    hilog.info(DOMAIN, MODULE, "onActiveUidChanged: uid=%{public}s activeUid=%{public}s active=%{public}s, browserId=%{public}d",
      this.viewUid, this.activeUid, String(active), this.browserId);
    if (active) {
      // Focus does not depend on the browser being alive — request it even
      // while CEF is still initializing, so keys work the moment the page
      // becomes interactive.
      this.requestFocusToRenderArea();
    }
    // Keyboard ownership channel (edge-tracked in setTabActive).
    this.setTabActive(active);
    // GPU visibility channel: WasHidden stops layout/frame capture in the
    // hidden tab and throttles the renderer. Browser not created yet: the
    // native side has a pending fallback, and onOsrCreated re-checks.
    if (this.gpuEnabled && this.isCreated()) {
      arkui_cef.cefSetWindowlessVisibility(this.browserId, active);
    }
  }

  aboutToAppear(): void {
    this.urlInput = this.viewUrl;
    // Focus target ids must be unique page-wide. Tabs pages pass a viewUid
    // token; fall back to a global counter when absent. Tokens must stay
    // id-safe (ASCII, no spaces).
    let uid = this.viewUid !== '' ? this.viewUid : 'w' + (OsrCefView.nextWindowUid++);
    this.renderAreaKey = 'osrRender_' + uid;
    this.focusAnchorKey = 'osrAnchor_' + uid;
    this.toolbarLastKey = 'osrToolbarLast_' + uid;
    this.xcController = new OsrSurfaceController(
      (surfaceId: string) => { this.onXcSurfaceCreated(surfaceId); },
      (surfaceId: string, w: number, h: number) => { this.onXcSurfaceChanged(surfaceId, w, h); },
      (surfaceId: string) => { this.onXcSurfaceDestroyed(surfaceId); });
    if (this.gpuEnabled) {
      // GPU mode defers browser creation until the Surface is ready with a
      // valid size; do not create early here.
      hilog.info(DOMAIN, MODULE, "GPU OSR waiting for XComponent surface before create");
      return;
    }
    if (this.browserId <= 0) {
      // Use physical pixels for CEF buffer size (OnPaint outputs physical pixels).
      // DSF is set to the actual device scale factor so CEF's GetViewRect returns
      // CSS pixels, making web content layout match non-OSR tabs.
      let dsf = this.getUIContext().vp2px(1);
      let wpx = Math.round(this.viewWidth * dsf);
      let hpx = Math.round(this.viewHeight * dsf);
      // Store physical-pixel dimensions for OnPaint buffer allocation
      this.viewWidth = wpx;
      this.viewHeight = hpx;
      hilog.info(DOMAIN, MODULE, "Begin to create OSR browser. dsf=%{public}f, size=%{public}d X %{public}d",
        dsf, wpx, hpx);
      this.createOsrCefView(this.viewUrl, wpx, hpx);
    }
  }

  aboutToDisappear(): void {
    // Set the disposal flag first: late surface/browser callbacks must not
    // revive a vanished component.
    this.disposed = true;
    this.creationGeneration++;
    this.destroy();
  }

  build() {
    Column() {
      // Toolbar
      Row() {
        Button('Back')
          .fontSize(12)
          .height(28)
          .enabled(this.isCreated())
          .onClick(() => this.goBack())

        Button('Forward')
          .fontSize(12)
          .height(28)
          .margin({ left: 4 })
          .enabled(this.isCreated())
          .onClick(() => this.goForward())

        Button('Reload')
          .fontSize(12)
          .height(28)
          .margin({ left: 4 })
          .enabled(this.isCreated())
          .onClick(() => this.reload())

        TextInput({ text: this.urlInput, controller: this.urlInputController })
          .fontSize(12)
          .height(28)
          .layoutWeight(1)
          .margin({ left: 4 })
          .enabled(this.isCreated())
          .onChange((val: string) => { this.urlInput = val; })
          .onSubmit(() => this.loadUrl(this.urlInput))
          .onFocus(() => { this.urlInputFocused = true; })
          .onBlur(() => { this.urlInputFocused = false; })
          .onClick(() => {
            this.setRenderAreaActive(false);
          })

        Button('Go')
          .fontSize(12)
          .height(28)
          .margin({ left: 4 })
          .enabled(this.isCreated())
          .id(this.toolbarLastKey)
          .onClick(() => this.loadUrl(this.urlInput))
      }
      .width('100%')
      .height(36)
      .padding({ left: 4, right: 4 })
      .backgroundColor(Color.White)

      // Title bar
      if (this.pageTitle) {
        Text(this.pageTitle)
          .fontSize(10)
          .fontColor('#666666')
          .width('100%')
          .height(18)
          .padding({ left: 8, right: 8 })
          .backgroundColor('#F0F0F0')
          .maxLines(1)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
      }

      // Render area — all events are captured and forwarded to CEF
      Column() {
        if (this.gpuEnabled) {
          // The GPU present node is permanent and the Surface lifecycle is
          // controller-managed; creation does not depend on browserId.
          // NOTE: the XComponent (SURFACE type) is NOT focusable — key events
          // and focus land on the anchor leaf below and the parent Column.
          XComponent({ id: this.gpuComponentId, type: XComponentType.SURFACE,
            controller: this.gpuController })
            .width('100%')
            .height('100%')
        } else if (this.useSurfaceMode) {
          // Direct-display mode: the NAPI layer writes CEF dirty regions
          // straight into the XComponent's window buffer queue (damage-
          // aware, vsync-aligned). No PixelMap, no per-frame @State churn.
          Stack() {
            if (this.xcController) {
              // NOTE: the XComponent (SURFACE type) is NOT focusable —
              // key events and focus land on the anchor leaf below and on
              // the parent Column instead.
              XComponent({ type: XComponentType.SURFACE, controller: this.xcController })
                .width('100%')
                .height('100%')
            }
            if (!this.surfaceReady) {
              Text('OSR Loading...')
                .fontSize(16)
                .fontColor(Color.Gray)
            }
          }
          .width('100%')
          .height('100%')
        } else if (this.pixelMap) {
          // Fallback display (surface attach failed): legacy PixelMap path.
          Image(this.pixelMap)
            .width('100%')
            .height('100%')
            .objectFit(ImageFit.Fill)
        } else {
          Column() {
            Text('OSR Loading...')
              .fontSize(16)
              .fontColor(Color.Gray)
          }
          .width('100%')
          .height('100%')
          .justifyContent(FlexAlign.Center)
          .backgroundColor(Color.Black)
        }
        // Invisible focus anchor. Neither the render-area Column (ArkUI
        // containers cannot take focus directly) nor the XComponent
        // ("not focusable" per requestFocus's own error) can be focused by
        // id, so activation requests focus HERE: a non-container focusable
        // leaf. Key events received by the anchor bubble up to the render
        // area's handler below; the anchor never intercepts touches.
        Text('')
          .focusable(true)
          .id(this.focusAnchorKey)
          .width(1)
          .height(1)
          .opacity(0)
          .hitTestBehavior(HitTestMode.None)
          .onFocus(() => {
            this.onRenderAreaFocusGained();
          })
          .onBlur(() => {
            this.onRenderAreaFocusLost();
          })
          .onKeyEvent((event: KeyEvent) => {
            this.handleRenderAreaKeyEvent(event);
          })
      }
      .width('100%')
      // layoutWeight allocates strictly from the remaining space: flexGrow
      // gets blown out of the parent by the child's intrinsic 100%-height
      // demand (the first layout pass resolves to the full TabContent height),
      // clipping the bottom of the render area and pushing the status bar out
      // of the window. layoutWeight ignores content demand and takes only the
      // leftover space.
      .layoutWeight(1)
      .clip(true)
      .backgroundColor(Color.Black)
      .hitTestBehavior(HitTestMode.Block)
      .focusable(true)
      .tabStop(true)
      .id(this.renderAreaKey)
      .onFocus(() => {
        this.onRenderAreaFocusGained();
      })
      .onBlur(() => {
        this.onRenderAreaFocusLost();
      })
      .onKeyEvent((event: KeyEvent) => {
        this.handleRenderAreaKeyEvent(event);
      })
      .onTouch((event: TouchEvent) => this.handleTouchEvent(event))
      .onMouse((event: MouseEvent) => this.handleMouseEvent(event))
      .onAxisEvent((event: AxisEvent) => this.handleAxisEvent(event))
      .onHover((isHover: boolean, _event: HoverEvent) => {
        // Fires for mouse/touchpad only (touch never triggers hover). When the
        // pointer LEAVES the render area, notify CEF so the page clears its
        // hover state (e.g. hovered links stop highlighting). Skip during an
        // in-progress left-button drag or a system HTML5 drag — the press
        // stream must continue.
        if (!isHover) {
          // Reset the window cursor to the default arrow. The cursor style
          // set via CursorController is WINDOW-scoped and CEF never emits a
          // cursor change for positions outside its view — without this,
          // the last web-page style (e.g. I-beam from the input test page)
          // persists over the tab bar / window title bar. Skip during a
          // system drag (the drag manager owns the cursor then).
          if (!this.isDragInProgress) {
            try {
              let cursorController = this.getUIContext().getCursorController();
              if (cursorController) {
                cursorController.restoreDefault();
              }
            } catch (e) {
              hilog.error(DOMAIN, MODULE, "restoreDefault failed: %{public}s", (e as Error).message);
            }
          }
          if (this.isCreated() && !this.mouseButtonPressed && !this.isDragInProgress) {
            arkui_cef.cefSendMouseMove(this.browserId, this.lastMouseX, this.lastMouseY, true);
          }
        }
      })
      .gesture(
        PinchGesture({ fingers: 2, distance: 0.1 })
          .onActionStart((event: GestureEvent) => {
            if (event.source === SourceType.TouchScreen) {
              // Touchscreen multi-touch is forwarded to CEF natively in
              // handleTouchEvent — the renderer (or the page, e.g. maps)
              // handles the pinch itself. Adding ctrl+wheel here would
              // double-zoom.
              return;
            }
            // Touchpad pinch → ctrl+wheel page zoom (25%-500% with re-layout),
            // like desktop Chrome's touchpad pinch. Touchpad input never
            // enters the touch stream, so this cannot conflict with
            // page-handled multi-touch.
            this.pinchLastScale = event.scale;
            this.pinchLogAccum = 0.0;
          })
          .onActionUpdate((event: GestureEvent) => {
            if (event.source === SourceType.TouchScreen) {
              return;
            }
            if (event.scale <= 0) {
              return;
            }
            // Log-scale accumulation: zoom presets are multiplicative, so log
            // mapping makes pinch-in / pinch-out round trips symmetric.
            this.pinchLogAccum += Math.log(event.scale / this.pinchLastScale);
            this.pinchLastScale = event.scale;
            // Emit ONE ctrl+wheel zoom tick per accumulated ~15% change
            // (Chromium zooms one preset step per wheel event).
            while (Math.abs(this.pinchLogAccum) >= OsrCefView.PINCH_ZOOM_STEP_LOG) {
              let deltaY = this.pinchLogAccum > 0 ? 120 : -120;
              // Touchpad pinch is a mouse-category gesture (single fingerList
              // record per docs); guard against an empty list as insurance.
              let px = event.fingerList.length > 0 ? Math.round(event.fingerList[0].localX) : this.lastMouseX;
              let py = event.fingerList.length > 0 ? Math.round(event.fingerList[0].localY) : this.lastMouseY;
              arkui_cef.cefSendMouseWheel(this.browserId, px, py, 0, deltaY, 0x04);
              let sign = this.pinchLogAccum > 0 ? 1 : -1;
              this.pinchLogAccum -= sign * OsrCefView.PINCH_ZOOM_STEP_LOG;
            }
          })
          .onActionEnd(() => {
            this.pinchLastScale = 1.0;
            this.pinchLogAccum = 0.0;
          })
      )
      .onAreaChange((_oldValue: Area, newValue: Area) => {
        let wpx = Math.round(this.getUIContext().vp2px(newValue.width as number));
        let hpx = Math.round(this.getUIContext().vp2px(newValue.height as number));

        // Cache component position and DSF for IME cursor computation
        this.componentGlobalX = newValue.globalPosition.x as number;
        this.componentGlobalY = newValue.globalPosition.y as number;
        this.cachedDsf = this.getUIContext().vp2px(1);

        if (wpx > 0 && hpx > 0) {
          this.viewWidth = wpx;
          this.viewHeight = hpx;
          this.layoutSizeReady = true;
          // GPU mode: once real layout sizes arrive and the Surface is
          // ready, creation can proceed.
          if (this.gpuEnabled && this.surfaceReady) {
            this.surfaceSizeReady = true;
            this.tryCreateGpuBrowser();
          }

          if (this.isCreated()) {
            // Set DSF to actual device scale factor for correct content sizing
            arkui_cef.cefOsrSetScale(this.browserId, this.cachedDsf);
            // Resize driver split: in surface mode the XComponent's
            // onSurfaceChanged is the authoritative resize source (it
            // reports the exact surface geometry); skip the duplicate post
            // here. In fallback (PixelMap) mode this remains the driver.
            if (!this.surfaceAttached) {
              arkui_cef.cefOsrResize(this.browserId, this.viewWidth, this.viewHeight);
            }
            // Pass component offset (window-relative, from globalPosition) for
            // cursor coordinate conversion. ArkTS adds windowRect.left/top
            // separately to get screen-absolute coordinates for IMF.
            arkui_cef.cefSetComponentOffset(this.browserId,
              Math.round(newValue.globalPosition.x as number),
              Math.round(newValue.globalPosition.y as number));
            this.syncWindowOrigin();
          }
        }
      })
      .onDragEnter((event: DragEvent) => {
        if (!this.isCreated()) return;
        let x = Math.round(event.getWindowX() - this.componentGlobalX);
        let y = Math.round(event.getWindowY() - this.componentGlobalY);
        // Self-drag: data is available in pendingDrag* (from CEF StartDragging).
        // External drag: getData() not available until onDrop — defer.
        if (this.isDragInProgress && (this.pendingDragText.length > 0 || this.pendingDragUrl.length > 0 ||
            this.pendingDragFilePaths.length > 0)) {
          hilog.info(DOMAIN, MODULE, "onDragEnter: self-drag, calling DragTargetDragEnter with pending data");
          arkui_cef.cefDragTargetDragEnter(this.browserId,
            this.pendingDragText, this.pendingDragHtml, this.pendingDragUrl,
            this.pendingDragFilePaths, x, y, 19);
          this.dragEnterCalled = true;
        } else {
          hilog.info(DOMAIN, MODULE, "onDragEnter: external drag, deferring to onDrop");
        }
        event.setResult(DragResult.DROP_ENABLED);
        event.dragBehavior = DragBehavior.MOVE;
      })
      .onDragMove((event: DragEvent) => {
        if (!this.isCreated()) return;
        let x = Math.round(event.getWindowX() - this.componentGlobalX);
        let y = Math.round(event.getWindowY() - this.componentGlobalY);
        // Track last drag position for DragSourceEndedAt (fix #1).
        this.lastDragMoveX = x;
        this.lastDragMoveY = y;
        if (this.dragEnterCalled) {
          arkui_cef.cefDragTargetDragOver(this.browserId, x, y, 19);
          // Use CEF's UpdateDragCursor feedback to enable/disable drop (fix #4).
          // lastDragOperation reflects CEF's response from the previous DragTargetDragOver.
          // For external drag (dragEnterCalled=false), always enable since CEF hasn't been notified yet.
          if (this.lastDragOperation === 0) {
            event.setResult(DragResult.DROP_DISABLED);
          } else {
            event.setResult(DragResult.DROP_ENABLED);
          }
        } else {
          event.setResult(DragResult.DROP_ENABLED);
        }
        event.dragBehavior = DragBehavior.MOVE;
      })
      .onDragLeave((event: DragEvent) => {
        if (!this.isCreated()) return;
        hilog.info(DOMAIN, MODULE, "onDragLeave: dragEnterCalled=%{public}s",
          this.dragEnterCalled ? "true" : "false");
        if (this.dragEnterCalled) {
          arkui_cef.cefDragTargetDragLeave(this.browserId);
          this.dragEnterCalled = false;
        }
      })
      .onDrop((event: DragEvent) => {
        if (!this.isCreated()) return;
        let x = Math.round(event.getWindowX() - this.componentGlobalX);
        let y = Math.round(event.getWindowY() - this.componentGlobalY);
        // Track final drag position for DragSourceEndedAt (fix #1).
        this.lastDragMoveX = x;
        this.lastDragMoveY = y;

        if (this.dragEnterCalled) {
          // Self-drag: DragTargetDragEnter was already called in onDragEnter.
          // Just call DragOver (final position) + Drop.
          hilog.info(DOMAIN, MODULE, "onDrop: self-drag (enter already called)");
          arkui_cef.cefDragTargetDragOver(this.browserId, x, y, 19);
          arkui_cef.cefDragTargetDrop(this.browserId, x, y);
        } else {
          // External drag: getData() is available now (only in onDrop).
          // Call Enter + Over + Drop in sequence.
          let data = this.extractDragData(event);
          hilog.info(DOMAIN, MODULE, "onDrop: external drag, text_len=%{public}d, url='%{public}s'",
            data.text.length, data.url);
          arkui_cef.cefDragTargetDragEnter(this.browserId, data.text, data.html, data.url, data.filePaths, x, y, 19);
          arkui_cef.cefDragTargetDragOver(this.browserId, x, y, 19);
          arkui_cef.cefDragTargetDrop(this.browserId, x, y);
        }

        // Clear state.
        this.dragEnterCalled = false;
        this.pendingDragText = '';
        this.pendingDragHtml = '';
        this.pendingDragUrl = '';
        this.pendingDragFilePaths = '';
        event.setResult(DragResult.DRAG_SUCCESSFUL);
        event.dragBehavior = DragBehavior.MOVE;
      })
      .allowDrop([
        uniformTypeDescriptor.UniformDataType.PLAIN_TEXT,
        uniformTypeDescriptor.UniformDataType.HTML,
        uniformTypeDescriptor.UniformDataType.HYPERLINK,
        uniformTypeDescriptor.UniformDataType.FILE,
        uniformTypeDescriptor.UniformDataType.IMAGE
      ])

      // Status bar — shows OSR state info. NOT part of the web: ArkUI native
      // Texts fed by CEF callbacks (scroll offset / cursor type). Deliberately
      // NOT focusable — the status bar does not participate in TAB traversal.
      if (this.isCreated()) {
        Row() {
          Text(`Scroll: ${this.scrollOffsetText || '0,0'}`)
            .fontSize(9)
            .fontColor('#999999')
            .layoutWeight(1)
            .maxLines(1)
            .textOverflow({ overflow: TextOverflow.Ellipsis })

          Text(`Cursor: ${this.cursorTypeText}`)
            .fontSize(9)
            .fontColor('#66AA66')
        }
        .width('100%')
        .height(16)
        .padding({ left: 8, right: 8 })
        .backgroundColor('#F5F5F5')
      }
    }
    .width('100%')
    .height('100%')
    .backgroundColor(Color.Black)
    .focusable(true)
    .onVisibleAreaChange([0.0, 0.01], (isVisible: boolean, currentRatio: number) => {
      this.visibleNow = isVisible && currentRatio > 0;
      // Pause/resume CEF rendering on full (in)visibility — tab switches and
      // scrolling off-screen. Same pattern as QCefView's QEvent::Show/Hide
      // → WasHidden().
      this.setTabActive(this.visibleNow);
    })
  }

  // Keyboard ownership follows the VISIBLE tab: a tab switch hands over the
  // window-level key filter, CEF focus and the IMF session to the tab the
  // user is looking at — without requiring a click first. Leaving the screen
  // tears the IMF session DOWN (not just hides the panel): a lingering IMS
  // binding keeps synthesizing candidates at stale cursor positions and
  // commits text into the previously focused field of an invisible browser.
  // Edge-triggered via the tabActive guard.
  // Retry delay for the post-activation focus request. A request issued
  // mid-switch-animation can be overridden by the Tabs/TabBar focus
  // management when the animation completes, so focus is re-requested after
  // it. MUST exceed BrowserTabsPage's Tabs animationDuration (currently
  // 300ms) — keep the two in sync when changing either.
  private static readonly FOCUS_RETRY_DELAY_MS: number = 400;
  // Delay before showing the IME panel after WINDOW_ACTIVE. Sceneboard
  // issues a system-level RequestHideInput (HIDE_INPUT_METHOD_ANIMATION)
  // ~5ms into the window-switch transition and it lands ~80ms in; a panel
  // shown before that gets killed (status bar flicker). The session itself
  // attaches immediately — only the panel show waits. Measured hide lands
  // by ~85ms; 150ms keeps a safety margin while staying imperceptible.
  private static readonly IME_PANEL_SHOW_DELAY_MS: number = 150;

  private setTabActive(active: boolean): void {
    if (!this.isCreated() || this.tabActive === active) {
      return;
    }
    this.tabActive = active;
    hilog.info(DOMAIN, MODULE, "OSRKEY setTabActive: %{public}s id=%{public}d",
      active ? "activate" : "deactivate", this.browserId);
    this.setOsrHidden(!active);
    if (active) {
      // Fresh activation resets the web-exit suppression (a stale armed flag
      // would swallow the focus anchor requests below).
      this.webExitArmed = false;
      this.setRenderAreaActive(true);
      arkui_cef.cefSendFocusEvent(this.browserId, true);
      // Move ArkUI focus onto the render area so the onKeyEvent fallback
      // channel receives key presses without requiring a click first. Retried
      // after the switch animation — see FOCUS_RETRY_DELAY_MS.
      this.requestFocusToRenderArea();
      setTimeout(() => {
        this.requestFocusToRenderArea();
      }, OsrCefView.FOCUS_RETRY_DELAY_MS);
      if (this.keyboardVisible) {
        // CEF believed an input field was focused before the switch. The
        // focus restoration above may NOT re-fire VKR(true) (the renderer
        // dedupes TextInputState changes), so the IMF session must be
        // recreated here — otherwise the blinking caret sits over a dead
        // keyboard (no panel, no candidates, no text).
        //
        // Defer past the Tabs switch animation: it drives a late ArkUI focus
        // reshuffle whose ClearInputMethodProxy teardown lands ~300ms after
        // activation and kills an immediately-attached session. Attaching at
        // FOCUS_RETRY_DELAY_MS (> animationDuration) lands after that
        // teardown; handleImeFocus()'s attach-or-heal self-corrects a stale
        // imeAttached flag.
        setTimeout(() => {
          if (this.disposed || !this.tabActive) {
            return;
          }
          this.handleImeFocus();
        }, OsrCefView.FOCUS_RETRY_DELAY_MS);
      }
    } else {
      if (this.imeAttached && !this.urlInputFocused) {
        this.detachInputMethod();
      }
      this.setRenderAreaActive(false);
    }
  }

  // ---- ArkTS key event fallback channel ----
  // The window-level KeyEventFilter is not invoked for every key press (its
  // dispatch by the input pipeline depends on the ArkUI focus state, as
  // observed: keys only reach the filter after the render area has been
  // clicked). This in-component channel covers the gap: once the render area
  // holds ArkUI focus (requested on activation), key events arrive here and
  // non-letter keys are forwarded to CEF. Letters are passed through so the
  // physical-key IME path keeps working. The two channels are complementary:
  // a filter interception consumes the event before ArkUI dispatch, so no
  // double forwarding can occur.
  private handleRenderAreaKeyEvent(event: KeyEvent): void {
    if (!this.isCreated() || !this.renderAreaActive) {
      return;
    }

    let mods = 0;
    if (event.getModifierKeyState) {
      try {
        if (event.getModifierKeyState(['Shift'])) {
          mods |= 0x02; // EVENTFLAG_SHIFT_DOWN
        }
        if (event.getModifierKeyState(['Ctrl'])) {
          mods |= 0x04; // EVENTFLAG_CONTROL_DOWN
        }
        if (event.getModifierKeyState(['Alt'])) {
          mods |= 0x08; // EVENTFLAG_ALT_DOWN
        }
      } catch (e) {
      }
    }

    // Letters A-Z without Ctrl/Alt: let the IME synthesize (CJK composition
    // + English text) — do not consume.
    let hasCtrl = (mods & 0x04) !== 0;
    let hasAlt = (mods & 0x08) !== 0;
    if (event.keyCode >= 2017 && event.keyCode <= 2042 && !hasCtrl && !hasAlt) {
      return;
    }

    // Non-letter keys: forward to CEF. KeyType.Down=0 (raw keydown) and
    // KeyType.Up=1 (keyup) match the CefKeyEvent type mapping.
    let keyType = event.type === KeyType.Up ? 1 : 0;
    let charCode = event.unicode !== undefined ? event.unicode : 0;
    arkui_cef.cefSendKeyEvent(this.browserId, event.keyCode, keyType, mods, charCode);

    // Char event for text keys (mirrors the window-filter path): Blink needs
    // the CHAR event for the key's default text/activation behavior.
    let isTextKey =
      (event.keyCode >= 2000 && event.keyCode <= 2009) ||  // Digits 0-9
      event.keyCode === 2050 ||                             // Space
      event.keyCode === 2054 ||                             // Enter
      event.keyCode === 2049;                               // Tab
    if (isTextKey && !hasCtrl && !hasAlt && event.type === KeyType.Down) {
      arkui_cef.cefSendKeyEvent(this.browserId, event.keyCode, 2 /*CHAR*/, mods, charCode);
    }

    // Consume the event and keep focus on the render area — ArkUI's own
    // focus traversal would otherwise move focus away on Tab/arrow keys.
    event.stopPropagation();
    if (event.type === KeyType.Down) {
      setTimeout(() => {
        this.requestFocusToRenderArea();
      }, 0);
    }
  }

  private requestFocusToRenderArea(): void {
    // Target the invisible anchor leaf — the only requestFocus-capable node:
    // the render-area Column is a container (cannot take focus directly) and
    // the XComponent reports "not focusable".
    // Skip while a web→native exit is settling: the pending per-keydown
    // re-anchor timer would otherwise pull focus back onto the render area
    // right after moveFocusOutOfWeb moved it out.
    if (this.webExitArmed) {
      return;
    }
    // focusMoveGuard: re-anchoring INSIDE the render area (Column → anchor)
    // fires the Column's onBlur; that must not release the web keyboard.
    // requestFocus takes effect in the current frame, so the guard only needs
    // to span this task tick.
    this.focusMoveGuard = true;
    try {
      this.getUIContext().getFocusController().requestFocus(this.focusAnchorKey);
    } catch (e) {
      hilog.warn(DOMAIN, MODULE, "requestFocus failed: %{public}s %{public}s",
        this.focusAnchorKey, (e as Error).message);
    }
    setTimeout(() => {
      this.focusMoveGuard = false;
    }, 0);
  }

  getBrowserId(): number {
    return this.browserId;
  }

  isCreated(): boolean {
    return this.browserId > 0;
  }

  // Pause/resume the CEF render pipeline (WasHidden). Called from visibility
  // transitions only — the osrHidden guard makes repeated events idempotent.
  private setOsrHidden(hidden: boolean): void {
    if (!this.isCreated() || this.osrHidden === hidden) {
      return;
    }
    this.osrHidden = hidden;
    hilog.info(DOMAIN, MODULE, "setOsrHidden: %{public}s", hidden ? "true" : "false");
    arkui_cef.cefWasHidden(this.browserId, hidden);
  }

  // ---- XComponent surface lifecycle ----

  private onXcSurfaceCreated(surfaceId: string): void {
    hilog.info(DOMAIN, MODULE, "onXcSurfaceCreated: %{public}s, browserId=%{public}d",
      surfaceId, this.browserId);
    if (this.surfaceAttached) {
      return;
    }
    this.pendingSurfaceId = surfaceId;
    this.attachSurfaceIfPossible();
  }

  private onXcSurfaceChanged(surfaceId: string, w: number, h: number): void {
    hilog.info(DOMAIN, MODULE, "onXcSurfaceChanged: %{public}s, %{public}dx%{public}d",
      surfaceId, w, h);
    this.pendingSurfaceW = w;
    this.pendingSurfaceH = h;
    // The XComponent rect is the AUTHORITATIVE render-area size (physical
    // px) — keep viewWidth/viewHeight and the browser in lockstep with it.
    // The Column's onAreaChange also drives this, but its first firing can
    // race the browser creation (or not fire at all in some layouts),
    // leaving the browser stuck at the aboutToAppear default
    // 800x600*dsf — observed as a persistent size mismatch between the
    // CEF frame and the surface.
    if (w > 0 && h > 0) {
      this.viewWidth = w;
      this.viewHeight = h;
      if (this.isCreated()) {
        arkui_cef.cefOsrResize(this.browserId, w, h);
      }
    }
    if (this.surfaceAttached && this.isCreated()) {
      arkui_cef.updateOsrSurfaceGeometry(this.browserId, w, h);
    }
    // The attach may have been deferred because the surface size was
    // unknown - the size has now arrived, retry immediately (otherwise
    // attach waits for the next surface event and the screen stays black in
    // between).
    if (!this.surfaceAttached) {
      this.attachSurfaceIfPossible();
    }
  }

  private onXcSurfaceDestroyed(surfaceId: string): void {
    hilog.info(DOMAIN, MODULE, "onXcSurfaceDestroyed: %{public}s", surfaceId);
    this.pendingSurfaceId = undefined;
    this.pendingSurfaceW = 0;
    this.pendingSurfaceH = 0;
    if (this.surfaceAttached) {
      this.surfaceAttached = false;
      this.surfaceReady = false;
      if (this.isCreated()) {
        arkui_cef.detachOsrSurface(this.browserId);
      }
    }
  }

  // Attaches the pending surface once BOTH the surface and the browser
  // exist (either may come up first). On attach failure the view falls back
  // to the legacy PixelMap display for the rest of its lifetime.
  private attachSurfaceIfPossible(): void {
    let surfaceId = this.pendingSurfaceId;
    if (!this.isCreated() || this.surfaceAttached || surfaceId === undefined) {
      return;
    }
    let w = this.pendingSurfaceW;
    let h = this.pendingSurfaceH;
    // Strictly require the real surface size reported by the XComponent
    // (from onSurfaceChanged); never fall back to viewWidth (its 800x600
    // default registers wrong geometry and triggers a chain of size-mismatch
    // dropped frames). onSurfaceChanged always follows onSurfaceCreated and
    // retries this method at its end.
    if (w <= 0 || h <= 0) {
      hilog.info(DOMAIN, MODULE,
        "attachSurfaceIfPossible: defer (surface size unknown), browserId=%{public}d", this.browserId);
      return;
    }
    // Pass the DSF so the native layer derives the size tolerance from it
    // (max frame/surface drift = physical mod dsf, bounded by ceil(dsf)).
    let ok = arkui_cef.attachOsrSurface(this.browserId, surfaceId, w, h, this.cachedDsf);
    if (ok) {
      this.surfaceAttached = true;
      this.surfaceReady = true;
      // Force the browser to the surface's size — they must agree (the
      // surface write path drops frames while they mismatch). This also
      // recovers from any stale viewWidth captured before layout settled.
      arkui_cef.cefOsrResize(this.browserId, w, h);
      // The surface geometry may have settled after the browser was created
      // (e.g. initial layout) — sync it now.
      if (this.pendingSurfaceW > 0) {
        arkui_cef.updateOsrSurfaceGeometry(this.browserId, this.pendingSurfaceW, this.pendingSurfaceH);
      }
      // After a successful attach in ANY mode, force a full repaint: while
      // no surface was attached, paint-event frame metadata was consumed and
      // dropped (napi drops unobservable frames), so the surface's first
      // frame for static content must be regenerated by invalidate -
      // otherwise the screen stays black until the user
      // clicks/scrolls/switches tabs.
      arkui_cef.cefInvalidate(this.browserId);
    } else {
      hilog.error(DOMAIN, MODULE, "attachOsrSurface failed, browserId=%{public}d, surface=%{public}s", this.browserId, String(this.pendingSurfaceId));
      this.useSurfaceMode = false;
      this.pendingSurfaceId = undefined;
    }
  }

  destroy(): void {
    // Bump the generation and set the disposal flag first: this cancels the
    // create loop that may be waiting for CEF, and makes a late onOsrCreated
    // close the new browser immediately instead of binding it to a vanished
    // component.
    this.disposed = true;
    this.creationGeneration++;
    this.detachInputMethod();
    if (this.pendingBrowserId > 0) {
      arkui_cef.destroyOsrCefView(this.pendingBrowserId);
      this.pendingBrowserId = -1;
    }
    if (this.surfaceAttached) {
      // Release the window handle before browser teardown (the NAPI layer
      // also cleans it up defensively in destroyOsrCefView).
      this.surfaceAttached = false;
      this.surfaceReady = false;
      if (this.isCreated()) {
        arkui_cef.detachOsrSurface(this.browserId);
      }
    }
    this.pendingSurfaceId = undefined;
    if (this.isCreated()) {
      hilog.info(DOMAIN, MODULE, "Destroying OSR browser, id: %{public}d", this.browserId);
      // Release native pixelmap handles BEFORE the ArkTS PixelMaps below —
      // the handles must never outlive the memory they point to.
      arkui_cef.unregisterOsrPixelmaps(this.browserId);
      arkui_cef.destroyOsrCefView(this.browserId);
      this.browserId = -1;
    }
    // this.pixelMap aliases pmA/pmB — release the owners only.
    if (this.pmA && this.pmA !== this.pmB) {
      this.pmA.release();
    }
    if (this.pmB) {
      this.pmB.release();
    }
    this.pmA = undefined;
    this.pmB = undefined;
    this.pixelMap = undefined;
    this.showA = true;
    this.pmWidth = 0;
    this.pmHeight = 0;
    this.pendingFullFrame = undefined;
    this.pixelMapCreating = false;
  }

  goBack(): void {
    if (this.isCreated()) {
      arkui_cef.cefGoBack(this.browserId);
    }
  }

  goForward(): void {
    if (this.isCreated()) {
      arkui_cef.cefGoForward(this.browserId);
    }
  }

  reload(): void {
    if (this.isCreated()) {
      arkui_cef.cefReload(this.browserId);
    }
  }


  loadUrl(url: string): void {
    if (!this.isCreated()) {
      hilog.info(DOMAIN, MODULE, "loadUrl: browser not created yet");
      return;
    }
    if (url.length === 0) {
      return;
    }
    // Add https:// prefix if missing
    if (!url.startsWith('http://') && !url.startsWith('https://') && !url.startsWith('file://')) {
      url = 'https://' + url;
      this.urlInput = url;
    }
    hilog.info(DOMAIN, MODULE, "loadUrl: %{public}s", url);
    arkui_cef.cefLoadUrl(this.browserId, url);
  }

  sleep(delay: number): Promise<void> {
    return new Promise<void>((resolve) => setTimeout(resolve, delay));
  }

  async createOsrCefView(url: string, width: number, height: number): Promise<void> {
    if (this.disposed || this.cefCreating || this.isCreated()) {
      hilog.info(DOMAIN, MODULE, "OSR is creating or created. id: %{public}d", this.browserId);
      return;
    }
    const generation = this.creationGeneration;
    while (!arkui_cef.isCefRunning()) {
      await this.sleep(20);
      // While waiting for CEF the component may have been disposed or the
      // Surface invalidated; this creation attempt must be abandoned.
      if (this.disposed || generation !== this.creationGeneration ||
          (this.gpuEnabled && !this.surfaceReady)) {
        hilog.info(DOMAIN, MODULE, "createOsrCefView aborted: disposed/generation/surface changed");
        return;
      }
    }

    hilog.info(DOMAIN, MODULE, "Begin to create OSR cef view, url: %{public}s, size: %{public}d X %{public}d",
      url, width, height);
    this.cefCreating = true;

    let id = this.gpuEnabled
      ? arkui_cef.createOsrCefView(url, width, height,
          // GPU mode: no CPU paint callback — frames stay on the native
          // presenter and never cross to ArkTS. Optional callback slots
          // before options are passed as undefined placeholders (the
          // options object is the last parameter, index 13).
          undefined,
          (newId: number) => { this.onOsrCreated(newId, generation); },
          (title: string) => { this.onOsrTitleChanged(title); },
          (next: boolean) => { this.onOsrFocusTake(next); },
          (offset: string) => { this.onOsrScrollOffset(offset); },
          (show: boolean) => { this.onOsrVirtualKeyboard(show); },
          (text: string) => { this.onOsrTextSelection(text); },
          (cursorType: number) => { this.onOsrCursorChange(cursorType); },
          undefined, undefined,
          { surfaceId: this.gpuSurfaceId })
      : arkui_cef.createOsrCefView(url, width, height,
          (regionData: ArrayBuffer | undefined, x: number, y: number, w: number, h: number,
            fullFrame: boolean, viewW: number, viewH: number) => {
            this.onOsrPaint(regionData, x, y, w, h, fullFrame, viewW, viewH);
          },
          (newId: number) => { this.onOsrCreated(newId, generation); },
          (title: string) => { this.onOsrTitleChanged(title); },
          (next: boolean) => { this.onOsrFocusTake(next); },
          (offset: string) => { this.onOsrScrollOffset(offset); },
          (show: boolean) => { this.onOsrVirtualKeyboard(show); },
          (text: string) => { this.onOsrTextSelection(text); },
          (cursorType: number) => { this.onOsrCursorChange(cursorType); },
          (text: string, html: string, durl: string, urlTitle: string, fileName: string, fileData: ArrayBuffer, x: number, y: number) => {
            this.onOsrStartDrag(text, html, durl, urlTitle, fileName, fileData, x, y);
          },
          (operation: number) => { this.onOsrUpdateDragCursor(operation); });

    this.pendingBrowserId = id;
    hilog.info(DOMAIN, MODULE, "OSR CEF starts to create, id: %{public}d", id);
  }

  onOsrCreated(id: number, generation: number): void {
    this.cefCreating = false;
    // Component disposed or Surface generation changed: close the late
    // browser immediately - no binding, no input initialization.
    if (this.disposed || generation !== this.creationGeneration ||
        (this.gpuEnabled && !this.surfaceReady)) {
      hilog.info(DOMAIN, MODULE, "onOsrCreated for stale generation=%{public}d, destroying id=%{public}d",
        generation, id);
      if (id > 0) {
        arkui_cef.destroyOsrCefView(id);
      }
      return;
    }
    hilog.info(DOMAIN, MODULE, "OSR CEF created, browserId=%{public}d", id);
    this.browserId = id;
    this.pendingBrowserId = -1;
    // The tab may have been switched away during creation: sync visibility
    // immediately so a hidden tab does not keep producing frames after
    // creation. The native side also has a pending fallback for calls made
    // before the browser exists; this is the ArkTS-side counterpart. (CPU
    // OSR views have no GPU presenter; their visibility follows the
    // onVisibleAreaChange edge channel.)
    if (this.gpuEnabled && !this.isMyTurn()) {
      hilog.info(DOMAIN, MODULE, "onOsrCreated: tab inactive, apply WasHidden, id=%{public}d", id);
      arkui_cef.cefSetWindowlessVisibility(id, false);
    }
    // Set DSF BEFORE any resize: GetViewRect divides width_ by the DSF, so
    // a resize processed while the DSF is still the default 1.0 makes the
    // renderer treat physical pixels as CSS pixels and double-scale the
    // next frame (observed as a 3971x2184 frame with dsf=1.9).
    // Set DSF to actual device scale factor so CEF GetViewRect returns CSS pixels.
    // This makes web content layout match non-OSR tabs (content not too small).
    // OnPaint buffer is still physical pixels (CSS * DSF).
    // Mouse/touch coordinates are sent in CSS pixels (vp) to match GetViewRect.
    let dsf = this.getUIContext().vp2px(1);
    this.cachedDsf = dsf;
    arkui_cef.cefOsrSetScale(this.browserId, dsf);
    arkui_cef.cefOsrResize(this.browserId, this.viewWidth, this.viewHeight);
    // Attach the surface AFTER the scale/resize posts so the attach-forced
    // resize runs with the correct DSF (post order is preserved).
    this.attachSurfaceIfPossible();
    // Apply the requested frame rate (creation default is 60; this allows
    // callers to lower it via the osrFrameRate prop). SetWindowlessFrameRate
    // takes effect on the CEF UI thread; static pages emit no extra frames
    // regardless of the cap, so the value only affects pages with dynamic
    // content.
    // GPU OSR: the native createBrowser already set 120 (matching the 120Hz
    // display; a lower cap here would alias TestUFO's 120/60/30 rows). Only
    // apply the prop override when the caller explicitly passed one, or when
    // running CPU OSR (creation default 60).
    if (this.gpuEnabled) {
      if (this.osrFrameRate > 0 && this.osrFrameRate !== 60) {
        arkui_cef.cefSetFrameRate(this.browserId, this.osrFrameRate);
      }
    } else if (this.osrFrameRate >= 1) {
      arkui_cef.cefSetFrameRate(this.browserId, this.osrFrameRate);
    }
    // Register window-level key event filter for non-text keys (digits, symbols, etc.)
    this.registerKeyFilter();
    // The activation signals (Tabs.onChange / visibility edge) may have fired
    // before the browser existed and were dropped by the isCreated guard in
    // setTabActive — take keyboard ownership now if this view is the active
    // tab (e.g. the user switched here while CEF was still initializing).
    if (this.visibleNow || this.isMyTurn()) {
      this.setTabActive(true);
    }
  }

  // ==================== GPU Surface lifecycle ====================
  // GPU mode: create the browser only when BOTH the surfaceId and a valid
  // size are ready; whichever arrives first is just cached.

  private onGpuSurfaceCreated(surfaceId: string): void {
    if (this.disposed) {
      return;
    }
    this.gpuSurfaceId = surfaceId;
    this.surfaceReady = true;
    // The first callback may carry only Created; if the real layout size
    // is already available the size counts as ready (800x600 default does
    // not).
    if (this.layoutSizeReady) {
      this.surfaceSizeReady = true;
    }
    this.creationGeneration++;
    hilog.info(DOMAIN, MODULE, "GPU onSurfaceCreated id=%{public}s size=%{public}dx%{public}d",
      surfaceId, this.viewWidth, this.viewHeight);
    this.tryCreateGpuBrowser();
  }

  private onGpuSurfaceChanged(surfaceId: string, w: number, h: number): void {
    if (this.disposed) {
      return;
    }
    // SurfaceRect sizes are in px; no vp2px conversion.
    if (w > 0 && h > 0) {
      this.viewWidth = w;
      this.viewHeight = h;
      this.surfaceSizeReady = true;
    }
    if (this.isCreated()) {
      arkui_cef.cefOsrSetScale(this.browserId, this.getUIContext().vp2px(1));
      arkui_cef.cefOsrResize(this.browserId, this.viewWidth, this.viewHeight);
    }
    this.tryCreateGpuBrowser();
  }

  private onGpuSurfaceDestroyed(surfaceId: string): void {
    hilog.info(DOMAIN, MODULE, "GPU onSurfaceDestroyed id=%{public}s", surfaceId);
    // Invalidate the Surface and bump the generation immediately: stop
    // in-flight creations and further frame submissions to a dead Surface.
    this.surfaceReady = false;
    this.gpuSurfaceId = '';
    this.surfaceSizeReady = false;
    this.creationGeneration++;
    if (this.pendingBrowserId > 0) {
      arkui_cef.destroyOsrCefView(this.pendingBrowserId);
      this.pendingBrowserId = -1;
    }
    this.destroy();
  }

  private tryCreateGpuBrowser(): void {
    if (this.disposed || this.isCreated() || this.cefCreating) {
      return;
    }
    if (!this.surfaceReady || this.gpuSurfaceId.length === 0 ||
        !this.surfaceSizeReady || this.viewWidth <= 0 || this.viewHeight <= 0) {
      return;
    }
    this.createOsrCefView(this.viewUrl, this.viewWidth, this.viewHeight);
  }

  private async registerKeyFilter(): Promise<void> {
    try {
      let context: common.UIAbilityContext = this.getUIContext().getHostContext() as common.UIAbilityContext;
      let win: window.Window = await window.getLastWindow(context);
      this.cachedWin = win;
      let windowId: number = win.getWindowProperties().id;
      this.cachedWindowId = windowId;
      // onAreaChange may have run before the window handle was cached;
      // deliver the origin now that both sides can be available.
      this.syncWindowOrigin();
      // Keep the native origin cache fresh when the window moves (floating
      // window drag). Cache update only — no IME call, so nothing races the
      // session lifecycle here.
      win.on('windowRectChange', (_data: window.RectChangeOptions) => {
        this.syncWindowOrigin();
      });
      hilog.info(DOMAIN, MODULE, "registerKeyFilter: windowId=%{public}d, browserId=%{public}d",
        windowId, this.browserId);
      arkui_cef.registerOsrKeyFilter(windowId, this.browserId);

      win.on('windowEvent', (event: window.WindowEventType) => {
        if (event === window.WindowEventType.WINDOW_INACTIVE) {
          this.focusLostFlag = true;
          // End the session, mirroring the input method service: it removes
          // the client on window deactivation (StopClientInput/
          // RemoveClientInfo, isStopInactiveClient), so keeping the session
          // "attached" locally is an illusion — the next ShowKeyboard fails
          // with 12800016 and the heal path re-attaches anyway. Detaching
          // here instead keeps both ledgers in sync and moves the teardown
          // echo (the service-side cleanup that lands ~20ms later) into the
          // inactive period, where the panel is already hidden. Re-attaching
          // back-to-back with a detach at activation time raced that echo:
          // the late HidePanel killed the fresh session's panel (status bar
          // flicker). No-op while the URL TextInput owns the panel
          // (imeAttached already cleared by the ownership handoff).
          this.detachInputMethod();
          if (this.isCreated() && this.renderAreaActive) {
            arkui_cef.cefSendFocusEvent(this.browserId, false);
          }
          // Deliberately NOT pausing rendering here: WINDOW_INACTIVE is a
          // focus event, not a visibility event. On PC form factors an
          // unfocused window can still be fully visible (and must keep
          // rendering and accepting wheel/touchpad input) — mirrors
          // cefclient, where focus and WasHidden are decoupled. Only
          // component-level invisibility (onVisibleAreaChange, e.g. tab
          // switches) pauses the render pipeline.
        } else if (event === window.WindowEventType.WINDOW_ACTIVE) {
          if (this.focusLostFlag) {
            this.focusLostFlag = false;
            hilog.info(DOMAIN, MODULE, "WINDOW_ACTIVE: restoring focus, kbVisible=%{public}s",
              this.keyboardVisible ? "true" : "false");
            if (this.isCreated() && this.renderAreaActive) {
              arkui_cef.cefSendFocusEvent(this.browserId, true);
            }
            // The session was ended at deactivation. Rebuild it immediately
            // WITHOUT showing the panel (keys route as soon as attach lands),
            // then show the panel once sceneboard's transition RequestHideInput
            // has passed — showing during the transition gets killed (status
            // bar flicker). handleImeFocus gates on keyboardVisible.
            this.handleImeFocus(false);
            if (this.keyboardVisible) {
              setTimeout(() => {
                if (this.disposed || !this.tabActive || !this.isImeOwner()) {
                  return;
                }
                this.showImeKeyboardWithHeal();
              }, OsrCefView.IME_PANEL_SHOW_DELAY_MS);
            }
          }
        }
      });
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "registerKeyFilter failed: %{public}s", (e as Error).message);
    }
  }

  onOsrPaint(regionData: ArrayBuffer | undefined, x: number, y: number, w: number, h: number,
             fullFrame: boolean, viewW: number, viewH: number): void {
    if (fullFrame) {
      // First frame or resize: the PixelMap must be (re)created at the new
      // size. Low-frequency path. Always carries pixel data.
      if (regionData === undefined) {
        return;
      }
      this.rebuildPixelMap(regionData, viewW, viewH);
      return;
    }

    if (this.pmWidth !== viewW || this.pmHeight !== viewH) {
      // Incremental frame at a new view size while the rebuild is still in
      // flight (or the PixelMap is not ready yet). The region payload is NOT
      // a full frame, so it can't be used to rebuild — drop it and ask CEF
      // for a full repaint. The next frame arrives as fullFrame and rebuilds
      // with complete content (one frame of latency at most).
      if (this.isCreated()) {
        arkui_cef.cefInvalidate(this.browserId);
      }
      return;
    }

    let pmFront = this.showA ? this.pmA : this.pmB;
    let pmBack = this.showA ? this.pmB : this.pmA;
    if (!pmFront || !pmBack || w <= 0 || h <= 0) {
      return;
    }

    if (regionData === undefined) {
      // Native fast path: the NAPI layer already wrote the dirty region into
      // BOTH persistent PixelMaps (they stay identical) — just swap the
      // displayed reference. The Image component reloads and keeps showing
      // the old (identical) image until the new src is ready, so there is no
      // black flash window.
      this.pixelMap = pmBack;
      this.showA = !this.showA;
      return;
    }

    // Fallback path: pixel data arrived via ArrayBuffer (native write path
    // unavailable or failed) — write it into both buffers ourselves.
    // regionData holds compact BGRA rows (w * 4 bytes per row),
    // so stride = w * 4 and offset = 0.
    try {
      let area: image.PositionArea = {
        pixels: regionData,
        offset: 0,
        stride: w * 4,
        region: { size: { width: w, height: h }, x: x, y: y }
      };
      pmFront.writePixelsSync(area);
      pmBack.writePixelsSync(area);
      // Swap the object reference — the Image component reloads and keeps
      // showing the old (identical) image until the new src is ready, so
      // there is no black flash window.
      this.pixelMap = pmBack;
      this.showA = !this.showA;
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "writePixelsSync failed: %{public}s", (e as Error).message);
    }
  }

  private rebuildPixelMap(frameData: ArrayBuffer, viewW: number, viewH: number): void {
    if (this.pixelMapCreating) {
      // A rebuild is already in flight — keep only the latest payload; it
      // will be applied when the current creation completes.
      this.pendingFullFrame = frameData;
      this.pendingFullFrameW = viewW;
      this.pendingFullFrameH = viewH;
      return;
    }
    this.pixelMapCreating = true;
    try {
      // CEF outputs BGRA; createPixelMap defaults srcPixelFormat to
      // BGRA_8888 when unspecified, matching directly. editable must be
      // true — the incremental path writes dirty regions into the
      // persistent buffers via writePixelsSync, which fails on
      // non-editable PixelMaps (documented behavior).
      let opts: image.InitializationOptions = {
        size: { width: viewW, height: viewH },
        pixelFormat: image.PixelMapFormat.BGRA_8888,
        editable: true,
      };
      image.createPixelMap(frameData, opts).then((pm: image.PixelMap) => {
        // Build the second buffer from the same full-frame payload so both
        // buffers start with identical content.
        image.createPixelMap(frameData, opts).then((pm2: image.PixelMap) => {
          let oldA = this.pmA;
          let oldB = this.pmB;
          this.pmA = pm;
          this.pmB = pm2;
          this.showA = true;
          this.pixelMap = this.pmA;
          this.pmWidth = viewW;
          this.pmHeight = viewH;
          // Register the new pair for native dirty-region writes BEFORE
          // retiring the old ArkTS PixelMaps — the NAPI registry must never
          // hold handles into released memory.
          if (this.isCreated()) {
            arkui_cef.registerOsrPixelmaps(this.browserId, pm, pm2);
          }
          if (oldA && oldA !== oldB) {
            this.retirePixelMap(oldA);
          }
          if (oldB) {
            this.retirePixelMap(oldB);
          }
          this.pixelMapCreating = false;
          // Diagnostic: compare the PixelMap size against the render area
          // size (viewWidth/viewHeight track onAreaChange). A persistent
          // mismatch means the viewport is systematically off — the bottom
          // of the page would look cut off.
          hilog.info(DOMAIN, MODULE,
            "rebuildPixelMap: pm=%{public}dx%{public}d, area=%{public}dx%{public}d",
            viewW, viewH, this.viewWidth, this.viewHeight);

          // Apply a full-frame payload that arrived while we were building.
          if (this.pendingFullFrame !== undefined) {
            let data = this.pendingFullFrame;
            let pw = this.pendingFullFrameW;
            let ph = this.pendingFullFrameH;
            this.pendingFullFrame = undefined;
            this.rebuildPixelMap(data, pw, ph);
          }
        }).catch((err: Error) => {
          hilog.error(DOMAIN, MODULE, "createPixelMap(2nd) failed: %{public}s", err.message);
          this.finishRebuildFailed(pm, viewW, viewH);
        });
      }).catch((err: Error) => {
        hilog.error(DOMAIN, MODULE, "createPixelMap failed: %{public}s", err.message);
        this.finishRebuildFailed(undefined, viewW, viewH);
      });
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "rebuildPixelMap error: %{public}s", (e as Error).message);
      this.finishRebuildFailed(undefined, viewW, viewH);
    }
  }

  private retirePixelMap(pm: image.PixelMap): void {
    // Deferred release. After swapping the Image src, the component keeps
    // rendering the OLD image while the new one loads — that old image is
    // this just-replaced PixelMap. Releasing it immediately makes that
    // "keep showing old image" path occasionally read a released object,
    // which shows as a black frame during rapid resizes. 200ms comfortably
    // covers the reload, and the memory is freed shortly after.
    setTimeout(() => {
      pm.release();
    }, 200);
  }

  private finishRebuildFailed(pm: image.PixelMap | undefined, viewW: number, viewH: number): void {
    // Rebuild failed: fall back to a single-buffer mode if we at least got
    // the first PixelMap (the double-buffer path requires both; with only
    // one buffer the incremental path can't swap references, so keep the
    // single PixelMap displayed statically and request full frames).
    if (pm) {
      let oldA = this.pmA;
      let oldB = this.pmB;
      this.pmA = pm;
      this.pmB = pm;
      this.showA = true;
      this.pixelMap = pm;
      this.pmWidth = viewW;
      this.pmHeight = viewH;
      if (oldA && oldA !== pm) {
        this.retirePixelMap(oldA);
      }
      if (oldB && oldB !== pm) {
        this.retirePixelMap(oldB);
      }
    }
    this.pixelMapCreating = false;
    this.pendingFullFrame = undefined;
  }

  onOsrTitleChanged(title: string): void {
    hilog.info(DOMAIN, MODULE, "OSR CEF title changed: %{public}s", title);
    this.pageTitle = title;
  }

  onOsrFocusTake(next: boolean): void {
    hilog.info(DOMAIN, MODULE, "OSR CEF focus taken, next=%{public}s", next ? "true" : "false");
    // Web content tabbed past its last (Shift+Tab: before its first) focusable
    // element — CEF asks the host to move focus out of the web view. Move the
    // ArkUI focus token off the render area onto the configured neighbor
    // (mirrors QCefView's onCefWindowLostTabFocus → nextInFocusChain walk).
    this.moveFocusOutOfWeb(next);
  }

  // ==================== Focus flow across the web boundary (TAB handling) ====================
  //
  // Focus model: the render area (with its invisible anchor) is one "stop"
  // in the ArkUI focus chain. When focus lands on the render area
  // (onFocus), the web page takes over the keyboard (SetFocus + the
  // window-level key filter); TABs circulating INSIDE the page are consumed
  // by the filter, and when the flow reaches the page boundary CEF calls
  // OnTakeFocus and the host "moves focus out" here:
  //   Backward (Shift+Tab leaving the page start) -> land on Go (the
  //   addressable-by-id stop before the render area in focus order); one
  //   keypress completes the transfer.
  //   Forward (Tab leaving the page end) -> there is no id-addressable stop
  //   after the render area (the wrap-around target is the built-in
  //   TabBar's active tab label, which has no component id), so focus stays
  //   on the render area stop itself (whole-page focus frame), the keyboard
  //   is back with the host, and the NEXT Tab wraps around to the TabBar
  //   via the framework's native traversal.
  // After moving out, the filter stops consuming and key events return to
  // ArkUI native focus traversal, until a later traversal lands on the
  // render area again (onFocus), closing the loop.
  /**
   * 网页边界退出：把键盘交还宿主，并按方向转移焦点。
   * @param next true=正向（Tab 流出网页末尾），false=逆向（Shift+Tab 流出网页开头）
   */
  private moveFocusOutOfWeb(next: boolean): void {
    if (!this.isCreated()) {
      return;
    }
    let wired = next ? this.focusNextId : this.focusPrevId;
    hilog.info(DOMAIN, MODULE, "moveFocusOutOfWeb: next=%{public}s, wired=%{public}s",
      next ? "true" : "false", wired);
    // Web-side focus was already cleared in the OnTakeFocus chain; also
    // release the browser focus and keyboard routing so subsequent keys
    // return to ArkUI native focus traversal.
    this.webExitArmed = true;
    setTimeout(() => {
      this.webExitArmed = false;
    }, 600);
    this.setRenderAreaActive(false);
    try {
      // Enter the focus-armed state: the user's very next Tab moves focus
      // directly (otherwise the first Tab only activates the focus state
      // without moving).
      this.getUIContext().getFocusController().activate(true);
      if (wired.length > 0) {
        this.getUIContext().getFocusController().requestFocus(wired);
        return;
      }
      if (!next) {
        // Backward default landing: Go - the stop before the render area
        // in focus order (the previousInFocusChain semantics of QCefView);
        // one keypress completes the transfer.
        this.getUIContext().getFocusController().requestFocus(this.toolbarLastKey);
        return;
      }
      // Forward default: there is no addressable next stop (the
      // wrap-around target is a built-in TabBar item). Focus stays on the
      // invisible anchor inside the render area scope (meaning "the web view
      // as a whole holds focus; the keyboard is back with the host"), and
      // the next Tab wraps out of the page via framework traversal. Address
      // the anchor directly (not the render-area container) - the landing
      // point is deterministic and does not depend on how the framework
      // resolves containers. A failed requestFocus is harmless: focus is
      // already on the anchor (kept re-anchored while the page circulated),
      // and traversal starts from the render area scope either way.
      this.getUIContext().getFocusController().requestFocus(this.focusAnchorKey);
    } catch (e) {
      // The address request may be rejected (e.g. an unfocusable ancestor
      // during a switch animation) - focus stays on the anchor and the flow
      // is unaffected (the next Tab still traverses from the render area
      // scope).
      hilog.warn(DOMAIN, MODULE, "moveFocusOutOfWeb: requestFocus failed: %{public}s",
        (e as Error).message);
    }
  }

  /**
   * 焦点进入渲染区（走焦遍历或本视图激活请求）。此时网页应接管键盘：
   * 浏览器获焦 + 窗口级按键过滤生效，随后的 TAB/Shift+TAB 将送入 CEF
   * 在网页内流转。幂等：已激活时不再重复处理。
   */
  private onRenderAreaFocusGained(): void {
    if (this.webExitArmed) {
      // The forward boundary exit just moved focus onto the render-area
      // stop itself (anchor -> Column, a whole-page focus-frame
      // intermediate state): at this moment the page must NOT take the
      // keyboard back, and the next Tab must continue wrapping out via
      // framework traversal. armed is cleared by timeout or by tab
      // re-activation.
      return;
    }
    // Focus back on the render area = the page retakes the keyboard; clear
    // the exit protection.
    this.webExitArmed = false;
    if (!this.tabActive || !this.isCreated() || this.renderAreaActive) {
      return;
    }
    hilog.info(DOMAIN, MODULE, "render area focus gained — handing keyboard to web, id=%{public}d",
      this.browserId);
    this.setRenderAreaActive(true);
    arkui_cef.cefSendFocusEvent(this.browserId, true);
  }

  /**
   * 焦点离开渲染区（移出网页或点击了其它控件）。释放键盘路由，让物理
   * 按键回到 ArkUI 原生走焦。focusMoveGuard 期间为渲染区内部焦点重锚
   * （Column ↔ anchor），不视为离开。
   */
  private onRenderAreaFocusLost(): void {
    // Focus leaving the render area = the boundary-exit traversal has
    // continued, so the exit protection is definitively cleared (more
    // reliable than the 600ms timeout fallback: on fast repeated presses the
    // wrap-around re-entry may land inside the timeout window, and a stale
    // armed flag would stop the page from taking the keyboard).
    this.webExitArmed = false;
    if (this.focusMoveGuard || !this.renderAreaActive) {
      return;
    }
    hilog.info(DOMAIN, MODULE, "render area focus lost — releasing keyboard, id=%{public}d",
      this.browserId);
    this.setRenderAreaActive(false);
  }

  onOsrScrollOffset(offset: string): void {
    this.scrollOffsetText = offset;
  }

  onOsrVirtualKeyboard(show: boolean): void {
    hilog.info(DOMAIN, MODULE, "OSR virtual keyboard: %{public}s", show ? "show" : "hide");
    this.keyboardVisible = show;
    if (show) {
      // Web input gained focus: (re)create the session and show the panel.
      // If the show fails (session killed by an external race) the heal
      // path in showImeKeyboardWithHeal re-attaches.
      if (!this.imeAttached) {
        this.attachInputMethod();
      } else {
        this.showImeKeyboardWithHeal();
      }
    } else {
      // Web input lost focus (focus moved to a link/button, a blank area,
      // or the browser blurred). End the session — NOT just hide the panel:
      // the document lifecycle is Attach → Show → Hide → Detach, and a
      // lingering session keeps the IMS binding alive, so physical keys
      // would keep pulling up candidates at a stale cursor and committing
      // text into the previously focused field.
      if (this.urlInputFocused) {
        // The URL TextInput is taking over: ArkUI owns the panel now. Our
        // teardown could race its attach at the input method service (a
        // late teardown kills the fresh ArkTS attach) — keep the session;
        // its callbacks are inert while renderAreaActive is false.
        hilog.info(DOMAIN, MODULE, "OSR virtual keyboard hide skipped — URL input owns the panel");
      } else {
        this.detachInputMethod();
      }
    }
  }

  onOsrTextSelection(text: string): void {
    this.selectedText = text;
  }

  onOsrCursorChange(cursorType: number): void {
    // CEF cef_cursor_type_t enum (sequential from 0):
    // CT_POINTER=0 CT_CROSS=1 CT_HAND=2 CT_IBEAM=3 CT_WAIT=4 CT_HELP=5
    // CT_EASTRESIZE=6 CT_NORTHRESIZE=7 CT_NORTHEASTRESIZE=8 CT_NORTHWESTRESIZE=9
    // CT_SOUTHRESIZE=10 CT_SOUTHEASTRESIZE=11 CT_SOUTHWESTRESIZE=12 CT_WESTRESIZE=13
    // CT_NORTHSOUTHRESIZE=14 CT_EASTWESTRESIZE=15
    // CT_NORTHEASTSOUTHWESTRESIZE=16 CT_NORTHWESTSOUTHEASTRESIZE=17
    // CT_COLUMNRESIZE=18 CT_ROWRESIZE=19
    // CT_MOVE=29 CT_VERTICALTEXT=30 CT_CELL=31 CT_CONTEXTMENU=32
    // CT_COPY=36 CT_NONE=37 CT_NOTALLOWED=38 CT_ZOOMIN=39 CT_ZOOMOUT=40
    // CT_GRAB=41 CT_GRABBING=42
    let pStyle: pointer.PointerStyle = pointer.PointerStyle.DEFAULT;
    let label: string = 'default';
    switch (cursorType) {
      case 0:  pStyle = pointer.PointerStyle.DEFAULT; label = 'pointer'; break;
      case 1:  pStyle = pointer.PointerStyle.CROSS; label = 'cross'; break;
      case 2:  pStyle = pointer.PointerStyle.HAND_POINTING; label = 'hand'; break;
      case 3:  pStyle = pointer.PointerStyle.TEXT_CURSOR; label = 'text'; break;
      case 4:  pStyle = pointer.PointerStyle.LOADING; label = 'wait'; break;
      case 5:  pStyle = pointer.PointerStyle.HELP; label = 'help'; break;
      case 6:  pStyle = pointer.PointerStyle.EAST; label = 'e-resize'; break;
      case 7:  pStyle = pointer.PointerStyle.NORTH; label = 'n-resize'; break;
      case 8:  pStyle = pointer.PointerStyle.NORTH_EAST_SOUTH_WEST; label = 'ne-resize'; break;
      case 9:  pStyle = pointer.PointerStyle.NORTH_WEST_SOUTH_EAST; label = 'nw-resize'; break;
      case 10: pStyle = pointer.PointerStyle.SOUTH; label = 's-resize'; break;
      case 11: pStyle = pointer.PointerStyle.NORTH_WEST_SOUTH_EAST; label = 'se-resize'; break;
      case 12: pStyle = pointer.PointerStyle.NORTH_EAST_SOUTH_WEST; label = 'sw-resize'; break;
      case 13: pStyle = pointer.PointerStyle.WEST; label = 'w-resize'; break;
      case 14: pStyle = pointer.PointerStyle.NORTH_SOUTH; label = 'ns-resize'; break;
      case 15: pStyle = pointer.PointerStyle.WEST_EAST; label = 'ew-resize'; break;
      case 16: pStyle = pointer.PointerStyle.NORTH_EAST_SOUTH_WEST; label = 'nesw-resize'; break;
      case 17: pStyle = pointer.PointerStyle.NORTH_WEST_SOUTH_EAST; label = 'nwse-resize'; break;
      case 18: pStyle = pointer.PointerStyle.WEST_EAST; label = 'col-resize'; break;
      case 19: pStyle = pointer.PointerStyle.NORTH_SOUTH; label = 'row-resize'; break;
      case 29: pStyle = pointer.PointerStyle.MOVE; label = 'move'; break;
      case 30: pStyle = pointer.PointerStyle.HORIZONTAL_TEXT_CURSOR; label = 'vtext'; break;
      case 31: pStyle = pointer.PointerStyle.CURSOR_CROSS; label = 'cell'; break;
      case 32: pStyle = pointer.PointerStyle.HELP; label = 'context*'; break;
      case 36: pStyle = pointer.PointerStyle.CURSOR_COPY; label = 'copy'; break;
      case 37: pStyle = pointer.PointerStyle.DEFAULT; label = 'none*'; break;
      case 38: pStyle = pointer.PointerStyle.CURSOR_FORBID; label = 'forbid'; break;
      case 39: pStyle = pointer.PointerStyle.ZOOM_IN; label = 'zoomin'; break;
      case 40: pStyle = pointer.PointerStyle.ZOOM_OUT; label = 'zoomout'; break;
      case 41: pStyle = pointer.PointerStyle.HAND_OPEN; label = 'grab'; break;
      case 42: pStyle = pointer.PointerStyle.HAND_GRABBING; label = 'grabbing'; break;
      default: pStyle = pointer.PointerStyle.DEFAULT; label = 'type=' + cursorType; break;
    }
    this.cursorTypeText = label;

    try {
      let cursorController = this.getUIContext().getCursorController();
      if (cursorType === 0) {
        cursorController.restoreDefault();
      } else {
        cursorController.setCursor(pStyle);
      }
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "setCursor failed: %{public}s", (e as Error).message);
    }
  }

  private getWindowRect(): window.Rect {
    if (this.cachedWin) {
      try {
        return this.cachedWin.getWindowProperties().windowRect;
      } catch (e) {
        hilog.error(DOMAIN, MODULE, "getWindowRect failed: %{public}s", (e as Error).message);
      }
    }
    let defaultRect: window.Rect = { left: 0, top: 0, width: 0, height: 0 };
    return defaultRect;
  }

  // Pushes the window's screen origin (physical px) to the native handler so
  // caret reports can be converted to screen-absolute coordinates without a
  // per-move round-trip through ArkTS. Safe to call before the browser exists.
  private syncWindowOrigin(): void {
    if (!this.isCreated() || !this.cachedWin) {
      return;
    }
    let winRect = this.getWindowRect();
    arkui_cef.cefSetWindowOrigin(this.browserId, winRect.left, winRect.top);
  }

  // ==================== Drag & Drop ====================

  onOsrStartDrag(text: string, html: string, durl: string, urlTitle: string,
                  fileName: string, fileData: ArrayBuffer, x: number, y: number): void {
    hilog.info(DOMAIN, MODULE,
      "onOsrStartDrag: text='%{public}s', url='%{public}s', fileName='%{public}s', fileDataSize=%{public}d, x=%{public}d, y=%{public}d",
      text.substring(0, 50), durl, fileName, fileData.byteLength, x, y);
    if (this.isDragInProgress) {
      hilog.info(DOMAIN, MODULE, "onOsrStartDrag: drag already in progress, skipping");
      return;
    }
    this.isDragInProgress = true;
    // Reset per-drag state: stale lastDragOperation from the previous drag
    // would corrupt the first DROP_ENABLED/DROP_DISABLED decision (#5).
    this.lastDragOperation = 0;

    // Cache full drag data for self-drag target (onDragEnter uses this to call
    // cefDragTargetDragEnter immediately, and onDrop uses it for the final drop).
    this.pendingDragText = text;
    this.pendingDragHtml = html;
    this.pendingDragUrl = durl;
    this.pendingDragFilePaths = '';
    // Initialize last drag position to start position as fallback.
    this.lastDragMoveX = x;
    this.lastDragMoveY = y;

    // Build UnifiedData with ALL non-empty records, matching CEF's non-OSR
    // adapter behavior (HandlePlainTextRecord/HandleUdsHtmlRecord/etc. each
    // add their data type conditionally). The number badge is suppressed via
    // previewOptions.numberBadge = false so multiple records don't show "+N".
    // Records carry exactly what the web page declared (FragmentText/Html/Url) —
    // data fidelity is preserved; the test page's dragstart sets text/plain
    // for links to match Chromium's default link-drag behavior.
    let unifiedData = new unifiedDataChannel.UnifiedData();
    if (text.length > 0) {
      let plainText = new unifiedDataChannel.PlainText();
      plainText.textContent = text;
      unifiedData.addRecord(plainText);
    }
    if (html.length > 0) {
      let htmlRecord = new unifiedDataChannel.HTML();
      htmlRecord.htmlContent = html;
      if (text.length > 0) {
        htmlRecord.plainContent = text;
      }
      unifiedData.addRecord(htmlRecord);
    }
    if (durl.length > 0) {
      let link = new unifiedDataChannel.Hyperlink();
      link.url = durl;
      link.description = urlTitle.length > 0 ? urlTitle : durl;
      unifiedData.addRecord(link);
    }

    // If CEF provided file contents (e.g. image dragged from <img>),
    // write to a temp file and add a File/Image UDMF record so external
    // apps (file manager, image viewer) can receive the drop.
    if (fileName.length > 0 && fileData.byteLength > 0) {
      try {
        let context = this.getUIContext().getHostContext() as common.UIAbilityContext;
        let cacheDir = context.cacheDir;
        // Sanitize fileName: strip path separators to prevent path traversal
        // (fileName originates from web content and is not trustworthy).
        let safeName = fileName.replace(/[/\\]/g, '_');
        // Use a dedicated dragdrop sub-directory, cleared before each drag-out
        // (mirrors the non-OSR adapter's DeletePathRecursively(dragdrop) behavior).
        let dragDir = `${cacheDir}/dragdrop`;
        if (fileIo.accessSync(dragDir)) {
          fileIo.rmdirSync(dragDir);
        }
        fileIo.mkdirSync(dragDir);
        let tempPath = `${dragDir}/${safeName}`;
        let file = fileIo.openSync(tempPath, fileIo.OpenMode.READ_WRITE | fileIo.OpenMode.CREATE | fileIo.OpenMode.TRUNC);
        fileIo.writeSync(file.fd, fileData);
        fileIo.closeSync(file);
        let fileUriStr = fileUri.getUriFromPath(tempPath);
        hilog.info(DOMAIN, MODULE, "onOsrStartDrag: wrote temp file %{public}s, uri=%{public}s", tempPath, fileUriStr);

        // Cache file path for self-drag drop-back (cefDragTargetDragEnter → AddFile)
        this.pendingDragFilePaths = tempPath;

        // Determine if it's an image by extension
        let isImage = /\.(png|jpg|jpeg|gif|bmp|webp|svg)$/i.test(safeName);
        if (isImage) {
          let image = new unifiedDataChannel.Image();
          image.imageUri = fileUriStr;
          unifiedData.addRecord(image);
        } else {
          let fileRecord = new unifiedDataChannel.File();
          fileRecord.uri = fileUriStr;
          unifiedData.addRecord(fileRecord);
        }
      } catch (e) {
        hilog.error(DOMAIN, MODULE, "onOsrStartDrag: write temp file failed: %{public}s", (e as Error).message);
      }
    }

    let previewOptions: DragPreviewOptions = { numberBadge: false };
    let dragInfo: dragController.DragInfo = {
      pointerId: 0,
      data: unifiedData,
      previewOptions: previewOptions
    };

    // Set drag preview text for the @Builder (match actual dragged content)
    this.dragPreviewText = fileName.length > 0 ? fileName.substring(0, 30) :
      (text.length > 0 ? text.substring(0, 30) :
        (durl.length > 0 ? durl.substring(0, 30) : 'Drag'));

    try {
      this.getUIContext().getDragController().executeDrag(
        () => { this.dragPreviewBuilder(); },
        dragInfo,
        (err, dragEventParam) => {
          this.isDragInProgress = false;
          this.pendingDragText = '';
          this.pendingDragHtml = '';
          this.pendingDragUrl = '';
          this.pendingDragFilePaths = '';
          this.dragEnterCalled = false;
          // The release that ended the drag is consumed by the drag system —
          // CEF never receives a matching mouse-up. Reset the button-state
          // flag so subsequent plain moves don't carry
          // EVENTFLAG_LEFT_MOUSE_BUTTON and trigger spurious drags.
          this.mouseButtonPressed = false;
          if (err) {
            hilog.error(DOMAIN, MODULE, "executeDrag error: %{public}s", err.message ?? '');
            arkui_cef.cefDragSourceEndedAt(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0);
            arkui_cef.cefDragSourceSystemDragEnded(this.browserId);
            // Synthetic mouse-up: the physical release was consumed by the drag
            // system, so CEF never saw it — without this the renderer keeps the
            // mouse-pressed state (CSS :active stays set, element stays dimmed).
            arkui_cef.cefSendMouseClick(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0, true, 1, 0);
            return;
          }
          let result = DragResult.DRAG_FAILED;
          let behavior = DragBehavior.COPY;
          if (dragEventParam && dragEventParam.event) {
            result = dragEventParam.event.getResult();
            behavior = dragEventParam.event.dragBehavior;
          }
          let cefOp = 0;
          if (result === DragResult.DRAG_SUCCESSFUL) {
            if (behavior === DragBehavior.MOVE) {
              cefOp = 16;
            } else {
              cefOp = 1;
            }
          }
          hilog.info(DOMAIN, MODULE,
            "executeDrag done: result=%{public}d, behavior=%{public}d, cefOp=%{public}d, pos=%{public}d,%{public}d",
            result, behavior, cefOp, this.lastDragMoveX, this.lastDragMoveY);
          arkui_cef.cefDragSourceEndedAt(this.browserId, this.lastDragMoveX, this.lastDragMoveY, cefOp);
          arkui_cef.cefDragSourceSystemDragEnded(this.browserId);
          // Synthetic mouse-up (see comment in err path above). Blink suppresses
          // click-after-drag, so this won't fire a click on the source element.
          arkui_cef.cefSendMouseClick(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0, true, 1, 0);
        }
      );
    } catch (e) {
      this.isDragInProgress = false;
      this.pendingDragText = '';
      this.pendingDragHtml = '';
      this.pendingDragUrl = '';
      this.pendingDragFilePaths = '';
      this.mouseButtonPressed = false;
      hilog.error(DOMAIN, MODULE, "executeDrag exception: %{public}s", (e as Error).message);
      arkui_cef.cefDragSourceEndedAt(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0);
      arkui_cef.cefDragSourceSystemDragEnded(this.browserId);
      arkui_cef.cefSendMouseClick(this.browserId, this.lastDragMoveX, this.lastDragMoveY, 0, true, 1, 0);
    }
  }

  onOsrUpdateDragCursor(operation: number): void {
    this.lastDragOperation = operation;
    hilog.info(DOMAIN, MODULE, "onOsrUpdateDragCursor: operation=%{public}d", operation);
  }

  private extractDragData(event: DragEvent): DragDataInfo {
    let text = '';
    let html = '';
    let url = '';
    let filePaths = '';
    try {
      let data = event.getData();
      if (data) {
        let records = data.getRecords();
        for (let i = 0; i < records.length; i++) {
          let record = records[i];
          let rtype = record.getType();
          if (rtype === uniformTypeDescriptor.UniformDataType.PLAIN_TEXT) {
            let pt = record as unifiedDataChannel.PlainText;
            text = pt.textContent;
          } else if (rtype === uniformTypeDescriptor.UniformDataType.HTML) {
            let hr = record as unifiedDataChannel.HTML;
            html = hr.htmlContent;
            if (text.length === 0 && hr.plainContent) {
              text = hr.plainContent;
            }
          } else if (rtype === uniformTypeDescriptor.UniformDataType.HYPERLINK) {
            let hl = record as unifiedDataChannel.Hyperlink;
            url = hl.url;
          } else if (rtype === uniformTypeDescriptor.UniformDataType.FILE) {
            let file = record as unifiedDataChannel.File;
            if (file.uri) {
              let fu = new fileUri.FileUri(file.uri);
              if (filePaths.length > 0) filePaths += '\n';
              filePaths += fu.path;
            }
          } else if (rtype === uniformTypeDescriptor.UniformDataType.IMAGE) {
            let img = record as unifiedDataChannel.Image;
            if (img.imageUri) {
              let fu = new fileUri.FileUri(img.imageUri);
              if (filePaths.length > 0) filePaths += '\n';
              filePaths += fu.path;
            }
          }
        }
      }
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "extractDragData error: %{public}s", (e as Error).message);
    }
    hilog.info(DOMAIN, MODULE, "extractDragData: text_len=%{public}d, html_len=%{public}d, url='%{public}s', filePaths_len=%{public}d",
      text.length, html.length, url, filePaths.length);
    let result: DragDataInfo = { text: text, html: html, url: url, filePaths: filePaths };
    return result;
  }

  @Builder
  dragPreviewBuilder() {
    Text(this.dragPreviewText)
      .fontSize(12)
      .fontColor('#333333')
      .padding({ left: 8, right: 8, top: 4, bottom: 4 })
      .backgroundColor('#f0f0f0')
      .border({ width: 1, color: '#cccccc', radius: 4 })
      .maxLines(1)
      .textOverflow({ overflow: TextOverflow.Ellipsis })
      .constraintSize({ maxWidth: 200 })
  }

  // ==================== Touch events (touchscreen) ====================
  handleTouchEvent(event: TouchEvent): void {
    if (!this.isCreated()) {
      return;
    }

    event.stopPropagation();

    // ArkUI fires onTouch for ALL pointer input (mouse, touchpad, touchscreen).
    // Routing (verified from docs):
    // - Touchscreen: SourceType.TouchScreen — forwarded to CEF as touch events.
    // - Touchpad: single-finger acts as mouse (SourceType.Mouse), two-finger
    //   swipe/pinch arrive via onAxis/PinchGesture — never as touch events here.
    // - A mouse left-press is ALSO converted to a touch event (SourceType.Mouse)
    //   and must be filtered out, otherwise CEF enables touch scrolling and the
    //   page scrolls when the user clicks and drags with a mouse.
    if (event.source === SourceType.Mouse) {
      return;
    }

    // CEF touch event types: TOUCHEVENT_RELEASED=0, TOUCHEVENT_PRESSED=1, TOUCHEVENT_MOVED=2, TOUCHEVENT_CANCELLED=3
    let touchType = 2;
    switch (event.type) {
      case TouchType.Down: touchType = 1; break;
      case TouchType.Up: touchType = 0; break;
      case TouchType.Move: touchType = 2; break;
      case TouchType.Cancel: touchType = 3; break;
      default: break;
    }

    // Forward touch events to CEF — ALL fingers, native semantics. Pages that
    // handle multi-touch themselves (maps) receive their pinch directly, like
    // desktop Chrome with a touchscreen. NO multi-touch takeover here: the
    // previous takeover (cancel touches + ArkTS pinch → ctrl+wheel) corrupted
    // the touch state on pages like maps (input dead until reload) — the
    // synthetic touchcancel broke Chromium's active-touch tracking.
    // Down/Up/Cancel are per-point events: forward only the points that
    // triggered this event. Cancel is per-point (e.g. a stylus interrupt
    // cancels only the finger's stream). Move: send all active touches.
    let touchList: TouchObject[] = event.touches;
    if (event.type === TouchType.Down || event.type === TouchType.Up ||
        event.type === TouchType.Cancel) {
      touchList = event.changedTouches;
    }
    for (let i = 0; i < touchList.length; i++) {
      let touch = touchList[i];
      arkui_cef.cefSendTouchEvent(this.browserId,
        Math.round(touch.x), Math.round(touch.y), touchType, touch.id);
    }

    // Touch input is forwarded as touch events ONLY (loop above) — native
    // Chromium touch semantics apply: tap = click (synthesized by the touch
    // gesture pipeline), long-press = word selection, drag = scroll.
    // Do NOT simulate mouse events for touchscreen input: a synthesized mouse
    // press-drag is treated by the renderer as an immediate text-selection
    // drag, overriding the native long-press-to-select behavior.
    // HTML5 drag is mouse-initiated in Chromium — test drag & drop with the
    // mouse.
    let touch0 = event.touches[0];
    if (touch0 && event.type === TouchType.Down && event.touches.length === 1) {
      // Focus bookkeeping only (single-finger Down) — no mouse events.
      this.lastInputX = touch0.x;
      this.lastInputY = touch0.y;
      this.setRenderAreaActive(true);
      this.stopUrlEditing();
      this.handleImeFocus();
      arkui_cef.cefSendFocusEvent(this.browserId, true);
    }
  }

  // ==================== Mouse events (move/click/one-finger touchpad) ====================
  // Touchpad single-finger input also arrives here as SourceType.Mouse
  // (verified from docs) — tap=Press/Release, swipe=Move, so touchpad is
  // fully covered by this handler plus handleAxisEvent (two-finger scroll).

  // EVENTFLAG_* helpers
  private buttonFlag(cefButton: number): number {
    if (cefButton === 1) {
      return 0x20; // EVENTFLAG_MIDDLE_MOUSE_BUTTON
    }
    if (cefButton === 2) {
      return 0x40; // EVENTFLAG_RIGHT_MOUSE_BUTTON
    }
    return 0x10;   // EVENTFLAG_LEFT_MOUSE_BUTTON
  }

  private buildMouseModifiers(event: MouseEvent, buttonMods: number): number {
    // Shift/Ctrl/Alt via BaseEvent.getModifierKeyState (API 12+); guarded
    // because the method is optional on older SDKs.
    let mods = buttonMods;
    if (event.getModifierKeyState) {
      try {
        if (event.getModifierKeyState(['Shift'])) {
          mods |= 0x02; // EVENTFLAG_SHIFT_DOWN
        }
        if (event.getModifierKeyState(['Ctrl'])) {
          mods |= 0x04; // EVENTFLAG_CONTROL_DOWN
        }
        if (event.getModifierKeyState(['Alt'])) {
          mods |= 0x08; // EVENTFLAG_ALT_DOWN
        }
      } catch (e) {
      }
    }
    return mods;
  }

  private heldButtonMods(event: MouseEvent): number {
    // Prefer the system-reported button state (MouseEvent.pressedButtons,
    // API 15+): authoritative for multi-button holds and self-correcting if
    // a Release was missed (e.g. the button was released outside this
    // component) — the next Move reads the true state.
    let pb = event.pressedButtons;
    if (pb !== undefined) {
      let mods = 0;
      for (let i = 0; i < pb.length; i++) {
        if (pb[i] === MouseButton.Left) {
          mods |= 0x10;
        } else if (pb[i] === MouseButton.Middle) {
          mods |= 0x20;
        } else if (pb[i] === MouseButton.Right) {
          mods |= 0x40;
        }
      }
      return mods;
    }
    // Fallback (pressedButtons unavailable): left-only tracking
    return this.mouseButtonPressed ? 0x10 : 0;
  }

  handleMouseEvent(event: MouseEvent): void {
    if (!this.isCreated()) {
      return;
    }

    // Stop propagation so Tabs doesn't handle the event (prevents tab switching on scroll)
    event.stopPropagation();

    // DSF is set to device scale factor, so CEF GetViewRect returns CSS pixels.
    // ArkUI mouse coordinates are in vp (CSS pixels), so no conversion needed.
    let x = Math.round(event.x);
    let y = Math.round(event.y);

    // ArkUI MouseButton: Left=1, Right=2, Middle=4, Back=8, Forward=16, None=0
    // CEF cef_mouse_button_type_t: MBT_LEFT=0, MBT_MIDDLE=1, MBT_RIGHT=2
    let cefButton = 0; // default left
    if (event.button === MouseButton.Left) {
      cefButton = 0;
    } else if (event.button === MouseButton.Middle) {
      cefButton = 1;
    } else if (event.button === MouseButton.Right) {
      cefButton = 2;
    }

    // ArkUI MouseAction: Press=1, Release=2, Move=3
    switch (event.action) {
      case MouseAction.Press: {
        this.lastInputX = event.x;
        this.lastInputY = event.y;
        this.setRenderAreaActive(true);
        this.stopUrlEditing();
        this.handleImeFocus();
        arkui_cef.cefSendFocusEvent(this.browserId, true);

        // Click count (LEFT only): double-click selects a word, triple-click
        // selects a paragraph — CEF recognizes these from click_count.
        let clickCount = 1;
        if (cefButton === 0) {
          let now = Date.now();
          if (now - this.lastClickTime <= OsrCefView.DOUBLE_CLICK_TIME_MS &&
              Math.abs(x - this.lastClickX) <= OsrCefView.DOUBLE_CLICK_SLOP_PX &&
              Math.abs(y - this.lastClickY) <= OsrCefView.DOUBLE_CLICK_SLOP_PX &&
              this.clickCount > 0) {
            // Triple-click wraps back to 1 (4th click = new single click)
            this.clickCount = this.clickCount >= 3 ? 1 : this.clickCount + 1;
          } else {
            this.clickCount = 1;
          }
          this.lastClickTime = now;
          this.lastClickX = x;
          this.lastClickY = y;
          clickCount = this.clickCount;
          this.mouseButtonPressed = true;
        }

        // Modifiers: the pressed button's own flag + Shift/Ctrl/Alt
        let pressMods = this.buildMouseModifiers(event, this.buttonFlag(cefButton));
        arkui_cef.cefSendMouseClick(this.browserId, x, y, cefButton, false, clickCount, pressMods);
        this.lastMouseX = x;
        this.lastMouseY = y;
        break;
      }
      case MouseAction.Release:
        arkui_cef.cefSendMouseClick(this.browserId, x, y, cefButton, true, 1, 0);
        if (cefButton === 0) {
          // Only LEFT clears the flag — releasing RIGHT/MIDDLE while LEFT is
          // held must not break an in-progress left-drag.
          this.mouseButtonPressed = false;
        }
        break;
      case MouseAction.Move: {
        // Forward move events including during a button-drag. Modifiers come
        // from the system-reported pressedButtons — accurate for multi-button
        // holds and self-correcting after a missed Release.
        let moveMods = this.buildMouseModifiers(event, this.heldButtonMods(event));
        arkui_cef.cefSendMouseMove(this.browserId, x, y, false, moveMods);
        this.lastMouseX = x;
        this.lastMouseY = y;
        break;
      }
      default:
        break;
    }
  }

  // ==================== Axis events (wheel / two-finger scroll) ====================
  // Wheel translation: axis values are forwarded event-by-event with a
  // per-source gain — NO threshold batching (the old batching caused the
  // "two-stage scroll": sub-threshold deltas waited, then jumped). The gain
  // is an EMPIRICAL calibration factor (ArkUI's axis units are documented
  // inconsistently: vp vs angle vs pixel per source), NOT a magic number —
  // tune each source independently.
  // Defaults keep the previously accepted feel (×3 was tuned historically;
  // the old batching did not change the total amount, only its delivery).
  // Sign note: docs say touchpad swipe-up = positive and wheel-up = negative
  // (opposite conventions), but the unified negation below matches the
  // accepted feel on this device — if either input ever feels inverted,
  // flip that source's sign below.
  private static readonly TOUCHPAD_WHEEL_GAIN: number = 3;
  private static readonly MOUSE_WHEEL_GAIN: number = 3;

  handleAxisEvent(event: AxisEvent): void {
    if (!this.isCreated()) {
      return;
    }

    let x = Math.round(event.x);
    let y = Math.round(event.y);
    let vAxis = event.getVerticalAxisValue();
    let hAxis = event.getHorizontalAxisValue();

    // Ctrl+wheel zoom: detect Ctrl modifier and send with EVENTFLAG_CONTROL_DOWN
    let isCtrlPressed = false;
    if (event.getModifierKeyState) {
      try {
        isCtrlPressed = event.getModifierKeyState(['Ctrl']);
      } catch (e) {
      }
    }
    let modifiers = isCtrlPressed ? 0x04 : 0;

    if (vAxis === 0 && hAxis === 0) {
      return;
    }

    // Distinguish the input source (documented use of sourceTool on axis
    // events): TOUCHPAD = two-finger swipe, MOUSE = wheel rotation.
    let gain = OsrCefView.MOUSE_WHEEL_GAIN;
    if (event.sourceTool === SourceTool.TOUCHPAD) {
      gain = OsrCefView.TOUCHPAD_WHEEL_GAIN;
    }

    // Direct pass-through: every axis event becomes one wheel event with the
    // same total delta — Chromium coalesces rapid events itself.
    let deltaY = Math.round(-vAxis * gain);
    let deltaX = Math.round(-hAxis * gain);
    if (deltaY === 0 && deltaX === 0) {
      // Sub-rounding remainder (< 0.5 units) — skip.
      return;
    }
    arkui_cef.cefSendMouseWheel(this.browserId, x, y, deltaX, deltaY, modifiers);
  }

  // ==================== Input Method Framework (IME) ====================

  private handleImeFocus(showPanel: boolean = true): void {
    // Only drive the panel when CEF has a focused input field
    // (keyboardVisible=true). Attach on first need; afterwards restore the
    // panel (e.g. after a URL-bar interlude hid it) with self-heal.
    if (!this.keyboardVisible) {
      return;
    }
    if (!this.imeAttached) {
      this.attachInputMethod(showPanel);
    } else if (showPanel) {
      this.showImeKeyboardWithHeal();
    }
  }

  // Shows the IME panel on the persistent session. If the show fails
  // (IME_ERR_DETACHED — the session was killed by an external race),
  // re-attach and retry once: self-healing instead of dead input.
  private showImeKeyboardWithHeal(): void {
    if (!this.imeAttached || OsrCefView.imeOwnerView !== this) {
      return;
    }
    let ok = arkui_cef.cefImeCShowKeyboard();
    if (!ok) {
      hilog.warn(DOMAIN, MODULE, "IME show failed");
      this.detachInputMethod();
      this.attachInputMethod();
    }
  }

  // Called when the user clicks the web render area: exit the URL
  // TextInput's edit state so its caret stops blinking and its IME editor
  // binding is released (via the component's OWN lifecycle —
  // TextInputController.stopEditing, API 10, "exit edit state"), letting letters
  // and IME input route to the web page's IMF C session.
  //
  // NOTE: FocusController.clearFocus() was tried here first and REGRESSED
  // the IMF C session: clearing the ArkTS focus chain races the C session
  // attach at the input method service (the ArkTS editor teardown can land
  // AFTER our attach and tear it down — IME panel disappears, letters stop
  // routing). stopEditing keeps the transition inside the TextInput's
  // well-defined edit-state lifecycle instead of the global focus chain.
  private stopUrlEditing(): void {
    if (!this.urlInputFocused) {
      return;
    }
    try {
      this.urlInputController.stopEditing();
    } catch (e) {
      hilog.error(DOMAIN, MODULE, "stopEditing failed: %{public}s", (e as Error).message);
    }
  }

  private setRenderAreaActive(active: boolean): void {
    this.renderAreaActive = active;
    if (active) {
      if (OsrCefView.imeOwnerView !== this) {
        // Take over the global key routing and the IMF session. Destroy the
        // global session regardless of which view owns it: across a tab
        // switch the activation and deactivation visibility edges fire in
        // either order, so the previous owner's own detach path may never
        // run (its imeAttached flag was already cleared by this takeover) —
        // a lingering session keeps the IMS binding alive (status bar stays
        // up, physical keys keep pulling stale candidates).
        let prev = OsrCefView.imeOwnerView;
        if (prev !== null) {
          prev.imeAttached = false;
        }
        arkui_cef.cefImeCDetach();
        this.imeAttached = false;
        OsrCefView.imeOwnerView = this;
      }
      // Already the owner (ordinary click inside our own render area while
      // typing): leave the live session alone.
    }
    arkui_cef.setRenderAreaActive(this.browserId, active);
    if (!active) {
      if (this.isCreated()) {
        arkui_cef.cefSendFocusEvent(this.browserId, false);
      }
    }
  }

  // The IMF C API session (g_imeCApi in the NAPI layer) is process-global:
  // exactly one window-level key filter and one IME session exist per window.
  // With several OSR views alive at once (Tabs builds every TabContent up
  // front), only the view whose render area the user last interacted with may
  // own that session — otherwise one view's VKR/window events detach or
  // replace the session another view is actively typing into.
  private static imeOwnerView: OsrCefView | null = null;

  private isImeOwner(): boolean {
    return OsrCefView.imeOwnerView === this && this.imeAttached;
  }

  private attachInputMethod(showPanel: boolean = true): void {
    if (this.imeAttached || this.imeAttaching) {
      return;
    }
    if (OsrCefView.imeOwnerView !== null && OsrCefView.imeOwnerView !== this) {
      // Another view owns the global session — never replace it from a
      // background view's callbacks.
      return;
    }
    OsrCefView.imeOwnerView = this;
    this.imeAttaching = true;

    // Compute initial cursor position (screen-absolute px). Prefer the
    // renderer's last known caret for THIS view: re-entering a tab whose
    // caret never moved produces no SelectionBoundsChanged event (upstream
    // dedupe), so without this pull the candidate window would sit at the
    // tap point, which can be far from the caret in an empty field.
    let winRect = this.getWindowRect();
    let cursorLeft = Math.round((this.lastInputX + this.componentGlobalX) * this.cachedDsf) + winRect.left;
    let cursorTop = Math.round((this.lastInputY + this.componentGlobalY) * this.cachedDsf) + winRect.top;
    let cursorWidth = 2;
    let cursorHeight = Math.round(20 * this.cachedDsf);
    let caret = arkui_cef.cefGetLastCaretScreen(this.browserId);
    if (caret.valid) {
      cursorLeft = caret.left;
      cursorTop = caret.top;
      cursorWidth = caret.width > 0 ? caret.width : 2;
      cursorHeight = caret.height > 0 ? caret.height : cursorHeight;
      hilog.info(DOMAIN, MODULE,
        "attachInputMethod: using renderer caret screen=(%{public}d, %{public}d) over tap estimate",
        cursorLeft, cursorTop);
    }
    hilog.info(DOMAIN, MODULE,
      "attachInputMethod: screen_px left=%{public}d top=%{public}d winRect=%{public}d,%{public}d",
      cursorLeft, cursorTop, winRect.left, winRect.top);

    let ok = arkui_cef.cefImeCAttach(
      this.cachedWindowId,
      cursorLeft, cursorTop, cursorWidth, cursorHeight,
      (text: string) => {
        if (this.isCreated() && this.renderAreaActive) {
          arkui_cef.cefImeCommitText(this.browserId, text);
        }
      },
      (length: number) => {
        if (this.isCreated() && this.renderAreaActive) {
          for (let i = 0; i < length; i++) {
            arkui_cef.cefSendKeyEvent(this.browserId, 2055, 0, 0);
            arkui_cef.cefSendKeyEvent(this.browserId, 2055, 1, 0);
          }
        }
      },
      () => {
        if (this.isCreated() && this.renderAreaActive) {
          arkui_cef.cefSendKeyEvent(this.browserId, 2054, 0, 0);
          arkui_cef.cefSendKeyEvent(this.browserId, 2054, 2, 0, 13);
          arkui_cef.cefSendKeyEvent(this.browserId, 2054, 1, 0);
        }
      },
      (text: string) => {
        if (this.isCreated() && this.renderAreaActive) {
          arkui_cef.cefImeSetComposition(this.browserId, text);
        }
      },
      () => {
        if (this.isCreated() && this.renderAreaActive) {
          arkui_cef.cefImeCancelComposition(this.browserId);
        }
      },
      showPanel
    );

    this.imeAttaching = false;
    if (ok) {
      this.imeAttached = true;
      hilog.info(DOMAIN, MODULE, "IME attached (C API) successfully");
    } else {
      hilog.error(DOMAIN, MODULE, "IME attach (C API) failed");
    }
  }

  private detachInputMethod(): void {
    if (!this.imeAttached) {
      return;
    }
    this.imeAttached = false;
    // The global session may already belong to another view (this one lost
    // ownership without a detach) — only detach what we still own.
    if (OsrCefView.imeOwnerView === this) {
      OsrCefView.imeOwnerView = null;
      arkui_cef.cefImeCDetach();
      hilog.info(DOMAIN, MODULE, "IME detached (C API)");
    }
  }

}
