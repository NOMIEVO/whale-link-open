// main/whale_file.c —— 变长区段分配器（一个大池子，按需切）。
//
// 布局：整个 storage 分区就是一个池子，没有预切槽位。每存一个文件就在池子里
// 找一段连续空间，起点按 4KB（扇区）对齐。段内就是裸字节流，没有头部 ——
// 设备读资源时 offset 就是文件内偏移，不用再剥壳。
//
// 元数据（偏移/长度/CRC/名字）整张表存在 NVS 的一个 blob 里，只在
// 【校验通过】和【删除】时落盘，所以「表里有名字」==「内容可信」。
// 每次 DATA 块都写 NVS 会把 24KB 的 nvs 分区很快写满并触发垃圾回收，
// 那是几十毫秒级的卡顿，绝不能放在传输路径上。
//
// 为什么是 first-fit 而不是别的：条目最多 16 个，线性扫一遍几微秒就完了，
// 复杂度换不来任何收益。真正要防的是碎片 —— 靠 compact() 手动整理。
#include "whale_file.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "nvs_flash.h"
#include "nvs.h"
#define TAG "whale_file"

#define NVS_NS         "whalefile"
#define NVS_KEY_TABLE  "seg"       // 整张区段表一个 blob
#define SECTOR_SZ      WHALE_FILE_ALIGN
#define MMU_PAGE       0x10000u    // C3 的 MMU 页 64KB，mmap 的 offset 必须按它对

// magic 从 WHF1 改到 WHF2：旧版本是 4 个独立的 f0..f3 blob，语义完全不同，
// 换 magic 能让它们自动失效，不用写迁移代码（测试工程，数据丢了重新推）。
#define TABLE_MAGIC  0x57484632u   // "WHF2"

#define SEG_FREE      0u
#define SEG_USED      1u

typedef struct {
    uint32_t off;      // 分区内起始偏移（4KB 对齐）
    uint32_t len;      // 实际字节数
    uint32_t crc32;
    uint32_t flags;    // SEG_FREE / SEG_USED
    char name[WHALE_FILE_NAME_LEN + 1];
} seg_t;

typedef struct {
    uint32_t magic;
    uint32_t count;    // = WHALE_FILE_SLOT_COUNT，版本不匹配就当空表
    seg_t seg[WHALE_FILE_SLOT_COUNT];
} seg_table_t;

static const esp_partition_t *s_part;
static seg_table_t s_tab;

// 当前是否有一段 flash 正被 mmap。放在文件顶部是因为 compact() 也要看它 ——
// 映射期间搬数据会让 LVGL 读到半新半旧的字节。
static const void *s_map_ptr;
static uint32_t s_mapped_len;
static esp_partition_mmap_handle_t s_map_handle;

// 当前传输的游标。s_active 之前它们没有意义。
static bool s_active;
static uint8_t s_index;
static uint32_t s_off;          // 本次写入的区段起点
static uint32_t s_total_len;
static uint32_t s_expected_crc;
static char s_name[WHALE_FILE_NAME_LEN + 1];
static uint32_t s_written;
static uint32_t s_erased;       // 已擦除字节（扇区对齐）
static bool s_had_old;          // 该索引本来有旧文件（内容即将被毁）

// 校验回读缓冲。512B 放静态区，避免 NimBLE host 任务的栈顶压力。
static uint8_t s_verify_buf[512];

// 碎片整理的搬运缓冲。2KB 是速度与 RAM 的折中：搬 1MB 要 512 次
// 擦+读+写，实测 1~2 秒；换成 4KB 快一倍但多吃 2KB 静态 RAM。
static uint8_t s_move_buf[2048];

// ---- 表读写 -----------------------------------------------------------------

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1u) / a * a;
}

static void load_table(void)
{
    memset(&s_tab, 0, sizeof(s_tab));
    s_tab.magic = TABLE_MAGIC;
    s_tab.count = WHALE_FILE_SLOT_COUNT;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;   // 全新设备，空表就是正确状态
    }
    seg_table_t tmp;
    size_t len = sizeof(tmp);
    const esp_err_t err = nvs_get_blob(h, NVS_KEY_TABLE, &tmp, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(tmp) || tmp.magic != TABLE_MAGIC
        || tmp.count != WHALE_FILE_SLOT_COUNT) {
        return;   // 旧版本或损坏，按空表处理
    }
    memcpy(&s_tab, &tmp, sizeof(s_tab));
}

