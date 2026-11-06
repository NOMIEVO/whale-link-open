---
name: whale-link
description: 通过 BLE 驱动 ESP32-C3 小屏挂件——对时、余额快照、传感器上报、低帧率投屏、Flash 文件存储、按需挂载（设备反向索取手机照片）。当需要给挂件新增数据能力、编写/对接对端 App 或固件、或排查 BLE 链路（连不上、投屏 0 fps、写不进去、按了没反应）时使用。
---

# WhaleLink 集成与排错手册

一份「怎么用、发什么、怎么判断成功、出错查哪里」的操作手册。协议规格在
`firmware/docs/phone-link-protocol.zh_CN.md`；本文件是它的**执行版**。

---

## 何时用这个技能

- 要给挂件加一条数据通路（新传感器、新指令、新资源类型）
- 要写一个自己的 App/上位机来驱动这块板子
- BLE 链路出问题：搜不到、连上但 0 fps、写不进去、设备按了没反应
- 要在别的 ESP32 项目里复制「手机文件 → flash → 零拷贝显示」这套做法

---

## 先记住三条硬约束

这三条决定了所有方案的形状，违反它们的方案一定做不成：

| 约束 | 实测值 | 后果 |
|---|---|---|
| **ATT 单包上限** | 512 B（Android `GATT_MAX_ATTR_LEN`） | 单块 payload 最多 494–498 B，超了**本地直接拒收**，不是设备丢包 |
| **有效吞吐** | 20–30 KB/s（2M PHY + 快连接参数） | 1 MB 文件约 40–60 s；「实时视频」不可能 |
| **可用堆** | ~33 KB（BLE 常驻后） | 240×320 RGB565 = 150 KB，**大图必须 mmap，不能拷内存** |

推论：
- 投屏只能低帧率（1–15 fps），文档里不要写「实时投屏」
- 大文件只能落 flash，不能进 RAM
- 每新增一个 GATT 特征，**老用户必须清一次 GATT 缓存**（见排错 §1）

---

## GATT 布局

服务 `4f7c0001-1e2d-4a5b-8c3f-9d0e1f2a3b4c`，设备名 `WhaleBT-XXXX`（MAC 后两字节）。

| UUID 尾号 | 名称 | 属性 | 方向 | 用途 |
|---|---|---|---|---|
| `0x02` | Balance | Write / WriteNR | 手机 → 设备 | 余额快照，40 B |
| `0x03` | Request | **Notify** | 设备 → 手机 | 设备反向请求（1 或 4 B） |
| `0x04` | Time | Write / WriteNR | 手机 → 设备 | 对时，8 B |
| `0x05` | Sensor | Write / WriteNR | 手机 → 设备 | 传感器，24 B |
| `0x06` | Frame | Write / WriteNR | 手机 → 设备 | 投屏分块，16 B 头 + payload |
| `0x07` | File | Write + **Notify** | 双向 | 文件/资源传输 + 28 B 状态回报 |

全部**小端**。长度不足或 `ver != 1` 一律丢弃。

---

## 连接与就绪（这一步做错，后面全废）

```
1. 扫描 → 按名字前缀 "WhaleBT-" 过滤
   ⚠️ 不要用 ScanFilter.setServiceUuid 过滤：UUID128 放在 scan response 里，
      某些机型对 scan response 的 service 过滤支持不稳 → 永远扫不到
2. 连接 → discoverServices()
3. requestMtu(517)          ← 必须，否则 24 B 的传感器帧都发不出去
4. onMtuChanged 里做两件事：订阅 File CCCD、订阅 Request CCCD
```

### 双 CCCD 必须串行订阅

Android 的 GATT **同一时刻只受理一个描述符写**。连着写第二个会判 busy 且
**不重试**（`writeDescriptor()` 返回 false，静默丢弃）。

正确做法：队列 + `onDescriptorWrite` 驱动，一条回调回来再发下一条。

