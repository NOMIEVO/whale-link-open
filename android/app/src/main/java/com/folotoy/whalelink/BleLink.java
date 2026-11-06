package com.folotoy.whalelink;

import android.Manifest;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Log;

import androidx.core.content.ContextCompat;

import java.util.List;

import java.util.UUID;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * 与挂件的 BLE 连接。设备是 GATT server，手机是 central —— 和推余额的
 * 方向一致，只是多写几个特征。
 *
 * 所有写操作都排到单独的 tx 线程串行执行：Android 的 writeCharacteristic()
 * 在上一次写没完成时会直接返回 false，多线程并发写必然丢包。
 */
public class BleLink {

    private static final String TAG = "WhaleBle";

    public static final UUID SVC = UUID.fromString("4f7c0001-1e2d-4a5b-8c3f-9d0e1f2a3b4c");
    public static final UUID CHR_BALANCE = UUID.fromString("4f7c0002-1e2d-4a5b-8c3f-9d0e1f2a3b4c");
    public static final UUID CHR_REQUEST = UUID.fromString("4f7c0003-1e2d-4a5b-8c3f-9d0e1f2a3b4c");
    public static final UUID CHR_TIME = UUID.fromString("4f7c0004-1e2d-4a5b-8c3f-9d0e1f2a3b4c");
    public static final UUID CHR_SENSOR = UUID.fromString("4f7c0005-1e2d-4a5b-8c3f-9d0e1f2a3b4c");
    public static final UUID CHR_FRAME = UUID.fromString("4f7c0006-1e2d-4a5b-8c3f-9d0e1f2a3b4c");
    public static final UUID CHR_FILE = UUID.fromString("4f7c0007-1e2d-4a5b-8c3f-9d0e1f2a3b4c");

    public interface Listener {
        void onState(String state, String detail);

        void onReady(int mtu);

        void onDisconnected();

        void onLog(String line);

        /**
         * 设备主动要一张资源（长按 OK 触发）。
         *
         * 运行在主线程，实现里可以直接碰 UI，但读相册/解码要自己开线程 ——
         * 这条回调一堵，GATT 回调线程就跟着堵。
         *
         * @param slot  设备希望写进哪个 Flash 槽位
         * @param index 资源序号，设备不解释含义，由手机映射到相册第 N 张
         */
        void onResourceRequest(int slot, int index);
    }

    private final Context ctx;
    private final Listener listener;
    private final Handler mainHandler = new Handler(android.os.Looper.getMainLooper());

    private final HandlerThread txThread;
    private final Handler txHandler;

    private BluetoothAdapter adapter;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic chrTime;
    private BluetoothGattCharacteristic chrSensor;
    private BluetoothGattCharacteristic chrFrame;
    private BluetoothGattCharacteristic chrFile;
    private BluetoothGattCharacteristic chrRequest;

    private volatile boolean ready;
    private volatile int mtu = 23;
    private volatile boolean manualDisconnect;
    /** 旧 GATT 缓存刷新尝试次数（每次连接最多 2 次，防止死循环）。 */
    private int gattRefreshAttempts;
    /** 本连接是否已订阅过 File CCCD（刷新重发现服务会再走一遍 MTU 流程）。 */
    private volatile boolean fileCccdSubscribed;
    /**
     * CCCD 订阅队列。
     *
     * Android 的 GATT 客户端同一时刻只受理一个描述符写：连着写第二个会直接被
     * 判 busy（writeDescriptor 返回 false），而且不会重试。之前只订阅 File 时
     * 这个坑不明显；加了 Request 之后两个订阅连着发，Request 那条必然被吞掉，
     * 表现就是「设备按 OK 毫无反应」—— 设备 notify 没人订阅，NimBLE 直接回错。
     * 所以改成队列 + onDescriptorWrite 驱动，一条完成再发下一条。
     */
    private final java.util.ArrayDeque<BluetoothGattCharacteristic> cccdQueue =
            new java.util.ArrayDeque<>();
    /** 当前有一条 CCCD 写正在飞，等 onDescriptorWrite 回收。 */
    private boolean cccdBusy;
    /** 本连接是否已订阅过 Request CCCD（设备 → 手机的请求通道）。 */
    private volatile boolean requestCccdSubscribed;
    /** 最近一次投屏写失败的原因，给界面显示 —— 0 fps 时用户得知道卡在哪一层。 */
    private volatile String lastTxError = "";

    public String lastTxError() {
        return lastTxError;
    }
    private String deviceName = "";
    private volatile BluetoothDevice currentDevice;

    public BleLink(Context ctx, Listener listener) {
        this.ctx = ctx.getApplicationContext();
        this.listener = listener;
        BluetoothManager bm = (BluetoothManager) this.ctx.getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = bm != null ? bm.getAdapter() : null;
        txThread = new HandlerThread("ble-tx");
        txThread.start();
        txHandler = new Handler(txThread.getLooper());
    }

