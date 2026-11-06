// main/whale_file.h —— 「手机文件 → 设备 Flash」的持久化存储（变长区段分配）。
//
// 分区表里 storage 是【一整块】约 4.94MB 的池子，没预切成固定槽位；
// 怎么切、每个文件放哪，全在 whale_file.c 里按需分配。这样：
//   * 38KB 的照片不再白占 1MB（旧设计 4 个槽位实际只能放 4 张图）；
//   * 大资源能吃到几 MB（旧设计单文件上限就是 1MB）；
//   * 条目数从 4 涨到 16，小文件可以多放几个。
//
// 为什么不用 LittleFS / SPIFFS（很重要，别走回头路）：
//   零拷贝显示靠 esp_partition_mmap 把 flash 映射成内存地址给 LVGL 直读 ——
//   C3 只有 33KB 可用堆，240×320 的 RGB565 整屏是 150KB，拷进内存必然失败。
//   而日志结构文件系统（LittleFS/SPIFFS）不保证文件数据在 flash 上连续，
//   一旦跨了物理页，mmap 出来的就是错的字节。所以必须自己管连续区段。
//
// 分配器的四条硬约束：
//   1. 一个文件必须物理连续；
//   2. 区段起点按 4KB 扇区对齐（flash 擦除粒度）；
//   3. mmap 的 offset 要按 64KB MMU 页对齐 —— 映射时向前取整，返回指针
//      加上段内偏移，对上层透明；
//   4. 元数据只在校验通过后落 NVS，所以「表里有名字」==「内容可信」。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 文件条目上限（索引 0..15）。协议里 slot 是 u8，空间够，这里只是内存表的尺寸。
#define WHALE_FILE_SLOT_COUNT  16u
#define WHALE_FILE_NAME_LEN    12u               // 文件名上限（不含结尾 NUL）
#define WHALE_FILE_ALIGN       4096u             // 区段对齐 = 扇区大小

// 某个索引当前存了什么（来自 NVS 元数据，读取很快）。
typedef struct {
    uint32_t len;      // 字节数
    uint32_t crc32;    // 写入时校验通过的 CRC32
    uint32_t off;      // 在 storage 分区内的起始偏移（mmap/调试用）
    char name[WHALE_FILE_NAME_LEN + 1];
} whale_file_info_t;

// 找 storage 分区、把 NVS 里的区段表读进内存。必须在 app_main 里调一次。
// 失败（分区不存在）只影响文件功能，不影响 BLE 其它特征。
esp_err_t whale_file_init(void);

// 该索引是否存有有效文件；out 可为 NULL（只判断存在性）。
bool whale_file_slot_info(uint8_t index, whale_file_info_t *out);

// 整个池子里还能用的字节数（已扣碎片：返回的是「当前能一次分配出的最大空闲段」）。
uint32_t whale_file_free_bytes(void);

// 已用条目数。
uint8_t whale_file_used_count(void);

// ---- 一次传输的三步 ---------------------------------------------------------
// begin/data/end 全部运行在 NimBLE host 任务上下文（由 whale_ble 回调进来）：
// flash 写/擦会短暂阻塞该任务，但连接监督超时是 6 秒，4KB 擦除的几十毫秒
// 完全在安全范围内。中间任何一步失败，手机重发 BEGIN 即可重新开始。

// 校验参数并分配区段。名字存进 NVS 要等校验通过才落盘 ——
// 校验失败的传输不该在区段表里留名字。
//
// 复用规则：该索引已有文件且新长度不大于旧长度时【原地覆盖】，
// 不搬数据也不重新分配；否则先释放旧区段再按 first-fit 找新位置。
// 这样反复刷新同一张图不会把池子搅成碎片。
esp_err_t whale_file_begin(uint8_t index, uint32_t total_len, uint32_t crc32,
                           const char *name);

// 顺序写入一块（边写边擦：写入游标跨进未擦除的 4KB 扇区时先擦那一扇区，
// 把擦除时间摊到整次传输里，避免 begin 一次擦几百 KB 卡死 BLE）。
esp_err_t whale_file_data(const uint8_t *data, uint32_t len);

// 从 flash 读回全部字节重算 CRC32（真校验，能抓位翻转/写偏移）。
// *crc_ok 表示内容对不对；返回值表示流程本身有没有出错。
esp_err_t whale_file_end(bool *crc_ok);

// 放弃当前传输（手机断开/重发 BEGIN 时调用），已写内容作废。
// 注意：如果本次是「原地覆盖」，旧文件也已经被破坏了 —— 会顺手清掉它的元数据。
void whale_file_abort(void);

// 当前传输已写入的字节数（UI 进度显示用）。
uint32_t whale_file_written(void);

// ---- 区段管理 ---------------------------------------------------------------

// 删除该索引的文件：擦掉它占的扇区 + 清 NVS 元数据。索引为空也返回 OK（幂等）。
esp_err_t whale_file_erase_slot(uint8_t index);

// 对已存内容做回读校验（读全部字节重算 CRC，与元数据里的比对）。
// *crc_ok 表示内容是否完好。索引为空返回 ESP_ERR_NOT_FOUND。
esp_err_t whale_file_verify_slot(uint8_t index, bool *crc_ok);

// 碎片整理：把有效区段按偏移顺序往前挪，合并所有空洞。
//
// 为什么需要它：删掉一个夹在中间的大文件后，那块空间和「末尾空闲」不连续，
// 想再存一个更大的文件就会失败 —— 明明总量够。整理一次就能解决。
//
// 代价：要按 4KB 逐块读+写，搬 1MB 约 1~2 秒，期间 BLE 回调会被阻塞，
// 所以只在手机明确发命令时做，别自动触发。
// 有区段正被 mmap 时返回 ESP_ERR_INVALID_STATE —— 先 unmap 再来。
esp_err_t whale_file_compact(void);

// ---- 零拷贝读取（显示照片这类大块内容必需）----------------------------------
//
// C3 没有 PSRAM，BLE 常驻后可用堆只剩三十来 KB，而 240×320 的 RGB565 整屏
// 就是 150KB —— 拷进内存这条路走不通。但 flash 是【可以直接寻址】的：
// esp_partition_mmap 把区段映射进 DROM 地址空间（0x3C000000 起），拿到的是
// 一个普通 const 指针，LVGL 当图像源直接读，一个字节的内存都不占。
//
// 代价：映射期间那一块 flash 不能被擦写（擦除会立刻让映射内容变垃圾），
// 所以显示中的区段不要同时接收新数据 —— 换图时先 unmap 再写。
// 同一时刻只保留一个映射；重复 map 会先自动 unmap 上一个。
esp_err_t whale_file_map_slot(uint8_t index, const uint8_t **out_ptr,
                              uint32_t *out_len);

// 解除映射。没映射时调用是安全的（幂等）。
void whale_file_unmap(void);

#ifdef __cplusplus
}
#endif
