// main/whale_link_diag.c —— 联调统计的实现。
//
// 所有 note_*() 都在 NimBLE host 任务上下文执行：只有整数自增和定长拷贝，
// 没有 malloc、没有 ESP_LOG（host 任务里打日志会明显拖慢连接事件），
// 也不会碰 LVGL。读侧由主循环单方面来取，二者之间只靠 32 位对齐读写保证
// 单字段的原子性 —— 统计用途，允许跨字段短暂不一致。
#include "whale_link_diag.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

// 北京时间的固定偏移：设备没有联网，时区只能写死（与 main.c 保持一致）。
#define DIAG_BEIJING_OFFSET_SEC (8 * 3600)

static volatile bool s_connected;
static volatile uint16_t s_mtu;

static volatile uint32_t s_time_count;
static volatile uint32_t s_time_unix;
static volatile int16_t s_time_tz;
static volatile int64_t s_time_last_us = -1;

static volatile uint32_t s_accel_count;
static volatile uint32_t s_gyro_count;
static volatile uint32_t s_grav_count;
static volatile uint32_t s_loc_count;
static volatile int32_t s_accel[3];
static volatile int32_t s_gyro[3];
static volatile int32_t s_loc[3];
static volatile uint32_t s_loc_speed;

static volatile uint32_t s_frame_count;
static volatile uint32_t s_frame_reject;
static volatile uint16_t s_frame_w;
static volatile uint16_t s_frame_h;
static volatile uint32_t s_frame_bytes;
static volatile int64_t s_frame_last_us = -1;

static volatile uint32_t s_snap_count;

void whale_diag_reset(void)
{
    s_time_count = 0;
    s_time_unix = 0;
    s_time_tz = 0;
    s_time_last_us = -1;
    s_accel_count = 0;
    s_gyro_count = 0;
    s_grav_count = 0;
    s_loc_count = 0;
    memset((void *)s_accel, 0, sizeof(s_accel));
    memset((void *)s_gyro, 0, sizeof(s_gyro));
    memset((void *)s_loc, 0, sizeof(s_loc));
    s_loc_speed = 0;
    s_frame_count = 0;
    s_frame_reject = 0;
    s_frame_w = 0;
    s_frame_h = 0;
    s_frame_bytes = 0;
    s_frame_last_us = -1;
    s_snap_count = 0;
}

void whale_diag_link(bool connected, uint16_t mtu)
{
    s_connected = connected;
    s_mtu = mtu;
}

void whale_diag_time(uint32_t unix_sec, int16_t tz_min)
{
    s_time_unix = unix_sec;
    s_time_tz = tz_min;
    s_time_last_us = esp_timer_get_time();
    s_time_count = s_time_count + 1;
}

void whale_diag_sensor(const whale_ble_sensor_t *s)
{
    if (s == NULL) return;

    switch (s->kind) {
    case WHALE_SENSOR_ACCEL:
        s_accel[0] = s->v[0];
        s_accel[1] = s->v[1];
        s_accel[2] = s->v[2];
        s_accel_count = s_accel_count + 1;
        break;
    case WHALE_SENSOR_GYRO:
        s_gyro[0] = s->v[0];
        s_gyro[1] = s->v[1];
        s_gyro[2] = s->v[2];
        s_gyro_count = s_gyro_count + 1;
        break;
    case WHALE_SENSOR_GRAVITY:
        s_grav_count = s_grav_count + 1;
        break;
    case WHALE_SENSOR_LOCATION:
        s_loc[0] = s->v[0];
        s_loc[1] = s->v[1];
        s_loc[2] = s->v[2];
        s_loc_speed = s->aux & 0xFFFFu;
        s_loc_count = s_loc_count + 1;
        break;
    default:
        break;
    }
}

void whale_diag_frame(const whale_ble_frame_t *f, bool accepted)
{
    if (f == NULL) return;
    if (accepted) {
        s_frame_w = f->w;
        s_frame_h = f->h;
        s_frame_bytes = f->len;
        s_frame_last_us = esp_timer_get_time();
        s_frame_count = s_frame_count + 1;
    } else {
        s_frame_reject = s_frame_reject + 1;
    }
}

void whale_diag_snapshot(void)
{
    s_snap_count = s_snap_count + 1;
}

whale_diag_t whale_diag_read(void)
{
    whale_diag_t d;
    memset(&d, 0, sizeof(d));

    d.now_us = esp_timer_get_time();

    d.connected = s_connected;
    d.mtu = s_mtu;

    d.time_count = s_time_count;
    d.time_unix = s_time_unix;
    d.time_tz_min = s_time_tz;
    d.time_age_us = (s_time_last_us < 0) ? -1 : (d.now_us - s_time_last_us);

    d.accel_count = s_accel_count;
    d.gyro_count = s_gyro_count;
    d.grav_count = s_grav_count;
    d.loc_count = s_loc_count;
    for (int i = 0; i < 3; i++) {
        d.accel[i] = s_accel[i];
        d.gyro[i] = s_gyro[i];
        d.loc[i] = s_loc[i];
    }
    d.loc_speed_cms = s_loc_speed;

    d.frame_count = s_frame_count;
    d.frame_reject = s_frame_reject;
    d.frame_w = s_frame_w;
    d.frame_h = s_frame_h;
    d.frame_bytes = s_frame_bytes;
    d.frame_age_us = (s_frame_last_us < 0) ? -1 : (d.now_us - s_frame_last_us);

    d.snap_count = s_snap_count;
    return d;
}

