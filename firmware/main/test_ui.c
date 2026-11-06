// main/test_ui.c —— 测试台画面。
//
// 布局（240x320）：
//
//   WHALE LINK TESTER          87%      顶栏 y 0..32
//   ┌ LINK ─────────────────┐            y 36..110
//   │ BLE   CONNECTED       │
//   │ MTU 185    SNAP 3     │
//   │ WhaleBT-DA8A          │
//   └───────────────────────┘
//   ┌ TIME ─────────────────┐            y 116..180
//   │ 17:48:02      AGE 2s  │
//   └───────────────────────┘
//   ┌ MOTION ───────────────┐            y 186..238
//   │ ACC  12, -4, 998      │
//   │ GPS  39.9073,116.3912 │
//   └───────────────────────┘
//   ┌ MIRROR ───────────────┐            y 244..272
//   │ 24 f   120x160  5.2KB │
//   └───────────────────────┘
//   ┌ FILE ─────────────────┐            y 278..306
//   │ test.bin 262KB OK     │
//   └───────────────────────┘
//   OK=REQ UP=CLR DN=ADV UPL=TEST        y 310
//
// 全部 ASCII。字体走 LVGL 自带的 Montserrat，不再依赖产品侧的中文字模 ——
// 这样测试固件和产品固件在字库、图片、界面代码上完全没有交集。
#include "test_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp_display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#define TAG "test_ui"

// ---- 配色 ------------------------------------------------------------------

#define C_BG      0x0D1320
#define C_CARD    0x18202E
#define C_LINE    0x2A3546
#define C_TEXT    0xE8EEF8
#define C_DIM     0x8494AC
#define C_OK      0x35D07F
#define C_WARN    0xF0A03C
#define C_FAIL    0xE85C5C
#define C_ACCENT  0x4EA8FF

// ---- 布局 ------------------------------------------------------------------

#define SCREEN_W 240
#define SCREEN_H 320

#define CARD_X   8
#define CARD_W   (SCREEN_W - CARD_X * 2)
#define PAD      10

static lv_obj_t *s_batt;
static lv_obj_t *s_ble_state;
static lv_obj_t *s_mtu;
static lv_obj_t *s_snap;
static lv_obj_t *s_dev;
static lv_obj_t *s_clock;
static lv_obj_t *s_age;
static lv_obj_t *s_acc;
static lv_obj_t *s_pos;
static lv_obj_t *s_mir;
static lv_obj_t *s_file;
static bool s_ready;

// ---- 投屏画面 --------------------------------------------------------------
// 手机发来的是 RGB565 + RLE（token 格式见 App 侧 RleCodec.java）。设备侧要
// 先解成一块真正的位图，再交给 lv_image 显示 —— LVGL 自带的 RLE 解码器认的是
// 它自己的压缩格式，跟这里的流不是一回事，不能直接喂。
//
// 尺寸上限跟 App 端的三档分辨率走：120x160(默认) / 96x128 / 160x213。
// 最大那档 34080 像素，解码成 RGB565 要 66.6 KB —— 按需分配，不用就一直不占。
#define MIRROR_MAX_W     160
#define MIRROR_MAX_H     213
#define MIRROR_MAX_PX    (MIRROR_MAX_W * MIRROR_MAX_H)

static lv_obj_t *s_mirror_panel;    // 全屏黑底容器，默认隐藏
static lv_obj_t *s_mirror_img;
static lv_obj_t *s_mirror_info;     // 左上角角标：帧序号与尺寸
static lv_image_dsc_t s_mirror_dsc;
static uint8_t *s_mirror_px;        // 解码后的 RGB565 位图
static uint32_t s_mirror_cap;
static uint16_t s_mirror_w;         // 当前位图的像素尺寸，增量刷新靠它校验
static uint16_t s_mirror_h;
static uint32_t s_mirror_scale;     // 当前缩放倍率（256 = 1.0），脏区反算用
static volatile bool s_mirror_on;