```java
// 入队
cccdQueue.add(chrFile); cccdQueue.add(chrRequest); pumpCccd();
// onDescriptorWrite 里：cccdBusy = false; pumpCccd();
```

**两条都必须订**：

| 没订哪条 | 症状 |
|---|---|
| File | 不知道写没写成功，收不到 CRC 校验结论 |
| Request | **设备按 OK 完全没反应**（notify 找不到订阅者，NimBLE 直接报错） |

连接参数：日常 100–200 ms / latency 4；**传大数据前切到 15–30 ms / latency 0**，
否则一个包等一个连接事件，吞吐掉到十分之一。

---

## 功能 1 · 对时（8 B）

```
u8  ver = 1
u8  flags     bit0=时间有效  bit1=带时区
i16 tz_min    相对 UTC 的分钟数（北京 = +480）
u32 unix_sec  UTC 秒
```

连上发一次，之后每 60 s 重发。设备重启/深睡后走时基准归零，等下一次同步。

**判据**：设备屏幕 TIME 卡片显示北京时间，`AGE` 每秒递增。

---

## 功能 2 · 余额快照（40 B）

手机查好后推给设备——**设备本身不联网**（C3 无经典蓝牙，Wi-Fi 在测试固件里是关的）。

设备只把它显示在 LINK 卡片上。字段定义沿用产品侧的
`deepseek-balance-whale-bluetooth` 协议文档，本仓库不重复定义。

---

## 功能 3 · 传感器（24 B）

```
u8  ver = 1
u8  kind      0=加速度 1=角速度 2=重力 9=位置
u8  flags     bit0=本帧有效
u8  seq       只用于统计丢包，不重传
u32 t_ms      自本次连接起的相对毫秒
s32 v[3]      定点整数，单位随 kind
u32 aux       位置帧：bit0-15 速度 cm/s，bit16-31 航向 0.01°
```

固定点整数，不用浮点——跨语言编码无字节序/精度歧义。10 Hz 采样约占链路 5%。

---

## 功能 4 · 投屏（16 B 头 + payload）

```
u8  ver = 1
u8  type      0=BEGIN 1=CHUNK 2=END
u8  fmt       0=RGB565+RLE  1=裸 RGB565  2=瓦片增量
u8  flags     bit0=整帧
u16 w,h       BEGIN 有效
u16 seq       帧序号
u16 idx       块序号，从 0 开始
u16 total     本帧总块数
u16 len       本包 payload 字节数
```

块大小 = `MTU - 3(ATT头) - 16(本头)`，**再留 4 B 余量**。MTU 517 时约 494。

### 三种编码怎么选

| fmt | 何时用 | 设备侧行为 |
|---|---|---|
| 0 RLE | 默认；UI 截图通常压 3–8× | 解码后重建整幅位图 |
| 1 裸像素 | RLE 压不动（复杂照片会膨胀 1.5×）时兜底 | 长度必须严格 `w*h*2` |
| 2 瓦片增量 | **画面大部分静止**时最省带宽 | 只把变化块 memcpy 进原位图 + 局部 invalidate |

**瓦片格式（fmt=2）**：

```
[0]    块数 n
[1]    块边长 s（必须是 2 的幂，本工程用 8）
[2..]  n 项：tx, ty (u8，单位=块)，后接 s*s*2 字节 RGB565（块内行优先）
```

约束：
- 瓦片帧**只做叠加**，必须先有一帧全量把位图建好，否则整帧丢弃
- 静止画面必须周期性补一帧全量（本工程 `STILL_REFRESH = 20` 帧），
  否则设备侧「5 秒无新帧」会以为画面该收起

### 分辨率上限

| 格式 | 上限 | 原因 |
|---|---|---|
| 裸像素 / RLE | 120×160（38400 px） | 堆里放不下更大的解码缓冲 |
| 瓦片增量 | 受限于已建好的位图 | 同上 |

