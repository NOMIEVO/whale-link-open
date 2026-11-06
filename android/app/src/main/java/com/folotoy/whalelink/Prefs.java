package com.folotoy.whalelink;

import android.content.Context;
import android.content.SharedPreferences;

/** 开关与投屏参数。默认只开时间，其余由用户显式打开。 */
public final class Prefs {

    private static final String FILE = "whale_link";

    private final SharedPreferences sp;

    public Prefs(Context ctx) {
        sp = ctx.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }

    public boolean timeEnabled() {
        return sp.getBoolean("time", true);
    }

    public void setTimeEnabled(boolean v) {
        sp.edit().putBoolean("time", v).apply();
    }

    public boolean motionEnabled() {
        return sp.getBoolean("motion", false);
    }

    public void setMotionEnabled(boolean v) {
        sp.edit().putBoolean("motion", v).apply();
    }

    public boolean locationEnabled() {
        return sp.getBoolean("location", false);
    }

    public void setLocationEnabled(boolean v) {
        sp.edit().putBoolean("location", v).apply();
    }

    public boolean mirrorEnabled() {
        return sp.getBoolean("mirror", false);
    }

    public void setMirrorEnabled(boolean v) {
        sp.edit().putBoolean("mirror", v).apply();
    }

    /**
     * 直推裸像素（不做 RLE 压缩）。
     *
     * 设备端两条路都支持：fmt=0 走 RLE 解码，fmt=1 纯 memcpy。开着它能让设备
     * 端省掉 3~5 ms 的解码，但一帧从约 5 KB 涨到 38400 字节 —— 在 BLE 上
     * 块数从 12 变 78，空口时间直接翻六倍。默认关闭，只在对比测试时打开。
     */
    public boolean rawPixel() {
        return sp.getBoolean("raw_px", false);
    }

    public void setRawPixel(boolean v) {
        sp.edit().putBoolean("raw_px", v).apply();
    }

    /**
     * 增量刷新：逐帧比对，只发变化过的 8×8 块。默认开 —— 静止画面几乎不占
     * 带宽，是帧率提升最直接的一条路。关掉则退回整帧 RLE，用于对照。
     */
    public boolean tileMode() {
        return sp.getBoolean("tile", true);
    }

    public void setTileMode(boolean v) {
        sp.edit().putBoolean("tile", v).apply();
    }

    public String deviceAddress() {
        return sp.getString("addr", "");
    }

    public void setDeviceAddress(String addr) {
        sp.edit().putString("addr", addr).apply();
    }

    public String deviceName() {
        return sp.getString("name", "");
    }

    public void setDeviceName(String name) {
        sp.edit().putString("name", name).apply();
    }

    /** 0=120×160 1=96×128 2=160×213 */
    public int resIndex() {
        return sp.getInt("res", 0);
    }

    public void setResIndex(int v) {
        sp.edit().putInt("res", v).apply();
    }

    /**
     * 是否已经为相册权限弹过一次窗。
     *
     * 用来区分「还没问过」和「问过被拒」：Android 在用户拒绝后
     * shouldShowRequestPermissionRationale 会返回 true 一次，再拒就变 false，
     * 此时再调 requestPermissions 根本不会弹窗 —— 只会静默失败。
     * 没有这个标志就无法判断该不该引导用户去系统设置。
     */
    public boolean photoPermissionAsked() {
        return sp.getBoolean("photo_asked", false);
    }

    public void setPhotoPermissionAsked() {
        sp.edit().putBoolean("photo_asked", true).apply();
    }

    /** 0=1000ms 1=500ms 2=2000ms */
    public int intervalIndex() {
        return sp.getInt("interval", 0);
    }

    public void setIntervalIndex(int v) {
        sp.edit().putInt("interval", v).apply();
    }

    public int[] resolution() {
        switch (resIndex()) {
            case 1:
                return new int[]{96, 128};
            case 2:
                // 设备端解码上限 120×160；这一档只是"更省带宽"，不要再给更大的
                // 分辨率 —— 160×213 的解码缓冲要 66.6 KB，C3 上一定分配失败，
                // 表现就是帧全被拒、屏幕永远不出图。
                return new int[]{80, 107};
            default:
                return new int[]{120, 160};
        }
    }

    public int intervalMs() {
        switch (intervalIndex()) {
            case 1:
                return 500;
            case 2:
                return 1000;
            default:
                // 默认 200 ms。一帧 120×160 是 12 块，每块占一个 15 ms 的
                // 连接间隔，空口那一段约 180 ms —— 200 ms 的节奏正好贴着
                // 链路能力。再往上加间隔只是在浪费带宽，不会更快。
                return 200;
        }
    }
}
