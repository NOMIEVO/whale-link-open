// main/main.c —— WhaleLink 联调固件。
//
// 这是一个**独立的测试程序**，不是产品固件：
//   * 不出余额界面、不记账、不放音效、不深睡；
//   * 屏幕上只有四块状态卡片，用来确认手机 App 把数据送到了；
//   * 代码上也不链接产品侧的 whale_model / whale_ui / whale_audio 等模块，
//     与 ai-passport 主工程只共享 BSP 驱动和 BLE 协议实现。
//
// 按键（屏幕底部也写着）：
//   OK 短按   向手机请求一次余额
//   OK 长按   向手机要下一张资源（按需挂载，见 whale_ble_request_resource）
//   UP 短按   统计清零，便于重新数
//   UP 长按   内建自检图开关（验证显示链路）
//   DN 短按   收起正在显示的资源
//   DN 长按   重新广播（手机搜不到时）
//
// 串口每 5 秒打一行汇总，格式见 whale_link_diag.c。
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "test_ui.h"
#include "whale_ble.h"
#include "whale_file.h"
#include "whale_link_diag.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "whale_test";

#define KEY_QUEUE_DEPTH   8
#define UI_PERIOD_US      (500 * 1000)    // 界面刷新节拍：时间要每秒跳，500 ms 够
#define PRINT_PERIOD_US   (5 * 1000 * 1000)
#define BATT_PERIOD_US    (10 * 1000 * 1000)
#define STREAM_IDLE_US    (5 * 1000 * 1000)

static QueueHandle_t s_key_q;
static volatile bool s_dirty = true;
static bool s_streaming;
static int64_t s_last_frame_us;
static int64_t s_last_draw_us;
static int64_t s_last_print_us;
static int64_t s_last_batt_us;
// 屏幕上是自检图案（不是手机发来的帧）。自检图不接手机也不收帧，
// 必须挡住主循环那条「5 秒没新帧就收起画面」，否则刚显示就被自己收掉了。
static bool s_selftest;

// ---- 投屏帧的中转缓冲 ------------------------------------------------------
// whale_ble 回调里的 data 只在回调期间有效（下一帧的 idx=0 就从缓冲头部覆盖），
// 而解码 + 绘制要 1~3 ms、还得持 LVGL 锁，绝不能放在 NimBLE host 任务里做。
// 所以在这里用一块固定缓冲 + 互斥量：host 拷进来，app_task 拷出去再解码。
#define FRAME_BUF_BYTES   (40u * 1024u)   // 与 whale_ble 的 WHALE_FRAME_MAX 对齐

// 「按需挂载」用的展示位数量。存储池现在最多 16 个条目，但按需取图只在
// 前 4 个之间轮转 —— 那是「看一眼就走」的位置，翻十几张不该把池子占满。
#define RES_DISPLAY_SLOTS 4u
static SemaphoreHandle_t s_frame_mtx;
static uint8_t *s_frame_buf;
static uint32_t s_frame_len;
static uint16_t s_frame_w;
static uint16_t s_frame_h;
static uint8_t s_frame_fmt;
static volatile bool s_frame_new;

// ---- BLE 回调：全部跑在 NimBLE host 任务里 ----------------------------------
// 这里只碰整数统计，绝不调用 LVGL —— 跨任务碰界面必须走 test_ui_*（内部持锁）。

static void on_snapshot(const whale_ble_snapshot_t *snap, void *arg)
{
    (void)arg;
    whale_diag_snapshot();
    if (snap != NULL) {
        ESP_LOGI(TAG, "余额: 总 %lld 赠金 %lld 充值 %lld result=%d",
                 (long long)snap->balance.total_cents,
                 (long long)snap->balance.granted_cents,
                 (long long)snap->balance.topped_cents, (int)snap->result);
    }
    s_dirty = true;
}

static void on_time(uint32_t unix_sec, int16_t tz_min, void *arg)
{
    (void)arg;
    whale_diag_time(unix_sec, tz_min);
    ESP_LOGI(TAG, "对时: unix=%lu tz=%d", (unsigned long)unix_sec, (int)tz_min);
    s_dirty = true;
}