超过 `MIRROR_MAX_PX` 设备会拒收整帧并打日志 `投屏 %ux%u 超出解码上限`。
这是**设备拒绝**，不是传输失败——看日志不要误判成丢包。

---

## 功能 5 · 文件存储（手机 → 设备 Flash）

`storage` 分区 4.94 MB，**变长分配**，最多 16 条目，单文件上限 ≈4.9 MB。

### 写方向（16 B 头）

```
[0]  ver = 1
[1]  cmd      1=BEGIN 2=DATA 3=END 4=LIST 5=DELETE 6=VERIFY 7=COMPACT
[2]  index    条目索引 0–15
[3]  保留
[4..7]   u32 total_len
[8..9]   u16 idx           BEGIN=0，DATA 从 1 连续递增
[10..11] u16 total_blocks  含 BEGIN 和 END
[12..13] u16 plen          本包净荷字节数
[14..15] u16 保留
```

- **BEGIN** 净荷 = `crc32(u32 LE) + 文件名(12 B，NUL 填充，ASCII)`
- **DATA** 净荷 = 文件切片
- **END** 净荷为空，但**包长必须 > 16**（补 1 字节占位）——设备对 ≤16 B 的包直接丢弃
- LIST / DELETE / VERIFY / COMPACT 是单包命令，同样要补占位字节

`idx` 断裂或长度不符 → 设备 notify `status=6`，整次作废，必须从 BEGIN 重来。

### 读方向（notify，28 B 定长）

```
[0] ver  [1] status  [2] index  [3] 保留
[4..7]   u32 written
[8..11]  u32 crc
[12..15] u32 total_len
[16..27] name(12 B)
```

| status | 含义 | 手机该做什么 |
|---|---|---|
| 1 | 接受 BEGIN | 开始发 DATA |
| 2 | 进度（每 32 块一次） | 更新界面 |
| 3 | 完成 / 校验通过 | 成功 |
| 4 | 完成但 CRC 不符 | 重传 |
| 5 | 拒绝（索引/长度非法） | 检查文件是否超上限 |
| 6 | 序号断裂 | 从 BEGIN 重来 |
| 7 | 设备写 Flash 失败 | 可能是空间不足，先 COMPACT |
| 8 | 条目信息（LIST 逐条回报） | 记录 |
| 9 | 已删除 | — |
| 10 | 条目为空 | — |
| 11 | 整理完成，`total_len` = 整理后最大可分配空闲 | 更新可用空间 |
| 12 | 整理失败（多半有区段正被 mmap） | 先收起显示的资源 |
| 13 | **列表结束**，`index`=条目数，`total_len`=最大可分配空闲 | 结束 LIST |

> v4 起 LIST **只回报有内容的条目**，末尾补一条 `status=13` 带汇总。
> 早期版本逐槽回报 16 条（含空槽）；连续 16 条 notify 会压爆 NimBLE 的 mbuf。

### 本地 CRC ≠ 落盘 CRC

必须**以设备回读校验的结论为准**（status 3/4）。手机侧算的 CRC 只用于
提前发现「发出去的内容就错了」，不能证明 flash 里写对了。

---

## 功能 6 · 按需挂载（设备 → 手机 → 设备）

让设备访问远超自身 Flash 的资源库（手机相册几千张）。

### 上行请求：复用 Request 特征 notify（4 B）

```
[0]=1                         请查一次余额
[0]=2 [1]=index [2..3]=idx    请把第 idx 号资源送到条目 index（u16 LE）
```

**不新增特征**——复用已有 Request，就避开了「加特征 → 老用户 GATT 缓存失效」的坑。

### 下行：走功能 5 的完整流程

手机把资源转成设备可直接当图像源用的格式，拼 12 B 头：

```
[0..3]  magic "WHLP"
[4..5]  w (u16 LE)
[6..7]  h (u16 LE)
[8]     fmt = 0（RGB565 小端）
[9..11] 保留
[12..]  紧密排列的 RGB565 像素，**无 padding**
```

