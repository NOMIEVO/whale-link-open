// main/whale_ble.c —— 蓝牙版：NimBLE GATT server，接收手机推送的余额。
//
// 设备做 peripheral（GATT server），手机做 central。链路方向：
//   手机写 Balance 特征 → 设备收到余额 → 记账 + 刷新界面
//   设备 Notify Request 特征 → 手机收到「请查一次」→ 去问 DeepSeek
//
// 广播里带完整设备名与服务 UUID，安卓端可以按 UUID 过滤扫描，
// 不用把周围所有 BLE 设备都扫一遍（后台扫描尤其要这么做）。

#include "whale_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_nimble_hci.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "whale_ble";

// ---- UUID ------------------------------------------------------------------
//
// 私有 128 位 UUID：4f7c000x-1e2d-4a5b-8c3f-9d0e1f2a3b4c
// BLE_UUID128_INIT 是按内存顺序逐字节写的，也就是把上面的字节序整个反过来。
// 手写错了不会报错，只会对不上 —— 改 UUID 时两处要一起改。
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x3f, 0x8c,
    0x5b, 0x4a, 0x2d, 0x1e, 0x01, 0x00, 0x7c, 0x4f);

static const ble_uuid128_t s_chr_balance_uuid = BLE_UUID128_INIT(
    0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x3f, 0x8c,
    0x5b, 0x4a, 0x2d, 0x1e, 0x02, 0x00, 0x7c, 0x4f);

static const ble_uuid128_t s_chr_request_uuid = BLE_UUID128_INIT(
    0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x3f, 0x8c,
    0x5b, 0x4a, 0x2d, 0x1e, 0x03, 0x00, 0x7c, 0x4f);

// 手机 → 设备的三个新特征：时间、传感器、投屏分块。
static const ble_uuid128_t s_chr_time_uuid = BLE_UUID128_INIT(
    0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x3f, 0x8c,
    0x5b, 0x4a, 0x2d, 0x1e, 0x04, 0x00, 0x7c, 0x4f);

static const ble_uuid128_t s_chr_sensor_uuid = BLE_UUID128_INIT(
    0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x3f, 0x8c,
    0x5b, 0x4a, 0x2d, 0x1e, 0x05, 0x00, 0x7c, 0x4f);

static const ble_uuid128_t s_chr_frame_uuid = BLE_UUID128_INIT(
    0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x3f, 0x8c,
    0x5b, 0x4a, 0x2d, 0x1e, 0x06, 0x00, 0x7c, 0x4f);

// 文件传输（手机 → 设备 Flash）。写方向收块，notify 方向回状态。
static const ble_uuid128_t s_chr_file_uuid = BLE_UUID128_INIT(
    0x4c, 0x3b, 0x2a, 0x1f, 0x0e, 0x9d, 0x3f, 0x8c,
    0x5b, 0x4a, 0x2d, 0x1e, 0x07, 0x00, 0x7c, 0x4f);

// ---- 报文 ------------------------------------------------------------------
//
// 手机写进 Balance 特征的内容，全部小端：
//   u8  version(=1)
//   u8  result        0=查到  1=Key 无效  2=服务器忙  3=手机没网  4=数据异常
//   u8  flags         bit0 = is_available
//   u8  reserved      置 0，留着以后加字段不用改版本号
//   s64 total_cents
//   s64 granted_cents
//   s64 topped_cents
//   u32 unix_time     UTC 秒
//   char currency[4]  不补结尾 NUL，按长度截断
// 合计 1 + 1 + 1 + 1 + 8 + 8 + 8 + 4 + 4 = 40 字节。
//
// 40 字节远超默认 ATT_MTU(23) 的单包净荷，所以手机端必须先把 MTU 谈到 64
// 以上（安卓 requestMtu(64) 即可）。谈不下来就要分包，而分包得自己组包
// 去重，不如直接谈大 —— 设备端收到长度不足的报文一律丢弃。
#define WHALE_BLE_VERSION   1u
#define WHALE_BLE_PKT_LEN   40u
#define WHALE_BLE_CURR_LEN  4u

// Request 特征（设备 → 手机）的取值。
//
// 这是设备主动向手机要东西的唯一上行通道，所以指令码都挤在这一个字节里。
// 载荷最多 4 字节：手机端的 GATT 读回调按长度分支处理。
#define WHALE_BLE_REQ_REFRESH  1u
// 按需挂载：[0]=2 [1]=request_slot [2..3]=资源序号(u16 LE)
#define WHALE_BLE_REQ_RESOURCE 2u

static uint64_t rd_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

static uint32_t rd_le32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