static void on_sensor(const whale_ble_sensor_t *s, void *arg)
{
    (void)arg;
    whale_diag_sensor(s);
}

static void on_frame(const whale_ble_frame_t *f, void *arg)
{
    (void)arg;

    // data 指着 whale_ble 内部的接收缓冲，回调一返回就可能被下一帧覆盖，
    // 所以必须当场拷走。拿不到锁说明上一帧还没解码完 —— 直接丢掉这一帧，
    // 投屏少一帧没人看得出来，堵住 BLE 链路才是真问题。
    if (f->len > FRAME_BUF_BYTES ||
        s_frame_buf == NULL || s_frame_mtx == NULL ||
        xSemaphoreTake(s_frame_mtx, 0) != pdTRUE) {
        whale_diag_frame(f, false);
        return;
    }

    memcpy(s_frame_buf, f->data, f->len);
    s_frame_len = f->len;
    s_frame_w = f->w;
    s_frame_h = f->h;
    s_frame_fmt = f->fmt;
    xSemaphoreGive(s_frame_mtx);

    whale_diag_frame(f, true);
    s_frame_new = true;
    s_selftest = false;      // 真帧来了就顶掉自检图
    s_last_frame_us = esp_timer_get_time();
    s_dirty = true;

    // 投屏期间要换成短连接间隔，否则一包等一个连接事件，1 fps 都到不了。
    if (!s_streaming) {
        s_streaming = true;
        whale_ble_set_streaming(true);
    }
}

// ---- 文件传输 --------------------------------------------------------------
//
// 回调跑在 NimBLE host 任务里：flash 写/擦就在这里做（whale_file 内部
// 边写边擦，单次只擦一个 4KB 扇区），UI 用 test_ui_file_status —— 它
// 自己持 LVGL 锁，跨任务调用是安全的。
static uint32_t s_file_ui_div;   // DATA 块计数器：别每块都刷屏

// ---- 按需挂载（设备要资源，手机送一张过来）----------------------------------
//
// 状态很少，但每个都有明确用途：
//   s_res_shown_slot —— 屏幕上那张照片来自哪个槽位。槽位要被擦写时先得靠它
//     把映射撤掉，否则 LVGL 会去读擦除后的 0xFF。
//   s_res_next      —— 下一次要请求的资源序号。用按键自增，等价于「下一张」。
//   s_res_slot      —— 请求写进哪个槽位。4 个槽位轮转，天然实现「最近 4 张
//     可离线看」，不需要真正的 LRU 表。
static int s_res_shown_slot = -1;
static uint16_t s_res_next;
static uint8_t s_res_slot;

// 把槽位里的资源显示出来：先映射，再看头部，认得出就交给 UI。
static void show_slot_resource(uint8_t slot)
{
    const uint8_t *px = NULL;
    uint32_t len = 0;
    if (whale_file_map_slot(slot, &px, &len) != ESP_OK) return;

    if (len < WHALE_RES_HDR_LEN) {
        whale_file_unmap();          // 不是资源文件，按普通文件处理
        return;
    }

    uint32_t magic = 0;
    memcpy(&magic, px, 4);
    if (magic != WHALE_RES_MAGIC) {
        whale_file_unmap();
        return;
    }

    uint16_t w = 0, h = 0;
    memcpy(&w, px + 4, 2);
    memcpy(&h, px + 6, 2);
    const uint32_t need = (uint32_t)w * h * 2u;
    if (px[8] != 0 || w == 0 || h == 0 || len < WHALE_RES_HDR_LEN + need) {
        ESP_LOGW(TAG, "资源头异常: %ux%u fmt=%u len=%lu", (unsigned)w,
                 (unsigned)h, (unsigned)px[8], (unsigned long)len);
        whale_file_unmap();
        return;
    }

    if (!test_ui_resource_show(px + WHALE_RES_HDR_LEN, w, h)) {
        whale_file_unmap();
        return;
    }
    s_res_shown_slot = slot;
    ESP_LOGI(TAG, "已显示槽位%u 的资源 %ux%u (%luKB, 零拷贝)", (unsigned)slot,
             (unsigned)w, (unsigned)h, (unsigned long)(need / 1024u));
}