    public boolean isReady() {
        return ready;
    }

    public int mtu() {
        return mtu;
    }

    public String deviceName() {
        return deviceName;
    }

    public String deviceAddress() {
        BluetoothDevice d = currentDevice;
        if (d == null) {
            return "";
        }
        try {
            return d.getAddress();
        } catch (SecurityException e) {
            return "";
        }
    }

    private boolean hasPerm(String p) {
        return ContextCompat.checkSelfPermission(ctx, p) == PackageManager.PERMISSION_GRANTED;
    }

    private boolean canScan() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return hasPerm(Manifest.permission.BLUETOOTH_SCAN) && hasPerm(Manifest.permission.BLUETOOTH_CONNECT);
        }
        return hasPerm(Manifest.permission.ACCESS_FINE_LOCATION);
    }

    // ---- 扫描 ---------------------------------------------------------------

    public void scan(String namePrefix, long timeoutMs) {
        if (adapter == null || !adapter.isEnabled()) {
            post(() -> listener.onState("蓝牙未开启", "请在系统设置里打开蓝牙"));
            return;
        }
        if (!canScan()) {
            post(() -> listener.onState("缺少蓝牙权限", "请回到 App 授予权限"));
            return;
        }

        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) {
            post(() -> listener.onState("无法开始扫描", "扫描器不可用"));
            return;
        }

        // 不再用 setServiceUuid 过滤。
        //
        // 设备那边的 128 位 UUID 只能放在 scan response 里（广播包 31 字节装不下
        // flags + 名字 + UUID128，塞进广播会让设备直接不广播）。而 ScanFilter 的
        // serviceUuid 匹配依赖系统把 adv 与 scan response 合并后的结果，个别机型/
        // 系统版本上并不稳定 —— 表现就是"设备在广播，App 却一直搜不到"。
        // 名字在广播包里，最可靠；扫描窗口本来就短（十几秒），不过滤的功耗可以忽略。
        ScanSettings settings = new ScanSettings.Builder()
                .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                .build();

        post(() -> listener.onState("正在扫描", "找名字以 " + namePrefix + " 开头的设备"));

        try {
            scanner.startScan(null, settings, scanCallback);
        } catch (SecurityException e) {
            post(() -> listener.onState("扫描被拒绝", "权限不足"));
            return;
        }

        mainHandler.postDelayed(this::stopScan, timeoutMs);
    }

    public void stopScan() {
        if (scanner != null && canScan()) {
            try {
                scanner.stopScan(scanCallback);
            } catch (Exception ignored) {
            }
        }
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, ScanResult result) {
            BluetoothDevice dev = result.getDevice();
            String found = dev.getName();
            if (found == null && result.getScanRecord() != null) {
                found = result.getScanRecord().getDeviceName();
            }
            if (found == null || !found.startsWith("WhaleBT-")) {
                return;
            }
            final String name = found;
            stopScan();
            deviceName = name;
            post(() -> listener.onLog("发现 " + name + " (" + dev.getAddress() + ")"));
            connect(dev);
        }

        @Override
        public void onScanFailed(int errorCode) {
            post(() -> listener.onState("扫描失败", "错误码 " + errorCode));
        }
    };

    // ---- 连接 ---------------------------------------------------------------

    public void connect(BluetoothDevice device) {
        if (!canScan()) {
            post(() -> listener.onState("缺少蓝牙权限", "请回到 App 授予权限"));
            return;
        }
        manualDisconnect = false;
        gattRefreshAttempts = 0;
        fileCccdSubscribed = false;
        requestCccdSubscribed = false;
        cccdBusy = false;
        cccdQueue.clear();
        currentDevice = device;
        post(() -> listener.onState("正在连接", device.getAddress()));
        try {
            gatt = device.connectGatt(ctx, false, gattCallback, BluetoothDevice.TRANSPORT_LE);
        } catch (SecurityException e) {
            post(() -> listener.onState("连接被拒绝", "权限不足"));
        }
    }

    public void disconnect() {
        manualDisconnect = true;
        ready = false;
        if (gatt != null) {
            try {
                gatt.disconnect();
                gatt.close();
            } catch (Exception ignored) {
            }
            gatt = null;
        }
    }

    private final BluetoothGattCallback gattCallback = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(BluetoothGatt g, int status, int newState) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                post(() -> listener.onState("已连接，正在发现服务", deviceName));
                try {
                    g.discoverServices();
                } catch (SecurityException ignored) {
                }
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                ready = false;
                post(() -> {
                    listener.onState("连接断开", manualDisconnect ? "已主动断开" : "会自动重连");
                    listener.onDisconnected();
                });
                try {
                    g.close();
                } catch (Exception ignored) {
                }
                if (!manualDisconnect) {
                    // 交给 Service 决定何时重连，避免在这里死循环
                }
            }
        }

        @Override
        public void onServicesDiscovered(BluetoothGatt g, int status) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                post(() -> listener.onState("服务发现失败", "status=" + status));
                return;
            }
            BluetoothGattService svc = g.getService(SVC);
            if (svc == null) {
                post(() -> listener.onState("不是挂件设备", "缺少约定的服务 UUID"));
                return;
            }
            chrTime = svc.getCharacteristic(CHR_TIME);
            chrSensor = svc.getCharacteristic(CHR_SENSOR);
            chrFrame = svc.getCharacteristic(CHR_FRAME);
            chrFile = svc.getCharacteristic(CHR_FILE);
            chrRequest = svc.getCharacteristic(CHR_REQUEST);

            // 固件加了 File 特征之后，手机系统还在用旧的 GATT 属性表缓存 ——
            // 服务能找到，新特征却拿不到。这不是固件问题：调 refresh() 清掉
            // 缓存再重新发现一次服务就有了；refresh 失败时提示用户关开蓝牙。
            if (chrFile == null && gattRefreshAttempts < 2) {
                gattRefreshAttempts++;
                if (refreshGattCache(g)) {
                    post(() -> listener.onLog("检测到旧 GATT 缓存（缺 File 特征），已请求刷新"));
                    mainHandler.postDelayed(() -> {
                        try {
                            g.discoverServices();
                        } catch (SecurityException ignored) {
                        }
                    }, 600);
                    return;
                }
                post(() -> listener.onLog("缺 File 特征且无法自动刷新：请关开手机蓝牙；"
                        + "若配对过，到系统蓝牙设置里「忽略此设备」后重连"));
            }

            // 谈大 MTU：24 字节的传感器帧超过默认 23 字节的净荷
            try {
                g.requestMtu(517);
            } catch (SecurityException ignored) {
            }
        }

        @Override
        public void onMtuChanged(BluetoothGatt g, int newMtu, int status) {
            mtu = newMtu;
            // 2M PHY 由设备端在连接建立时主动请求（LL_PHY_REQ），这里不再插手。
            // 两边同时发起 PHY 更新会让安卓的 GATT 层进入 busy 状态一段时间，
            // 那期间 writeCharacteristic 一律返回 201，正好卡死第一帧。
            subscribeFileNotify();
            ready = true;
            post(() -> {
                listener.onState("已就绪", "MTU " + newMtu + "，可以发送数据");
                listener.onReady(newMtu);
            });
        }

        @Override
        public void onCharacteristicWrite(BluetoothGatt g, BluetoothGattCharacteristic c, int status) {
            // 带响应写的确认也走这里。status 不是 0 就说明这一块在空口/ATT 层
            // 就没被接受（133 超时、0x0D 长度、0x03 权限），比"写不进去"具体得多。
            if (status != BluetoothGatt.GATT_SUCCESS) {
                lastRc = status;
                lastTxError = "写确认失败 status=" + status;
                Log.w(TAG, "write status=" + status);
            }
        }

        @Override
        public void onDescriptorWrite(BluetoothGatt g,
                                      android.bluetooth.BluetoothGattDescriptor d, int status) {
            // CCCD 写的确认。这一步是订阅唯一可靠的验收点：writeDescriptor 返回
            // true 只代表"受理了"，真正写进对端才走到这里。
            cccdBusy = false;
            final String who = d != null && d.getCharacteristic() != null
                    ? (CHR_REQUEST.equals(d.getCharacteristic().getUuid()) ? "设备请求通道"
                       : CHR_FILE.equals(d.getCharacteristic().getUuid()) ? "文件状态回报"
                       : d.getCharacteristic().getUuid().toString())
                    : "?";
            if (status == BluetoothGatt.GATT_SUCCESS) {
                post(() -> listener.onLog("已订阅「" + who + "」"));
            } else {
                post(() -> listener.onLog("订阅「" + who + "」失败 status=" + status));
                // 失败就放回队列再试一次，避免一次 busy 直接断掉整条通道
                BluetoothGattCharacteristic c = d != null ? d.getCharacteristic() : null;
                if (c != null) {
                    txHandler.post(() -> {
                        cccdQueue.add(c);
                        pumpCccd();
                    });
                }
            }
            txHandler.post(() -> pumpCccd());
        }

        // ---- File 特征的 notify（设备 → 手机：进度 / 校验结果 / 槽位列表）----

        @Override
        public void onCharacteristicChanged(BluetoothGatt g,
                                            BluetoothGattCharacteristic c, byte[] data) {
            // API 33+ 的新签名，data 即特征值
            handleNotify(c, data);
        }

        @Override
        public void onCharacteristicChanged(BluetoothGatt g, BluetoothGattCharacteristic c) {
            // 老 API：值藏在特征里
            handleNotify(c, c.getValue());
        }
    };

    /** 28 字节定长小端：ver/status/slot/rsv/written/crc/total_len/name[12]。 */
    private void handleNotify(BluetoothGattCharacteristic c, byte[] data) {
        if (c == null || data == null) {
            return;
        }

        // Request 特征：设备主动要东西。载荷只有 1~4 字节，
        // 走下面那条 28 字节的文件分支会被整包丢掉。
        if (CHR_REQUEST.equals(c.getUuid())) {
            handleRequestNotify(data);
            return;
        }

        if (data.length < 28) {
            return;
        }
        if (!CHR_FILE.equals(c.getUuid())) {
            return;
        }
        final int status = data[1] & 0xFF;
        final int slot = data[2] & 0xFF;
        final long written = (data[4] & 0xFFL) | (data[5] & 0xFFL) << 8
                | (data[6] & 0xFFL) << 16 | (data[7] & 0xFFL) << 24;
        final long crc = (data[8] & 0xFFL) | (data[9] & 0xFFL) << 8
                | (data[10] & 0xFFL) << 16 | (data[11] & 0xFFL) << 24;
        final long totalLen = (data[12] & 0xFFL) | (data[13] & 0xFFL) << 8
                | (data[14] & 0xFFL) << 16 | (data[15] & 0xFFL) << 24;
        final FileListener fl = fileListener;
        if (fl != null) {
            int nul = 16;
            while (nul < 28 && data[nul] != 0) {
                nul++;
            }
            final String name = new String(data, 16, nul - 16,
                    java.nio.charset.StandardCharsets.US_ASCII);
            post(() -> {
                if (fileListener != null) {
                    fileListener.onFileNotify(status, slot, written, crc, totalLen, name);
                }
            });
        }
    }

    /**
     * 设备 → 手机的上行请求。
     *
     * 目前只有两种，都在 Request 特征上，靠第一字节区分：
     *   [0]=1                    请查一次余额
     *   [0]=2 [1]=slot [2..3]=idx 请送第 idx 号资源到槽位 slot（按需挂载）
     */
    private void handleRequestNotify(byte[] data) {
        if (data.length < 1) {
            return;
        }
        final int cmd = data[0] & 0xFF;
        if (cmd == 2) {
            if (data.length < 4) {
                return;
            }
            final int slot = data[1] & 0xFF;
            final int index = (data[2] & 0xFF) | ((data[3] & 0xFF) << 8);
            post(() -> {
                listener.onLog("设备请求资源 #" + index + " → 槽位 " + slot);
                listener.onResourceRequest(slot, index);
            });
            return;
        }
        // cmd=1（要余额）由 LinkService 自己处理，这里不重复触发。
    }

    /**
     * 清掉系统缓存的 GATT 属性表（隐藏 API refresh()）。
     *
     * Android 会把第一次连接时发现的 GATT 数据库按设备地址缓存起来，
     * 之后固件加了特征，手机拿到的还是旧表 —— 服务在、新特征永远 null。
     * refresh() 没有公开 API，只能反射调用；个别 ROM 上会被拦，
     * 失败就走兜底提示（关开手机蓝牙同样能清缓存）。
     */
    private boolean refreshGattCache(BluetoothGatt g) {
        try {
            java.lang.reflect.Method m = BluetoothGatt.class.getMethod("refresh");
            Object r = m.invoke(g);
            return r instanceof Boolean && (Boolean) r;
        } catch (Exception e) {
            Log.w(TAG, "refresh() 不可用：" + e);
            return false;
        }
    }

    /**
     * 建立两条 notify 订阅：File（收传输进度/校验结果）和 Request
     * （收设备发来的「给我送第 N 张」）。
     *
     * 两条都必须订：File 不订就不知道写没写成功；Request 不订，设备按 OK
     * 时 notify 会失败，用户看到的现象是「按了没反应」。
     */
    private void subscribeFileNotify() {
        if (!fileCccdSubscribed && chrFile == null && chrRequest == null) {
            // 特征还没拿到（服务发现没走完），留着标志位等重发现后再来
            post(() -> listener.onLog("特征表还没就绪，稍后重试订阅"));
            return;
        }
        if (!fileCccdSubscribed && chrFile != null) {
            fileCccdSubscribed = true;
            enqueueCccd(chrFile);
        }
        if (!requestCccdSubscribed && chrRequest != null) {
            requestCccdSubscribed = true;
            enqueueCccd(chrRequest);
        }
    }

    private void enqueueCccd(BluetoothGattCharacteristic c) {
        txHandler.post(() -> {
            cccdQueue.add(c);
            pumpCccd();
        });
    }

    /**
     * 发下一条 CCCD 写。必须等上一条的 onDescriptorWrite 回来才能发下一条，
     * 否则 GATT busy 会把后面那条静默丢弃。
     *
     * 任何一条失败（本地开关没打开 / 没描述符 / writeDescriptor 返回 false）
     * 都要继续处理队列里的下一条：否则卡住的往往是排在后面的那条 —— 也就是
     * 「设备请求通道」，症状正是设备按 OK 没反应。
     */
    private void pumpCccd() {
        while (true) {
            if (cccdBusy || gatt == null) {
                return;
            }
            BluetoothGattCharacteristic c = cccdQueue.poll();
            if (c == null) {
                return;
            }
            final String who = CHR_REQUEST.equals(c.getUuid()) ? "设备请求通道"
                    : CHR_FILE.equals(c.getUuid()) ? "文件状态回报" : c.getUuid().toString();

            if ((c.getProperties() & BluetoothGattCharacteristic.PROPERTY_NOTIFY) == 0) {
                post(() -> listener.onLog("「" + who + "」不支持 notify（固件过旧？）"));
                continue;
            }
            try {
                if (!gatt.setCharacteristicNotification(c, true)) {
                    post(() -> listener.onLog("订阅「" + who + "」失败：本地开关没打开"));
                    continue;
                }
                android.bluetooth.BluetoothGattDescriptor cccd =
                        c.getDescriptor(UUID.fromString("00002902-0000-1000-8000-00805f9b34fb"));
                if (cccd == null) {
                    post(() -> listener.onLog("「" + who + "」找不到 CCCD 描述符"));
                    continue;
                }
                boolean ok;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    ok = gatt.writeDescriptor(cccd,
                            android.bluetooth.BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
                            == BluetoothStatusCodes.SUCCESS;
                } else {
                    cccd.setValue(android.bluetooth.BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    ok = gatt.writeDescriptor(cccd);
                }
                if (ok) {
                    cccdBusy = true;      // 等 onDescriptorWrite 回收再发下一条
                } else {
                    post(() -> listener.onLog("订阅「" + who + "」未被受理，稍后重试"));
                }
            } catch (SecurityException e) {
                post(() -> listener.onLog("订阅「" + who + "」被权限拒绝"));
            }
            // 发出去了就等回调；没发出去（ok=false 或异常）继续循环试下一条
            if (cccdBusy) {
                return;
            }
        }
    }

    // ---- 文件传输 -----------------------------------------------------------

    /** 设备 → 手机的文件状态回报。回调在主线程。 */
    public interface FileListener {
        void onFileNotify(int status, int slot, long written, long crc, long totalLen, String name);
    }

    private volatile FileListener fileListener;

    public void setFileListener(FileListener l) {
        fileListener = l;
    }

    /**
     * 发送一个文件到指定槽位：BEGIN → DATA×N → END。
     * 同步等待完成（内部在 tx 线程跑，和投屏共用串行队列，互不并发）。
     * 返回 false 时 lastTxError() 有具体原因。
     */
    public boolean sendFile(byte[] data, String name, int slot, long timeoutMs) {
        if (!ready || gatt == null || chrFile == null) {
            // 走到这里基本只剩一种情况：系统缓存了旧 GATT 表，
            // 固件里的 File 特征手机看不到。refresh() 已在服务发现阶段试过，
            // 失败时只能靠用户关开蓝牙把缓存清掉。
            lastTxError = chrFile == null
                    ? "缺 File 特征：关开手机蓝牙后重连一次" : "链路未就绪";
            return false;
        }
        if (data == null || data.length == 0) {
            lastTxError = "文件为空";
            return false;
        }
        if (slot < 0 || slot > 3) {
            lastTxError = "槽位只能是 0–3";
            return false;
        }
        if (mtu < 64) {
            lastTxError = "MTU 只有 " + mtu + "，没协商成功";
            return false;
        }
        final java.util.zip.CRC32 crc = new java.util.zip.CRC32();
        crc.update(data);
        final int crcVal = (int) crc.getValue();
        // 每块净荷上限和投屏一致：MTU-3 的 ATT 净空，再留 4 字节余量
        // （个别 ROM 的栈会把边界卡得比标称值紧）。
        final int plenMax = Math.max(20, mtu - 3 - 16 - 4);
        final int dataBlocks = (data.length + plenMax - 1) / plenMax;
        final int totalBlocks = dataBlocks + 2;   // BEGIN + DATA×N + END

        final CountDownLatch latch = new CountDownLatch(1);
        final boolean[] result = new boolean[1];
        boolean posted = txHandler.post(() -> {
            try {
                result[0] = writeFileBlocks(data, name, slot, crcVal,
                        plenMax, totalBlocks, timeoutMs);
            } finally {
                latch.countDown();
            }
        });
        if (!posted) {
            lastTxError = "发送线程已退出";
            return false;
        }
        try {
            if (!latch.await(timeoutMs + 2000, TimeUnit.MILLISECONDS)) {
                lastTxError = "文件发送超时(" + totalBlocks + " 块)";
                return false;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        if (result[0]) {
            lastTxError = "";
        } else if (lastTxError.isEmpty()) {
            lastTxError = "文件发送失败";
        }
        return result[0];
    }

    private boolean writeFileBlocks(byte[] data, String name, int slot, int crcVal,
                                    int plenMax, int totalBlocks, long timeoutMs) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMs;
        final int dataBlocks = totalBlocks - 2;   // 总块数 = BEGIN + DATA×N + END

        // BEGIN：idx=0，净荷 = crc32 + 文件名(12B)
        byte[] beginPayload = Frames.fileBeginPayload(crcVal, name);
        if (!writeFilePacket(Frames.fileHeader(Frames.FILE_CMD_BEGIN, slot,
                        data.length, 0, totalBlocks, beginPayload.length), beginPayload, deadline)) {
            lastTxError = "BEGIN 发送失败 " + rcName(lastRc);
            return false;
        }

        // DATA：idx=1..dataBlocks
        for (int i = 0; i < dataBlocks; i++) {
            if (!ready || gatt == null) {
                lastTxError = "发到第 " + i + " 块时链路断了";
                return false;
            }
            int off = i * plenMax;
            int len = Math.min(plenMax, data.length - off);
            byte[] chunk = java.util.Arrays.copyOfRange(data, off, off + len);
            if (!writeFilePacket(Frames.fileHeader(Frames.FILE_CMD_DATA, slot,
                            data.length, i + 1, totalBlocks, len), chunk, deadline)) {
                lastTxError = "第 " + i + "/" + dataBlocks + " 块失败 "
                        + rcName(lastRc) + " mtu=" + mtu;
                return false;
            }
        }

        // END：idx=total-1，净荷为空（但固件要求包长 > 16，补 1 字节占位）
        if (!writeFilePacket(Frames.fileHeader(Frames.FILE_CMD_END, slot,
                        data.length, totalBlocks - 1, totalBlocks, 0),
                new byte[1], deadline)) {
            lastTxError = "END 发送失败 " + rcName(lastRc);
            return false;
        }
        return true;
    }

    /** 一块 = 16 字节头 + 净荷，整体走一次带响应写（和投屏同一个 writeUntil）。 */
    private boolean writeFilePacket(byte[] header, byte[] payload, long deadline) {
        byte[] packet = new byte[header.length + payload.length];
        System.arraycopy(header, 0, packet, 0, header.length);
        System.arraycopy(payload, 0, packet, header.length, payload.length);
        return writeUntil(chrFile, packet, deadline,
                BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
    }

    /** 槽位管理命令：LIST(4) / DELETE(5) / VERIFY(6)。固件要求包长 > 16，带 1 字节占位。 */
    public boolean fileCmd(int cmd, int slot) {
        if (!ready || gatt == null || chrFile == null) {
            lastTxError = "链路未就绪";
            return false;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final boolean[] result = new boolean[1];
        boolean posted = txHandler.post(() -> {
            try {
                byte[] header = Frames.fileHeader(cmd, slot, 0, 0, 0, 1);
                result[0] = writeUntil(chrFile,
                        java.nio.ByteBuffer.allocate(17)
                                .order(java.nio.ByteOrder.LITTLE_ENDIAN)
                                .put(header).put((byte) 0).array(),
                        SystemClock.elapsedRealtime() + 5000,
                        BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            } finally {
                latch.countDown();
            }
        });
        if (!posted) {
            return false;
        }
        try {
            latch.await(6000, TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        return result[0];
    }

    // ---- 发送 ---------------------------------------------------------------

    /**
     * 异步入队。投屏帧量大，调用方要自己控制节奏（上一帧没发完就丢帧）。
     */
    public void send(byte[] data) {
        txHandler.post(() -> writeRaw(data));
    }

    /**
     * 投屏帧专用：把整帧的所有块一次性交进 tx 线程，跨线程只有一次。
     *
     * 之前是每块调一次 sendBlocking，74 块就是 74 次 post + 74 次 latch 等待，
     * 这部分排队开销比空口传输本身还大。改成整帧一次排队后，tx 线程里连续写，
     * 只在控制器队列满（writeCharacteristic 返回 false）时让出 1 ms 再试。
     *
     * 固定走 Frame 特征，不按长度猜特征 —— 之前靠长度区分 time(8)/sensor(24)，
     * 投屏包正好 24 字节（头 16 + 末尾块 8）时会被误发到 sensor 特征，整帧就残了。
     */
    public boolean sendFrameBlocks(List<byte[]> blocks, long timeoutMs) {
        if (!ready) {
            lastTxError = "链路未就绪";
            return false;
        }
        if (gatt == null) {
            lastTxError = "Gatt 为空";
            return false;
        }
        if (chrFrame == null) {
            // 服务里找不到 Frame 特征。多半是系统缓存了旧的 GATT 数据库
            // （固件加过特征之后没清缓存），或者 UUID 对不上。
            lastTxError = "缺 Frame 特征(可试重启蓝牙)";
            return false;
        }
        if (blocks == null || blocks.isEmpty()) {
            lastTxError = "帧为空";
            return false;
        }
        if (mtu < 64) {
            // MTU 没谈成时每块只有 4 字节净荷，一帧要拆几百块，空口根本来不及。
            // 与其让用户看一个 0 fps，不如直接说是 MTU 的问题。
            lastTxError = "MTU 只有 " + mtu + "，没协商成功";
            return false;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final boolean[] result = new boolean[1];
        boolean posted = txHandler.post(() -> {
            try {
                result[0] = writeBlocks(blocks, timeoutMs);
            } finally {
                latch.countDown();
            }
        });
        if (!posted) {
            lastTxError = "发送线程已退出";
            return false;
        }
        try {
            // 比内部 deadline 多留一点余量，保证能拿到真实结果而不是超时误判。
            if (!latch.await(timeoutMs + 800, TimeUnit.MILLISECONDS)) {
                lastTxError = "写阻塞超时(" + blocks.size() + " 块)";
                return false;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        if (!result[0] && lastTxError.isEmpty()) {
            lastTxError = "写失败(" + blocks.size() + " 块)";
        }
        if (result[0]) {
            lastTxError = "";
        }
        return result[0];
    }

    private boolean writeBlocks(List<byte[]> blocks, long timeoutMs) {
        final long deadline = SystemClock.elapsedRealtime() + timeoutMs;
        int sent = 0;
        for (byte[] b : blocks) {
            if (!ready || gatt == null) {
                lastTxError = "写到第 " + sent + " 块时链路断了";
                return false;
            }
            if (!writeAny(chrFrame, b, deadline)) {
                // 把 GATT 返回码一起写出来。没有这个数字，现场只能靠猜：
                // 201=控制器忙(队列满)、3=属性没写权限(多半是系统缓存了旧
                // GATT 表)、6=长度超过 MTU-3。
                lastTxError = "第 " + sent + "/" + blocks.size() + " 块失败 "
                        + rcName(lastRc) + " mtu=" + mtu;
                return false;
            }
            sent++;
        }
        return true;
    }

    /**
     * 写一块：首选 write-no-response（不占回程带宽），失败就退回带响应写。
     *
     * 退回这一步不是多余 —— 安卓会把第一次配对时的 GATT 特征属性缓存在系统里，
     * 之后固件改了特征 flags，系统仍然按旧的来：本地直接拒掉 no-response 写，
     * 一块都发不出去，而带响应写反而是通的。11 块多等的那些 ACK 也就几十毫秒。
     */
    private boolean writeAny(BluetoothGattCharacteristic c, byte[] data, long deadlineMs) {
        final int props = c.getProperties();
        final boolean rsp = (props & BluetoothGattCharacteristic.PROPERTY_WRITE) != 0;

        // 投屏块优先用「带响应写」：每块都有 ATT 确认，安卓这层的排队和流控
        // 才可预测。一帧 11 块、每块等一个连接间隔（15 ms）也就 165 ms，
        // 根本用不上 no-response 那点回程带宽 —— 而 no-response 一旦撞上
        // 控制器队列满，writeCharacteristic 会直接阻塞或长期返回 201，
        // 表现就是"第 0 块写不进去"，一帧都发不出去。
        if (rsp) {
            return writeUntil(c, data, deadlineMs, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
        }
        return writeUntil(c, data, deadlineMs, BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE);
    }

    /**
     * 控制器一次能吞的包有限，返回 busy 就让出 2 ms 再试。
     * 但「属性不支持」这类错重试一万次也不会变，直接放弃，把码交给上层。
     */
    private boolean writeUntil(BluetoothGattCharacteristic c, byte[] data, long deadlineMs, int writeType) {
        while (SystemClock.elapsedRealtime() < deadlineMs) {
            if (writeOnce(c, data, writeType)) {
                return true;
            }
            if (lastRc > 0 && lastRc != BluetoothStatusCodes.ERROR_GATT_WRITE_REQUEST_BUSY) {
                return false;
            }
            try {
                Thread.sleep(2);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return false;
    }

    /** 最近一次写操作的状态码。成功是 0，系统没给码（老 API）记 -1。 */
    private volatile int lastRc = -1;

    private static String rcName(int rc) {
        if (rc == 0) return "ok";
        if (rc == BluetoothStatusCodes.ERROR_GATT_WRITE_REQUEST_BUSY) return "busy(201)";
        if (rc == BluetoothStatusCodes.ERROR_MISSING_BLUETOOTH_CONNECT_PERMISSION) {
            return "缺蓝牙权限(4)";
        }
        // 下面几个用字面量：不同 API 级别的 BluetoothStatusCodes 常量名不完全一致，
        // 写常量名会让低版本 SDK 编译不过。
        if (rc == 3) return "无写权限(3)";
        if (rc == 6) return "长度超MTU(6)";
        if (rc == 8) return "已断开(8)";
        if (rc == 200) return "写不允许(200)";
        if (rc < 0) return "系统未返回码";
        return "rc=" + rc;
    }

    private boolean writeOnce(BluetoothGattCharacteristic c, byte[] data, int writeType) {
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                final int rc = gatt.writeCharacteristic(c, data, writeType);
                lastRc = rc;
                return rc == BluetoothStatusCodes.SUCCESS;
            }
            c.setValue(data);
            c.setWriteType(writeType);
            final boolean ok = gatt.writeCharacteristic(c);
            lastRc = ok ? 0 : -1;
            return ok;
        } catch (SecurityException e) {
            // 权限被撤了，重试没意义，让上层走断线重连。
            lastRc = BluetoothStatusCodes.ERROR_MISSING_BLUETOOTH_CONNECT_PERMISSION;
            return false;
        } catch (Exception e) {
            lastRc = -3;
            Log.w(TAG, "write 异常 " + e);
            return false;
        }
    }

    /**
     * 阻塞式发送：投屏需要知道每一块到底写没写进去，写不进去就放弃这一帧。
     * 内部仍然排到 tx 线程执行，保证不会和其他写并发。
     */
    public boolean sendBlocking(byte[] data, long timeoutMs) {
        if (!ready) {
            return false;
        }
        final CountDownLatch latch = new CountDownLatch(1);
        final boolean[] result = new boolean[1];
        boolean posted = txHandler.post(() -> {
            try {
                result[0] = writeRaw(data);
            } finally {
                latch.countDown();
            }
        });
        if (!posted) {
            return false;
        }
        try {
            if (!latch.await(timeoutMs, TimeUnit.MILLISECONDS)) {
                return false;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
        return result[0];
    }

    private boolean writeRaw(byte[] data) {
        if (!ready || gatt == null) {
            return false;
        }
        BluetoothGattCharacteristic c = pick(data);
        if (c == null) {
            return false;
        }

        // 一律用「带响应写」。
        //
        // 这里踩过一个很隐蔽的坑：WRITE_TYPE_NO_RESPONSE 的写不保证回调
        // onCharacteristicWrite，而安卓正是靠这个回调清掉内部的 mDeviceBusy
        // 标志。标志一旦没人清，之后每一次 writeCharacteristic 都返回 201
        // (ERROR_GATT_WRITE_REQUEST_BUSY) —— 对 8 字节的时间帧看不出来（数据
        // 已经在之前发出去了，设备照样收到），但投屏 11 块会一块都写不进去，
        // 界面永远停在 0 fps。带响应写每块都有 ATT 确认，busy 一定能清。
        // 代价：每块多等一个连接间隔，时间/传感器是秒级节奏，完全无所谓。
        for (int attempt = 0; attempt < 60; attempt++) {
            boolean ok = false;
            try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    final int rc = gatt.writeCharacteristic(c, data,
                            BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                    lastRc = rc;
                    ok = rc == BluetoothStatusCodes.SUCCESS;
                } else {
                    c.setValue(data);
                    c.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                    ok = gatt.writeCharacteristic(c);
                    lastRc = ok ? 0 : -1;
                }
            } catch (SecurityException e) {
                return false;
            } catch (Exception e) {
                Log.w(TAG, "write 异常 " + e);
            }
            if (ok) {
                return true;
            }
            // 控制器/ATT 还没做完上一笔，等一下再试
            try {
                Thread.sleep(4);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return false;
            }
        }
        return false;
    }

    /** 用长度和首字节判断该走哪个特征 —— 协议里长度互不重叠。 */
    private BluetoothGattCharacteristic pick(byte[] d) {
        if (d.length == 8) {
            return chrTime;
        }
        if (d.length == 24) {
            return chrSensor;
        }
        return chrFrame;
    }

    private void post(Runnable r) {
        mainHandler.post(r);
    }

    public void release() {
        disconnect();
        txThread.quitSafely();
    }
}