static esp_err_t save_table(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    esp_err_t err = nvs_set_blob(h, NVS_KEY_TABLE, &s_tab, sizeof(s_tab));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "区段表写 NVS 失败 err=%d（内容已落 flash，重启后表里看不到）", err);
    }
    return err;
}

// ---- 分配 -------------------------------------------------------------------

// 收集已用区段并按偏移升序排好（条目最多 16 个，插入排序足够）。
typedef struct {
    uint32_t off;
    uint32_t len;
} span_t;

static uint32_t collect_used(uint8_t skip_idx, span_t *out, uint32_t cap)
{
    uint32_t n = 0;
    for (uint8_t i = 0; i < WHALE_FILE_SLOT_COUNT; i++) {
        if (i == skip_idx || s_tab.seg[i].flags != SEG_USED) continue;
        if (n >= cap) break;
        out[n].off = s_tab.seg[i].off;
        out[n].len = s_tab.seg[i].len;
        n++;
    }
    // 插入排序：按 off 升序
    for (uint32_t i = 1; i < n; i++) {
        span_t key = out[i];
        uint32_t j = i;
        while (j > 0 && out[j - 1].off > key.off) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return n;
}

// first-fit：在 skip_idx 之外的空隙里找第一段 >= need 的连续空间。
// 失败时 *out_free 回填「当前最大的连续空闲段」，便于上层给出有意义的报错。
static bool find_span(uint8_t skip_idx, uint32_t need, uint32_t *out_off,
                      uint32_t *out_free)
{
    span_t used[WHALE_FILE_SLOT_COUNT];
    const uint32_t n = collect_used(skip_idx, used, WHALE_FILE_SLOT_COUNT);

    uint32_t cursor = 0;
    uint32_t best = 0;          // 最大的连续空闲段
    for (uint32_t i = 0; i < n; i++) {
        if (used[i].off > cursor) {
            const uint32_t gap = used[i].off - cursor;
            if (gap >= need) {
                *out_off = cursor;
                if (out_free) *out_free = gap;
                return true;
            }
            if (gap > best) best = gap;
        }
        const uint32_t end = used[i].off + align_up(used[i].len, SECTOR_SZ);
        if (end > cursor) cursor = end;
    }
    const uint32_t tail = s_part->size > cursor ? s_part->size - cursor : 0;
    if (out_free) *out_free = tail > best ? tail : best;
    if (tail >= need) {
        *out_off = cursor;
        return true;
    }
    return false;
}

// ---- 初始化 -----------------------------------------------------------------

esp_err_t whale_file_init(void)
{
    // subtype 故意用 ANY：partitions.csv 里写的是 spiffs，但真按 SPIFFS
    // 子类型过滤的话，将来换成自定义 subtype 这行就静默找不到了。
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (s_part == NULL) {
        ESP_LOGE(TAG, "找不到 storage 分区(partitions.csv 没生效?)");
        return ESP_ERR_NOT_FOUND;
    }

    load_table();

    uint32_t used_bytes = 0;
    uint8_t used_cnt = 0;
    for (uint8_t i = 0; i < WHALE_FILE_SLOT_COUNT; i++) {
        if (s_tab.seg[i].flags != SEG_USED) continue;
        used_cnt++;
        used_bytes += align_up(s_tab.seg[i].len, SECTOR_SZ);
        ESP_LOGI(TAG, "  #%u %-12s %luB @%08lx crc=%08lx",
                 (unsigned)i, s_tab.seg[i].name,
                 (unsigned long)s_tab.seg[i].len,
                 (unsigned long)s_tab.seg[i].off,
                 (unsigned long)s_tab.seg[i].crc32);
    }
    ESP_LOGI(TAG, "storage 池 %luKB @0x%x，变长分配（条目上限 %u），"
                  "已用 %u 项 / %luKB",
             (unsigned long)(s_part->size / 1024), (unsigned)s_part->address,
             (unsigned)WHALE_FILE_SLOT_COUNT, (unsigned)used_cnt,
             (unsigned long)(used_bytes / 1024));
    return ESP_OK;
}

bool whale_file_slot_info(uint8_t index, whale_file_info_t *out)
{
    if (index >= WHALE_FILE_SLOT_COUNT) return false;
    if (s_tab.seg[index].flags != SEG_USED) return false;
    if (out != NULL) {
        out->len = s_tab.seg[index].len;
        out->crc32 = s_tab.seg[index].crc32;
        out->off = s_tab.seg[index].off;
        memcpy(out->name, s_tab.seg[index].name, sizeof(out->name));
        out->name[WHALE_FILE_NAME_LEN] = '\0';
    }
    return true;
}

uint32_t whale_file_free_bytes(void)
{
    uint32_t off = 0, free_max = 0;
    // need 给个不可能满足的值，让 find_span 只回填 free_max 不返回成功。
    find_span(0xFF, s_part != NULL ? s_part->size + 1u : 1u, &off, &free_max);
    return free_max;
}

uint8_t whale_file_used_count(void)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < WHALE_FILE_SLOT_COUNT; i++) {
        if (s_tab.seg[i].flags == SEG_USED) n++;
    }
    return n;
}