// 要资源：轮转槽位 + 序号自增。没连手机时只更新序号，界面提示一句。
static void request_next_resource(void)
{
    const uint8_t slot = s_res_slot;
    const uint16_t idx = s_res_next;

    if (whale_ble_request_resource(slot, idx) != ESP_OK) {
        test_ui_file_status("RES offline", false);
        ESP_LOGW(TAG, "要资源失败：手机没连上");
        return;
    }

    // 序号先自增，失败也不回退 —— 用户再按一次就是下一张，不会卡在同一张。
    s_res_next = (uint16_t)(s_res_next + 1u);
    // 只在【前 4 个条目】之间轮转，不跟着条目总数（16）走：
    // 按需挂载是「看一眼就走」的展示位，用满了就覆盖最旧的；
    // 若每次都要新索引，翻十几张就把池子占满，正常文件反而没地方放。
    s_res_slot = (uint8_t)((s_res_slot + 1u) % RES_DISPLAY_SLOTS);

    char line[40];
    snprintf(line, sizeof(line), "RES #%u -> #%u", (unsigned)idx, (unsigned)slot);
    test_ui_file_status(line, true);
}

static bool on_file_msg(uint8_t kind, const whale_ble_file_msg_t *m, void *arg)
{
    (void)arg;
    char line[48];

    switch (kind) {
    case WHALE_FILE_MSG_BEGIN: {
        // 这个条目正在屏幕上显示的话，必须先撤图 + 撤映射：
        // 紧接着的擦除会让已映射的地址立刻变成 0xFF，LVGL 还照着读就会花屏。
        if (s_res_shown_slot == (int)m->slot) {
            test_ui_resource_hide();
            whale_file_unmap();
            s_res_shown_slot = -1;
        }
        const esp_err_t err = whale_file_begin(m->slot, m->total_len, m->crc32,
                                               m->name);
        if (err != ESP_OK) {
            // 空间不足和参数非法要分开报：前者让用户去删/整理，
            // 后者说明手机发的包有问题，处理方式完全不同。
            snprintf(line, sizeof(line), err == ESP_ERR_NO_MEM
                                          ? "FILE #%u NO SPACE"
                                          : "FILE #%u REJECT", (unsigned)m->slot);
            test_ui_file_status(line, false);
            ESP_LOGW(TAG, "文件 BEGIN 被拒: #%u \"%s\" %luB err=%d",
                     (unsigned)m->slot, m->name, (unsigned long)m->total_len, err);
            return false;
        }
        snprintf(line, sizeof(line), "FILE #%u %s 0%%", (unsigned)m->slot, m->name);
        test_ui_file_status(line, true);
        s_file_ui_div = 0;
        return true;
    }
    case WHALE_FILE_MSG_DATA: {
        if (whale_file_data(m->data, m->len) != ESP_OK) {
            snprintf(line, sizeof(line), "FILE #%u WRITE ERR", (unsigned)m->slot);
            test_ui_file_status(line, false);
            return false;
        }
        // 8 块 ≈ 4KB 刷一次进度，别把 LVGL 刷爆
        if ((++s_file_ui_div & 0x7u) == 0) {
            const uint32_t wr = whale_file_written();
            snprintf(line, sizeof(line), "FILE %lu/%luB %lu%%",
                     (unsigned long)wr, (unsigned long)m->total_len,
                     (unsigned long)(wr * 100u / m->total_len));
            test_ui_file_status(line, true);
        }
        return true;
    }
    case WHALE_FILE_MSG_END: {
        bool ok = false;
        const esp_err_t err = whale_file_end(&ok);
        if (err == ESP_OK && ok) {
            snprintf(line, sizeof(line), "FILE #%u %s %luKB OK",
                     (unsigned)m->slot, m->name,
                     (unsigned long)(m->total_len / 1024u));
            test_ui_file_status(line, true);

            // 落盘校验通过就自动显示。认不出 WHLP 头（普通文件）时
            // show_slot_resource 自己会撤退，不影响文件功能。
            show_slot_resource(m->slot);
        } else {
            snprintf(line, sizeof(line), "FILE #%u %s %s",
                     (unsigned)m->slot, m->name,
                     err == ESP_OK ? "CRC BAD" : "VERIFY ERR");
            test_ui_file_status(line, false);
        }
        return err == ESP_OK && ok;
    }
    case WHALE_FILE_MSG_LIST: {
        // 只回报【有内容的条目】，末尾补一条结束标记。
        // 不逐条报空位的原因：条目上限是 16，全报就是 16 条 notify，
        // 每条都要占一个 mbuf，在 NimBLE 的缓冲池里连续压 16 个很容易
        // 触发 ENOMEM 丢包；而手机真正关心的只是"有哪些、还剩多少空间"。
        uint8_t n = 0;
        for (uint8_t i = 0; i < WHALE_FILE_SLOT_COUNT; i++) {
            whale_file_info_t info;
            if (!whale_file_slot_info(i, &info)) continue;
            whale_ble_file_notify(8, i, 0, info.crc32, info.len, info.name);
            n++;
        }
        // status=13：列表结束。slot 字段带条目数，total_len 字段带
        // 「当前能一次分配出的最大空闲字节」—— 手机据此就能判断下一个
        // 文件放不放得下，不用再猜。
        whale_ble_file_notify(13, n, 0, 0, whale_file_free_bytes(), NULL);
        snprintf(line, sizeof(line), "FILE %u 项 空闲%luKB", (unsigned)n,
                 (unsigned long)(whale_file_free_bytes() / 1024u));
        test_ui_file_status(line, true);
        return true;
    }
    case WHALE_FILE_MSG_DELETE: {
        const bool ok = whale_file_erase_slot(m->slot) == ESP_OK;
        snprintf(line, sizeof(line), "FILE #%u %s", (unsigned)m->slot,
                 ok ? "DELETED" : "DEL ERR");
        test_ui_file_status(line, ok);
        if (ok) whale_ble_file_notify(9, m->slot, 0, 0, 0, NULL);
        return ok;
    }
    case WHALE_FILE_MSG_VERIFY: {
        bool ok = false;
        const esp_err_t err = whale_file_verify_slot(m->slot, &ok);
        if (err == ESP_ERR_NOT_FOUND) {
            // 空条目校验不算失败，但要用不同的状态码让手机知道原因
            whale_ble_file_notify(10, m->slot, 0, 0, 0, NULL);
            return true;
        }
        snprintf(line, sizeof(line), "FILE #%u VERIFY %s",
                 (unsigned)m->slot, ok ? "OK" : "BAD");
        test_ui_file_status(line, ok);
        whale_ble_file_notify(ok ? 3 : 4, m->slot, 0, 0, 0, NULL);
        return ok;
    }
    case WHALE_FILE_MSG_COMPACT: {
        // 整理要搬 flash 数据，先把正在显示的图和映射撤掉：
        // 映射期间搬动会让 LVGL 读到半新半旧的字节，画面上就是一条撕裂。
        test_ui_resource_hide();
        whale_file_unmap();
        const esp_err_t err = whale_file_compact();
        const bool ok = err == ESP_OK;
        snprintf(line, sizeof(line), "FILE COMPACT %s", ok ? "OK" : "ERR");
        test_ui_file_status(line, ok);
        // status=11 整理完成 / 12 整理失败（失败多半是还有映射没撤）
        whale_ble_file_notify(ok ? 11 : 12, 0, 0, 0, whale_file_free_bytes(), NULL);
        return ok;
    }
    default:
        return false;
    }
}

