// main/whale_ble.h —— 蓝牙版：手机 App 经 BLE GATT 推送余额。
//
// 与 Wi-Fi 版的分工正好反过来：
//   Wi-Fi 版 —— 设备自己连 Wi-Fi、自己 HTTPS 去问 DeepSeek。
//   蓝牙版   —— 设备只管显示与记账，查询交给手机 App，结果经 BLE 写进来。
//
// 这么换换来三件事：设备不用 Wi-Fi、不用 mbedTLS、不用配网（也就没有
// 「连不上家里 Wi-Fi」这个故障源）；代价是手机要在蓝牙范围内并常驻 App。
//
// 报文格式与 GATT 布局见 docs/development/applications/
// deepseek-balance-whale-bluetooth.md。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 余额三个分项。
//
// 产品固件里这个结构体在 whale_model.h（那个模块管账本、峰谷、配置）。
// 测试固件不链接 whale_model.c —— 它只验证链路，不需要记账 —— 所以字段
// 定义搬到这里，保持与产品报文一致即可。
#define WHALE_CURRENCY_LEN 8

typedef struct {
    int64_t total_cents;      // 总余额
    int64_t granted_cents;    // 赠金
    int64_t topped_cents;     // 充值
    bool available;
    char currency[WHALE_CURRENCY_LEN];
} whale_balance_t;

// 手机那趟查询的结果。
//
// 蓝牙版里设备自己不联网，也就无从判断"为什么没数据"：Wi-Fi 版能靠
// HTTP 状态码分级，这边只能由手机把原因带过来。少了它，设备端只能一律
// 显示"稍后重试"，用户照样不知道该去查 Key 还是等对方恢复。
typedef enum {
    WHALE_BLE_RES_OK = 0,    // 查到了
    WHALE_BLE_RES_AUTH,      // API Key 无效(401 / 403)
    WHALE_BLE_RES_BUSY,      // 服务器侧:429 限流或 5xx
    WHALE_BLE_RES_NET,       // 手机自己没网 / 连不上 DeepSeek
    WHALE_BLE_RES_DATA,      // 拿到了响应但读不出余额
} whale_ble_result_t;

// 手机写进来的一次余额快照。
//
// unix_time 不能省：账本要按北京日期跨日重建基线，而设备没有 Wi-Fi 就
// 没有 SNTP，日期只能由手机带过来。
typedef struct {
    whale_balance_t balance;   // 总额 / 赠金 / 充值三项。分项不能少 ——
                               // 记账靠它区分「自己充的钱」和「平台送的钱」
    uint32_t unix_time;        // UTC 秒
    whale_ble_result_t result; // 结果非 OK 时，balance 的内容无意义
} whale_ble_snapshot_t;

// 收到一次快照时的回调。
//
// 运行在 NimBLE host 任务上下文，回调里别做耗时的事：要更新界面就发消息
// 给主循环，别在这里直接碰 LVGL（跨任务碰 LVGL 必须持锁，见 v28 的教训）。
typedef void (*whale_ble_snapshot_cb_t)(const whale_ble_snapshot_t *snap, void *arg);

// ---- 手机传感器 / 时间 / 投屏（协议见 docs/.../phone-link-protocol.zh_CN.md）----

typedef enum {
    WHALE_SENSOR_ACCEL = 0,     // v[0..2] 单位 1e-3 m/s²
    WHALE_SENSOR_GYRO = 1,      // v[0..2] 单位 1e-3 rad/s
    WHALE_SENSOR_GRAVITY = 2,
    WHALE_SENSOR_LOCATION = 9,  // 纬度/经度 ×1e7，海拔 cm
} whale_ble_sensor_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t flags;
    uint8_t seq;
    uint32_t t_ms;
    int32_t v[3];
    uint32_t aux;      // location: bit0-15 速度 cm/s，bit16-31 航向 0.01°
} whale_ble_sensor_t;

// 帧数据的编码方式。手机端会自己挑：RLE 压得动就用 RLE，压不动（复杂
// 画面会膨胀到 1.5 倍）就退回原样像素 —— 否则一帧直接超过 40 KB 上限，
// 整帧被丢，屏幕上就是"忽然不动了"。
typedef enum {
    WHALE_FRAME_FMT_RLE = 0,   // RGB565 + RLE token 流
    WHALE_FRAME_FMT_RAW = 1,   // 裸 RGB565，小端，逐行紧密排列
} whale_frame_fmt_t;

