package com.folotoy.whalelink;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.WindowManager;

import java.nio.ByteBuffer;

/**
 * 投屏：MediaProjection → VirtualDisplay（直接输出到目标分辨率）→ RGB565+RLE
 * → 按 MTU 分块写 Frame 特征。
 *
 * 为什么 VirtualDisplay 直接用目标尺寸而不是先截大图再缩：省掉一次 1080p
 * 位图拷贝，手机上 CPU 和内存都划算。density 按比例缩放，被投影的 App 会
 * 按等比小屏重新布局，视觉上基本一致。
 */
public class MirrorSource {

    public interface Sink {
        /**
         * 把一帧的所有块按顺序一次性写下去。返回 true 表示整帧都进了控制器队列。
         * 整帧一次调用而不是逐块调用，是因为每块的跨线程排队开销比传输本身还大。
         */
        boolean writeAll(java.util.List<byte[]> blocks, long timeoutMs);

        int mtu();
    }

    private static final String TAG = "WhaleMirror";

    private final Context ctx;
    private final Prefs prefs;
    private final Sink sink;
    private final MediaProjection projection;

    private ImageReader reader;
    private VirtualDisplay display;
    private HandlerThread encThread;
    private Handler encHandler;

    private volatile boolean running;
    private volatile boolean busy;

    private long lastSent;
    private int seq;
    private int w;
    private int h;
    private int density;
    /** 当前这一帧用的编码格式，随帧头一起发出去。只在 enc 线程里读写。 */
    private int fmt = 0;

    /** 0 = RGB565+RLE，1 = 裸 RGB565，2 = 增量块。设备端按这个字段决定怎么解。 */
    private static final int FMT_RLE = 0;
    private static final int FMT_RAW = 1;
    private static final int FMT_TILE = 2;

    /** 设备端 WHALE_FRAME_MAX 是 40960 字节，超了整帧会被丢掉。 */
    private static final int FRAME_MAX = 40_000;

    // ---- 增量刷新 ----------------------------------------------------------
    // 8×8 的块。120×160 正好切成 15×20 = 300 块，没有边缘残块；其余档位
    // 可能除不尽（80×107 就有），所以打包时每块一律填满 8×8，越界补 0，
    // 设备端再按有效行列裁掉 —— 这样两端的长度校验才恒等。
    private static final int TILE = 8;
    private static final int TILE_BYTES = TILE * TILE * 2;

    /**
     * 增量帧的字节预算。整帧 RLE 实测 5~8 KB，脏块超过这个量级就没有意义，
     * 还不如整帧发 —— 省下的只是设备端的解码时间，而空口反而更贵。
     */
    private static final int TILE_BUDGET = 6_000;

    /**
     * 每这么多帧强制一次全量。增量依赖「手机上缓存 == 设备上位图」这个前提，
     * 而一帧写成功只代表进了控制器队列，不代表设备真的贴上了。长时间跑下来
     * 万一对不上，画面会有一块永远停在旧内容上 —— 定期全量把它兜回来。
     */
    private static final int FULL_EVERY = 40;

    /**
     * 静止多少帧之后补一帧。
     *
     * 增量最大的好处是「画面没动就一个字节都不发」，但设备端有「5 秒没有新帧
     * 就把投屏收起来」的超时（见 main.c 的 STREAM_IDLE_US）。真的一直不发，
     * 用户会看到画面自己消失。所以静止一段时间就补一帧整图 —— 4 秒一次、
     * 5 KB 左右，平均带宽 1.2 KB/s，占 BLE 有效带宽不到 5%，可以忽略。
     */
    private static final int STILL_REFRESH = 20;

    /** 上一帧像素，增量比对的基准。只在发送成功后更新。 */
    private short[] prevPx;
    /** 本帧像素，等 sendFrame 成功后转正为 prevPx。 */
    private short[] pendingPx;
    /** 下一帧必须整帧。启动、失败重试、强制刷新时置位。 */
    private boolean needFull = true;
    private int fullCountdown = FULL_EVERY;
    private int[] dirtyList = new int[0];
    /** 本帧发出去的脏块数；0 表示走的是整帧（RLE 或裸像素）。 */
    private int curTiles;
    /** 本帧是不是「画面没变、一个字节都没发」。 */
    private boolean curEmpty;
    private int stillCount;