// ---- 按键 ------------------------------------------------------------------

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // 回调跑在共享 esp_timer 任务上：只入队，别在这里做任何事。
    if (ev != BSP_BTN_CLICK && ev != BSP_BTN_LONG) return;
    if (s_key_q == NULL) return;
    const uint8_t code = (uint8_t)btn | (ev == BSP_BTN_LONG ? 0x80 : 0x00);
    (void)xQueueSend(s_key_q, &code, 0);
}

static void handle_key(uint8_t code)
{
    const bool is_long = (code & 0x80) != 0;
    const bsp_btn_t btn = (bsp_btn_t)(code & 0x7F);

    if (is_long) {
        // 长按只留几个动作。重新广播是现场最常见的需求：手机搜不到设备。
        if (btn == BSP_BTN_OK) {
            // 长按 OK：要下一张资源。这是「按需挂载」的用户入口 ——
            // 设备主动向手机索取，手机再把图送过来。
            request_next_resource();
        } else if (btn == BSP_BTN_DOWN) {
            whale_ble_restart_advertising();
            ESP_LOGI(TAG, "重新广播");
        } else if (btn == BSP_BTN_UP) {
            // 显示内建测试图案：不接手机也能验证「解码 → lv_image → 铺屏」
            // 这一整条显示链路，把"设备显示坏了"和"手机没发帧"彻底分开。
            if (s_selftest) {
                s_selftest = false;
                test_ui_mirror_hide();
                ESP_LOGI(TAG, "自检图已收起");
            } else if (test_ui_mirror_selftest()) {
                s_selftest = true;
                ESP_LOGI(TAG, "自检图已显示（红/绿/蓝/白四象限 + 中心棋盘）");
            } else {
                ESP_LOGW(TAG, "自检图显示失败");
            }
        }
        return;
    }

    switch (btn) {
    case BSP_BTN_OK:
        if (whale_ble_request_refresh() == ESP_OK) {
            ESP_LOGI(TAG, "已向手机请求一次余额");
        } else {
            ESP_LOGW(TAG, "还没连上手机，请求已忽略");
        }
        break;
    case BSP_BTN_UP:
        whale_diag_reset();
        ESP_LOGI(TAG, "统计已清零");
        s_dirty = true;
        break;
    case BSP_BTN_DOWN:
        // 短按 DOWN：收掉正在显示的资源，回到状态卡片。
        // （重新广播已挪到长按 DOWN，短按原本与它重复，正好腾出来。）
        if (s_res_shown_slot >= 0) {
            test_ui_resource_hide();
            whale_file_unmap();
            s_res_shown_slot = -1;
            ESP_LOGI(TAG, "资源已收起");
        } else {
            whale_ble_restart_advertising();
            ESP_LOGI(TAG, "重新广播");
        }
        break;
    default:
        break;
    }
}