// ---- 一次传输的三步 ---------------------------------------------------------

esp_err_t whale_file_begin(uint8_t index, uint32_t total_len, uint32_t crc32,
                           const char *name)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (index >= WHALE_FILE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    if (total_len == 0 || total_len > s_part->size) return ESP_ERR_INVALID_ARG;

    // 上一次没结束的传输先作废：手机重发 BEGIN 就是"重来"的意思。
    if (s_active) whale_file_abort();

    const bool existing = s_tab.seg[index].flags == SEG_USED;
    uint32_t off = 0;

    if (existing && total_len <= s_tab.seg[index].len) {
        // 新内容不比旧的長 → 原地覆盖。省一次分配，也避免把池子搅碎。
        off = s_tab.seg[index].off;
    } else {
        // 需要新位置。已有旧文件时把它当空闲（因为马上要释放），
        // 但【先算出位置再动表】—— 分配失败时旧元数据要原样留着。
        uint32_t free_max = 0;
        if (!find_span(index, align_up(total_len, SECTOR_SZ), &off, &free_max)) {
            ESP_LOGW(TAG, "空间不足：#%u 要 %luB，最大连续空闲只有 %luB"
                          "（总量够但碎了就先 COMPACT）",
                     (unsigned)index, (unsigned long)total_len,
                     (unsigned long)free_max);
            return ESP_ERR_NO_MEM;
        }
        if (existing) {
            // 分配成功了，才把旧的标记为释放。此时旧内容尚未擦除，
            // 万一本次传输中途失败，abort() 会一并清掉这条元数据。
            ESP_LOGI(TAG, "#%u 旧区段 %08lx/%luB 移到 %08lx/%luB",
                     (unsigned)index, (unsigned long)s_tab.seg[index].off,
                     (unsigned long)s_tab.seg[index].len, (unsigned long)off,
                     (unsigned long)total_len);
        }
    }

    s_index = index;
    s_off = off;
    s_total_len = total_len;
    s_expected_crc = crc32;
    s_written = 0;
    s_erased = 0;     // 擦除游标从区段头开始，写多少擦多少
    s_had_old = existing;
    strlcpy(s_name, name != NULL ? name : "", sizeof(s_name));
    s_active = true;

    ESP_LOGI(TAG, "开始接收 #%u \"%s\" %luB → %08lx%s", (unsigned)index, s_name,
             (unsigned long)total_len, (unsigned long)off,
             existing ? "（覆盖旧内容）" : "");
    return ESP_OK;
}

// 写入游标即将进入未擦除区域时，把下一扇区擦掉。一次一扇区而不是
// 一次擦全量：4KB 擦除典型几十毫秒，几百 KB 全擦要十几秒 —— 那会让
// NimBLE host 任务卡到像死机，BEGIN 的 ACK 都发不出去。
static esp_err_t erase_ahead(void)
{
    while (s_written + SECTOR_SZ > s_erased && s_erased < s_total_len) {
        const uint32_t off = s_off + s_erased;
        const esp_err_t err = esp_partition_erase_range(s_part, off, SECTOR_SZ);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "擦除失败 off=%08lx err=%d", (unsigned long)off, err);
            s_active = false;
            return err;
        }
        s_erased += SECTOR_SZ;
    }
    return ESP_OK;
}