    private long statFrames;
    private long statBytes;
    private long statTiles;
    private long statWindowStart;
    private int fps;
    /** 帧率 ×10。整数 fps 会把 0.9 显示成 0，看着像"完全没工作"。 */
    private int fpsTenths;
    /** 上一秒平均每帧发了几个块（0 表示整帧 RLE/裸帧）。 */
    private int lastTilesPerFrame;

    public MirrorSource(Context ctx, Prefs prefs, MediaProjection projection, Sink sink) {
        this.ctx = ctx.getApplicationContext();
        this.prefs = prefs;
        this.projection = projection;
        this.sink = sink;
    }

    /**
     * 建立虚拟屏。失败返回 false —— 调用方必须处理，否则开关显示"开"、
     * 实际什么也没录，界面上一点线索都没有。
     */
    public boolean start() {
        int[] wh = prefs.resolution();
        w = wh[0];
        h = wh[1];

        WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
        DisplayMetrics dm = new DisplayMetrics();
        if (wm != null) {
            wm.getDefaultDisplay().getRealMetrics(dm);
        } else {
            dm.densityDpi = 320;
            dm.widthPixels = 1080;
        }
        density = Math.max(80, (int) (dm.densityDpi * (w / (float) Math.max(1, dm.widthPixels))));

        encThread = new HandlerThread("mirror-enc");
        encThread.start();
        encHandler = new Handler(encThread.getLooper());

        try {
            reader = ImageReader.newInstance(w, h, PixelFormat.RGBA_8888, 2);
            reader.setOnImageAvailableListener(this::onImage, encHandler);

            display = projection.createVirtualDisplay(
                    "whale-link",
                    w, h, density,
                    DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR,
                    reader.getSurface(), null, encHandler);
        } catch (Exception e) {
            // Android 14 上授权过期、或 projection 已被系统回收，这里会抛
            // SecurityException。以前没接住，Service 直接崩，用户只看到
            // "开关是开的但没画面"。
            Log.w(TAG, "建立虚拟屏失败：" + e);
            releaseQuietly();
            return false;
        }

        if (display == null) {
            Log.w(TAG, "createVirtualDisplay 返回 null");
            releaseQuietly();
            return false;
        }

        running = true;
        lastSent = 0;
        needFull = true;             // 新会话的第一帧一定是全量，设备端才有位图可叠加
        fullCountdown = FULL_EVERY;
        prevPx = null;
        pendingPx = null;
        statWindowStart = SystemClock.elapsedRealtime();
        Log.i(TAG, "投屏启动 " + w + "x" + h + " density=" + density);
        return true;
    }

    private void releaseQuietly() {
        try {
            if (display != null) display.release();
        } catch (Exception ignored) {
        }
        display = null;
        try {
            if (reader != null) reader.close();
        } catch (Exception ignored) {
        }
        reader = null;
        if (encThread != null) {
            encThread.quitSafely();
            encThread = null;
        }
    }

    public void stop() {
        running = false;
        try {
            if (display != null) {
                display.release();
            }
        } catch (Exception ignored) {
        }
        display = null;
        try {
            if (reader != null) {
                reader.close();
            }
        } catch (Exception ignored) {
        }
        reader = null;
        if (encThread != null) {
            encThread.quitSafely();
            encThread = null;
        }
        try {
            projection.stop();
        } catch (Exception ignored) {
        }
    }

    public boolean isRunning() {
        return running;
    }

    public int fps() {
        return fps;
    }

    /** 帧率 ×10，用来在状态行显示一位小数。 */
    public int fpsTenths() {
        return fpsTenths;
    }

    public long bytes() {
        return statBytes;
    }