// ---- 主循环 ----------------------------------------------------------------

static void app_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint8_t code;
        if (xQueueReceive(s_key_q, &code, pdMS_TO_TICKS(200)) == pdTRUE) {
            handle_key(code);
        }

        const int64_t now = esp_timer_get_time();

        // 统计里的连接状态与 MTU 由这里统一同步一次，UI 与串口读到的是同一份值。
        whale_diag_link(whale_ble_connected(), whale_ble_mtu());

        // 有新帧就解出来画到屏上。手机端 1~2 fps，200 ms 一拍跟得上；
        // 期间连来多帧的话，后到的会把标志位重设，最终只画最新那一帧。
        if (s_frame_new) {
            s_frame_new = false;
            if (s_frame_buf != NULL && s_frame_mtx != NULL &&
                xSemaphoreTake(s_frame_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
                const uint32_t len = s_frame_len;
                const uint16_t fw = s_frame_w;
                const uint16_t fh = s_frame_h;
                const uint8_t ffmt = s_frame_fmt;
                if (!test_ui_mirror_show(s_frame_buf, len, fw, fh, ffmt)) {
                    // 尺寸越界 / 分配失败 / 流不完整都会走到这里。屏幕保持原样，
                    // 串口留一行，现场好判断是哪一种。
                    ESP_LOGW(TAG, "投屏帧没显示出来(%ux%u fmt=%u, %lu 字节, 堆 %u)",
                             (unsigned)fw, (unsigned)fh, (unsigned)ffmt, (unsigned long)len,
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
                }
                xSemaphoreGive(s_frame_mtx);
            }
        }

        // 投屏停了就把连接参数降回省电档：短间隔的功耗是长间隔的好几倍。
        // 自检图不走蓝牙，不能拿它当"还在投屏"，否则连接参数一直挂在耗电档。
        if (s_streaming && now - s_last_frame_us > STREAM_IDLE_US) {
            s_streaming = false;
            whale_ble_set_streaming(false);
            if (!s_selftest) {
                test_ui_mirror_hide();  // 画面一起收起来，回到状态卡片
            }
            ESP_LOGI(TAG, "5 秒没有新画面，连接参数回到省电档");
        }

        // 投屏画面盖着整屏时不去刷底下的卡片 —— 那些 label 一个像素也看不见，
        // 却会被 LVGL 当成脏区重绘，白吃掉本来就不宽裕的刷新预算。
        if (!test_ui_mirror_visible() && (s_dirty || now - s_last_draw_us >= UI_PERIOD_US)) {
            s_dirty = false;
            s_last_draw_us = now;
            const whale_diag_t d = whale_diag_read();
            test_ui_update(&d);
        }

        if (now - s_last_batt_us >= BATT_PERIOD_US) {
            s_last_batt_us = now;
            test_ui_battery(bsp_battery_soc());
        }

        if (now - s_last_print_us >= PRINT_PERIOD_US) {
            s_last_print_us = now;
            const whale_diag_t d = whale_diag_read();
            whale_diag_log(&d);

            // 与上面那行分开打：diag 是「业务统计」，这一行是「链路事实」。
            // 投屏发不出去时先看这行 —— itvl 是不是真的 6~12(7.5~15ms)、
            // blk 到底是 0（包没到）还是涨（包到了）。
            uint16_t itvl = 0, lat = 0, tmo = 0;
            whale_ble_conn_params(&itvl, &lat, &tmo);
            ESP_LOGI(TAG, "链路 itvl=%u(1.25ms) lat=%u tmo=%u(10ms) blk=%lu bad=%lu heap=%u",
                     (unsigned)itvl, (unsigned)lat, (unsigned)tmo,
                     (unsigned long)whale_ble_block_count(),
                     (unsigned long)whale_ble_block_bad(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "WhaleLink 联调固件启动（独立测试程序，非产品固件）");

    // NVS 必须最先初始化：BLE controller 要用它存 PHY 校准数据，
    // 否则每次开机都做全校准。
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 需要重建(0x%x),擦除后重试", nvs_err);
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || bsp_lvgl_init() == NULL) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败");
        return;
    }
    bsp_display_backlight(100);

    if (!test_ui_create()) {
        ESP_LOGE(TAG, "测试界面创建失败");
        return;
    }

    // 投屏中转缓冲一次分配到位，之后指针不再变 —— 只有指针恒定，才能在
    // NimBLE 任务和 UI 任务之间安全共享（realloc 会让对端手里变成野指针）。
    s_frame_buf = heap_caps_malloc(FRAME_BUF_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_frame_mtx = xSemaphoreCreateMutex();
    if (s_frame_buf == NULL || s_frame_mtx == NULL) {
        ESP_LOGW(TAG, "投屏中转缓冲不可用(%u 字节)，投屏只统计不显示",
                 (unsigned)FRAME_BUF_BYTES);
    }

    // 蓝牙要在界面之后起：设备名要显示在 LINK 卡片上。
    if (whale_ble_init(on_snapshot, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "蓝牙初始化失败，测试无法继续");
        return;
    }
    whale_ble_set_time_cb(on_time, NULL);
    whale_ble_set_sensor_cb(on_sensor, NULL);
    whale_ble_set_frame_cb(on_frame, NULL);
    whale_ble_set_file_cb(on_file_msg, NULL);
    whale_file_init();   // 找不到分区只打日志，不影响其它功能

    s_key_q = xQueueCreate(KEY_QUEUE_DEPTH, sizeof(uint8_t));
    if (s_key_q == NULL || bsp_button_init(on_key, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败");
    }

    ESP_LOGI(TAG, "设备名 %s，等待手机连接", whale_ble_device_name());
    xTaskCreate(app_task, "test_app", 4096, NULL, 5, NULL);
}