esp_err_t whale_file_data(const uint8_t *data, uint32_t len)
{
    if (!s_active || data == NULL) return ESP_ERR_INVALID_STATE;
    if (s_written + len > s_total_len) {
        ESP_LOGW(TAG, "超出声明的长度(%lu+%lu > %lu)，丢弃整次传输",
                 (unsigned long)s_written, (unsigned long)len,
                 (unsigned long)s_total_len);
        s_active = false;
        return ESP_ERR_INVALID_SIZE;
    }

    if (erase_ahead() != ESP_OK) return ESP_FAIL;

    const uint32_t off = s_off + s_written;
    const esp_err_t err = esp_partition_write(s_part, off, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入失败 off=%08lx len=%lu err=%d",
                 (unsigned long)off, (unsigned long)len, err);
        s_active = false;
        return err;
    }
    s_written += len;
    return ESP_OK;
}

esp_err_t whale_file_end(bool *crc_ok)
{
    if (crc_ok != NULL) *crc_ok = false;
    if (!s_active) return ESP_ERR_INVALID_STATE;
    s_active = false;

    if (s_written != s_total_len) {
        ESP_LOGW(TAG, "长度不符: 声明 %lu 实收 %lu",
                 (unsigned long)s_total_len, (unsigned long)s_written);
        // 内容不完整，旧文件也已经被覆盖了一部分 —— 元数据不能再留着。
        if (s_had_old) {
            s_tab.seg[s_index].flags = SEG_FREE;
            save_table();
        }
        return ESP_ERR_INVALID_SIZE;
    }

    // 读回全部字节重算 CRC —— 这一步才算真校验：写的时候数据对不代表
    // 落盘对（虽然概率小），读回能抓住任何写入层的错位/位翻转。
    uint32_t crc = 0;
    uint32_t done = 0;
    while (done < s_total_len) {
        const uint32_t chunk = s_total_len - done > sizeof(s_verify_buf)
                             ? sizeof(s_verify_buf) : s_total_len - done;
        if (esp_partition_read(s_part, s_off + done, s_verify_buf, chunk) != ESP_OK) {
            ESP_LOGE(TAG, "回读失败 off=%lu", (unsigned long)done);
            if (s_had_old) {
                s_tab.seg[s_index].flags = SEG_FREE;
                save_table();
            }
            return ESP_FAIL;
        }
        crc = esp_rom_crc32_le(crc, s_verify_buf, chunk);
        done += chunk;
    }

    if (crc != s_expected_crc) {
        ESP_LOGW(TAG, "CRC 不符: 期望 %08lx 实算 %08lx",
                 (unsigned long)s_expected_crc, (unsigned long)crc);
        // 内容已经是垃圾，元数据必须清掉 —— 否则表里那个名字指向坏数据。
        s_tab.seg[s_index].flags = SEG_FREE;
        save_table();
        return ESP_OK;   // 流程没错，内容错，由 *crc_ok 表达
    }

    // 校验通过才认这条区段。
    s_tab.seg[s_index].off = s_off;
    s_tab.seg[s_index].len = s_total_len;
    s_tab.seg[s_index].crc32 = crc;
    s_tab.seg[s_index].flags = SEG_USED;
    strlcpy(s_tab.seg[s_index].name, s_name, sizeof(s_tab.seg[s_index].name));
    if (save_table() != ESP_OK) {
        ESP_LOGE(TAG, "元数据落盘失败(内容已写入,重启后表里看不到)");
        return ESP_FAIL;
    }

    if (crc_ok != NULL) *crc_ok = true;
    ESP_LOGI(TAG, "#%u \"%s\" %luB @%08lx 校验通过 crc=%08lx",
             (unsigned)s_index, s_name, (unsigned long)s_total_len,
             (unsigned long)s_off, (unsigned long)crc);
    return ESP_OK;
}

