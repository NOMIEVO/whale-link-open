package com.folotoy.whalelink;

import android.Manifest;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import androidx.annotation.Nullable;
import androidx.core.app.NotificationCompat;
import androidx.core.content.ContextCompat;

import java.lang.ref.WeakReference;
import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

/**
 * 前台服务：保持 BLE 连接，按开关把时间 / 运动 / 位置 / 画面送出去。
 *
 * 为什么必须是前台服务：Android 9 之后，后台应用收不到连续模式的传感器
 * 事件，GPS 也会被切断。挂件要持续拿数据，就只能挂一个常驻通知。
 */
public class LinkService extends Service implements BleLink.Listener,
        MotionSource.Sink, LocationSource.Sink, MirrorSource.Sink {

    private static final String TAG = "LinkService";

    public static final String ACTION_START = "com.folotoy.whalelink.START";
    public static final String ACTION_STOP = "com.folotoy.whalelink.STOP";
    public static final String ACTION_SETTINGS = "com.folotoy.whalelink.SETTINGS";
    public static final String ACTION_RECONNECT = "com.folotoy.whalelink.RECONNECT";
    public static final String ACTION_SET_PROJECTION = "com.folotoy.whalelink.PROJECTION";
    public static final String ACTION_STOP_MIRROR = "com.folotoy.whalelink.STOP_MIRROR";
    public static final String ACTION_SEND_FILE = "com.folotoy.whalelink.SEND_FILE";
    public static final String ACTION_FILE_CMD = "com.folotoy.whalelink.FILE_CMD";
    // 手机侧手动触发一次「按需挂载」，等同于在设备上长按 OK。
    // 存在的理由很实际：调试时设备可能在手边够不到，而按键又只有实体键。
    public static final String ACTION_MOUNT_NEXT = "com.folotoy.whalelink.MOUNT_NEXT";

    public static final String EXTRA_RESULT_CODE = "result_code";
    public static final String EXTRA_RESULT_DATA = "result_data";
    public static final String EXTRA_URI = "uri";
    public static final String EXTRA_SLOT = "slot";
    public static final String EXTRA_FILE_CMD = "file_cmd";

    private static final int NOTIF_ID = 41;
    private static final String CHANNEL = "link";

    public interface Ui {
        void onStatus(String state, String detail);

        void onLog(String line);

        void onStats(String stats);
    }

    private static WeakReference<Ui> sUi = new WeakReference<>(null);

    public static void setUi(Ui ui) {
        sUi = new WeakReference<>(ui);
    }

    /** 投屏是否真的在跑。界面靠它区分「开关开着」和「画面真的在送」。 */
    private static volatile boolean sMirroring;

    public static boolean isMirroring() {
        return sMirroring;
    }

    /**
     * 「开关记着是开的，但当前没有屏幕录制授权」。
     *
     * MediaProjection 的授权只活在拿到它的那个进程里：App 被系统杀掉重启、
     * 或者重装之后，prefs 里 mirror=true 还在，投影对象却没了。这时开关
     * 显示是开的、设备端一帧都收不到，用户完全看不出问题在哪。
     * Service 发现这种状态就置位，Activity 回到前台时据此重新申请授权。
     */
    private static volatile boolean sNeedProjection;

    public static boolean needProjection() {
        return sNeedProjection;
    }

    public static void clearNeedProjection() {
        sNeedProjection = false;
    }

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ArrayDeque<String> logs = new ArrayDeque<>();

    private Prefs prefs;
    private BleLink ble;
    private MotionSource motion;
    private LocationSource location;
    private MirrorSource mirror;
    private MediaProjection projection;
    /** 按需挂载的资源库（相册）。设备要一张就转码一张送过去。 */
    private ResourceLibrary library;

    private String stateText = "正在启动";
    private String detailText = "";
    private boolean foregroundStarted;

    private boolean timeTickRunning;
    private long lastTimeSync;
    private long lastTimeSentMs;
    private int reconnectAttempts;

    private final Runnable timeTick = new Runnable() {
        @Override
        public void run() {
            sendTime();
            main.postDelayed(this, 60_000);
        }
    };

    private final Runnable statsTick = new Runnable() {
        @Override
        public void run() {
            if (mirror != null && mirror.isRunning()) {
                String err = ble != null ? ble.lastTxError() : "";
                if (mirror.fpsTenths() == 0 && err != null && !err.isEmpty()) {
                    // 一帧都没成功，却看不出原因最坑人 —— 直接把失败的那一层写出来。
                    pushStats("投屏 0 fps · " + err);
                } else {
                    // 带上每帧脏块数：0 表示这一秒全是整帧，增量没生效（或是
                    // 画面一直在整屏变化）；个位数说明增量真的在省带宽。
                    String s = String.format(Locale.US, "投屏 %d.%d fps · %d 块/帧 · 累计 %d KB",
                            mirror.fpsTenths() / 10, mirror.fpsTenths() % 10,
                            mirror.tilesPerFrame(), mirror.bytes() / 1024);
                    pushStats(s);
                }
            } else if (prefs.mirrorEnabled()) {
                // 开关开着但没在送画面 —— 这是最容易被误判成"设备坏了"的状态，
                // 必须在界面上写清楚。
                pushStats("投屏未启动 · 回 App 重新授权");
            } else if (ble != null && ble.isReady()) {
                pushStats("已连接 · MTU " + ble.mtu());
            }
            main.postDelayed(this, 2000);
        }
    };

    // ---- 生命周期 -----------------------------------------------------------

    @Override
    public void onCreate() {
        super.onCreate();
        prefs = new Prefs(this);
        ble = new BleLink(this, this);
        ble.setFileListener(this::onFileNotify);
        motion = new MotionSource(this, this);
        location = new LocationSource(this, this);
        library = new ResourceLibrary(this);
        createChannel();
        main.postDelayed(statsTick, 2000);
    }

    @Override
    public int onStartCommand(@Nullable Intent intent, int flags, int startId) {
        final String action = intent != null ? intent.getAction() : null;

        if (ACTION_STOP.equals(action)) {
            stopSelf();
            return START_NOT_STICKY;
        }

        startForegroundCompat(false);

        if (ACTION_SET_PROJECTION.equals(action) && intent != null) {
            // API 33 起 getParcelableExtra(String) 已废弃，个别 ROM 上会返回
            // null —— 那就等于"授权失败"，开关会被关掉，用户只觉得投屏打不开。
            Intent data;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                data = intent.getParcelableExtra(EXTRA_RESULT_DATA, Intent.class);
            } else {
                data = intent.getParcelableExtra(EXTRA_RESULT_DATA);
            }
            startMirror(intent.getIntExtra(EXTRA_RESULT_CODE, 0), data);
            return START_STICKY;
        }
        if (ACTION_STOP_MIRROR.equals(action)) {
            stopMirror();
            return START_STICKY;
        }
        if (ACTION_SEND_FILE.equals(action) && intent != null) {
            android.net.Uri uri = intent.getParcelableExtra(EXTRA_URI);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                uri = intent.getParcelableExtra(EXTRA_URI, android.net.Uri.class);
            }
            final int slot = intent.getIntExtra(EXTRA_SLOT, 0);
            sendFileFromUri(uri, slot);
            return START_STICKY;
        }
        if (ACTION_FILE_CMD.equals(action) && intent != null) {
            final int slot = intent.getIntExtra(EXTRA_SLOT, 0);
            final int cmd = intent.getIntExtra(EXTRA_FILE_CMD, Frames.FILE_CMD_LIST);
            runFileCmd(cmd, slot);
            return START_STICKY;
        }
        if (ACTION_MOUNT_NEXT.equals(action)) {
            // 与设备端按键同语义：序号自增、4 个槽位轮转。
            // 计数器放在服务里而非界面里，界面重建（转屏/回前台）不会把序号清零。
            final int idx = mountIndex;
            final int slot = mountIndex % 4;
            mountIndex = mountIndex + 1;
            onResourceRequest(slot, idx);
            return START_STICKY;
        }
        if (ACTION_SETTINGS.equals(action)) {
            applyPrefs();
            return START_STICKY;
        }
        if (ACTION_RECONNECT.equals(action)) {
            reconnectNow();
            return START_STICKY;
        }

        // 默认：启动连接
        applyPrefs();
        if (!ble.isReady()) {
            reconnectNow();
        }
        return START_STICKY;
    }

    @Override
    public void onDestroy() {
        main.removeCallbacksAndMessages(null);
        stopTimeTick();
        if (motion != null) {
            motion.stop();
        }
        if (location != null) {
            location.stop();
        }
        if (mirror != null) {
            mirror.stop();
            mirror = null;
        }
        projection = null;
        if (ble != null) {
            ble.release();
        }
        setStatus("已停止", "服务已关闭");
        super.onDestroy();
    }

    @Nullable
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    // ---- 开关 ---------------------------------------------------------------

    private void applyPrefs() {
        if (prefs.timeEnabled()) {
            startTimeTick();
            sendTime();
        } else {
            stopTimeTick();
        }

        if (prefs.motionEnabled()) {
            motion.start();
            log("运动传感器：" + motion.describe());
        } else {
            motion.stop();
        }

        if (prefs.locationEnabled()) {
            String err = location.start();
            if (err != null) {
                log("定位未启动：" + err);
                setStatus(stateText, err);
            } else {
                log("GPS 已启动");
            }
        } else {
            location.stop();
        }
    }

    private void startTimeTick() {
        if (timeTickRunning) {
            return;
        }
        timeTickRunning = true;
        main.postDelayed(timeTick, 60_000);
    }

    private void stopTimeTick() {
        timeTickRunning = false;
        main.removeCallbacks(timeTick);
    }

    // ---- BLE ----------------------------------------------------------------

    private void reconnectNow() {
        String addr = prefs.deviceAddress();
        if (addr == null || addr.isEmpty()) {
            ble.scan("WhaleBT-", 12_000);
            return;
        }
        try {
            android.bluetooth.BluetoothManager bm =
                    (android.bluetooth.BluetoothManager) getSystemService(Context.BLUETOOTH_SERVICE);
            if (bm == null || bm.getAdapter() == null) {
                setStatus("蓝牙不可用", "本机没有蓝牙适配器");
                return;
            }
            android.bluetooth.BluetoothDevice dev = bm.getAdapter().getRemoteDevice(addr);
            ble.connect(dev);
        } catch (SecurityException e) {
            setStatus("缺少蓝牙权限", "请回到 App 授权");
        } catch (Exception e) {
            log("重连失败：" + e);
            ble.scan("WhaleBT-", 12_000);
        }
    }

    @Override
    public void onState(String state, String detail) {
        setStatus(state, detail);
        if (state.contains("已就绪")) {
            reconnectAttempts = 0;
        }
    }

    @Override
    public void onReady(int mtu) {
        log("链路就绪，MTU=" + mtu);
        prefs.setDeviceAddress(ble.deviceAddress());
        prefs.setDeviceName(ble.deviceName());
        sendTime();
        applyPrefs();
        if (prefs.mirrorEnabled() && mirror == null && projection == null) {
            sNeedProjection = true;
            log("投屏开关是开的，但屏幕录制授权已失效，请回 App 重新授权");
        }
    }

    @Override
    public void onDisconnected() {
        stopMirror();
        if (reconnectAttempts < 1000) {
            reconnectAttempts++;
        }
        long delay = Math.min(30_000, 2000L * reconnectAttempts);
        log("断开，将在 " + delay / 1000 + " 秒后重连");
        main.postDelayed(this::reconnectNow, delay);
    }

    @Override
    public void onLog(String line) {
        log(line);
    }

    // ---- 按需挂载：设备要一张资源 -------------------------------------------

    /**
     * 设备长按 OK 后会发来「要第 N 号资源，写到槽位 S」。
     *
     * 这里的动作顺序很关键：
     *   1. 先扫相册拿清单（不读图，几千张全解码会 OOM）
     *   2. 按 index 取第 N 张，解码 + 等比缩到 120×160 + 转 RGB565
     *   3. 拼 12 字节 WHLP 头，用已有的 sendFile 推到设备槽位
     * 只有第 3 步走 BLE，前两步都在本地完成 —— 这正是「手机当资源库」
     * 的意义：把解码和缩放这些重活留在有 CPU 的一侧。
     */
    @Override
    public void onResourceRequest(int slot, int index) {
        if (ble == null || !ble.isReady()) {
            log("设备要资源，但链路未就绪");
            return;
        }
        if (fileSending) {
            log("上一次传输还没结束，忽略本次资源请求");
            return;
        }
        // 单独开线程：扫相册 + 解码可能上百毫秒，绝不能占主线程
        new Thread(() -> {
            try {
                if (library.size() == 0) {
                    int n = library.refresh();
                    if (n == -2) {
                        // 之前这里和「相册空」共用一句话，导致权限问题被当成
                        // 「手机里没照片」，方向完全跑偏。必须分开说。
                        log("没有相册权限：回 App 点「要下一张照片」重新授权，"
                                + "或到系统设置里给 whale-link 打开「照片和视频」");
                        return;
                    }
                    if (n < 0) {
                        log("扫相册失败，稍后再试");
                        return;
                    }
                    if (n == 0) {
                        log("相册里没有照片（相机胶卷为空？）");
                        return;
                    }
                    log("相册已加载 " + n + " 张");
                }
                ResourceLibrary.Item item = library.get(index);
                if (item == null) {
                    log("取不到第 " + index + " 张");
                    return;
                }
                byte[] pkg = library.build(item);
                if (pkg == null) {
                    log("转码失败：" + item.name);
                    return;
                }
                if (pkg.length > 1024 * 1024) {
                    log("转码后 " + pkg.length / 1024 + "KB 超过槽位上限");
                    return;
                }

                fileSending = true;
                log("挂载 #" + index + " " + item.name + " (" + pkg.length / 1024
                        + "KB) → 槽位 " + slot);
                long budget = 2000 + (pkg.length / 400 + 32) * 40L;
                final boolean ok = ble.sendFile(pkg, "RES" + (index % 1000), slot, budget);
                if (!ok) {
                    log("资源发送失败：" + ble.lastTxError());
                }
            } catch (Exception e) {
                log("资源处理异常：" + e);
            } finally {
                fileSending = false;
            }
        }, "res-mount").start();
    }

    // ---- 数据源 -------------------------------------------------------------

    private void sendTime() {
        if (ble == null || !ble.isReady()) {
            return;
        }
        // 节流：连上时 onReady 和 applyPrefs 会各调一次，开关切换也会再调，
        // 实测能到每秒 2~3 次。每一次写入都要占一个连接事件，投屏时就是净损耗。
        long nowMs = SystemClock.elapsedRealtime();
        if (nowMs - lastTimeSentMs < 2000) {
            return;
        }
        lastTimeSentMs = nowMs;

        long now = System.currentTimeMillis();
        int tz = TimeZone.getDefault().getOffset(now) / 60000;
        ble.send(Frames.time(now / 1000, tz));
        lastTimeSync = nowMs;

        SimpleDateFormat fmt = new SimpleDateFormat("MM-dd HH:mm:ss", Locale.US);
        log("已发送时间 " + fmt.format(new Date(now)) + " (UTC" + (tz >= 0 ? "+" : "") + tz / 60 + ")");
    }

    @Override
    public void onSensor(int kind, int seq, int v0, int v1, int v2, int aux) {
        if (ble == null || !ble.isReady()) {
            return;
        }
        long t = SystemClock.elapsedRealtime() & 0xFFFFFFFFL;
        ble.send(Frames.sensor(kind, seq, t, v0, v1, v2, aux));
    }

    // ---- 投屏 ---------------------------------------------------------------

    private void startMirror(int resultCode, Intent data) {
        if (data == null || resultCode == 0) {
            log("投屏授权失败");
            prefs.setMirrorEnabled(false);
            return;
        }
        // Android 14 要求：进入 mediaProjection 类型前台服务之后才能取 projection
        startForegroundCompat(true);

        try {
            MediaProjectionManager mpm =
                    (MediaProjectionManager) getSystemService(Context.MEDIA_PROJECTION_SERVICE);
            projection = mpm.getMediaProjection(resultCode, data);
        } catch (Exception e) {
            log("获取投屏会话失败：" + e);
            projection = null;
        }
        if (projection == null) {
            prefs.setMirrorEnabled(false);
            startForegroundCompat(false);
            return;
        }

        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                main.post(() -> {
                    log("投屏被系统停止");
                    stopMirror();
                });
            }
        }, main);

        mirror = new MirrorSource(this, prefs, projection, this);
        if (!mirror.start()) {
            // 授权过期 / 建不出虚拟屏。别装作开着了，关掉开关并让界面重新申请。
            log("投屏启动失败（授权可能已过期），请在 App 里重新打开投屏");
            mirror = null;
            projection = null;
            prefs.setMirrorEnabled(false);
            sMirroring = false;
            sNeedProjection = false;
            setStatus("投屏未启动", "请重新打开投屏开关授权");
            if (foregroundStarted) {
                startForegroundCompat(false);
            }
            return;
        }
        sMirroring = true;
        sNeedProjection = false;
        int[] wh = prefs.resolution();
        setStatus("已就绪", "投屏 " + wh[0] + "×" + wh[1]);
        log("投屏开始，" + wh[0] + "×" + wh[1]);
    }

    private void stopMirror() {
        if (mirror != null) {
            mirror.stop();
            mirror = null;
        }
        sMirroring = false;
        projection = null;
        prefs.setMirrorEnabled(false);
        if (foregroundStarted) {
            startForegroundCompat(false);
        }
    }

    @Override
    public boolean writeAll(java.util.List<byte[]> blocks, long timeoutMs) {
        return ble != null && ble.sendFrameBlocks(blocks, timeoutMs);
    }

    @Override
    public int mtu() {
        return ble != null ? ble.mtu() : 23;
    }

    // ---- 文件 → 设备 Flash ---------------------------------------------------

    /** 正在发送文件（防止两次发送撞在同一条 BLE 队列上）。 */
    private volatile boolean fileSending;

    /** 「按需挂载」的下一个序号。跨界面重建保留，语义与设备端按键一致。 */
    private int mountIndex;

    private void sendFileFromUri(android.net.Uri uri, int slot) {
        if (uri == null) {
            log("文件 URI 为空");
            return;
        }
        if (fileSending) {
            log("上一次发送还没结束，稍等");
            return;
        }
        if (ble == null || !ble.isReady()) {
            log("设备未连接，无法发送文件");
            return;
        }
        fileSending = true;
        // 读文件 + BLE 分块都是耗时操作：读 1MB 内容、空口传几十秒，
        // 绝不能占主线程，也不能占 tx 线程之外的 GATT 调用线程。
        new Thread(() -> {
            try {
                final String name = sanitizeName(uri);
                // 选中的是图片就按「资源」处理：解码 + 等比缩到 120×160 + 转 RGB565
                // + 拼 WHLP 头。直接推 JPEG 原始字节在设备上毫无意义 —— 固件里没有
                // JPEG 解码器，mmap 当图像源读时 magic 也对不上，只会得到花屏。
                // 这条路径同时是「相册权限拿不到」时的兜底：SAF 选图不需要任何权限。
                final String mime = getContentResolver().getType(uri);
                final boolean isImage = mime != null && mime.startsWith("image/");
                byte[] data;
                if (isImage) {
                    data = library.build(uri);
                    if (data == null) {
                        log("图片转码失败：" + name);
                        return;
                    }
                    log("已转成设备可显示格式 " + data.length / 1024 + "KB");
                } else {
                    try (java.io.InputStream in = getContentResolver().openInputStream(uri)) {
                        if (in == null) {
                            log("打不开所选文件");
                            return;
                        }
                        data = readAll(in);
                    } catch (Exception e) {
                        log("读文件失败：" + e);
                        return;
                    }
                }
                if (data.length > 1024 * 1024) {
                    log("文件 " + data.length / 1024 + " KB 超过槽位上限 1024 KB");
                    return;
                }
                log("开始发送 " + name + " (" + data.length + " B) → 槽位 " + slot);
                // 1MB ≈ 2100 块（净荷 494），空口按块数给足时间：每块 40ms 预算。
                long budget = 2000 + (data.length / 400 + 32) * 40L;
                final boolean ok = ble.sendFile(data, name, slot, budget);
                if (!ok) {
                    log("发送失败：" + ble.lastTxError());
                }
                // 成功与否的最终结论以设备 notify 的 status=3/4 为准（回读校验）。
            } finally {
                fileSending = false;
            }
        }, "file-send").start();
    }

    private static byte[] readAll(java.io.InputStream in) throws java.io.IOException {
        java.io.ByteArrayOutputStream bos = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[64 * 1024];
        int n;
        while ((n = in.read(buf)) > 0) {
            bos.write(buf, 0, n);
        }
        return bos.toByteArray();
    }

    /** 协议只给 12 字节文件名（ASCII）。取 Uri 最后一段，去非 ASCII，截 12。 */
    private String sanitizeName(android.net.Uri uri) {
        String last = uri.getLastPathSegment();
        if (last == null) {
            last = "file.bin";
        }
        int slash = last.lastIndexOf('/');
        if (slash >= 0) {
            last = last.substring(slash + 1);
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < last.length() && sb.length() < Frames.FILE_NAME_LEN; i++) {
            char c = last.charAt(i);
            if (c >= 0x20 && c < 0x7F) {
                sb.append(c);
            }
        }
        return sb.length() == 0 ? "file.bin" : sb.toString();
    }

    private void runFileCmd(int cmd, int slot) {
        if (ble == null || !ble.isReady()) {
            log("设备未连接");
            return;
        }
        if (fileSending) {
            log("正在发送文件，稍后再操作槽位");
            return;
        }
        new Thread(() -> {
            boolean ok = ble.fileCmd(cmd, slot);
            if (!ok) {
                log("命令发送失败：" + ble.lastTxError());
            }
            // LIST 的结果经 notify(8 逐条 + 13 结束)，DELETE=9，VERIFY=3/4，
            // COMPACT=11/12。
        }, "file-cmd").start();
    }

    /** 把字节数说成人话：存储池大小在几 MB 量级，报原始字节数没人看得懂。 */
    private static String humanSize(long bytes) {
        if (bytes >= 1024L * 1024L) {
            return String.format(Locale.US, "%.1f MB", bytes / 1024.0 / 1024.0);
        }
        if (bytes >= 1024) {
            return String.format(Locale.US, "%d KB", bytes / 1024);
        }
        return bytes + " B";
    }

    /** 设备 notify 回报（主线程回调）。status 语义见 Frames.FILE_ST_*。 */
    private void onFileNotify(int status, int slot, long written, long crc,
                              long totalLen, String name) {
        switch (status) {
            case Frames.FILE_ST_BEGIN_ACK:
                log("设备接受传输：条目 " + slot + "，共 " + humanSize(totalLen));
                break;
            case Frames.FILE_ST_PROGRESS:
                pushStats(String.format(Locale.US, "写入中 %d / %d B (%d%%)",
                        written, totalLen, totalLen > 0 ? written * 100 / totalLen : 0));
                break;
            case Frames.FILE_ST_DONE_OK:
                log("✓ 条目 " + slot + " 写入完成，回读 CRC 校验通过");
                pushStats("文件已存入条目 " + slot);
                break;
            case Frames.FILE_ST_DONE_BAD_CRC:
                log("✗ 条目 " + slot + " CRC 校验失败（设备读回与发送不符）");
                break;
            case Frames.FILE_ST_REJECT:
                log("✗ 设备拒绝：条目/长度非法，或池子放不下（可点「整理空间」）");
                break;
            case Frames.FILE_ST_SEQ_ERR:
                log("✗ 序号断裂，传输作废（已写 " + written + " B）");
                break;
            case Frames.FILE_ST_FLASH_ERR:
                log("✗ 设备写 Flash 失败（已写 " + written + " B）");
                break;
            case Frames.FILE_ST_SLOT_INFO:
                log(String.format(Locale.US, "条目 %d：%s %s", slot, name,
                        humanSize(totalLen)));
                break;
            case Frames.FILE_ST_DELETED:
                log("条目 " + slot + " 已删除");
                break;
            case Frames.FILE_ST_SLOT_EMPTY:
                log("条目 " + slot + "：空（没有文件）");
                break;
            case Frames.FILE_ST_LIST_END:
                // slot 字段复用成条目数，totalLen 复用成最大可分配空闲字节
                log(String.format(Locale.US, "列表结束：共 %d 项，还可放入 %s 的单个文件",
                        slot, humanSize(totalLen)));
                pushStats("设备存储 " + slot + " 项 / 空闲 " + humanSize(totalLen));
                break;
            case Frames.FILE_ST_COMPACT_OK:
                log("✓ 碎片整理完成，当前最大可用 " + humanSize(totalLen));
                pushStats("整理完成，空闲 " + humanSize(totalLen));
                break;
            case Frames.FILE_ST_COMPACT_ERR:
                log("✗ 整理失败（可能还有图片在设备上显示着，先收起再试）");
                break;
            default:
                log("文件状态 status=" + status + " slot=" + slot + " written=" + written);
                break;
        }
    }

    // ---- 通知 ---------------------------------------------------------------

    private void createChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        NotificationChannel ch = new NotificationChannel(
                CHANNEL, getString(R.string.notif_channel_name), NotificationManager.IMPORTANCE_LOW);
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);
    }

    private Notification buildNotification() {
        Intent open = new Intent(this, MainActivity.class);
        PendingIntent pi = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        Intent stop = new Intent(this, LinkService.class).setAction(ACTION_STOP);
        PendingIntent stopPi = PendingIntent.getService(this, 1, stop,
                PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

        return new NotificationCompat.Builder(this, CHANNEL)
                .setSmallIcon(R.drawable.ic_notification)
                .setContentTitle(getString(R.string.notif_title))
                .setContentText(stateText + (detailText.isEmpty() ? "" : " · " + detailText))
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setContentIntent(pi)
                .addAction(0, "停止", stopPi)
                .build();
    }

    private void startForegroundCompat(boolean withProjection) {
        Notification n = buildNotification();

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            int types = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE;
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.ACCESS_FINE_LOCATION)
                    == PackageManager.PERMISSION_GRANTED) {
                types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION;
            }
            if (withProjection) {
                types |= ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION;
            }
            try {
                startForeground(NOTIF_ID, n, types);
                foregroundStarted = true;
                return;
            } catch (Exception e) {
                Log.w(TAG, "带类型的前台服务启动失败，退回普通方式：" + e);
            }
        }
        startForeground(NOTIF_ID, n);
        foregroundStarted = true;
    }

    // ---- 状态与日志 ---------------------------------------------------------

    private void setStatus(String state, String detail) {
        stateText = state;
        detailText = detail == null ? "" : detail;
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (foregroundStarted) {
            try {
                nm.notify(NOTIF_ID, buildNotification());
            } catch (Exception ignored) {
            }
        }
        main.post(() -> {
            Ui u = sUi.get();
            if (u != null) {
                u.onStatus(stateText, detailText);
            }
        });
    }

    private void pushStats(String s) {
        main.post(() -> {
            Ui u = sUi.get();
            if (u != null) {
                u.onStats(s);
            }
        });
    }

    private void log(String line) {
        final String stamped = new SimpleDateFormat("HH:mm:ss", Locale.US).format(new Date()) + "  " + line;
        Log.i(TAG, stamped);
        synchronized (logs) {
            logs.addLast(stamped);
            while (logs.size() > 40) {
                logs.removeFirst();
            }
        }
        main.post(() -> {
            Ui u = sUi.get();
            if (u != null) {
                u.onLog(stamped);
            }
        });
    }
}