// 只认版本号，长度不足直接拒（多半是 MTU 没谈拢被截断了）。
// 金额用 int64 存，报文里也是 int64，不做范围夹取 —— 手机传来的数应该是
// 接口原样抄过来的。
//
// result 只收已知的几种，别的按"数据异常"处理：宁可显示一句含糊的"数据
// 异常"，也不要把未知枚举值直接塞进界面分支。
static bool decode_snapshot(const uint8_t *buf, size_t len, whale_ble_snapshot_t *out)
{
    if (buf == NULL || out == NULL || len < WHALE_BLE_PKT_LEN) return false;
    if (buf[0] != WHALE_BLE_VERSION) return false;

    const uint8_t raw_result = buf[1];
    out->result = (raw_result <= WHALE_BLE_RES_DATA) ? (whale_ble_result_t)raw_result
                                                     : WHALE_BLE_RES_DATA;
    out->balance.available = (buf[2] & 0x01u) != 0;
    out->balance.total_cents = (int64_t)rd_le64(buf + 4);
    out->balance.granted_cents = (int64_t)rd_le64(buf + 12);
    out->balance.topped_cents = (int64_t)rd_le64(buf + 20);
    out->unix_time = rd_le32(buf + 28);

    const size_t cur = WHALE_CURRENCY_LEN - 1;
    const size_t n = cur < WHALE_BLE_CURR_LEN ? cur : WHALE_BLE_CURR_LEN;
    memcpy(out->balance.currency, buf + 32, n);
    out->balance.currency[n] = '\0';
    return true;
}

// ---- 状态 ------------------------------------------------------------------

static whale_ble_snapshot_cb_t s_cb;
static void *s_cb_arg;
static bool s_connected;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
// 协商出来的 ATT_MTU。联调固件要把它显示在自检页上 —— 手机端如果没谈 MTU，
// 40 字节的余额报文和投屏分块都会被截断，这是联调时最常见的失败原因。
// 23 是 BLE 规范的默认 ATT_MTU（连接建立后、谁都没谈之前的值）。
static volatile uint16_t s_mtu = 23;
static uint16_t s_attr_request;   // Request 特征的值句柄，notify 时要用
static uint16_t s_attr_file;     // File 特征的值句柄（状态 notify 用）
static char s_dev_name[16];

void ble_store_config_init(void);   // 由 NimBLE 提供，头文件里没有公开声明

// ---- GATT ------------------------------------------------------------------

// 手机写余额。运行在 host 任务里，所以只做解析 + 回调，不做界面操作。
static int chr_balance_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint8_t buf[64];
    uint16_t len = 0;
    const int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
    if (rc != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    whale_ble_snapshot_t snap;
    if (!decode_snapshot(buf, len, &snap)) {
        ESP_LOGW(TAG, "余额报文无法识别(%u 字节),丢弃", (unsigned)len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    ESP_LOGI(TAG, "收到快照 result=%d 总=%lld 赠=%lld 充=%lld (t=%lu)",
             (int)snap.result,
             (long long)snap.balance.total_cents,
             (long long)snap.balance.granted_cents,
             (long long)snap.balance.topped_cents,
             (unsigned long)snap.unix_time);

    if (s_cb != NULL) s_cb(&snap, s_cb_arg);
    return 0;
}

// Request 特征只用来 notify，读写都没实际内容。
//
// 这里不能顺手记句柄：WRITE_DSC 传进来的是 CCCD 描述符的句柄，而 notify
// 要的是特征值句柄，两者不是同一个。值句柄在 on_sync 里用
// ble_gatts_find_chr() 取，那时候 GATT 表已经注册完了。
static int chr_request_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

// ---- 时间 / 传感器 / 投屏 --------------------------------------------------
//
// 三个特征都只写不读：数据是手机推来的，设备没有回读的需求。
// 解析全部在 host 任务里做，硬件回调里只允许 memcpy —— 上层要界面操作
// 就自己入队（v28 的教训：跨任务碰 LVGL 必须持锁）。

#define WHALE_FRAME_HDR_LEN  16u
// 单帧 RLE 流上限。120×160 的裸像素是 38400 字节，RLE 对照片类内容最坏
// 膨胀 1.5 倍，留到 40 KB 正好接住；再大就不是「压不动」而是分辨率超了。
#define WHALE_FRAME_MAX      40960u
// 解码后的裸像素上限 = 120×160×2。这条线不是随手指的：C3 没有 PSRAM，
// BLE 常驻后可用堆只剩七八十 KB，而解码缓冲和解码后的位图要同时在场。
// 160×213 需要 66.6 KB 的解码缓冲，加上接收缓冲就破了 130 KB —— 一定失败。
// 所以分辨率上限锁在 120×160，App 端的三档也按这条线给。
#define WHALE_PX_MAX         38400u
#define WHALE_WRITE_BUF      600u     // MTU 517 + 头，够一块

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static whale_ble_time_cb_t s_time_cb;
static void *s_time_arg;
static whale_ble_sensor_cb_t s_sensor_cb;
static void *s_sensor_arg;
static whale_ble_frame_cb_t s_frame_cb;
static void *s_frame_arg;
static whale_ble_file_cb_t s_file_cb;
static void *s_file_arg;

// 文件传输的接收状态。s_file_next 是"期待的下一个块 idx"，
// BEGIN=idx0，DATA 从 idx1 连续递增，END=idx total-1。
static bool s_file_active;
static uint16_t s_file_next;
static uint16_t s_file_total;
static uint8_t s_file_slot;
static uint32_t s_file_total_len;   // BEGIN 声明的总长，DATA 回调要带出去算进度
static uint32_t s_file_written;
static uint32_t s_file_crc;         // BEGIN 声明的期望 CRC，END 时回填实算值

static uint8_t *s_rx;
static uint32_t s_rx_cap;
static uint32_t s_rx_len;
static uint16_t s_rx_seq;
static uint16_t s_rx_total;
static uint16_t s_rx_next;
static uint16_t s_rx_w;
static uint16_t s_rx_h;
static uint8_t s_rx_fmt;
static bool s_rx_active;
static bool s_streaming;

// 投屏块计数：收到几块（任何写都算）、被丢弃几块（序号断裂/长度非法）。
// 手机报"第 0 块写不进去"时，这两个数就是分界线：
//   blk 一直 0  → 包没到空口，问题在手机/系统侧
//   blk 涨、bad 也涨 → 包到了但拼不起来，问题在协议或链路丢包
static uint32_t s_blk_rx;
static uint32_t s_blk_bad;

static int chr_time_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint8_t buf[32];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 || len < 8) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (buf[0] != WHALE_BLE_VERSION) return 0;

    const int16_t tz_min = (int16_t)rd_le16(buf + 2);
    const uint32_t unix_sec = rd_le32(buf + 4);
    // 2023-11 之前的时间戳一定是没填的零值，宁可不认，否则账本日期会跳到 1970
    if (unix_sec < 1700000000u) return 0;

    if (s_time_cb != NULL) s_time_cb(unix_sec, tz_min, s_time_arg);
    return 0;
}