typedef struct {
    uint16_t w;
    uint16_t h;
    uint16_t seq;
    uint8_t fmt;           // whale_frame_fmt_t
    uint32_t len;
    const uint8_t *data;   // 只在回调期间有效，回调返回后可能被下一帧覆盖
} whale_ble_frame_t;

typedef void (*whale_ble_time_cb_t)(uint32_t unix_sec, int16_t tz_min, void *arg);
typedef void (*whale_ble_sensor_cb_t)(const whale_ble_sensor_t *s, void *arg);
typedef void (*whale_ble_frame_cb_t)(const whale_ble_frame_t *f, void *arg);

// ---- 文件传输（手机 → 设备 Flash，协议复用 16 字节块头）--------------------
//
// 块头字段与投屏帧头对齐，但语义不同：
//   [0] ver  [1] cmd(1=BEGIN 2=DATA 3=END)  [2] slot  [3] 保留
//   [4..7] total_len(u32)  [8..9] idx  [10..11] total_blocks  [12..13] plen
// BEGIN 的净荷 = crc32(u32 LE) + 文件名(12B, NUL 填充)；END 净荷为空。
// idx 从 0(BEGIN) 连续递增到 total-1(END)，断裂即整次作废。

#define WHALE_FILE_NAME_LEN 12

typedef enum {
    WHALE_FILE_MSG_BEGIN = 1,   // name/total_len/crc32 有效
    WHALE_FILE_MSG_DATA  = 2,   // data/len 有效，回调返回后 data 失效
    WHALE_FILE_MSG_END   = 3,   // 设备侧做回读校验，结果经返回值给出
    WHALE_FILE_MSG_LIST  = 4,   // 请求条目列表：回调里逐条调 whale_ble_file_notify
    WHALE_FILE_MSG_DELETE = 5,  // 删除 slot 指定的条目
    WHALE_FILE_MSG_VERIFY = 6,  // 校验 slot：回读重算 CRC，结果经返回值给出
    WHALE_FILE_MSG_COMPACT = 7, // 碎片整理：把有效区段前移，合并空洞
} whale_ble_file_msg_kind_t;

typedef struct {
    uint8_t slot;
    uint32_t total_len;
    uint32_t crc32;
    const uint8_t *data;    // 仅 DATA 有效
    uint32_t len;           // 仅 DATA 有效
    char name[WHALE_FILE_NAME_LEN + 1];
} whale_ble_file_msg_t;

// 返回值 = 这一步是否成功：BEGIN 被接受 / DATA 写入成功 / END 校验通过 /
// DELETE、VERIFY 执行成功。返回 false 时 whale_ble 会 notify 对应错误码。
// 运行在 NimBLE host 任务：flash 写/擦都在这里做，别碰 LVGL（要更新界面
// 就像其它回调一样发消息给主循环；UI 更新接口自己持锁的除外）。
typedef bool (*whale_ble_file_cb_t)(uint8_t kind, const whale_ble_file_msg_t *m,
                                    void *arg);
void whale_ble_set_file_cb(whale_ble_file_cb_t cb, void *arg);

// 设备 → 手机：File 特征上的状态 notify（28 字节：
// ver/status/slot/保留/written(u32)/crc(u32)/total_len(u32)/name[12]）。
// status: 1=接受BEGIN 2=进度 3=完成/校验通过 4=完成但CRC错
//         5=拒绝(槽位/长度非法) 6=序号断裂 7=设备写flash失败
//         8=槽位信息(LIST 逐槽回报) 9=已删除 10=槽位为空
// name/total_len 只在 LIST(8) 里有意义，其它状态填 0。
// 手机没订阅 CCCD 时只返回 ESP_ERR_INVALID_STATE，不影响接收方向。
esp_err_t whale_ble_file_notify(uint8_t status, uint8_t slot,
                                uint32_t written, uint32_t crc,
                                uint32_t total_len, const char *name);

// 三个回调都跑在 NimBLE host 任务里：只做拷贝/入队，别碰 LVGL。
// 可以不注册任何回调 —— 那样对应特征收到数据就直接丢弃。
void whale_ble_set_time_cb(whale_ble_time_cb_t cb, void *arg);
void whale_ble_set_sensor_cb(whale_ble_sensor_cb_t cb, void *arg);
void whale_ble_set_frame_cb(whale_ble_frame_cb_t cb, void *arg);

