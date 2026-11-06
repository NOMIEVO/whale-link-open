package com.folotoy.whalelink;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Bundle;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

import java.util.ArrayList;
import java.util.List;

public class MainActivity extends AppCompatActivity {

    private static final int REQ_PERM = 1001;
    private static final int REQ_PROJECTION = 1002;
    private static final int REQ_FILE_PICK = 1003;
    private static final int REQ_PHOTO_PERM = 1004;

    private Prefs prefs;

    private TextView tvState;
    private TextView tvDetail;
    private TextView tvStats;
    private TextView tvLog;
    private Button btnConnect;
    private Switch swTime;
    private Switch swMotion;
    private Switch swLocation;
    private Switch swMirror;
    private LinearLayout mirrorOpts;
    private Spinner spRes;
    private Spinner spInterval;
    private CheckBox cbRaw;
    private CheckBox cbTile;
    private Spinner spSlot;

    private boolean suppress;

    /** 用户在等相册权限：一旦授予就补执行那次挂载，不用再点一次按钮。 */
    private boolean pendingMountAfterPhotoPerm;
    /** 每次 Activity 实例只自动申请一次，避免用户点"取消"后被反复弹窗。 */
    private boolean autoProjectionTried;
    private final List<String> logLines = new ArrayList<>();

    private final LinkService.Ui ui = new LinkService.Ui() {
        @Override
        public void onStatus(String state, String detail) {
            tvState.setText(state);
            tvDetail.setText(detail);
        }

        @Override
        public void onLog(String line) {
            appendLog(line);
        }

        @Override
        public void onStats(String stats) {
            tvStats.setText(stats);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        prefs = new Prefs(this);

        tvState = findViewById(R.id.tv_state);
        tvDetail = findViewById(R.id.tv_detail);
        tvStats = findViewById(R.id.tv_stats);
        tvLog = findViewById(R.id.tv_log);
        btnConnect = findViewById(R.id.btn_connect);
        swTime = findViewById(R.id.sw_time);
        swMotion = findViewById(R.id.sw_motion);
        swLocation = findViewById(R.id.sw_location);
        swMirror = findViewById(R.id.sw_mirror);
        mirrorOpts = findViewById(R.id.mirror_opts);
        spRes = findViewById(R.id.sp_res);
        spInterval = findViewById(R.id.sp_interval);
        cbRaw = findViewById(R.id.cb_raw);
        cbTile = findViewById(R.id.cb_tile);
        spSlot = findViewById(R.id.sp_slot);

        setupSpinners();
        restoreSwitches();
        setupListeners();

        if (hasBlePermissions()) {
            startLink(LinkService.ACTION_START);
        } else {
            tvDetail.setText("先授予蓝牙权限，再点下面的按钮");
        }
    }

    private void setupSpinners() {
        ArrayAdapter<CharSequence> res = ArrayAdapter.createFromResource(
                this, R.array.res_labels, android.R.layout.simple_spinner_item);
        res.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spRes.setAdapter(res);
        spRes.setSelection(prefs.resIndex());
        spRes.setOnItemSelectedListener(new SimpleItemSelected(pos -> {
            prefs.setResIndex(pos);
            restartMirrorIfRunning();
        }));

        ArrayAdapter<CharSequence> interval = ArrayAdapter.createFromResource(
                this, R.array.interval_labels, android.R.layout.simple_spinner_item);
        interval.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spInterval.setAdapter(interval);
        spInterval.setSelection(prefs.intervalIndex());
        spInterval.setOnItemSelectedListener(new SimpleItemSelected(pos -> {
            prefs.setIntervalIndex(pos);
            restartMirrorIfRunning();
        }));

        // 直推裸像素：给对比测试用的开关。编码那一层每帧实时读这个偏好，
        // 所以切完下一帧就生效，不用重开投屏。
        // 默认关闭 —— 38400 字节拆 78 块，比 RLE 的 12 块慢六倍，唯一的
        // 好处是设备端省掉 3~5 ms 解码时间。
        cbRaw.setChecked(prefs.rawPixel());
        cbRaw.setOnCheckedChangeListener((b, checked) -> {
            if (suppress) return;
            prefs.setRawPixel(checked);
            appendLog(checked ? "下一帧起直推裸像素（画质无损，但慢）"
                    : "下一帧起恢复 RLE 压缩");
        });

        // 增量刷新：逐帧比对，只发变化过的 8×8 块。编码层每帧实时读偏好，
        // 切换后自动走一次整帧（needFull），不需要重开投屏。
        cbTile.setChecked(prefs.tileMode());
        cbTile.setOnCheckedChangeListener((b, checked) -> {
            if (suppress) return;
            prefs.setTileMode(checked);
            appendLog(checked ? "只发变化块：静止画面几乎不占带宽"
                    : "改回整帧重传（每帧都发全图）");
        });
        // 条目选择：设备不再把 flash 切成等长槽位，而是拿一整块池子按文件
        // 实际大小分配连续区段，16 只是条目数上限。所以这里叫「条目」不叫「槽位」，
        // 免得让人以为每个都占固定 1MB。
        java.util.List<String> entryLabels = new ArrayList<>();
        for (int i = 0; i < Frames.FILE_MAX_ENTRIES; i++) {
            entryLabels.add("条目 " + i);
        }
        ArrayAdapter<String> slots = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, entryLabels);
        slots.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spSlot.setAdapter(slots);

        // ---- 文件 → 设备 Flash ----

        findViewById(R.id.btn_mount_next).setOnClickListener(v -> {
            // 相册权限单独判：蓝牙权限早就有了，「要照片」还额外需要读媒体。
            // 之前只判蓝牙、于是这条路径永远不会触发权限申请，用户看到的是
            // 「没有可用的照片」且不弹任何窗口 —— 现象就是这样来的。
            if (!hasPhotoPermission()) {
                pendingMountAfterPhotoPerm = true;
                requestPhotoPermission();
                return;
            }
            mountNext();
        });

        findViewById(R.id.btn_file_send).setOnClickListener(v -> {
            if (!hasBlePermissions()) {
                requestBlePermissions();
                return;
            }
            // SAF：系统文件选择器，不用申请存储权限
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            try {
                startActivityForResult(pick, REQ_FILE_PICK);
            } catch (Exception e) {
                appendLog("没有可用的文件选择器");
            }
        });

        findViewById(R.id.btn_file_list).setOnClickListener(v ->
                fileCmd(Frames.FILE_CMD_LIST));
        findViewById(R.id.btn_file_verify).setOnClickListener(v ->
                fileCmd(Frames.FILE_CMD_VERIFY));
        findViewById(R.id.btn_file_delete).setOnClickListener(v ->
                fileCmd(Frames.FILE_CMD_DELETE));
        findViewById(R.id.btn_file_compact).setOnClickListener(v -> {
            appendLog("请求设备整理存储碎片（要搬 flash 数据，约 1~2 秒，期间别关 App）");
            fileCmd(Frames.FILE_CMD_COMPACT);
        });
    }