static int chr_sensor_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    uint8_t buf[32];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (len < 24 || buf[0] != WHALE_BLE_VERSION) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    whale_ble_sensor_t s = { 0 };
    s.kind = buf[1];
    s.flags = buf[2];
    s.seq = buf[3];
    s.t_ms = rd_le32(buf + 4);
    s.v[0] = (int32_t)rd_le32(buf + 8);
    s.v[1] = (int32_t)rd_le32(buf + 12);
    s.v[2] = (int32_t)rd_le32(buf + 16);
    s.aux = rd_le32(buf + 20);

    if ((s.flags & 0x01u) && s_sensor_cb != NULL) s_sensor_cb(&s, s_sensor_arg);
    return 0;
}

// 投屏分块：idx=0 开始新帧，idx+1==total 结束。
// 手机用 write-no-response 连发，ATT 层保序，所以只校验 idx 递增即可。
static int chr_frame_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    s_blk_rx++;     // 只要 ATT 层把写送到这儿就算收到了

    static uint8_t buf[WHALE_WRITE_BUF];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 || len <= WHALE_FRAME_HDR_LEN) {
        s_blk_bad++;
        return 0;
    }
    if (buf[0] != WHALE_BLE_VERSION) {
        s_blk_bad++;
        return 0;
    }

    const uint16_t w = rd_le16(buf + 4);
    const uint16_t h = rd_le16(buf + 6);
    const uint16_t seq = rd_le16(buf + 8);
    const uint16_t idx = rd_le16(buf + 10);
    const uint16_t total = rd_le16(buf + 12);
    const uint16_t plen = rd_le16(buf + 14);
    const uint8_t fmt = buf[2];

    if (plen == 0 || (uint32_t)plen + WHALE_FRAME_HDR_LEN > len) {
        s_blk_bad++;
        return 0;
    }

    if (idx == 0) {
        if (w == 0 || h == 0 || total == 0) return 0;
        const uint32_t px_bytes = (uint32_t)w * (uint32_t)h * 2u;
        // 这里必须用「解码后的裸像素」判断，不能用 RLE 流长度 —— 一帧 160×213
        // 的 RLE 流可能只有 5 KB（看着完全放得下 40 KB 缓冲），但解码要 66 KB，
        // 那才是真正会爆的地方。踩过的坑：只按缓冲长度放行，帧被收下、解码
        // 分配失败，界面上什么都不出，串口也只有一句糊话。
        if (px_bytes > WHALE_PX_MAX) {
            ESP_LOGW(TAG, "投屏 %ux%u 超出解码上限(%u 字节，最大 120x160)",
                     (unsigned)w, (unsigned)h, (unsigned)WHALE_PX_MAX);
            s_blk_bad++;
            return 0;
        }
        // 缓冲装的是**压缩后**的 RLE 流，不是裸像素。RLE 对照片类内容最坏会
        // 膨胀到 1.5 倍，所以不能按 px_bytes 分配 —— 统一按协议上限给，一次到位。
        if (s_rx_cap < WHALE_FRAME_MAX) {
            uint8_t *p = realloc(s_rx, WHALE_FRAME_MAX);
            if (p == NULL) {
                ESP_LOGW(TAG, "投屏缓冲分配失败(%u 字节)，当前可用堆不足", (unsigned)WHALE_FRAME_MAX);
                return 0;
            }
            s_rx = p;
            s_rx_cap = WHALE_FRAME_MAX;
        }
        s_rx_len = 0;
        s_rx_seq = seq;
        s_rx_total = total;
        s_rx_next = 0;
        s_rx_w = w;
        s_rx_h = h;
        s_rx_fmt = fmt;
        s_rx_active = true;
    } else if (!s_rx_active || seq != s_rx_seq || idx != s_rx_next) {
        // 中间块丢了就整帧丢弃，等下一帧 —— 半张图显示出来只会更奇怪
        s_rx_active = false;
        s_blk_bad++;
        return 0;
    }

    if (s_rx_len + plen > s_rx_cap) {
        s_rx_active = false;
        s_blk_bad++;
        return 0;
    }
    memcpy(s_rx + s_rx_len, buf + WHALE_FRAME_HDR_LEN, plen);
    s_rx_len += plen;
    s_rx_next = (uint16_t)(idx + 1u);

    if (s_rx_next >= s_rx_total) {
        s_rx_active = false;
        if (s_frame_cb != NULL) {
            whale_ble_frame_t f = {
                .w = s_rx_w,
                .h = s_rx_h,
                .seq = s_rx_seq,
                .fmt = s_rx_fmt,
                .len = s_rx_len,
                .data = s_rx,
            };
            s_frame_cb(&f, s_frame_arg);
        }
    }

    // 收到投屏就提速；app_task 里 5 秒没新帧会切回省电参数
    if (!s_streaming) whale_ble_set_streaming(true);
    return 0;
}