// 投屏时把连接间隔调快（15–30 ms / latency 0），否则一包一个连接事件，
// 吞吐会被压到十分之一。停投屏后调 false 恢复省电参数。
esp_err_t whale_ble_set_streaming(bool on);

// 启动 BLE。cb 可为 NULL（只想先跑起来看广播时用）。
// 必须在 nvs_flash_init() 之后调用 —— controller 要用 NVS 存 PHY 校准数据。
esp_err_t whale_ble_init(whale_ble_snapshot_cb_t cb, void *arg);

// 当前是否有手机连着。
bool whale_ble_connected(void);

// 广播名(形如 "WhaleBT-3A2B")。要在屏幕上给用户看 —— 他得照着这个名字
// 在手机 App 的扫描列表里找到这一台。whale_ble_init() 之前调用会得到空串。
const char *whale_ble_device_name(void);

// 协商出来的 ATT_MTU（没连接时是默认的 23）。联调时用它判断手机有没有
// 真的谈过 MTU —— 没谈的话投屏分块和 40 字节余额报文都会被截断。
uint16_t whale_ble_mtu(void);

// 投屏写计数：block_count 是 ATT 层收到的块数（任何写都算），
// block_bad 是解析失败/序号断裂被丢弃的块数。
// 手机报"第 N 块写不进去"时，这两个数是判断责任方的分界线：
//   blk 一直是 0        → 包根本没到设备，问题在手机/系统侧
//   blk 涨、bad 也涨    → 包到了但拼不成帧，问题在链路丢包或协议
uint32_t whale_ble_block_count(void);
uint32_t whale_ble_block_bad(void);

// 实际生效的连接参数（不是我们请求的那份）。conn_itvl 单位 1.25 ms，
// supervision_timeout 单位 10 ms，没连接时全为 0。
// 安卓系 ROM 经常拒绝 7.5–15 ms 的短间隔，实际值才是判断"发不出去"的依据。
void whale_ble_conn_params(uint16_t *itvl_units, uint16_t *latency, uint16_t *timeout);

// 重新广播。手机怎么都搜不到时给用户一个可以自己试的动作
// (比"重启设备"温和)。连着手机时改为主动断开,断开事件里会自动重播。
esp_err_t whale_ble_restart_advertising(void);

// 深睡前收尾。必须调:controller 还开着时调制解调器电源域不会掉电,
// 深睡电流会从微安级涨到毫安级 —— 电池供电的挂件睡一晚就没电了。
void whale_ble_prepare_deep_sleep(void);

// 设备 → 手机：请查一次余额（用户按了刷新键时调）。
// 没有手机连着时返回 ESP_ERR_INVALID_STATE。
esp_err_t whale_ble_request_refresh(void);

// ---- 按需挂载（设备发请求，手机送一张过来）----------------------------------
//
// 方向与文件推送正好相反：先由设备说"我要第 N 号资源"，手机再从自己的
// 资源库里取出来、缩放到屏幕尺寸、转成 RGB565，用上面那套 BEGIN/DATA/END
// 推进 request_slot 指定的槽位。
//
// 为什么不把 24MB 整库搬进 flash：storage 只有 4.94MB、BLE 只有 25KB/s，
// 整库要 16 分钟且根本装不下。所以设备端的定位是【缓存窗口】而不是镜像：
// 手机是资源库（容量不受限），设备只有 4 个 1MB 槽位轮着用。
//
// index 是手机侧资源库里的序号，设备不解释它的含义；手机收到后自己映射到
// 相册第 N 张 / 关卡包第 N 个。
esp_err_t whale_ble_request_resource(uint8_t request_slot, uint16_t index);

// 资源写进槽位后的落盘格式（手机按这个拼，设备按这个解析后直接显示）。
//
// 12 字节头 + 紧密排列的 RGB565 小端像素。设备把它整块 mmap 进地址空间
// 交给 LVGL，所以【像素数据必须是文件里连续的、无填充的】—— 有 padding
// 就得额外拷一份，150KB 拷不进 33KB 的堆。
#define WHALE_RES_MAGIC      0x504C4857u   // "WHLP"
#define WHALE_RES_HDR_LEN    12u

// 设备侧解析出来的资源头。
typedef struct {
    uint16_t w;
    uint16_t h;
    uint8_t fmt;          // 0 = RGB565 小端
    uint32_t data_len;    // 头后面的像素字节数
} whale_res_hdr_t;

#ifdef __cplusplus
}
#endif