    /** 上一秒平均每帧发了几个脏块（0 = 走整帧）。用来判断增量有没有真的生效。 */
    public int tilesPerFrame() {
        return lastTilesPerFrame;
    }

    private void onImage(ImageReader r) {
        Image img = null;
        try {
            img = r.acquireLatestImage();
        } catch (Exception e) {
            return;
        }
        if (img == null) {
            return;
        }
        if (!running || busy) {
            img.close();
            return;
        }

        long now = SystemClock.elapsedRealtime();
        if (now - lastSent < prefs.intervalMs()) {
            img.close();
            return;
        }
        lastSent = now;
        busy = true;

        try {
            byte[] payload = encode(img);
            if (payload != null) {
                sendFrame(payload);
            }
        } catch (OutOfMemoryError e) {
            Log.w(TAG, "编码内存不足，跳过一帧");
        } catch (Exception e) {
            Log.w(TAG, "编码失败 " + e);
        } finally {
            busy = false;
            img.close();
        }
    }

    /** ImageReader 的 ARGB_8888 帧 → RGB565 像素数组。行 padding 要先裁掉。 */
    private short[] toPixels(Image img) {
        Image.Plane plane = img.getPlanes()[0];
        ByteBuffer buf = plane.getBuffer();
        int pixelStride = plane.getPixelStride();
        int rowStride = plane.getRowStride();
        int rowPadding = rowStride - pixelStride * w;

        Bitmap padded = Bitmap.createBitmap(w + rowPadding / Math.max(1, pixelStride), h,
                Bitmap.Config.ARGB_8888);
        padded.copyPixelsFromBuffer(buf);

        int[] argb = new int[w * h];
        padded.getPixels(argb, 0, w, 0, 0, w, h);
        padded.recycle();

        short[] px = new short[w * h];
        for (int i = 0; i < px.length; i++) {
            px[i] = RleCodec.toRgb565(argb[i]);
        }
        return px;
    }

    private byte[] encode(Image img) {
        short[] px = toPixels(img);
        pendingPx = px;

        // 增量：和上一帧逐块比对，只打包变了的块。画面没动就返回 0 字节。
        // 前提是设备端已经有一张同尺寸的位图可叠加 —— needFull 为真说明刚
        // 启动或者上一帧没发成功，这时必须先整帧把位图建起来。
        if (prefs.tileMode() && !needFull && prevPx != null && prevPx.length == px.length) {
            byte[] tiles = buildTiles(px);
            if (tiles != null) {
                curTiles = tiles.length == 0 ? 0 : (tiles[0] & 0xFF);
                curEmpty = tiles.length == 0;
                fmt = FMT_TILE;
                return tiles;
            }
            // 脏块太多，增量比整帧还贵，这一帧退回整帧路径
        }

        curTiles = 0;
        curEmpty = false;
        int rawBytes = px.length * 2;

        // 用户强制直推：完全跳过 RLE 编码，设备端只做 memcpy。
        // 空口代价很实在（38400 B 拆 78 块 vs 压缩后约 5000 B 拆 12 块），
        // 保留这个开关是为了能拿真实帧率对比，不是为了日常使用。
        if (prefs.rawPixel() && rawBytes <= FRAME_MAX) {
            fmt = FMT_RAW;
            return toRaw(px);
        }

        // RLE 只对大片纯色有效。照片、视频、渐变壁纸这类内容会把它撑到
        // 原始大小的 1.5 倍，一帧就直接顶到设备端 40 KB 上限之上 ——
        // 结果就是"刚连上能看到画面，一切到别的 App 就再也不动了"。
        // 所以压不动就发裸像素：带宽贵一点，但一定画得出来。
        byte[] rle = RleCodec.encode(px);
        if (rle.length >= rawBytes && rawBytes <= FRAME_MAX) {
            fmt = FMT_RAW;
            return toRaw(px);
        }
        fmt = FMT_RLE;
        return rle;
    }

    /** 画面一个像素都没变。用一个共享的空数组表示，避免每帧新建对象。 */
    private static final byte[] NO_CHANGE = new byte[0];