void whale_file_abort(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "放弃传输 #%u 已写 %luB", (unsigned)s_index,
                 (unsigned long)s_written);
    }
    s_active = false;

    // 覆盖模式下旧内容已被写了几个字节（甚至擦了几个扇区），已经不可信了。
    // 与其让表里留一个指向垃圾的名字，不如直接清掉这条。
    if (s_had_old) {
        s_tab.seg[s_index].flags = SEG_FREE;
        save_table();
        ESP_LOGW(TAG, "#%u 旧文件已被本次失败的传输破坏，元数据已清除",
                 (unsigned)s_index);
        s_had_old = false;
    }
}

uint32_t whale_file_written(void)
{
    return s_written;
}

// ---- 区段管理 ---------------------------------------------------------------

esp_err_t whale_file_erase_slot(uint8_t index)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (index >= WHALE_FILE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;
    if (s_active && s_index == index) whale_file_abort();

    uint32_t erase_len = 0;
    uint32_t off = 0;
    if (s_tab.seg[index].flags == SEG_USED) {
        off = s_tab.seg[index].off;
        erase_len = align_up(s_tab.seg[index].len, SECTOR_SZ);
    }

    // 先清元数据再擦：万一擦到一半掉电，表里已经没有这个文件了，
    // 不会出现「名字还在、内容半截」的悬空状态。
    s_tab.seg[index].flags = SEG_FREE;
    memset(s_tab.seg[index].name, 0, sizeof(s_tab.seg[index].name));
    save_table();

    if (erase_len > 0) {
        const esp_err_t err = esp_partition_erase_range(s_part, off, erase_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "擦除失败 #%u off=%08lx len=%lu err=%d",
                     (unsigned)index, (unsigned long)off,
                     (unsigned long)erase_len, err);
            return err;
        }
    }
    ESP_LOGI(TAG, "#%u 已删除(擦 %luB @%08lx)", (unsigned)index,
             (unsigned long)erase_len, (unsigned long)off);
    return ESP_OK;
}