// 文件分块。idx 断裂/长度不符 → 整次作废并 notify 错误码，
// 手机从 BEGIN 重来即可（设备端 whale_file 会被新的 BEGIN 复位游标）。
// BEGIN 的接受与否、DATA 的落盘、END 的回读校验都交给注册方回调决定，
// 这里只管协议层：序号、状态机、notify。
static int chr_file_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;

    static uint8_t buf[WHALE_WRITE_BUF];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0
        || len <= WHALE_FRAME_HDR_LEN) {
        return 0;
    }
    if (buf[0] != WHALE_BLE_VERSION) return 0;

    const uint8_t cmd = buf[1];
    const uint8_t slot = buf[2];
    const uint32_t total_len = rd_le32(buf + 4);
    const uint16_t idx = rd_le16(buf + 8);
    const uint16_t total = rd_le16(buf + 10);
    const uint16_t plen = rd_le16(buf + 12);
    if ((uint32_t)plen + WHALE_FRAME_HDR_LEN > len) return 0;

    // 收到任何文件写都先把连接参数拉到快档 —— 文件传输和投屏一样吃吞吐。
    if (!s_streaming) whale_ble_set_streaming(true);

    if (cmd == 1) {   // BEGIN
        if (plen < 4 + 1 || total == 0) {
            whale_ble_file_notify(5, slot, 0, 0, 0, NULL);
            return 0;
        }
        whale_ble_file_msg_t m = { 0 };
        m.slot = slot;
        m.total_len = total_len;
        m.crc32 = rd_le32(buf + WHALE_FRAME_HDR_LEN);
        const uint8_t *nm = buf + WHALE_FRAME_HDR_LEN + 4;
        const size_t nm_len = plen - 4 < WHALE_FILE_NAME_LEN ? plen - 4
                                                             : WHALE_FILE_NAME_LEN;
        memcpy(m.name, nm, nm_len);
        m.name[nm_len] = '\0';

        const bool ok = s_file_cb != NULL && s_file_cb(WHALE_FILE_MSG_BEGIN, &m, s_file_arg);
        if (!ok) {
            s_file_active = false;
            whale_ble_file_notify(5, slot, 0, 0, 0, NULL);
            return 0;
        }
        s_file_active = true;
        s_file_next = 1;
        s_file_total = total;
        s_file_slot = slot;
        s_file_total_len = total_len;
        s_file_written = 0;
        s_file_crc = m.crc32;
        whale_ble_file_notify(1, slot, 0, 0, 0, NULL);
        return 0;
    }

    if (cmd == 2) {   // DATA
        if (!s_file_active || idx != s_file_next) {
            s_file_active = false;
            whale_ble_file_notify(6, slot, s_file_written, 0, 0, NULL);
            return 0;
        }
        whale_ble_file_msg_t m = {
            .slot = slot,
            .total_len = s_file_total_len,
            .data = buf + WHALE_FRAME_HDR_LEN,
            .len = plen,
        };
        if (s_file_cb == NULL || !s_file_cb(WHALE_FILE_MSG_DATA, &m, s_file_arg)) {
            s_file_active = false;
            whale_ble_file_notify(7, slot, s_file_written, 0, 0, NULL);
            return 0;
        }
        s_file_written += plen;
        s_file_next = (uint16_t)(idx + 1u);
        // 每 32 块报一次进度，让手机端不至于对着黑盒等几十秒。
        if ((s_file_next & 0x1Fu) == 0) {
            whale_ble_file_notify(2, slot, s_file_written, 0, 0, NULL);
        }
        return 0;
    }

    if (cmd == 3) {   // END
        if (!s_file_active) {
            whale_ble_file_notify(6, slot, s_file_written, 0, 0, NULL);
            return 0;
        }
        s_file_active = false;
        whale_ble_file_msg_t m = {
            .slot = s_file_slot,
            .total_len = s_file_total_len,
            .crc32 = s_file_crc,
            .len = s_file_written,
        };
        const bool ok = s_file_cb != NULL && s_file_cb(WHALE_FILE_MSG_END, &m, s_file_arg);
        whale_ble_file_notify(ok ? 3 : 4, s_file_slot, s_file_written, s_file_crc, 0, NULL);
        return 0;
    }

    // ---- 条目管理：这几个不是传输状态机的成员，直接派发给回调。
    // LIST 由回调逐条调 whale_ble_file_notify(8,...)，空条目回报(10)。
    // COMPACT 没有 slot 语义，slot 字段填 0。
    if (cmd == 4 || cmd == 5 || cmd == 6 || cmd == 7) {
        whale_ble_file_msg_t m = { .slot = slot };
        const uint8_t kind = cmd == 4 ? WHALE_FILE_MSG_LIST
                           : cmd == 5 ? WHALE_FILE_MSG_DELETE
                           : cmd == 6 ? WHALE_FILE_MSG_VERIFY
                                      : WHALE_FILE_MSG_COMPACT;
        if (s_file_cb != NULL) s_file_cb(kind, &m, s_file_arg);
        return 0;
    }

    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_chr_balance_uuid.u,
                .access_cb = chr_balance_access,
                // 只写不读：余额是手机推过来的，设备没有自己的查询能力。
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_chr_request_uuid.u,
                .access_cb = chr_request_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                // CCCD:手机靠写它来订阅/退订 notify。描述符的权限字段叫
                // att_flags,不是 characteristics 那个 flags —— 名字很像,
                // 写错只会换来一句"has no member named"。
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16),
                        .access_cb = chr_request_access,
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
                    },
                    { 0 },
                },
            },
            {
                .uuid = &s_chr_time_uuid.u,
                .access_cb = chr_time_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_chr_sensor_uuid.u,
                .access_cb = chr_sensor_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_chr_frame_uuid.u,
                .access_cb = chr_frame_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_chr_file_uuid.u,
                .access_cb = chr_file_access,
                // 写方向收块；notify 方向回状态（接受/进度/校验结果）。
                // 校验结果必须走 notify：手机本地算的 CRC 对，不代表
                // 设备落盘的内容对，只有回读校验的结论才是真的。
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
                       | BLE_GATT_CHR_F_NOTIFY,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16),
                        .access_cb = chr_file_access,
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
                    },
                    { 0 },
                },
            },
            { 0 },
        },
    },
    { 0 },
};