**像素必须连续无填充**：设备会把这个条目整体 `esp_partition_mmap` 进 DROM
直接当 LVGL 图像源。有 padding 就得额外拷一份，而 150 KB 拷不进 33 KB 的堆。

### 时序（照片，120×160）

| 步骤 | 耗时 |
|---|---|
| 按键 → 手机收到请求 | 瞬时 |
| 扫相册 + 解码 + 缩放 + 转码 | 100–500 ms |
| BLE 传 38 KB | ≈1.5 s |
| 落盘 + 回读 CRC + mmap 显示 | ≈200 ms |

**合计约 2 秒/张**——适合「选一张看一眼」，不适合做幻灯片。

### 写条目前的强制动作

某条目正在屏幕上显示时，**必须先撤图 + 撤映射再擦写**：

```c
if (shown_index == target) {
    test_ui_resource_hide();
    whale_file_unmap();   // 不撤映射，擦除后 LVGL 会读到 0xFF → 花屏
}
```

---

## 排错清单

按现象查，每条都标注了「判据」——先确认现象属于哪一层，再改。

### §1 搜不到设备 / 缺某个特征

| 判据 | 根因 | 处理 |
|---|---|---|
| 日志 `设置广播数据失败 rc=4` | **广播包超过 31 字节**（flags + 名字 + UUID128 = 35） | UUID128 放 scan response，adv 里只留 flags + 名字 |
| 服务能找到但某特征 `getCharacteristic()` 返回 null | Android 缓存了旧 GATT 属性表 | 调 `BluetoothGatt.refresh()` 后重新 discoverServices；仍不行则关开手机蓝牙，或系统设置里「忽略此设备」 |

> 加特征后**老用户都会踩一次**。想避免就复用已有特征 + 命令码区分，而不是加新特征。

### §2 连上但投屏 0 fps / 块写不进去

| 判据 | 根因 | 处理 |
|---|---|---|
| `writeCharacteristic` 立刻返回 false，日志 0 块 | **单包 > 512 B**，Android 本地拒收 | payloadMax = `mtu - 3 - 16 - 4` |
| 每块都返回 201 busy | 用了 `WRITE_TYPE_NO_RESPONSE`（它不清 mDeviceBusy） | 改 `WRITE_TYPE_DEFAULT` |
| 设备日志 `超出解码上限` / `长度不符` | 分辨率超过 120×160，或裸帧长度 ≠ `w*h*2` | 降档 |
| fps 显示 0 但确有帧 | 整数截断（0.9 → 0） | 用十分之一为单位 |
| 前几帧通、随后卡死 | 双方同时发起 PHY 更新，GATT 进入 busy | **只让设备端发起** PHY 请求，App 侧不要 `setPreferredPhy` |

### §3 设备按键没反应

| 判据 | 根因 | 处理 |
|---|---|---|
| 设备屏幕 FILE 卡片写 `RES offline` | **手机没订阅 Request 的 CCCD**，notify 无接收者 | 订阅 Request（见「连接与就绪」） |
| 设备屏幕写 `RES #n -> #s` 但手机无日志 | 请求到了但 App 没处理 | 检查 `onCharacteristicChanged` 是否只处理了 File 特征 |
| 完全没有反应 | **按的是短按**（短按 = 查余额，长按 ≥0.5 s 才要资源） | 长按 |

### §4 图片传到设备但不显示

| 判据 | 根因 |
|---|---|
| 设备日志 `资源头异常` | magic 不是 `WHLP`；或发的是 JPEG（设备没有 JPEG 解码器） |
| 花屏 | 像素有 padding，或该条目在擦写时仍处于 mmap 状态 |
| 什么都不显示但校验通过 | 条目被写了，但没有触发显示（END 成功后要主动调显示） |

### §5 改了代码但设备行为没变