// 把 UTC 秒换成北京时间 HH:MM:SS。设备不联网也没 SNTP，时区写死东八区。
static void fmt_hms(uint32_t unix_sec, char *out, size_t cap)
{
    uint32_t local = unix_sec + DIAG_BEIJING_OFFSET_SEC;
    const uint32_t sec_of_day = local % 86400u;
    snprintf(out, cap, "%02u:%02u:%02u",
             (unsigned)(sec_of_day / 3600u),
             (unsigned)((sec_of_day / 60u) % 60u),
             (unsigned)(sec_of_day % 60u));
}

// 负数也要能显示成 -12.3 这种形式，所以先取绝对值再补符号。
static void fmt_deg(int32_t raw, char *out, size_t cap)
{
    const bool neg = raw < 0;
    int32_t v = neg ? -raw : raw;
    const int32_t whole = v / 10000000;
    const int32_t frac = (v % 10000000) / 100;   // 保留 5 位小数
    snprintf(out, cap, "%s%ld.%05ld", neg ? "-" : "", (long)whole, (long)frac);
}

int whale_diag_format(const whale_diag_t *d, char lines[WHALE_DIAG_LINES][WHALE_DIAG_LINE_LEN])
{
    if (d == NULL || lines == NULL) return 0;

    int n = 0;

    snprintf(lines[n], WHALE_DIAG_LINE_LEN, "LINK SELF TEST");
    n++;

    if (d->connected) {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "BLE ON  MTU %u", (unsigned)d->mtu);
    } else {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "BLE OFF waiting");
    }
    n++;

    snprintf(lines[n], WHALE_DIAG_LINE_LEN, "S%lu T%lu A%lu",
             (unsigned long)d->snap_count, (unsigned long)d->time_count,
             (unsigned long)d->accel_count);
    n++;

    if (d->time_age_us >= 0) {
        char hms[12];
        fmt_hms(d->time_unix + (uint32_t)(d->time_age_us / 1000000), hms, sizeof(hms));
        // 距离上次对时的秒数：>120 s 说明 App 只发了一次就没再发（不正常）
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "TIME %s -%lds", hms,
                 (long)(d->time_age_us / 1000000));
    } else {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "TIME --:--:-- nosync");
    }
    n++;

    if (d->accel_count > 0) {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "ACC %ld,%ld,%ld",
                 (long)d->accel[0], (long)d->accel[1], (long)d->accel[2]);
    } else {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "ACC none");
    }
    n++;

    if (d->loc_count > 0) {
        // 12 字节刚好够 "-180.00000\0"，拼起来 25 字节，40 的行缓冲装得下。
        char lat[12];
        char lon[12];
        fmt_deg(d->loc[0], lat, sizeof(lat));
        fmt_deg(d->loc[1], lon, sizeof(lon));
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "%s,%s", lat, lon);
    } else if (d->gyro_count > 0) {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "GYR %ld,%ld,%ld",
                 (long)d->gyro[0], (long)d->gyro[1], (long)d->gyro[2]);
    } else {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "GPS/GYR none");
    }
    n++;

    if (d->frame_count > 0) {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "MIR %lu %ux%u %luK",
                 (unsigned long)d->frame_count, (unsigned)d->frame_w,
                 (unsigned)d->frame_h, (unsigned long)(d->frame_bytes / 1024u));
    } else if (d->frame_reject > 0) {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "MIR reject %lu",
                 (unsigned long)d->frame_reject);
    } else {
        snprintf(lines[n], WHALE_DIAG_LINE_LEN, "MIR none");
    }
    n++;

    return n;
}

void whale_diag_log(const whale_diag_t *d)
{
    if (d == NULL) return;
    ESP_LOGI("diag", "BLE=%d MTU=%u snap=%lu time=%lu(%lds) acc=%lu gyr=%lu gps=%lu mir=%lu/%lu",
             d->connected ? 1 : 0, (unsigned)d->mtu,
             (unsigned long)d->snap_count, (unsigned long)d->time_count,
             (long)(d->time_age_us < 0 ? -1 : (d->time_age_us / 1000000)),
             (unsigned long)d->accel_count, (unsigned long)d->gyro_count,
             (unsigned long)d->loc_count, (unsigned long)d->frame_count,
             (unsigned long)d->frame_reject);
}