static int gatt_svr_init(void)
{
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) return rc;

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    return rc;
}

// ---- GAP -------------------------------------------------------------------

// start_advertise() 在 gap_event() 之前定义，先声明一下。
static int gap_event(struct ble_gap_event *event, void *arg);

static void start_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    int rc;

    memset(&fields, 0, sizeof fields);
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)s_dev_name;
    fields.name_len = strlen(s_dev_name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置广播数据失败 rc=%d", rc);
        return;
    }

    // 128 位服务 UUID 必须放扫描响应，不能塞进广播包。
    //
    // 31 字节的广播预算怎么算都不够：flags 3 + 名字 12 的 AD 结构 14 + UUID128
    // 的 AD 结构 18 = 35 → ble_gap_adv_set_fields() 直接返回 BLE_HS_EMSGSIZE(4)，
    // 然后 start_advertise() 提前 return，**设备一声不响地不广播**，手机自然搜不到。
    // 这个错在日志里只有一行 rc=4，很容易被"已在广播"那句误报盖过去。
    // 把 UUID 挪到 scan response 后：广播 17 字节、响应 18 字节，两边都合法，
    // 安卓会把 adv 与 scan rsp 合并成一个 ScanRecord，过滤照样有效。
    memset(&rsp_fields, 0, sizeof rsp_fields);
    rsp_fields.uuids128 = &s_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "设置扫描响应失败 rc=%d(手机将按名字过滤)", rc);
    }

    memset(&adv_params, 0, sizeof adv_params);
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    // 未连接时用慢一点的广播间隔省电；连上后由连接参数决定功耗。
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(300);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(400);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "启动广播失败 rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "开始广播,名字=%s", s_dev_name);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        // 连上或没连上都会来这个事件，靠 status 区分。
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "手机已连接 handle=%u", (unsigned)s_conn_handle);

            // 连接参数：一上来就给投屏档。
            //
            // 以前这里给的是省电档（100–200 ms、latency=4），本意是"没投屏时
            // 别费电"，可投屏档要等收到第一帧才切 —— 而第一帧恰恰是最需要
            // 快速间隔的那一帧：120×160 的帧拆 78 块，每块等一个连接事件，
            // 200 ms × 78 = 15.6 秒，手机端 2.5 秒就放弃了。于是第一帧永远
            // 发不完，永远收不到帧，也就永远不会切快档 —— 死锁。
            //
            // 这是联调固件，功耗不在考虑范围内；就算后面切回"省电档"，
            // 那一档也保持在 30–60 ms，不再回到百毫秒级。
            struct ble_gap_upd_params params = {
                .itvl_min = BLE_GAP_CONN_ITVL_MS(7.5),
                .itvl_max = BLE_GAP_CONN_ITVL_MS(15),
                .latency = 0,
                .supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(6000),
                .min_ce_len = 0,
                .max_ce_len = BLE_GAP_CONN_ITVL_MS(15),
            };
            const int rc = ble_gap_update_params(s_conn_handle, &params);
            if (rc != 0) {
                ESP_LOGW(TAG, "更新连接参数失败 rc=%d(保持默认)", rc);
            }

            // 谈 2M PHY。手机侧也会主动 setPreferredPhy，两边谁先发起都行，
            // 但设备主动发一次能保证不走默认的 1M —— 投屏时这一个设置差一倍速率。
            // 第四个参数是 coded PHY 的选项（S2/S8），走 2M 时给 0 就行。
            const int prc = ble_gap_set_prefered_le_phy(
                s_conn_handle, BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK,
                BLE_GAP_LE_PHY_CODED_ANY);
            if (prc != 0) {
                ESP_LOGW(TAG, "请求 2M PHY 失败 rc=%d(继续用 1M)", prc);
            }
        } else {
            s_connected = false;
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGW(TAG, "连接失败 status=%d,继续广播", event->connect.status);
            start_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_rx_active = false;
        s_file_active = false;   // 传输中途断开：手机重连后从 BEGIN 重来
        s_streaming = false;
        s_mtu = 23;
        ESP_LOGI(TAG, "手机断开(%d),继续广播", event->disconnect.reason);
        start_advertise();
        return 0;

    case BLE_GAP_EVENT_MTU:
        // 手机主动谈 MTU 时来这个事件。数值超过我们配置的 128 说明手机更强，
        // 但对端实际按 min(两者) 用，这里记的是协商结果。
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU 协商结果 %u", (unsigned)s_mtu);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        // BLE_HS_FOREVER 不会到这里；留着是以防改成定时广播。
        start_advertise();
        return 0;

    default:
        return 0;
    }
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE 就绪");

    // 到这里 GATT 表才注册完，可以查出 Request 特征的值句柄。
    // 查不到就只是发不出「请刷新」的 notify，收余额不受影响。
    uint16_t def_handle = 0;
    uint16_t val_handle = 0;
    if (ble_gatts_find_chr(&s_svc_uuid.u, &s_chr_request_uuid.u,
                           &def_handle, &val_handle) == 0 && val_handle != 0) {
        s_attr_request = val_handle;
        ESP_LOGI(TAG, "Request 特征值句柄=%u", (unsigned)s_attr_request);
    } else {
        ESP_LOGW(TAG, "没找到 Request 特征,notify 发不出去");
    }

    val_handle = 0;
    if (ble_gatts_find_chr(&s_svc_uuid.u, &s_chr_file_uuid.u,
                           &def_handle, &val_handle) == 0 && val_handle != 0) {
        s_attr_file = val_handle;
        ESP_LOGI(TAG, "File 特征值句柄=%u", (unsigned)s_attr_file);
    } else {
        ESP_LOGW(TAG, "没找到 File 特征,文件状态 notify 发不出去");
    }

    start_advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset(%d)", reason);
}

