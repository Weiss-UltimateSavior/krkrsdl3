import { hilog } from '@kit.PerformanceAnalysisKit';
import krkr from 'libkrkrsdl3.so';
import { common } from '@kit.AbilityKit';
import { DownloadDirManager } from '../utils/DownloadDirManager';
import window from '@ohos.window';
import inputMethod from '@ohos.inputMethod';
import display from '@ohos.display';

const TAG = 'krkrsdl3';
const DOMAIN = 0x3200;

@Entry
@Component
struct Index {
    private xComponentId: string = 'xcomponent_krkrsdl3';
    private xComponentController: XComponentController = new XComponentController();
    private engineInited: boolean = false;
    private gamePath: string = '';
    private density: number = 1;
    private lastTapTime: number = 0;
    private lastTapX: number = 0;
    private lastTapY: number = 0;
    private isDoubleTap: boolean = false;
    private readonly doubleTapTime: number = 400;
    private readonly doubleTapDist: number = 30;

    async aboutToAppear(): Promise<void> {
        hilog.info(DOMAIN, TAG, 'aboutToAppear');
        let ctx = getContext(this) as common.UIAbilityContext;
        this.density = display.getDefaultDisplaySync().densityPixels;

        // 注册 C++ 回调
        try {
            krkr.registerCallbacks(
                 (title: string): void => {
                    window.getLastWindow(ctx).then((win: window.Window): void => {
                        try { win.setWindowTitle(title); } catch (e) {}
                    });
                },
                (full: boolean): void => {
                    window.getLastWindow(ctx).then((win: window.Window): void => {
                        try { win.setFullScreen(full); } catch (e) {}
                    });
                },
                (show: boolean, _x: number, _y: number, _w: number, _h: number): void => {
                    let ctrl = inputMethod.getController();
                    try { if (show) ctrl.showTextInput(); else ctrl.hideTextInput(); } catch (e) {}
                }
            );
        } catch (e) { hilog.error(DOMAIN, TAG, 'registerCallbacks: %{public}s', String(e)); }

        try { krkr.setResourceManager(ctx.resourceManager); } catch (e) {}

        await DownloadDirManager.getInstance().init(ctx);
        await DownloadDirManager.getInstance().initDownloadDirectory();
        this.gamePath = DownloadDirManager.getInstance().getDownloadDirPath() + '/data.xp3';
        this.launchGame();
    }

    private launchGame(): void {
        if (this.engineInited || this.gamePath.length === 0) return;
        try { this.engineInited = krkr.initEngine(this.gamePath); } catch (e) {
            hilog.error(DOMAIN, TAG, 'initEngine: %{public}s', String(e));
        }
    }

    private sendTouch(type: number, xVP: number, yVP: number, button: number): void {
        if (!this.engineInited) return;
        let x = Math.round(xVP * this.density);
        let y = Math.round(yVP * this.density);
        try { krkr.sendMouseEvent(type, x, y, button); } catch (e) {}
    }

    aboutToDisappear(): void {
        try { krkr.shutdown(); } catch (e) {}
    }

    build() {
        Column() {
            XComponent({
                id: this.xComponentId,
                type: XComponentType.SURFACE,
                libraryname: 'krkrsdl3',
                controller: this.xComponentController
            })
            .onLoad((): void => {
                let sid = this.xComponentController.getXComponentSurfaceId();
                try { krkr.setSurface(sid); } catch (e) {
                    hilog.error(DOMAIN, TAG, 'setSurface: %{public}s', String(e));
                }
            })
            .onAreaChange((oldArea: Area, newArea: Area) => {
                // 当 XComponent 尺寸变化时，通知 C++ 层更新视口
                const widthVP = newArea.width as number;
                const heightVP = newArea.height as number;
                let widthPx = Math.round(widthVP * this.density);
                let heightPx = Math.round(heightVP * this.density);

                if (widthPx > 0 && heightPx > 0) {
                    try {
                        if (typeof krkr.updateWindowSize === 'function') {
                            krkr.updateWindowSize(widthPx, heightPx);
                        }
                    } catch (e) {
                        hilog.error(DOMAIN, TAG, 'updateWindowSize error: %{public}s', String(e));
                    }
                }
            })
            .width('100%')
            .height('100%')
        }
        .width('100%')
        .height('100%')
        .backgroundColor('#000000')
        .onTouch((event: TouchEvent): void => {
            if (event.touches.length === 0) return;
            let t = event.touches[0];
            if (event.type === TouchType.Down) {
                let now = Date.now();
                let dx = t.x - this.lastTapX;
                let dy = t.y - this.lastTapY;
                if ((now - this.lastTapTime) < this.doubleTapTime &&
                    Math.sqrt(dx * dx + dy * dy) < this.doubleTapDist) {
                    this.isDoubleTap = true;
                    this.sendTouch(0, t.x, t.y, 2);
                } else {
                    this.isDoubleTap = false;
                    this.sendTouch(0, t.x, t.y, event.touches.length >= 2 ? 2 : 1);
                }
            } else if (event.type === TouchType.Up) {
                if (this.isDoubleTap) {
                    this.sendTouch(1, t.x, t.y, 2);
                    this.isDoubleTap = false;
                } else {
                    this.sendTouch(1, t.x, t.y, event.touches.length >= 2 ? 2 : 1);
                }
                this.lastTapTime = Date.now();
                this.lastTapX = t.x;
                this.lastTapY = t.y;
            } else if (event.type === TouchType.Move) {
                this.sendTouch(2, t.x, t.y, 0);
            }
        })
        .onMouse((event: MouseEvent): void => {
            if (event.action === MouseAction.Press) {
                let btn = event.button === MouseButton.Right ? 2 : event.button === MouseButton.Middle ? 3 : 1;
                this.sendTouch(0, event.x, event.y, btn);
            } else if (event.action === MouseAction.Release) {
                let btn = event.button === MouseButton.Right ? 2 : event.button === MouseButton.Middle ? 3 : 1;
                this.sendTouch(1, event.x, event.y, btn);
            } else if (event.action === MouseAction.Move) {
                this.sendTouch(2, event.x, event.y, 0);
            }
        })
    }
}
