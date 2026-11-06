// main/whale_link_diag.h —— 联调固件的链路自检统计。
//
// 这个工程存在的唯一目的是**验证手机 App**：时间、传感器、投屏三条通路
// 到底有没有数据、数据长什么样、多快、有没有丢。
//
// 设计上刻意做成「旁路」：
//   * 所有 note_*() 都跑在 NimBLE host 任务里，只做整数自增/拷贝，
//     不分配内存、不打日志、不碰 LVGL；
//   * 主循环每 500 ms 读一份快照画到诊断屏，每 5 s 打一行串口。
// 这样即使 App 发得再快，也不会把 host 任务拖住（拖住就会掉连接）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "whale_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

// 40 字节:最长的两种行是 "MIR 4294967295 65535x65535 4294967K" 与
// "(-180.00000,-180.00000)",24 字节装不下 —— GCC 的 -Werror=format-truncation
// 会把这种"可能截断"直接当错误。屏幕实际显示都很短,缓冲留宽不吃 RAM(共 320 B)。
#define WHALE_DIAG_LINES     7
#define WHALE_DIAG_LINE_LEN  40

typedef struct {
    // 链路
    bool connected;
    uint16_t mtu;

    // 时间特征
    uint32_t time_count;
    uint32_t time_unix;      // 最后一次对时的 UTC 秒
    int16_t time_tz_min;
    int64_t time_age_us;     // 距最后一次对时；-1 = 从没对过

    // 传感器特征（按 kind 分开计数，一眼看出 App 开的是哪几个开关）
    uint32_t accel_count;
    uint32_t gyro_count;
    uint32_t grav_count;
    uint32_t loc_count;
    int32_t accel[3];        // 1e-3 m/s²
    int32_t gyro[3];         // 1e-3 rad/s
    int32_t loc[3];          // 纬度/经度 ×1e7，海拔 cm
    uint32_t loc_speed_cms;  // 来自 aux

    // 投屏特征
    uint32_t frame_count;
    uint32_t frame_reject;   // 收到但没能显示（缓冲不够/尺寸非法）
    uint16_t frame_w;
    uint16_t frame_h;
    uint32_t frame_bytes;    // 编码后长度
    int64_t frame_age_us;

    // 余额快照
    uint32_t snap_count;

    int64_t now_us;          // 读快照那一刻的 esp_timer 值
} whale_diag_t;

// 清零所有计数（开机时不需要，重播广播后想重新计数时可调）。
void whale_diag_reset(void);

void whale_diag_link(bool connected, uint16_t mtu);
void whale_diag_time(uint32_t unix_sec, int16_t tz_min);
void whale_diag_sensor(const whale_ble_sensor_t *s);
void whale_diag_frame(const whale_ble_frame_t *f, bool accepted);
void whale_diag_snapshot(void);

whale_diag_t whale_diag_read(void);

// 生成诊断屏文本（纯 ASCII，字体子集只含 32..126 与中文，够用）。
// 返回实际写入的行数。
int whale_diag_format(const whale_diag_t *d,
                      char lines[WHALE_DIAG_LINES][WHALE_DIAG_LINE_LEN]);

// 一行串口摘要，形如：
//   [diag] BLE=1 MTU=185 snap=3 time=5(-2s) acc=120 gyr=0 gps=12 mir=24/1
void whale_diag_log(const whale_diag_t *d);

#ifdef __cplusplus
}
#endif