    /**
     * 打包脏块。返回 null = 脏块太多，改用整帧；返回长度 0 = 画面完全没变。
     *
     * 块内一律填满 TILE×TILE，越界像素补 0。设备端按有效行列裁切，两边的
     * 长度校验才恒等 —— 80×107 这种除不尽的档位就靠这个。
     */
    private byte[] buildTiles(short[] px) {
        final int tw = (w + TILE - 1) / TILE;
        final int th = (h + TILE - 1) / TILE;
        final int cap = tw * th;
        if (dirtyList.length < cap) dirtyList = new int[cap + 8];

        int n = 0;
        for (int ty = 0; ty < th; ty++) {
            for (int tx = 0; tx < tw; tx++) {
                if (!tileDiffers(px, prevPx, tx, ty)) continue;
                dirtyList[n++] = (tx & 0xFF) | ((ty & 0xFF) << 8);
                // 边数边判预算：真到"整屏都在变"的时候没必要数完
                if (n > 255 || n * (2 + TILE_BYTES) > TILE_BUDGET) return null;
            }
        }
        if (n == 0) return NO_CHANGE;

        byte[] out = new byte[2 + n * (2 + TILE_BYTES)];
        out[0] = (byte) n;
        out[1] = (byte) TILE;
        int o = 2;
        for (int i = 0; i < n; i++) {
            final int tx = dirtyList[i] & 0xFF;
            final int ty = (dirtyList[i] >> 8) & 0xFF;
            out[o++] = (byte) tx;
            out[o++] = (byte) ty;
            for (int r = 0; r < TILE; r++) {
                final int y = ty * TILE + r;
                final int row = (y < h) ? y * w : 0;
                for (int c = 0; c < TILE; c++) {
                    final int x = tx * TILE + c;
                    final short v = (y < h && x < w) ? px[row + x] : 0;
                    out[o++] = (byte) (v & 0xFF);
                    out[o++] = (byte) ((v >> 8) & 0xFF);
                }
            }
        }
        return out;
    }

    private boolean tileDiffers(short[] a, short[] b, int tx, int ty) {
        final int x0 = tx * TILE;
        final int y0 = ty * TILE;
        final int x1 = Math.min(x0 + TILE, w);
        final int y1 = Math.min(y0 + TILE, h);
        for (int y = y0; y < y1; y++) {
            final int row = y * w;
            for (int x = x0; x < x1; x++) {
                if (a[row + x] != b[row + x]) return true;
            }
        }
        return false;
    }

    /** 裸 RGB565 小端，与设备端 lv_image_dsc RGB565 的内存布局一致。 */
    private static byte[] toRaw(short[] px) {
        byte[] out = new byte[px.length * 2];
        for (int i = 0, o = 0; i < px.length; i++) {
            short v = px[i];
            out[o++] = (byte) (v & 0xFF);
            out[o++] = (byte) ((v >> 8) & 0xFF);
        }
        return out;
    }