static void host_task(void *param)
{
    (void)param;
    // 只有 nimble_port_stop() 才会返回。
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---- 对外接口 --------------------------------------------------------------

esp_err_t whale_ble_init(whale_ble_snapshot_cb_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_attr_request = 0;
    s_attr_file = 0;

    // 设备名带 MAC 尾号，多台挂件放在一起也分得清。
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_dev_name, sizeof(s_dev_name), "WhaleBT-%02X%02X", mac[4], mac[5]);

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败 %d", ret);
        return ret;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // 安全：要求加密链路。余额数字不算高敏，但如果不要求加密，
    // 周围任何设备都能随手把你的余额改成任意一个数。
    // 用 Just Works 的 LE Secure Connections —— 不用在设备上输 PIN。
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;

    int rc = gatt_svr_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT 服务注册失败 %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(s_dev_name);
    if (rc != 0) {
        ESP_LOGW(TAG, "设备名设置失败 %d", rc);
    }

    ble_store_config_init();

    // 广播在 on_sync 里启动（那时 host 才就绪），这里只把 host 任务跑起来。
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool whale_ble_connected(void)
{
    return s_connected;
}

const char *whale_ble_device_name(void)
{
    return s_dev_name;
}

uint16_t whale_ble_mtu(void)
{
    return s_mtu;
}

// 联调用的两个探针：投屏写到底是"没到设备"还是"到了但拼不起来"，
// 这两条数据能一眼分清责任方。手机端写不进去时，先看 blk 有没有涨。
uint32_t whale_ble_block_count(void)
{
    return s_blk_rx;
}

uint32_t whale_ble_block_bad(void)
{
    return s_blk_bad;
}

void whale_ble_conn_params(uint16_t *itvl_units, uint16_t *latency, uint16_t *timeout)
{
    if (itvl_units != NULL) *itvl_units = 0;
    if (latency != NULL) *latency = 0;
    if (timeout != NULL) *timeout = 0;
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_gap_conn_desc d;
    memset(&d, 0, sizeof d);
    if (ble_gap_conn_find(s_conn_handle, &d) != 0) return;
    // conn_itvl 单位 1.25 ms，supervision_timeout 单位 10 ms —— 原样给出去，
    // 打印时再换算，免得两头各算一遍容易错。
    if (itvl_units != NULL) *itvl_units = d.conn_itvl;
    if (latency != NULL) *latency = d.conn_latency;
    if (timeout != NULL) *timeout = d.supervision_timeout;
}

esp_err_t whale_ble_restart_advertising(void)
{
    if (s_connected) {
        // 连着的时候"重新广播"没有意义(连接期间本来就不广播),按用户
        // 的真实意图理解成"断开重来":断开事件里会自动重新开始广播。
        ESP_LOGI(TAG, "已连接,改为先断开再重播");
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        return ESP_OK;
    }

    const int rc = ble_gap_adv_stop();
    // 压根没在广播也会返回 EALREADY / ENOENT,那不算失败。
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "停止广播失败 rc=%d", rc);
        return ESP_FAIL;
    }
    start_advertise();
    return ESP_OK;
}