// ---- 已挂载资源（照片）------------------------------------------------------
// 与投屏面板结构相同，但数据源是 flash 映射地址而非堆缓冲：这里【没有】
// 像素缓冲，dsc.data 直接指向 whale_file_map_slot 返回的地址。
// 150KB 的整屏照片就是这么显示的 —— 靠 mmap，不靠 malloc。
static lv_obj_t *s_res_panel;
static lv_obj_t *s_res_img;
static lv_image_dsc_t s_res_dsc;
static volatile bool s_res_on;

// 与产品界面的 make_box / make_label 同形，但这里是独立实现：
// 测试固件不 include 产品 UI 的任何头文件，两边改动互不影响。
static lv_obj_t *card(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_pos(box, CARD_X, y);
    lv_obj_set_size(box, CARD_W, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, int h,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, h);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(l, "");
    return l;
}

bool test_ui_create(void)
{
    if (s_ready) return true;
    if (!bsp_lvgl_lock(1000)) return false;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // 顶栏：标题 + 电量。不放任何产品元素。
    lv_obj_t *title = label(scr, PAD, 7, 180, 20, &lv_font_montserrat_16, C_TEXT);
    lv_label_set_text(title, "WHALE LINK TESTER");

    s_batt = label(scr, SCREEN_W - PAD - 56, 7, 56, 20, &lv_font_montserrat_16, C_DIM);
    lv_obj_set_style_text_align(s_batt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_batt, "--");

    // ---- LINK ----
    lv_obj_t *c1 = card(scr, 36, 74);
    lv_obj_t *l_ble = label(c1, PAD, 8, 44, 18, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(l_ble, "BLE");

    s_ble_state = label(c1, PAD + 44, 8, CARD_W - PAD * 2 - 44, 18,
                        &lv_font_montserrat_14, C_WARN);
    lv_label_set_text(s_ble_state, "WAITING");

    s_mtu = label(c1, PAD, 30, 96, 18, &lv_font_montserrat_14, C_TEXT);
    lv_label_set_text(s_mtu, "MTU --");

    s_snap = label(c1, PAD + 96, 30, CARD_W - PAD * 2 - 96, 18,
                   &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_snap, "SNAP 0");

    s_dev = label(c1, PAD, 50, CARD_W - PAD * 2, 18, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(s_dev, "WhaleBT-????");

    // ---- TIME ----
    lv_obj_t *c2 = card(scr, 116, 64);
    lv_obj_t *l_time = label(c2, PAD, 4, 60, 16, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(l_time, "TIME");

    s_clock = label(c2, PAD, 22, 150, 32, &lv_font_montserrat_24, C_WARN);
    lv_label_set_text(s_clock, "--:--:--");

    s_age = label(c2, PAD + 150, 30, CARD_W - PAD * 2 - 150, 18,
                  &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_age, "");

    // ---- MOTION ----（压到 52px 给 FILE 卡腾地方）
    lv_obj_t *c3 = card(scr, 186, 52);
    s_acc = label(c3, PAD, 6, CARD_W - PAD * 2, 18, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_acc, "ACC  none");

    s_pos = label(c3, PAD, 28, CARD_W - PAD * 2, 18, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_pos, "GPS/GYR  none");

    // ---- MIRROR ----（单行）
    lv_obj_t *c4 = card(scr, 244, 28);
    s_mir = label(c4, PAD, 5, CARD_W - PAD * 2, 18, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_mir, "MIR  no frames");

    // ---- FILE ----（手机 → Flash 槽位传输状态）
    lv_obj_t *c5 = card(scr, 278, 28);
    s_file = label(c5, PAD, 5, CARD_W - PAD * 2, 18, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(s_file, "FILE none");

    // 底部按键说明。测试台的操作就这几个，直接写在屏幕上省得翻文档。
    // 只放得下一行（CARD_W=224，14px 字约 8px/字符），所以挑最常用的：
    // 取图、查余额、收起。自检图（长按 UP）和重新广播（长按 DN）写在文档里。
    lv_obj_t *hint = label(scr, CARD_X, 310, CARD_W, 16, &lv_font_montserrat_14, C_DIM);
    lv_label_set_text(hint, "OK=REQ HOLD-OK=IMG DN=CLOSE");

    // ---- 投屏画面 ----
    // 最后创建 = z 序最上。默认 HIDDEN，有帧才盖上来。整块纯黑是因为画面
    // 铺不满时留白比露出底下的卡片好看，也方便一眼看出缩放对不对。
    s_mirror_panel = lv_obj_create(scr);
    lv_obj_set_pos(s_mirror_panel, 0, 0);
    lv_obj_set_size(s_mirror_panel, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_mirror_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_mirror_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_mirror_panel, 0, 0);
    lv_obj_set_style_border_width(s_mirror_panel, 0, 0);
    lv_obj_set_style_pad_all(s_mirror_panel, 0, 0);
    lv_obj_remove_flag(s_mirror_panel, LV_OBJ_FLAG_SCROLLABLE);
    // 放大后的图像会超出对象自身框，父容器必须允许溢出，否则边缘被裁掉。
    lv_obj_add_flag(s_mirror_panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(s_mirror_panel, LV_OBJ_FLAG_HIDDEN);

    s_mirror_img = lv_image_create(s_mirror_panel);
    lv_obj_set_pos(s_mirror_img, 0, 0);
    // 走 image 自己的变换（draw unit 的 blit），不要用 obj 的 transform_scale ——
    // 后者会给对象开一块 ARGB8888 中间层，C3 上能把帧率打到十几帧以下。
    lv_image_set_pivot(s_mirror_img, 0, 0);

    // 角标压在画面左上角：告诉人这是第几帧、原始分辨率多少。
    s_mirror_info = label(s_mirror_panel, 4, 4, 200, 18, &lv_font_montserrat_14, C_ACCENT);
    lv_label_set_text(s_mirror_info, "");

    // 资源画面：与投屏面板并列的另一块全屏层。分开建而不是复用同一块，
    // 是因为两者数据源的性质不同 —— 投屏是堆缓冲，资源是 flash 映射，
    // 共用一套 dsc 容易在切换时把已映射地址当成可 free 的缓冲。
    s_res_panel = lv_obj_create(scr);
    lv_obj_set_pos(s_res_panel, 0, 0);
    lv_obj_set_size(s_res_panel, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_res_panel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_res_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_res_panel, 0, 0);
    lv_obj_set_style_border_width(s_res_panel, 0, 0);
    lv_obj_set_style_pad_all(s_res_panel, 0, 0);
    lv_obj_remove_flag(s_res_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_res_panel, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_flag(s_res_panel, LV_OBJ_FLAG_HIDDEN);

    s_res_img = lv_image_create(s_res_panel);
    lv_obj_set_pos(s_res_img, 0, 0);
    lv_image_set_pivot(s_res_img, 0, 0);

    lv_screen_load(scr);

    s_ready = true;
    bsp_lvgl_unlock();
    return true;
}

// 把 1e-7 度的定点值换成 4 位小数，够看位置、也不会撑破卡片宽度。
static void fmt_lat(int32_t raw, char *out, size_t cap)
{
    const bool neg = raw < 0;
    int32_t v = neg ? -raw : raw;
    snprintf(out, cap, "%s%ld.%04ld", neg ? "-" : "",
             (long)(v / 10000000), (long)((v % 10000000) / 1000));
}

void test_ui_update(const whale_diag_t *d)
{
    if (!s_ready || d == NULL) return;
    if (!bsp_lvgl_lock(200)) return;

    // 64 字节：投屏那行的 "%lu f  %ux%u  %lu.%lu KB" 在 GCC 看来最坏能到 60 字节，
    // 用 40 会触发 -Werror=format-truncation。屏幕上的实际文本远短于此。
    char buf[64];

    // 连接状态用颜色区分：连上是绿、等着是橙。
    if (d->connected) {
        lv_label_set_text(s_ble_state, "CONNECTED");
        lv_obj_set_style_text_color(s_ble_state, lv_color_hex(C_OK), 0);
    } else {
        lv_label_set_text(s_ble_state, "WAITING");
        lv_obj_set_style_text_color(s_ble_state, lv_color_hex(C_WARN), 0);
    }

    snprintf(buf, sizeof(buf), "MTU %u", (unsigned)d->mtu);
    lv_label_set_text(s_mtu, buf);

    snprintf(buf, sizeof(buf), "SNAP %lu", (unsigned long)d->snap_count);
    lv_label_set_text(s_snap, buf);

    // ---- 时间 ----
    if (d->time_age_us >= 0) {
        const uint32_t local = d->time_unix + 8u * 3600u + (uint32_t)(d->time_age_us / 1000000);
        const uint32_t sod = local % 86400u;
        snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
                 (unsigned)(sod / 3600u), (unsigned)((sod / 60u) % 60u), (unsigned)(sod % 60u));
        lv_label_set_text(s_clock, buf);
        lv_obj_set_style_text_color(s_clock, lv_color_hex(C_TEXT), 0);

        snprintf(buf, sizeof(buf), "AGE %lds", (long)(d->time_age_us / 1000000));
        lv_label_set_text(s_age, buf);
    } else {
        lv_label_set_text(s_clock, "--:--:--");
        lv_obj_set_style_text_color(s_clock, lv_color_hex(C_WARN), 0);
        lv_label_set_text(s_age, "no sync");
    }

    // ---- 运动 ----
    if (d->accel_count > 0) {
        snprintf(buf, sizeof(buf), "ACC  %ld, %ld, %ld",
                 (long)d->accel[0], (long)d->accel[1], (long)d->accel[2]);
        lv_label_set_text(s_acc, buf);
        lv_obj_set_style_text_color(s_acc, lv_color_hex(C_TEXT), 0);
    } else {
        lv_label_set_text(s_acc, "ACC  none");
        lv_obj_set_style_text_color(s_acc, lv_color_hex(C_DIM), 0);
    }

    if (d->loc_count > 0) {
        char lat[12];
        char lon[12];
        fmt_lat(d->loc[0], lat, sizeof(lat));
        fmt_lat(d->loc[1], lon, sizeof(lon));
        snprintf(buf, sizeof(buf), "GPS  %s,%s", lat, lon);
        lv_label_set_text(s_pos, buf);
        lv_obj_set_style_text_color(s_pos, lv_color_hex(C_TEXT), 0);
    } else if (d->gyro_count > 0) {
        snprintf(buf, sizeof(buf), "GYR  %ld, %ld, %ld",
                 (long)d->gyro[0], (long)d->gyro[1], (long)d->gyro[2]);
        lv_label_set_text(s_pos, buf);
        lv_obj_set_style_text_color(s_pos, lv_color_hex(C_TEXT), 0);
    } else {
        lv_label_set_text(s_pos, "GPS/GYR  none");
        lv_obj_set_style_text_color(s_pos, lv_color_hex(C_DIM), 0);
    }

    // ---- 投屏 ----
    if (d->frame_count > 0) {
        snprintf(buf, sizeof(buf), "%lu f  %ux%u  %lu.%lu KB",
                 (unsigned long)d->frame_count, (unsigned)d->frame_w, (unsigned)d->frame_h,
                 (unsigned long)(d->frame_bytes / 1000u),
                 (unsigned long)((d->frame_bytes % 1000u) / 100u));
        lv_label_set_text(s_mir, buf);
        lv_obj_set_style_text_color(s_mir, lv_color_hex(C_TEXT), 0);
    } else if (d->frame_reject > 0) {
        snprintf(buf, sizeof(buf), "MIR  rejected %lu", (unsigned long)d->frame_reject);
        lv_label_set_text(s_mir, buf);
        lv_obj_set_style_text_color(s_mir, lv_color_hex(C_FAIL), 0);
    } else {
        lv_label_set_text(s_mir, "MIR  no frames");
        lv_obj_set_style_text_color(s_mir, lv_color_hex(C_DIM), 0);
    }

    bsp_lvgl_unlock();
}

// ---- 投屏：解码 + 显示 -----------------------------------------------------

// token 首字节：bit7=1 → run，长度 = 低 7 位，后跟 2 字节 RGB565；
//               bit7=0 → raw，个数 = 低 7 位，后跟 n×2 字节。
// 必须刚好填满 count 个像素才算成功 —— 少一个像素都说明这帧在传输中残了。
static bool rle_decode(const uint8_t *in, uint32_t len, uint16_t *out, uint32_t count)
{
    uint32_t i = 0;
    uint32_t o = 0;

    while (i < len) {
        const uint8_t tok = in[i++];
        const uint32_t n = (uint32_t)(tok & 0x7Fu);
        if (n == 0 || o + n > count) return false;

        if (tok & 0x80u) {                       // run：同一颜色重复 n 次
            if (i + 2u > len) return false;
            const uint16_t v = (uint16_t)(in[i] | ((uint16_t)in[i + 1] << 8));
            i += 2u;
            for (uint32_t k = 0; k < n; k++) out[o++] = v;
        } else {                                 // raw：n 个连续像素
            if (i + n * 2u > len) return false;
            for (uint32_t k = 0; k < n; k++, i += 2u) {
                out[o++] = (uint16_t)(in[i] | ((uint16_t)in[i + 1] << 8));
            }
        }
    }
    return o == count;
}

// ---- 增量刷新：只把变化过的块贴进位图 --------------------------------------
//
// 整帧重传的问题不在压缩率，而在「画面没动也在重发同一份像素」。实测静止
// 一帧 RLE 仍有 5~8 KB，要占 12 个连接事件（约 180 ms），帧率被空口时间锁
// 在 5 fps 上下。手机端逐帧比对之后，静止时一帧只有几个块、一两百字节，
// 一个连接事件就能发完 —— 瓶颈这才从空口挪到别处。
//
// 载荷格式（fmt=2）：
//   [0]   块数 n
//   [1]   块边长 s（像素，固定 8，必须是 2 的幂）
//   [2..] n 项，每项：tx, ty（uint8，单位是块），后接 s*s*2 字节 RGB565，
//                     块内行优先，每行跨 s 个像素
//
// 这里不做任何解码 —— 手机发来的就是最终像素，按坐标 memcpy 到对应偏移。
// 这才是「只搬运」的本意：设备既不用整帧解码，也不用重绘整屏。
static bool mirror_apply_tiles(const uint8_t *in, uint32_t len, uint16_t w, uint16_t h)
{
    if (len < 2) return false;
    const uint32_t n = in[0];
    const uint32_t s = in[1];
    if (n == 0 || s < 2 || s > 64 || (s & (s - 1u)) != 0) {   // 必须是 2 的幂
        ESP_LOGW(TAG, "增量帧头部非法 n=%u s=%u", (unsigned)n, (unsigned)s);
        return false;
    }
    const uint32_t tb = s * s * 2u;                            // 每块像素字节数
    const uint32_t want = 2u + n * (2u + tb);
    if (len != want) {
        ESP_LOGW(TAG, "增量帧长度不符：%u，按 n=%u s=%u 应为 %u",
                 (unsigned)len, (unsigned)n, (unsigned)s, (unsigned)want);
        return false;
    }
    // 增量只做叠加，位图必须先由全量帧建好。尺寸对不上说明手机重启过或者
    // 换了分辨率 —— 这一帧丢掉，等它下一帧全量。
    if (s_mirror_px == NULL || s_mirror_cap < (uint32_t)w * h * 2u ||
        s_mirror_w != w || s_mirror_h != h) {
        ESP_LOGW(TAG, "增量帧没有可叠加的位图(%ux%u，现有 %ux%u)，丢弃",
                 (unsigned)w, (unsigned)h, (unsigned)s_mirror_w, (unsigned)s_mirror_h);
        return false;
    }

    uint16_t minx = 0xFFFF, miny = 0xFFFF, maxx = 0, maxy = 0;
    uint32_t off = 2;
    uint32_t applied = 0;

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t tx = in[off];
        const uint32_t ty = in[off + 1u];
        const uint8_t *src = in + off + 2u;
        off += 2u + tb;

        const uint32_t x0 = tx * s;
        const uint32_t y0 = ty * s;
        if (x0 >= w || y0 >= h) continue;                      // 越界块单独跳过

        const uint32_t cols = (x0 + s <= w) ? s : (w - x0);
        const uint32_t rows = (y0 + s <= h) ? s : (h - y0);
        uint8_t *dst = s_mirror_px + ((uint32_t)y0 * w + x0) * 2u;
        for (uint32_t r = 0; r < rows; r++) {
            memcpy(dst + r * (uint32_t)w * 2u, src + r * s * 2u, cols * 2u);
        }

        if (x0 < minx) minx = (uint16_t)x0;
        if (y0 < miny) miny = (uint16_t)y0;
        if (x0 + cols > maxx) maxx = (uint16_t)(x0 + cols);
        if (y0 + rows > maxy) maxy = (uint16_t)(y0 + rows);
        applied++;
    }

    if (applied == 0) return true;                             // 全是越界块，不算错

    if (!s_mirror_on) {
        // 面板被 5 秒超时收起来了，增量帧把它拉回来。这次只能整块重绘一遍。
        lv_obj_remove_flag(s_mirror_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_mirror_panel);
        s_mirror_on = true;
        return true;
    }

    // 只标脏真正变过的那块屏幕。整对象 invalidate 会让 LVGL 把整张 240x320
    // 的缩放图重画一遍（7.7 万像素 + 2 倍缩放），空口省下的时间就被设备端
    // 吃回去了，等于白改。area 要传**屏幕坐标**（lv_obj_invalidate 内部也是
    // 拿 obj_get_coords 的结果传下去的），所以先取图像对象的顶层坐标，再加
    // 缩放后的偏移。
    lv_area_t ic;
    lv_obj_get_coords(s_mirror_img, &ic);
    lv_area_t a;
    a.x1 = ic.x1 + (int32_t)((uint32_t)minx * s_mirror_scale / 256u);
    a.y1 = ic.y1 + (int32_t)((uint32_t)miny * s_mirror_scale / 256u);
    a.x2 = ic.x1 + (int32_t)((uint32_t)maxx * s_mirror_scale / 256u) - 1;
    a.y2 = ic.y1 + (int32_t)((uint32_t)maxy * s_mirror_scale / 256u) - 1;
    lv_obj_invalidate_area(s_mirror_img, &a);
    return true;
}

bool test_ui_mirror_show(const uint8_t *payload, uint32_t len, uint16_t w, uint16_t h,
                         uint8_t fmt)
{
    if (!s_ready || payload == NULL || w == 0 || h == 0) return false;

    const uint32_t px = (uint32_t)w * h;
    if (px > MIRROR_MAX_PX) {
        ESP_LOGW(TAG, "投屏 %ux%u 超出解码上限(%u 像素)", w, h, (unsigned)MIRROR_MAX_PX);
        return false;
    }
    if (!bsp_lvgl_lock(200)) return false;

    // fmt=2 走增量：只贴脏块，不重建位图、不重设缩放，也不重画整屏。
    if (fmt == 2) {
        const bool ok = mirror_apply_tiles(payload, len, w, h);
        bsp_lvgl_unlock();
        return ok;
    }

    bool ok = false;
    const uint32_t need = px * 2u;

    // 裸格式要求长度严格等于 px*2 —— 少了就是传输残缺，画出来只会是花屏。
    if (fmt == 1 && len != need) {
        ESP_LOGW(TAG, "裸帧长度不符：%u 字节，应为 %u", (unsigned)len, (unsigned)need);
        bsp_lvgl_unlock();
        return false;
    }

    if (need > s_mirror_cap) {
        // 放在内部 RAM：解码与绘制都是热路径，PSRAM 有没有都别指望。
        uint8_t *p = heap_caps_malloc(need, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (p == NULL) {
            ESP_LOGW(TAG, "投屏位图分配失败 %u 字节，剩余堆 %u",
                     (unsigned)need, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        } else {
            heap_caps_free(s_mirror_px);
            s_mirror_px = p;
            s_mirror_cap = need;
        }
    }

    bool decoded = false;
    if (s_mirror_px != NULL && need <= s_mirror_cap) {
        if (fmt == 1) {
            memcpy(s_mirror_px, payload, need);      // 裸帧：搬运就完事
            decoded = true;
        } else {
            decoded = rle_decode(payload, len, (uint16_t *)s_mirror_px, px);
        }
    }

    if (decoded) {
        s_mirror_w = w;
        s_mirror_h = h;
        s_mirror_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_mirror_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_mirror_dsc.header.flags = 0;
        s_mirror_dsc.header.w = w;
        s_mirror_dsc.header.h = h;
        s_mirror_dsc.header.stride = (uint16_t)(w * 2u);
        s_mirror_dsc.data_size = need;
        s_mirror_dsc.data = s_mirror_px;

        // 每次都重设 src：宽高可能变，LVGL 会重新读 header 算对象尺寸。
        lv_image_set_src(s_mirror_img, &s_mirror_dsc);

        // 整数倍率铺满屏（120x160 → 2 倍正好 240x320）。pivot 摆在左上角，
        // 缩放后左上角不动，所以直接按居中偏移摆对象就行。
        const uint32_t sx = (SCREEN_W * 256u) / w;
        const uint32_t sy = (SCREEN_H * 256u) / h;
        uint32_t scale = sx < sy ? sx : sy;
        if (scale == 0) scale = 1;
        s_mirror_scale = scale;      // 增量帧反算脏区要用
        lv_image_set_scale(s_mirror_img, scale);
        lv_obj_set_pos(s_mirror_img,
                       (SCREEN_W - (int)((uint32_t)w * scale / 256u)) / 2,
                       (SCREEN_H - (int)((uint32_t)h * scale / 256u)) / 2);

        char buf[48];
        // R = RLE，P = 裸像素。现场判断一帧走的是哪条路，比看字节数直观。
        snprintf(buf, sizeof(buf), "%ux%u %c %luB x%lu.%lu",
                 (unsigned)w, (unsigned)h, fmt == 1 ? 'P' : 'R',
                 (unsigned long)len,
                 (unsigned long)(scale / 256u),
                 (unsigned long)((scale % 256u) * 10u / 256u));
        lv_label_set_text(s_mirror_info, buf);

        lv_obj_remove_flag(s_mirror_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_mirror_panel);
        s_mirror_on = true;
        ok = true;
    }

    bsp_lvgl_unlock();
    return ok;
}

void test_ui_mirror_hide(void)
{
    if (!s_ready || !s_mirror_on) return;
    if (!bsp_lvgl_lock(200)) return;

    lv_obj_add_flag(s_mirror_panel, LV_OBJ_FLAG_HIDDEN);
    s_mirror_on = false;

    bsp_lvgl_unlock();
}

bool test_ui_mirror_visible(void)
{
    return s_mirror_on;
}

// ---- 已挂载资源显示 ---------------------------------------------------------
//
// px 是 flash 映射地址，不是堆缓冲 —— 这个区别决定了整个「按需挂载」方案
// 能不能成立：
//   * 内存拷贝方案：240x320 RGB565 = 150KB，而 BLE 常驻后可用堆只剩 ~33KB，
//     必然 malloc 失败。装不下就是装不下，没有优化余地。
//   * mmap 方案：把 storage 分区的槽位映射进 DROM（0x3C000000 起），得到一个
//     普通 const 指针，LVGL 走 draw unit 直接读 flash。内存开销 = 0。
//
// 唯一的约束是映射期间不能擦写该槽位，否则读到的是擦除后的 0xFF。
bool test_ui_resource_show(const uint8_t *px, uint16_t w, uint16_t h)
{
    if (!s_ready || px == NULL) return false;
    if (w == 0 || h == 0) return false;
    // 尺寸超过屏幕太多没意义（还要靠 LVGL 缩小，得不偿失），直接拒。
    if ((uint32_t)w * h > (uint32_t)MIRROR_MAX_PX * 6u) return false;
    if (!bsp_lvgl_lock(300)) return false;

    s_res_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_res_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_res_dsc.header.flags = 0;
    s_res_dsc.header.w = w;
    s_res_dsc.header.h = h;
    s_res_dsc.header.stride = (uint16_t)(w * 2u);
    s_res_dsc.data_size = (uint32_t)w * h * 2u;
    s_res_dsc.data = px;          // ★ 关键：直接指向 flash，零拷贝

    lv_image_set_src(s_res_img, &s_res_dsc);

    // 与投屏同样的整数倍率铺满策略，但允许缩小时用非整数倍率 ——
    // 照片常常比屏幕大，按「铺满且不裁切」算一个统一倍率更合适。
    const uint32_t sx = (SCREEN_W * 256u) / w;
    const uint32_t sy = (SCREEN_H * 256u) / h;
    uint32_t scale = sx < sy ? sx : sy;
    if (scale == 0) scale = 1;
    lv_image_set_scale(s_res_img, scale);
    lv_obj_set_pos(s_res_img,
                   (SCREEN_W - (int)((uint32_t)w * scale / 256u)) / 2,
                   (SCREEN_H - (int)((uint32_t)h * scale / 256u)) / 2);

    lv_obj_remove_flag(s_res_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_res_panel);
    s_res_on = true;

    bsp_lvgl_unlock();
    return true;
}

void test_ui_resource_hide(void)
{
    if (!s_ready || !s_res_on) return;
    if (!bsp_lvgl_lock(200)) return;
    lv_obj_add_flag(s_res_panel, LV_OBJ_FLAG_HIDDEN);
    // 把 src 摘掉：否则 panel 隐藏后 LVGL 仍可能因缓存失效去读已 unmap 的地址。
    lv_image_set_src(s_res_img, NULL);
    s_res_on = false;
    bsp_lvgl_unlock();
}

// FILE 卡的一行状态。fmt 里的 ASCII 拼好整行文本，超过卡片宽度 LVGL 会
// 自动截断显示，不用自己算字符数。可从任意任务调用（内部持锁），
// 文件传输回调跑在 NimBLE 任务里，直接调它就行。
void test_ui_file_status(const char *text, bool ok)
{
    if (!s_ready || text == NULL) return;
    if (!bsp_lvgl_lock(100)) return;
    lv_obj_set_style_text_color(s_file, lv_color_hex(ok ? C_OK : C_WARN), 0);
    lv_label_set_text(s_file, text);
    bsp_lvgl_unlock();
}

// ---- 投屏自检图案 ----------------------------------------------------------
//
// 不接手机也能验证「解码缓冲 → lv_image → 缩放 → 铺满屏」这条链路：
// 造一张同样尺寸的裸 RGB565 图，走完全相同的 test_ui_mirror_show 入口。
//
// 图案刻意做成四色象限 + 中心棋盘，方向、缩放倍率、色彩通道（RGB565 的
// 字节序）任何一处错了，一眼就看得出来：
//   左上红 / 右上绿 / 左下蓝 / 右下白，中间 40x40 黑白格
bool test_ui_mirror_selftest(void)
{
    if (!s_ready) return false;

    const uint16_t w = 120;
    const uint16_t h = 160;
    const uint32_t bytes = (uint32_t)w * h * 2u;

    uint8_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGW(TAG, "自检图分配失败(%u 字节)", (unsigned)bytes);
        return false;
    }

    // RGB565: r5<<11 | g6<<5 | b5，小端写入（低字节在前），与手机端一致。
    static const uint16_t QUAD[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    const uint16_t chk = 0x0000;
    const uint16_t chk2 = 0xFFFF;

    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            const int q = (y < h / 2 ? 0 : 2) + (x < w / 2 ? 0 : 1);
            uint16_t v = QUAD[q];
            // 正中间的棋盘，跨四个象限，用来确认没有镜像/翻转
            if (x >= w / 2 - 20 && x < w / 2 + 20 && y >= h / 2 - 20 && y < h / 2 + 20) {
                v = (((x / 5) + (y / 5)) & 1) ? chk : chk2;
            }
            const uint32_t o = ((uint32_t)y * w + x) * 2u;
            buf[o] = (uint8_t)(v & 0xFF);
            buf[o + 1] = (uint8_t)(v >> 8);
        }
    }

    const bool ok = test_ui_mirror_show(buf, bytes, w, h, 1);
    heap_caps_free(buf);
    return ok;
}

void test_ui_battery(int soc)
{
    if (!s_ready) return;
    if (!bsp_lvgl_lock(200)) return;

    if (soc < 0) {
        lv_label_set_text(s_batt, "--");
    } else {
        char buf[12];
        snprintf(buf, sizeof(buf), "%d%%", soc);
        lv_label_set_text(s_batt, buf);
        lv_obj_set_style_text_color(s_batt,
                                    lv_color_hex(soc <= 20 ? C_FAIL : C_DIM), 0);
    }
    bsp_lvgl_unlock();
}
