package com.folotoy.whalelink;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.SystemClock;

/**
 * 陀螺仪 + 加速度。传感器可能跑 50–200 Hz，但链路只要 10 Hz ——
 * 在 onSensorChanged 里只记最新值，定时器到了再发，别把采样率原样搬到 BLE。
 */
public class MotionSource implements SensorEventListener {

    public interface Sink {
        void onSensor(int kind, int seq, int v0, int v1, int v2, int aux);
    }

    private static final long EMIT_INTERVAL_MS = 100;

    private final SensorManager sm;
    private final Sink sink;

    private Sensor accel;
    private Sensor gyro;

    private final float[] acc = new float[3];
    private final float[] gyr = new float[3];
    private volatile boolean accValid;
    private volatile boolean gyrValid;

    private long lastEmit;
    private int seq;

    public MotionSource(Context ctx, Sink sink) {
        this.sm = (SensorManager) ctx.getSystemService(Context.SENSOR_SERVICE);
        this.sink = sink;
        if (sm != null) {
            accel = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
            gyro = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
        }
    }

    public boolean available() {
        return accel != null || gyro != null;
    }

    public String describe() {
        StringBuilder sb = new StringBuilder();
        if (accel != null) sb.append("加速度");
        if (gyro != null) sb.append(sb.length() > 0 ? " + 陀螺仪" : "陀螺仪");
        return sb.length() == 0 ? "本机无运动传感器" : sb.toString();
    }

    public void start() {
        if (sm == null) {
            return;
        }
        if (accel != null) {
            sm.registerListener(this, accel, SensorManager.SENSOR_DELAY_GAME);
        }
        if (gyro != null) {
            sm.registerListener(this, gyro, SensorManager.SENSOR_DELAY_GAME);
        }
        lastEmit = 0;
    }

    public void stop() {
        if (sm != null) {
            sm.unregisterListener(this);
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        if (event.sensor.getType() == Sensor.TYPE_ACCELEROMETER) {
            acc[0] = event.values[0];
            acc[1] = event.values[1];
            acc[2] = event.values[2];
            accValid = true;
        } else if (event.sensor.getType() == Sensor.TYPE_GYROSCOPE) {
            gyr[0] = event.values[0];
            gyr[1] = event.values[1];
            gyr[2] = event.values[2];
            gyrValid = true;
        } else {
            return;
        }

        long now = SystemClock.elapsedRealtime();
        if (now - lastEmit < EMIT_INTERVAL_MS) {
            return;
        }
        lastEmit = now;
        seq++;

        // 定点 1e-3：m/s² 与 rad/s 都乘 1000，接收端除回来就是原值
        if (accValid) {
            sink.onSensor(Frames.KIND_ACCEL, seq,
                    Math.round(acc[0] * 1000f),
                    Math.round(acc[1] * 1000f),
                    Math.round(acc[2] * 1000f), 0);
        }
        if (gyrValid) {
            sink.onSensor(Frames.KIND_GYRO, seq,
                    Math.round(gyr[0] * 1000f),
                    Math.round(gyr[1] * 1000f),
                    Math.round(gyr[2] * 1000f), 0);
        }
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }
}