void whale_ble_prepare_deep_sleep(void)
{
    // 顺序不能反:先停广播 → 停 host 任务 → 释放 host → 释放 HCI → 关 controller。
    // host 还在跑的时候关 controller,controller 会在下一次下发 HCI 命令时
    // 撞上已经释放的传输层。
    const int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "停止广播失败 rc=%d(仍会继续收尾)", rc);
    }

    // 这一步会阻塞等 host 任务退出。它和本函数都跑在普通任务上下文,
    // 不会自锁;万一没退出,深睡也只是晚一点发生。
    if (nimble_port_stop() != 0) {
        ESP_LOGW(TAG, "host 任务停止未确认(仍会继续收尾)");
    }
    if (nimble_port_deinit() != ESP_OK) ESP_LOGW(TAG, "host 释放未确认");
    if (esp_nimble_hci_deinit() != ESP_OK) ESP_LOGW(TAG, "HCI 释放未确认");
    if (esp_bt_controller_disable() != ESP_OK) ESP_LOGW(TAG, "controller 关闭未确认");

    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGI(TAG, "蓝牙已收尾,深睡后手机会看到本设备消失(唤醒后自动重播)");
}

// Request 特征上的上行通知。所有「设备要手机做点什么」都走这里。
// 返回前会把 om 交给协议栈，调用方不用管释放。
static esp_err_t notify_request(const uint8_t *payload, uint16_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_attr_request == 0) {
        ESP_LOGW(TAG, "Request 句柄还没拿到(手机可能没订阅 CCCD)");
        return ESP_ERR_INVALID_STATE;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, len);
    if (om == NULL) return ESP_ERR_NO_MEM;

    const int rc = ble_gatts_notify_custom(s_conn_handle, s_attr_request, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify 失败 rc=%d", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t whale_ble_request_refresh(void)
{
    const uint8_t code = WHALE_BLE_REQ_REFRESH;
    return notify_request(&code, sizeof(code));
}

esp_err_t whale_ble_request_resource(uint8_t request_slot, uint16_t index)
{
    const uint8_t pkt[4] = {
        (uint8_t)WHALE_BLE_REQ_RESOURCE,
        request_slot,
        (uint8_t)(index & 0xFFu),
        (uint8_t)(index >> 8),
    };
    const esp_err_t err = notify_request(pkt, sizeof(pkt));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "已向手机要资源 #%u -> 槽位%u", (unsigned)index,
                 (unsigned)request_slot);
    }
    return err;
}