    private void fileCmd(int cmd) {
        Intent i = new Intent(this, LinkService.class)
                .setAction(LinkService.ACTION_FILE_CMD)
                .putExtra(LinkService.EXTRA_SLOT, spSlot.getSelectedItemPosition())
                .putExtra(LinkService.EXTRA_FILE_CMD, cmd);
        androidx.core.content.ContextCompat.startForegroundService(this, i);
    }

    /** 触发一次「按需挂载」，与设备上长按 OK 等价。 */
    private void mountNext() {
        Intent i = new Intent(this, LinkService.class)
                .setAction(LinkService.ACTION_MOUNT_NEXT);
        androidx.core.content.ContextCompat.startForegroundService(this, i);
    }

    // ---- 相册权限 -----------------------------------------------------------

    /**
     * 是否已能读相册。
     *
     * Android 13 起 READ_EXTERNAL_STORAGE 对图片不再生效，必须用
     * READ_MEDIA_IMAGES；Android 14 起用户还能选「仅选择部分照片」，
     * 此时仅授予 READ_MEDIA_VISUAL_USER_SELECTED（只看到用户选的那几张）。
     * compileSdk 是 33，拿不到 UPSIDE_DOWN_CAKE 常量，只能比字面量 34。
     */
    private boolean hasPhotoPermission() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (granted(Manifest.permission.READ_MEDIA_IMAGES)) {
                return true;
            }
            return Build.VERSION.SDK_INT >= 34
                    && granted("android.permission.READ_MEDIA_VISUAL_USER_SELECTED");
        }
        return granted(Manifest.permission.READ_EXTERNAL_STORAGE);
    }

    private void requestPhotoPermission() {
        String perm;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perm = Manifest.permission.READ_MEDIA_IMAGES;
        } else {
            perm = Manifest.permission.READ_EXTERNAL_STORAGE;
        }
        if (!ActivityCompat.shouldShowRequestPermissionRationale(this, perm)
                && prefs.photoPermissionAsked()) {
            // 问过一次又被拒、且系统不再弹窗 —— 只能引导去设置，再问也没用。
            appendLog("相册权限已被拒绝且系统不再弹窗：请到「设置 → 应用 → whale-link → 权限」"
                    + "里打开「照片和视频」，或改用「选择文件并发送」手动挑图");
            pendingMountAfterPhotoPerm = false;
            return;
        }
        prefs.setPhotoPermissionAsked();
        ActivityCompat.requestPermissions(this, new String[]{perm}, REQ_PHOTO_PERM);
    }

    private void restoreSwitches() {
        suppress = true;
        swTime.setChecked(prefs.timeEnabled());
        swMotion.setChecked(prefs.motionEnabled());
        swLocation.setChecked(prefs.locationEnabled());
        swMirror.setChecked(prefs.mirrorEnabled());
        mirrorOpts.setVisibility(prefs.mirrorEnabled() ? LinearLayout.VISIBLE : LinearLayout.GONE);
        suppress = false;
    }

    private void setupListeners() {
        btnConnect.setOnClickListener(v -> {
            if (!hasBlePermissions()) {
                requestBlePermissions();
                return;
            }
            startLink(LinkService.ACTION_RECONNECT);
            btnConnect.setText("重新连接");
            appendLog("已请求连接");
        });

        swTime.setOnCheckedChangeListener((b, checked) -> {
            if (suppress) return;
            prefs.setTimeEnabled(checked);
            startLink(LinkService.ACTION_SETTINGS);
        });

        swMotion.setOnCheckedChangeListener((b, checked) -> {
            if (suppress) return;
            prefs.setMotionEnabled(checked);
            startLink(LinkService.ACTION_SETTINGS);
        });

        swLocation.setOnCheckedChangeListener((b, checked) -> {
            if (suppress) return;
            if (checked && !hasLocationPermission()) {
                requestLocationPermission();
                return;
            }
            prefs.setLocationEnabled(checked);
            startLink(LinkService.ACTION_SETTINGS);
        });

        swMirror.setOnCheckedChangeListener((b, checked) -> {
            if (suppress) return;
            mirrorOpts.setVisibility(checked ? LinearLayout.VISIBLE : LinearLayout.GONE);
            if (checked) {
                requestProjection();
            } else {
                prefs.setMirrorEnabled(false);
                startLink(LinkService.ACTION_STOP_MIRROR);
            }
        });
    }

    /** 弹系统的屏幕录制授权框。成功与否由 onActivityResult 收尾。 */
    private void requestProjection() {
        MediaProjectionManager mpm =
                (MediaProjectionManager) getSystemService(MEDIA_PROJECTION_SERVICE);
        startActivityForResult(mpm.createScreenCaptureIntent(), REQ_PROJECTION);
    }

    private void restartMirrorIfRunning() {
        if (prefs.mirrorEnabled()) {
            appendLog("参数已保存，重新打开投屏开关生效");
        }
    }

    // ---- 权限 ---------------------------------------------------------------

    private boolean hasBlePermissions() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            return granted(Manifest.permission.BLUETOOTH_SCAN)
                    && granted(Manifest.permission.BLUETOOTH_CONNECT);
        }
        return true;
    }

    private boolean hasLocationPermission() {
        return granted(Manifest.permission.ACCESS_FINE_LOCATION);
    }

    private boolean granted(String p) {
        return ContextCompat.checkSelfPermission(this, p) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestBlePermissions() {
        List<String> perms = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            perms.add(Manifest.permission.BLUETOOTH_SCAN);
            perms.add(Manifest.permission.BLUETOOTH_CONNECT);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            perms.add(Manifest.permission.POST_NOTIFICATIONS);
            // 「按需挂载」要从相册取图。Android 13 起拆成 READ_MEDIA_IMAGES，
            // 不加这条的话扫相册直接返回 0 张，界面上只会看到"没有可用照片"。
            perms.add(Manifest.permission.READ_MEDIA_IMAGES);
        } else {
            perms.add(Manifest.permission.READ_EXTERNAL_STORAGE);
        }
        if (!perms.isEmpty()) {
            ActivityCompat.requestPermissions(this, perms.toArray(new String[0]), REQ_PERM);
        }
    }

    private void requestLocationPermission() {
        ActivityCompat.requestPermissions(this,
                new String[]{Manifest.permission.ACCESS_FINE_LOCATION}, REQ_PERM + 1);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions,
                                          @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQ_PERM) {
            if (hasBlePermissions()) {
                startLink(LinkService.ACTION_START);
                tvDetail.setText("权限已就绪");
            } else {
                tvDetail.setText("蓝牙权限被拒绝，无法连接挂件");
            }
        } else if (requestCode == REQ_PERM + 1) {
            boolean ok = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
            suppress = true;
            swLocation.setChecked(ok);
            suppress = false;
            prefs.setLocationEnabled(ok);
            startLink(LinkService.ACTION_SETTINGS);
        } else if (requestCode == REQ_PHOTO_PERM) {
            if (hasPhotoPermission()) {
                appendLog("相册权限已授予，可以按需挂载照片了");
                if (pendingMountAfterPhotoPerm) {
                    pendingMountAfterPhotoPerm = false;
                    mountNext();
                }
            } else {
                pendingMountAfterPhotoPerm = false;
                String perm = Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU
                        ? Manifest.permission.READ_MEDIA_IMAGES
                        : Manifest.permission.READ_EXTERNAL_STORAGE;
                if (ActivityCompat.shouldShowRequestPermissionRationale(this, perm)) {
                    // 还能再问：下次点按钮会重新弹窗
                    appendLog("相册权限被拒绝。再点一次「要下一张照片」会重新询问");
                } else {
                    appendLog("相册权限被永久拒绝：请到「设置 → 应用 → whale-link → 权限」"
                            + "打开「照片和视频」。也可先用「选择文件并发送」手动挑一张");
                }
            }
        }
    }

    // ---- 投屏授权 -----------------------------------------------------------

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_FILE_PICK) {
            if (resultCode == Activity.RESULT_OK && data != null && data.getData() != null) {
                int slot = spSlot.getSelectedItemPosition();
                appendLog("已选文件，发送到槽位 " + slot + "…");
                Intent i = new Intent(this, LinkService.class)
                        .setAction(LinkService.ACTION_SEND_FILE)
                        .putExtra(LinkService.EXTRA_URI, data.getData())
                        .putExtra(LinkService.EXTRA_SLOT, slot);
                ContextCompat.startForegroundService(this, i);
            } else {
                appendLog("未选择文件");
            }
            return;
        }
        if (requestCode != REQ_PROJECTION) {
            return;
        }
        if (resultCode == Activity.RESULT_OK && data != null) {
            prefs.setMirrorEnabled(true);
            Intent i = new Intent(this, LinkService.class)
                    .setAction(LinkService.ACTION_SET_PROJECTION)
                    .putExtra(LinkService.EXTRA_RESULT_CODE, resultCode)
                    .putExtra(LinkService.EXTRA_RESULT_DATA, data);
            ContextCompat.startForegroundService(this, i);
            appendLog("已获得屏幕录制授权");
        } else {
            suppress = true;
            swMirror.setChecked(false);
            suppress = false;
            mirrorOpts.setVisibility(LinearLayout.GONE);
            prefs.setMirrorEnabled(false);
            appendLog("未授权投屏");
        }
    }

    // ---- 服务 ---------------------------------------------------------------

    private void startLink(String action) {
        Intent i = new Intent(this, LinkService.class).setAction(action);
        ContextCompat.startForegroundService(this, i);
    }

    private void appendLog(String line) {
        logLines.add(line);
        while (logLines.size() > 18) {
            logLines.remove(0);
        }
        StringBuilder sb = new StringBuilder();
        for (String s : logLines) {
            sb.append(s).append('\n');
        }
        tvLog.setText(sb.toString());
    }

    @Override
    protected void onResume() {
        super.onResume();
        LinkService.setUi(ui);
        restoreSwitches();
        maybeReacquireProjection();
    }

    /**
     * 开关记着是开的，可当前并没有在投屏 —— 几乎都是因为屏幕录制授权随着
     * 进程一起没了（App 被系统杀过、或重装过）。这时自动再弹一次系统授权框，
     * 用户点一下就恢复；不然他只会看到"开关是开的，设备却不显示"。
     *
     * 等 1.2 秒再判断，是因为刚启动时 Service 可能正在建立投屏，别误判成失败。
     */
    private void maybeReacquireProjection() {
        if (autoProjectionTried) {
            return;
        }
        if (!prefs.mirrorEnabled() || LinkService.isMirroring()) {
            return;
        }
        tvStats.postDelayed(() -> {
            if (isFinishing() || autoProjectionTried) {
                return;
            }
            if (prefs.mirrorEnabled() && !LinkService.isMirroring()) {
                autoProjectionTried = true;
                LinkService.clearNeedProjection();
                appendLog("投屏授权已失效，正在重新申请");
                requestProjection();
            }
        }, 1200);
    }

    @Override
    protected void onPause() {
        super.onPause();
        LinkService.setUi(null);
    }

    /** Spinner 只需要 onItemSelected，不想为了实现接口写两个空方法。 */
    private static class SimpleItemSelected
            implements android.widget.AdapterView.OnItemSelectedListener {
        private final java.util.function.IntConsumer consumer;
        private boolean first = true;

        SimpleItemSelected(java.util.function.IntConsumer c) {
            this.consumer = c;
        }

        @Override
        public void onItemSelected(android.widget.AdapterView<?> parent,
                                   android.view.View view, int position, long id) {
            if (first) {
                first = false;
                return;
            }
            consumer.accept(position);
        }

        @Override
        public void onNothingSelected(android.widget.AdapterView<?> parent) {
        }
    }
}