**先怀疑镜像没更新**，不要先怀疑逻辑：

```bash
# 1. 只 build 不会更新 full.bin，必须再 merge-bin
idf.py build
idf.py merge-bin -o $(pwd)/build/XXX-full.bin     # -o 必须绝对路径

# 2. 在镜像里搜你新加的中文字符串
grep -c "你新加的日志文字" build/XXX-full.bin     # 0 = 代码没进去
```

`nm` 只能证明 ELF 有符号，**证明不了烧进去的 bin 有**。

### §6 手机读不到相册

- Android 13+ 必须 `READ_MEDIA_IMAGES`（12 及以下用 `READ_EXTERNAL_STORAGE`）
- Android 14 的「仅选择部分照片」要认 `READ_MEDIA_VISUAL_USER_SELECTED`
- **权限判断函数必须和申请函数成对**：`if (!hasPhotoPermission()) requestPhotoPermission()`
  写成 `hasBlePermissions()` 会让整条申请路径变成死代码，表现是「静默查到 0 张」
- 兜底：用 SAF 选文件（`ACTION_OPEN_DOCUMENT`）不需要任何运行时权限

---

## 如果要复制这套存储方案

关键不是「会写 flash」，而是**让文件数据在物理上连续**：

1. 区段起点按 **4 KB（扇区）** 对齐，first-fit 分配
2. 同索引且新长度 ≤ 旧长度 → **原地覆盖**（反复刷新同一张图不会把池子搅碎）
3. 元数据（名字/长度/CRC32）存一个 NVS blob，换 magic 让旧表自动失效
4. `esp_partition_mmap` 的 offset 必须按 **64 KB MMU 页**对齐 →
   向前取整映射，返回指针补上页内偏移
5. 覆盖写入中断/CRC 失败 → **必须清掉该条目元数据**（旧内容已被部分覆盖，不可信）
6. COMPACT：按偏移升序前移区段，2 KB 缓冲逐块「擦 → 读 → 写」；
   **有区段处于 mmap 时拒绝执行**

**不要换 LittleFS / SPIFFS**：日志结构文件系统不保证文件数据物理连续，
跨页就映射不出正确字节，零拷贝显示这条路直接断掉。

---

## 代码地图

| 想改什么 | 去哪 |
|---|---|
| 加一条新指令 | `firmware/main/whale_ble.c` 特征表 + 对应 `*_access()`；`whale_ble.h` 消息结构 |
| 改存储策略 | `firmware/main/whale_file.c`（分配器）/ `whale_file.h`（上限宏） |
| 改界面 | `firmware/main/test_ui.c` |
| 改设备行为编排 | `firmware/main/main.c`（按键、回调、自动显示） |
| 改协议打包 | `android/.../Frames.java` |
| 改投屏策略 | `android/.../MirrorSource.java`（瓦片/心跳/缩放） |
| 改相册供给 | `android/.../ResourceLibrary.java` |
| 改 BLE 客户端 | `android/.../BleLink.java`（MTU / CCCD / 分块写） |

---

## 硬性禁止

- ❌ 不要在 BLE 回调里碰 `lv_*`——回调跑在 NimBLE host 任务，LVGL 不是线程安全的。
  必须持 `bsp_lvgl_lock()`，或发消息给主循环。
- ❌ 不要在 BLE 回调里做 38 KB 的解码——会直接拖垮连接事件节奏。
- ❌ 不要用 `WRITE_TYPE_NO_RESPONSE` 发分块——它不清 busy 标志，第二块起全部 201。
- ❌ 不要往设备发 JPEG——测试固件没有 JPEG 解码器。
- ❌ 不要在映射期间擦写同一区段——LVGL 会读到 0xFF 花屏。
- ❌ 不要给「对象」做旋转/缩放动画——LVGL 会走中间层，139×139 就掉到 13 fps。
  要缩放就动**图像本身**：`lv_image_set_scale_x/y()`。