// ---- 新回调与连接参数 ------------------------------------------------------

void whale_ble_set_time_cb(whale_ble_time_cb_t cb, void *arg)
{
    s_time_cb = cb;
    s_time_arg = arg;
}

void whale_ble_set_sensor_cb(whale_ble_sensor_cb_t cb, void *arg)
{
    s_sensor_cb = cb;
    s_sensor_arg = arg;
}

void whale_ble_set_frame_cb(whale_ble_frame_cb_t cb, void *arg)
{
    s_frame_cb = cb;
    s_frame_arg = arg;
}

void whale_ble_set_file_cb(whale_ble_file_cb_t cb, void *arg)
{
    s_file_cb = cb;
    s_file_arg = arg;
}

// File 特征上的状态 notify。包是 28 字节定长小端：
//   [0] ver [1] status [2] slot [3] 保留
//   [4..7] written [8..11] crc [12..15] total_len [16..27] name(NUL 填充)
// name/total_len 只有 LIST(8) 用，其它状态传 NULL/0 即可。
esp_err_t whale_ble_file_notify(uint8_t status, uint8_t slot,
                                uint32_t written, uint32_t crc,
                                uint32_t total_len, const char *name)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_attr_file == 0) return ESP_ERR_INVALID_STATE;

    uint8_t pkt[28] = { 0 };
    pkt[0] = WHALE_BLE_VERSION;
    pkt[1] = status;
    pkt[2] = slot;
    pkt[4] = (uint8_t)(written);
    pkt[5] = (uint8_t)(written >> 8);
    pkt[6] = (uint8_t)(written >> 16);
    pkt[7] = (uint8_t)(written >> 24);
    pkt[8] = (uint8_t)(crc);
    pkt[9] = (uint8_t)(crc >> 8);
    pkt[10] = (uint8_t)(crc >> 16);
    pkt[11] = (uint8_t)(crc >> 24);
    pkt[12] = (uint8_t)(total_len);
    pkt[13] = (uint8_t)(total_len >> 8);
    pkt[14] = (uint8_t)(total_len >> 16);
    pkt[15] = (uint8_t)(total_len >> 24);
    if (name != NULL) {
        strlcpy((char *)pkt + 16, name, WHALE_FILE_NAME_LEN + 1);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, sizeof(pkt));
    if (om == NULL) return ESP_ERR_NO_MEM;
    // 手机没订阅 CCCD 时这里会失败，属于正常情况（单向传输也能用），
    // 只是少了校验结果的回报。
    const int rc = ble_gatts_notify_custom(s_conn_handle, s_attr_file, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

// 投屏要的是每连接事件多塞几个包。默认那套 100–200 ms / latency 4 是为省电
// 定的，一包就要等一个连接间隔，48 块的一帧能拖到四秒以上。
esp_err_t whale_ble_set_streaming(bool on)
{
    s_streaming = on;
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    struct ble_gap_upd_params params = {
        // 省电档也别退太狠：投屏一停就切回百毫秒级，下一次有画面时又要花
        // 好几秒才谈得回快速档（连接参数更新要几个连接事件才生效）。
        // 30–60 ms 足够省电，也还能让一帧在 2~3 秒内传完。
        .itvl_min = on ? BLE_GAP_CONN_ITVL_MS(7.5) : BLE_GAP_CONN_ITVL_MS(30),
        .itvl_max = on ? BLE_GAP_CONN_ITVL_MS(15) : BLE_GAP_CONN_ITVL_MS(60),
        .latency = 0,
        .supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(on ? 6000 : 8000),
        // min_ce_len 给 0 = 不要求最短事件长度；max_ce_len 拉到间隔上限，
        // 让控制器在一个连接事件里能连发多个包，而不是发完一个就等下一轮。
        .min_ce_len = 0,
        .max_ce_len = BLE_GAP_CONN_ITVL_MS(on ? 15 : 60),
    };

    const int rc = ble_gap_update_params(s_conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "切换连接参数失败 rc=%d", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "连接参数切到%s模式", on ? "投屏(15–30ms)" : "省电(100–200ms)");
    return ESP_OK;
}