    private void sendFrame(byte[] payload) {
        // 画面一个像素都没变：一个字节都不用发。仍然算一次成功的采集 ——
        // 否则静止时 fps 会掉到 0，看着像链路挂了。
        if (payload.length == 0) {
            commit(true);
            tickStats();
            return;
        }

        int mtu = sink.mtu();
        // 单块净荷 = MTU - 3(ATT 写头) - 16(本协议帧头) - 4(余量)。
        // 下限必须是 20 而不是 32：MTU 还没协商成功（仍是默认 23）时，
        // Math.max(32, …) 会算出 32 字节的块，超过 ATT 单包上限，系统的
        // writeCharacteristic 直接以「长度超 MTU」拒掉 —— 表现就是每一
        // 块都写不进去、帧率永远 0，而原因藏在 MTU 上。
        // 再留 4 字节余量：净荷正好等于 MTU-3 时，个别 ROM 的边界判断会
        // 把这一包当成超长（差一错误的 `>=`），多花 1 块换掉这个风险很值。
        int payloadMax = Math.max(20, mtu - 3 - 16 - 4);
        int total = (payload.length + payloadMax - 1) / payloadMax;
        if (payload.length > FRAME_MAX || total > 1600) {
            Log.w(TAG, "帧太大(" + payload.length + " 字节)，丢弃");
            commit(false);
            return;
        }

        byte[] packet = new byte[16 + payloadMax];
        java.util.List<byte[]> blocks = new java.util.ArrayList<>(total);

        for (int idx = 0; idx < total; idx++) {
            int off = idx * payloadMax;
            int len = Math.min(payloadMax, payload.length - off);

            int type = idx == 0 ? Frames.TYPE_BEGIN
                    : (idx == total - 1 ? Frames.TYPE_END : Frames.TYPE_CHUNK);
            byte[] hdr = Frames.frameHeader(type, fmt, 0x01,
                    w, h, seq, idx, total, len);
            System.arraycopy(hdr, 0, packet, 0, 16);
            System.arraycopy(payload, off, packet, 16, len);

            byte[] slice = new byte[16 + len];
            System.arraycopy(packet, 0, slice, 0, 16 + len);
            blocks.add(slice);
            statBytes += slice.length;
        }

        // 整帧一次交下去。每块都要等一个连接事件：连接间隔 15 ms 时 78 块
        // 理论上 1.2 秒，但安卓的 GATT 写队列会在中间卡几下，按每块 60 ms
        // 估，最少给 4 秒。以前写死 2.5 秒 —— 碰上慢一点的第一帧，每帧都
        // 判超时，界面就永远停在 0 fps。
        long timeoutMs = Math.max(4000L, Math.min(15000L, total * 60L));
        boolean ok = running && sink.writeAll(blocks, timeoutMs);
        if (ok) {
            seq = (seq + 1) & 0xFFFF;
        }
        commit(ok);
        tickStats();
    }

    /**
     * 发送结果落地。
     *
     * 增量刷新对「手机缓存」和「设备位图」的一致性要求很高：一帧只要没确认
     * 发出去，就绝不能把它当成新基准 —— 否则下一帧只发差量，设备端会永久
     * 缺掉那一块，表现是画面上某一块定住不动，而且越用越多。所以失败时把
     * needFull 置起来，下一帧老老实实整帧重来。
     */
    private void commit(boolean ok) {
        if (ok) {
            prevPx = pendingPx;
            pendingPx = null;
            needFull = false;
            statTiles += curTiles;
            statFrames++;
            if (curEmpty) {
                // 画面一直没变，一个字节都没发。设备端有「5 秒无帧就收起
                // 投屏」的超时，真的一直不发，用户会看到画面自己消失 ——
                // 所以静止 STILL_REFRESH 帧后补一帧整图当心跳。
                if (++stillCount >= STILL_REFRESH) {
                    stillCount = 0;
                    needFull = true;
                }
            } else {
                stillCount = 0;
                if (curTiles > 0 && --fullCountdown <= 0) {
                    needFull = true;             // 定期整帧兜底，纠正长期漂移
                    fullCountdown = FULL_EVERY;
                }
            }
        } else {
            needFull = true;
            pendingPx = null;
        }
        curTiles = 0;
    }

    private void tickStats() {
        final long now = SystemClock.elapsedRealtime();
        if (now - statWindowStart < 1000) {
            return;
        }
        // 用一位小数。采集间隔 1 秒时，实际周期往往是 1.1~1.2 秒（编码 +
        // 传输都要时间），整数除法会把 0.9 fps 直接截成 0 —— 界面显示
        // "0 fps · 累计 99 KB"，让人以为一帧都没发出去，实际画面上屏了。
        final long dt = Math.max(1, now - statWindowStart);
        fpsTenths = (int) (statFrames * 10000L / dt);
        fps = fpsTenths / 10;
        lastTilesPerFrame = statFrames > 0 ? (int) (statTiles / statFrames) : 0;
        statFrames = 0;
        statTiles = 0;
        statWindowStart = now;
    }
}