esp_err_t whale_file_verify_slot(uint8_t index, bool *crc_ok)
{
    if (crc_ok != NULL) *crc_ok = false;
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (index >= WHALE_FILE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;

    whale_file_info_t info;
    if (!whale_file_slot_info(index, &info)) return ESP_ERR_NOT_FOUND;

    uint32_t crc = 0;
    uint32_t done = 0;
    while (done < info.len) {
        const uint32_t chunk = info.len - done > sizeof(s_verify_buf)
                             ? sizeof(s_verify_buf) : info.len - done;
        if (esp_partition_read(s_part, info.off + done, s_verify_buf, chunk) != ESP_OK) {
            return ESP_FAIL;
        }
        crc = esp_rom_crc32_le(crc, s_verify_buf, chunk);
        done += chunk;
    }
    if (crc_ok != NULL) *crc_ok = (crc == info.crc32);
    if (crc != info.crc32) {
        ESP_LOGW(TAG, "#%u CRC 不符: 存档 %08lx 实算 %08lx",
                 (unsigned)index, (unsigned long)info.crc32, (unsigned long)crc);
    }
    return ESP_OK;
}

// ---- 碎片整理 ---------------------------------------------------------------

esp_err_t whale_file_compact(void)
{
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (s_map_ptr != NULL) {
        // 映射期间搬数据 = 让 LVGL 读到半新半旧的字节。上层必须先撤图。
        return ESP_ERR_INVALID_STATE;
    }

    // 按偏移升序处理，逐个前移。索引表只有 16 项，直接选择排序取下一个。
    bool done[WHALE_FILE_SLOT_COUNT];
    memset(done, 0, sizeof(done));

    uint32_t cursor = 0;
    uint32_t moved = 0;
    uint8_t moved_files = 0;

    for (uint32_t step = 0; step < WHALE_FILE_SLOT_COUNT; step++) {
        int pick = -1;
        for (uint8_t i = 0; i < WHALE_FILE_SLOT_COUNT; i++) {
            if (done[i] || s_tab.seg[i].flags != SEG_USED) continue;
            if (pick < 0 || s_tab.seg[i].off < s_tab.seg[pick].off) pick = i;
        }
        if (pick < 0) break;
        done[pick] = true;

        const uint32_t src = s_tab.seg[pick].off;
        const uint32_t len = s_tab.seg[pick].len;
        const uint32_t span = align_up(len, SECTOR_SZ);

        if (src != cursor) {
            // 目标永远在源之前，且两者不重叠（cursor <= src），所以从前往后
            // 逐块搬是安全的：擦目标扇区不会碰到还没读的源数据。
            for (uint32_t p = 0; p < span; p += sizeof(s_move_buf)) {
                const uint32_t chunk = span - p > sizeof(s_move_buf)
                                     ? sizeof(s_move_buf) : span - p;
                if (esp_partition_read(s_part, src + p, s_move_buf, chunk) != ESP_OK) {
                    return ESP_FAIL;
                }
                if (esp_partition_erase_range(s_part, cursor + p, SECTOR_SZ) != ESP_OK) {
                    return ESP_FAIL;
                }
                if (esp_partition_write(s_part, cursor + p, s_move_buf, chunk) != ESP_OK) {
                    return ESP_FAIL;
                }
            }
            moved += span;
            moved_files++;
        }
        s_tab.seg[pick].off = cursor;
        cursor += span;
    }

    if (moved_files > 0) {
        if (save_table() != ESP_OK) return ESP_FAIL;
    }
    ESP_LOGI(TAG, "整理完成：搬了 %u 个文件 / %luKB，池内已用 %luKB",
             (unsigned)moved_files, (unsigned long)(moved / 1024),
             (unsigned long)(cursor / 1024));
    return ESP_OK;
}

// ---- 零拷贝读取 -------------------------------------------------------------

esp_err_t whale_file_map_slot(uint8_t index, const uint8_t **out_ptr,
                              uint32_t *out_len)
{
    if (out_ptr != NULL) *out_ptr = NULL;
    if (out_len != NULL) *out_len = 0;
    if (s_part == NULL) return ESP_ERR_INVALID_STATE;
    if (index >= WHALE_FILE_SLOT_COUNT) return ESP_ERR_INVALID_ARG;

    whale_file_info_t info;
    if (!whale_file_slot_info(index, &info)) return ESP_ERR_NOT_FOUND;
    if (info.len == 0) return ESP_ERR_INVALID_SIZE;

    // 换区段/换长度都要重新映射。句柄不暴露偏移，所以一律先撤旧的 ——
    // 重复 map 同一块只是多一次 munmap，成本可以忽略。
    whale_file_unmap();

    // mmap 的 offset 必须按 MMU 页（64KB）对齐，而区段只保证 4KB 对齐，
    // 所以向前取整到页边界，映射长度覆盖「页内偏移 + 数据长度」。
    // 返回给上层的指针补上这段页内偏移，调用方感知不到。
    const uint32_t aligned = info.off & ~(MMU_PAGE - 1u);
    const uint32_t prefix = info.off - aligned;
    const uint32_t map_len = align_up(prefix + info.len, MMU_PAGE);

    const void *ptr = NULL;
    const esp_err_t err = esp_partition_mmap(s_part, aligned, map_len,
                                             ESP_PARTITION_MMAP_DATA, &ptr,
                                             &s_map_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "#%u 映射失败 err=%d (%lu 字节)", (unsigned)index, err,
                 (unsigned long)map_len);
        return err;
    }

    s_map_ptr = (const uint8_t *)ptr + prefix;
    s_mapped_len = info.len;
    ESP_LOGI(TAG, "#%u 已映射到 %p (%luB @%08lx，页基址 %08lx)",
             (unsigned)index, s_map_ptr, (unsigned long)info.len,
             (unsigned long)info.off, (unsigned long)aligned);

    if (out_ptr != NULL) *out_ptr = (const uint8_t *)s_map_ptr;
    if (out_len != NULL) *out_len = info.len;
    return ESP_OK;
}

void whale_file_unmap(void)
{
    if (s_map_ptr == NULL) return;
    esp_partition_munmap(s_map_handle);
    s_map_ptr = NULL;
    s_mapped_len = 0;
}
