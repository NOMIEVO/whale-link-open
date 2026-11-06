package com.folotoy.whalelink;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.Looper;

import androidx.core.content.ContextCompat;

/**
 * GPS 位置。用系统 LocationManager 而不是 Play Services 的 FusedLocation ——
 * 少一个几 MB 的依赖，而且国内没 GMS 的机器也能跑。
 *
 * 单位：纬度/经度 ×1e7（int32 装得下 ±180e7），海拔 cm，速度 cm/s，航向 0.01°。
 */
public class LocationSource implements LocationListener {

    public interface Sink {
        void onSensor(int kind, int seq, int v0, int v1, int v2, int aux);
    }

    private final Context ctx;
    private final LocationManager lm;
    private final Sink sink;

    private int seq;
    private String provider;

    public LocationSource(Context ctx, Sink sink) {
        this.ctx = ctx;
        this.lm = (LocationManager) ctx.getSystemService(Context.LOCATION_SERVICE);
        this.sink = sink;
    }

    public boolean hasPermission() {
        return ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION)
                == PackageManager.PERMISSION_GRANTED;
    }

    public String start() {
        if (lm == null) {
            return "本机没有定位服务";
        }
        if (!hasPermission()) {
            return "缺少定位权限";
        }
        provider = lm.isProviderEnabled(LocationManager.GPS_PROVIDER)
                ? LocationManager.GPS_PROVIDER : LocationManager.NETWORK_PROVIDER;
        try {
            lm.requestLocationUpdates(provider, 1000L, 0f, this, Looper.getMainLooper());
        } catch (SecurityException e) {
            return "定位权限被拒绝";
        } catch (Exception e) {
            return "定位不可用：" + e.getMessage();
        }
        return null;
    }

    public void stop() {
        if (lm != null) {
            try {
                lm.removeUpdates(this);
            } catch (Exception ignored) {
            }
        }
    }

    @Override
    public void onLocationChanged(Location loc) {
        seq++;
        int lat = (int) Math.round(loc.getLatitude() * 1e7);
        int lon = (int) Math.round(loc.getLongitude() * 1e7);
        int alt = loc.hasAltitude() ? (int) Math.round(loc.getAltitude() * 100) : 0;

        int speed = 0;
        if (loc.hasSpeed() && loc.getSpeed() < 655) {
            speed = (int) Math.round(loc.getSpeed() * 100);   // m/s → cm/s
        }
        int heading = 0;
        if (loc.hasBearing()) {
            heading = (int) Math.round(loc.getBearing() * 100) % 36000;
        }
        int aux = (speed & 0xFFFF) | ((heading & 0xFFFF) << 16);

        sink.onSensor(Frames.KIND_LOCATION, seq, lat, lon, alt, aux);
    }

    @Override
    public void onStatusChanged(String p, int status, Bundle extras) {
    }

    @Override
    public void onProviderEnabled(String p) {
    }

    @Override
    public void onProviderDisabled(String p) {
    }
}
