# WhaleLink · 手机 ↔ ESP32-C3 挂件联调套件

用一部 Android 手机，通过 BLE 驱动一块 240×320 的小屏挂件：对时、推传感器、投屏、
把文件存进挂件的 Flash，以及让挂件反过来向手机「按需索取」一张照片。

一套可用于验证协议与硬件能力的**参考实现**，含固件与 App 两端源码。

[![Release](https://img.shields.io/github/v/release/NOMIEVO/whale-link-open?label=release&color=2ea44f)](https://github.com/NOMIEVO/whale-link-open/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-blue)](LICENSE)
[![Firmware](https://img.shields.io/badge/firmware-ESP32--C3-orange)](firmware/)
[![App](https://img.shields.io/badge/app-Android%208.0%2B-3ddc84)](android/)

> **English**: A BLE reference implementation for driving a 240×320 ESP32-C3 display
> accessory from an Android phone — clock sync, sensor streaming, low-fps screen mirroring,
> an on-device flash store with variable-length allocation, and on-demand resource loading
> where the device pulls a photo from the phone. Firmware (ESP-IDF 5.5) + Android app (Java).

---

## 不想编译？直接下这两个文件

### → [**⬇ 前往 Releases 页面下载**](https://github.com/NOMIEVO/whale-link-open/releases/latest)

Release 页面上的附件是**一个大按钮直接下载**，不用在仓库里找 Raw / Download。

也可以直接点这里：

| 文件 | 说明 |
|---|---|
| [WhaleLink-1.0-full.bin](https://github.com/NOMIEVO/whale-link-open/releases/download/v1.0/WhaleLink-1.0-full.bin) | ESP32-C3 整片固件（1.05 MB），`esptool` 从 `0x0` 写入 |
| [WhaleLink-1.0.apk](https://github.com/NOMIEVO/whale-link-open/releases/download/v1.0/WhaleLink-1.0.apk) | Android App（2.95 MB），直接安装 |
| [flash.bat](https://github.com/NOMIEVO/whale-link-open/releases/download/v1.0/flash.bat) | Windows 一键刷写，双击后传 COM 口 |

同样的文件也放在仓库 [`prebuilt/`](prebuilt/) 目录里（含源码同版本对照）。
详细步骤（图形化刷写工具、权限说明、排错表）见
[`prebuilt/README.md`](prebuilt/README.md)。

> 固件和 App **必须配套**：协议改过之后新旧版本混用会连不上。升级时两个一起换。

想自己改代码继续往下看。

---

## 这不是什么

- ❌ **不是产品固件**。界面是四块状态卡片，没有中文字模、没有角色资源、没有账本/音效/深睡。
- ❌ **不是「实时投屏」**。BLE 有效吞吐 20–30 KB/s，投屏是 1–15 fps 的画面回传。
- ❌ **不是 Wi-Fi 方案**。ESP32-C3 只有 BLE 5.0，无经典蓝牙，手机侧不做网络代理。

它是把「一颗没有 PSRAM 的小芯片 + 一条 25 KB/s 的无线链路」能做什么、边界在哪，
用可运行的代码说清楚。

## 功能

| 功能 | 方向 | 关键实现 |
|---|---|---|
| **对时** | 手机 → 设备 | 8 B 帧，带时区；设备用 `esp_timer` 累加走时 |
| **余额快照** | 手机 → 设备 | 40 B，手机查好后推给设备（设备不联网） |
| **传感器** | 手机 → 设备 | 24 B 定长帧，加速度/陀螺仪/重力/GPS，固定点整数 |
| **画面投屏** | 手机 → 设备 | RGB565 + RLE 分块；支持裸像素、**瓦片增量**（只传变化区域） |
| **文件存储** | 手机 → 设备 Flash | 4.94 MB 池，变长分配，最多 16 条目，单文件上限 ≈4.9 MB |
| **按需挂载** | 设备 → 手机 → 设备 | 设备按键发起请求，手机从相册取图、缩放转码后回推，设备 `mmap` 零拷贝显示 |

设备端另有一个**内建自检图**（长按 UP），不接手机也能验证「解码 → 缩放 → 铺屏」整条显示链路。

## 架构

```
┌──────────────────────────────┐        ┌────────────────────────────┐
│  Android (whale-link-android)│        │  ESP32-C3 (whale-link-firmware)
│                              │        │                            │
│  MirrorSource ─┐             │        │   whale_ble.c              │
│  MotionSource ─┼→ Frames ────┼─ BLE ─►│   ├─ Frame  ← 投屏分块      │
│  LocationSource┘   (打包)    │ GATT   │   ├─ Sensor ← 传感器        │
│                              │        │   ├─ Time   ← 对时          │
│  ResourceLibrary ────────────┼───────►│   └─ File   ← 文件/资源     │
│    (相册 → 缩放 → RGB565)     │        │           ↓                │
│                              │        │   whale_file.c             │
│  BleLink ◄───────────────────┼────────│   (变长分配 + NVS 元数据)   │
│    Request 特征 notify        │        │           ↓                │
│    ↑ 设备请求「给我第 N 张」   │        │   esp_partition_mmap       │
└──────────────────────────────┘        │           ↓                │
                                        │   LVGL 直接读 flash 作图像源 │
                                        └────────────────────────────┘
```

## 目录结构

```
whale-link/
├── README.md                  ← 本文件
├── SKILL.md                   ← 能力清单 + 调用方式 + 排错手册（给集成者/AI 用）
├── LICENSE
├── prebuilt/                  ← 编译好的成品（免编译直接用）
│   ├── WhaleLink-1.0-full.bin ← 整片固件镜像，0x0 起
│   ├── WhaleLink-1.0.apk      ← Android App
│   ├── flash.bat              ← Windows 一键刷写
│   ├── SHA256SUMS.txt         ← 校验和
│   └── README.md              ← 安装步骤 + 排错表
├── firmware/                  ← ESP-IDF 工程（C）
│   ├── main/
│   │   ├── whale_ble.c/.h     ← GATT server：7 个特征、协议解析、反向 notify
│   │   ├── whale_file.c/.h    ← Flash 存储：变长分配、CRC、mmap、碎片整理
│   │   ├── test_ui.c/.h       ← 四块状态卡片 + 投屏面板 + 资源显示
│   │   ├── main.c             ← 编排：按键、回调、显示
│   │   └── whale_link_diag.c  ← 运行统计
│   ├── components/bsp/        ← 板级驱动（屏幕/按键/电池/音频/I2C）
│   ├── docs/phone-link-protocol.zh_CN.md   ← 完整协议规格
│   ├── partitions.csv         ← 8 MB flash 分区（app 3 MB + storage 4.94 MB）
│   └── sdkconfig.defaults     ← 目标配置（LVGL / NimBLE / 关 Wi-Fi）
└── android/                   ← Android 工程（Java，minSdk 26）
    └── app/src/main/java/com/folotoy/whalelink/
        ├── BleLink.java       ← GATT 客户端：MTU、CCCD 串行订阅、分块写
        ├── LinkService.java   ← 前台服务：指标采集与编排
        ├── MirrorSource.java  ← 投屏：缩放、瓦片差分、静止心跳
        ├── ResourceLibrary.java ← 相册索引 → 按需解码 → RGB565
        ├── RleCodec.java      ← RGB565 / RLE 编解码
        ├── Frames.java        ← 协议常量与打包
        └── MainActivity.java  ← 界面
```

## 硬件

| 项 | 要求 |
|---|---|
| 芯片 | ESP32-C3（**无 PSRAM**，这是所有内存设计的出发点） |
| Flash | 8 MB |
| 屏幕 | 240×320 SPI（本工程按 80 MHz、单缓冲 40 行配置） |
| 按键 | 3 个，共用一个 ADC 引脚分压识别（上/下/确定） |
| 手机 | Android 8.0+（minSdk 26），支持 BLE |

## 快速开始

### 1. 固件

需要 **ESP-IDF 5.5.x**。

```bash
cd firmware
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build

# 生成可整片烧写的合并镜像
idf.py merge-bin -o $(pwd)/build/WhaleLinkTest-full.bin

# 烧写（把 COMx 换成你的端口）
esptool.py --chip esp32c3 -p COMx -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 build/WhaleLinkTest-full.bin
```

两个**必踩的坑**，先看这里能省一小时：

1. `merge-bin` 的 `-o` **必须给绝对路径**，且要和 `flash_args` 同目录，否则校验报 missing。
2. 只跑 `idf.py build` 不会更新 `*-full.bin`，**必须再跑 `merge-bin`**。
   否则你烧进去的还是上一次的镜像——现象是「代码改了但设备行为没变」。
   校验办法：在 bin 里搜你新加的中文字符串。

> ⚠️ `write_flash 0x0` 是整片写入，会擦掉 `nvs`（0x9000）和 `phy_init`，设备的
> 配对信息与存储的元数据全部重置。

### 2. App

需要 **JDK 17** 与 Android SDK（compileSdk 33）。

```bash
cd android
echo "sdk.dir=/path/to/Android/Sdk" > local.properties
./gradlew assembleDebug        # Windows 用 gradlew.bat assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

仓库自带 Gradle Wrapper（8.7），首次构建会自动下载对应版本，无需预装 Gradle。

装好后打开 App，它会自动扫描 `WhaleBT-XXXX` 并连接。

首次使用会申请蓝牙权限；点「要下一张照片」还会申请相册权限（Android 13+ 为
`READ_MEDIA_IMAGES`）。

## 使用

### 设备界面

屏幕上是四块卡片：**LINK**（连接/MTU/快照数）、**TIME**（对时结果）、
**MOTION**（加速度/GPS）、**MIRROR**（投屏帧率与累计字节）、**FILE**（存储与传输状态）。

底部一行 `OK=REQ HOLD-OK=IMG DN=CLOSE` 是按键说明（英文缩写因为工程只装了
Montserrat 字体，没引入中文字库）。

### 按键

| 按键 | 短按 | 长按（≥0.5 s） |
|---|---|---|
| **确定 OK** | 请手机查一次余额 | **要下一张资源**（触发按需挂载） |
| **上 UP** | 统计清零 | 显示/收起内建自检图 |
| **下 DN** | 收起正在显示的资源 | 重新广播（手机搜不到时用） |

### App 界面

- **投屏**：开关 + 三档分辨率。开启后自动切到快连接参数（15–30 ms）。
- **瓦片增量**：只传变化的 16×16 块，静止画面几乎零带宽，并每 20 帧补一次整帧做心跳。
- **文件储存**：选文件发送到条目 0–15；列表 / 校验 / 删除 / **整理空间（合并碎片）**。
- **要下一张照片**：等同设备长按 OK，从相册取下一张送到设备。

## 存储模型

`storage` 分区是**一整块 4.94 MB**，没有在分区表里切分。切分发生在 `whale_file.c`：

| | 早期版本（4×1 MB 固定槽） | 当前（变长分配） |
|---|---|---|
| 单文件上限 | 1 MB | ≈4.9 MB |
| 存一张 38 KB 照片占用 | 1 MB（浪费 96%） | 40 KB |
| 条目数 | 4 | 16 |

- 区段起点按 **4 KB**（扇区）对齐，**first-fit** 找第一个够大的空隙
- 写入同一索引且新长度 ≤ 旧长度 → **原地覆盖**，不搬数据、不产生碎片
- 删除中间文件会留下空洞 → 用 COMPACT 把后面的区段整体前移
- 元数据（名字/长度/CRC32）存 NVS，擦除只擦实际用到的扇区

### 为什么不用 LittleFS / SPIFFS

零拷贝显示依赖 `esp_partition_mmap` 把 flash 映射成内存地址给 LVGL 直读
（C3 可用堆只有 ~33 KB，而 240×320 RGB565 整屏是 150 KB，拷进内存必然失败）。
日志结构文件系统**不保证文件数据物理连续**，跨页就无法一次映射——所以必须自己
管理连续区段。这是整个设计的地基，不是可选项。

## 已知限制

| 限制 | 数值 | 原因 |
|---|---|---|
| BLE 吞吐 | 20–30 KB/s | C3 单连接、2M PHY、连接事件节奏 |
| 单个 ATT 写 | 512 B | Android `GATT_MAX_ATTR_LEN` 硬限，超了本地直接拒收 |
| 投屏分辨率上限 | 120×160（裸像素） | 堆里放不下更大的解码缓冲 |
| 整屏图像 | 必须走 mmap | 150 KB 位图 vs 33 KB 可用堆 |
| 按需挂载速度 | ≈2 s/张 | 转码 0.1–0.5 s + 传 38 KB 1.5 s + 落盘校验 0.2 s |

**改固件 GATT 表后，老用户必须清一次 GATT 缓存**：Android 按设备地址缓存属性表，
新增特征在旧缓存里不存在，`getCharacteristic()` 永远返回 null。App 已内置
`BluetoothGatt.refresh()` 自动刷新；仍失败时需关开手机蓝牙，或在系统设置里忽略该设备。

## 协议

完整规格见 [`firmware/docs/phone-link-protocol.zh_CN.md`](firmware/docs/phone-link-protocol.zh_CN.md)，
含 GATT 布局、逐字段报文、状态码表、时序与容量测算。

若你只想快速对接，看 [`SKILL.md`](SKILL.md)——它按「想做什么 → 发什么 → 怎么判断成功」
组织，并附了完整排错清单。

## 获取代码

```bash
git clone https://github.com/NOMIEVO/whale-link-open.git
cd whale-link-open
```

仓库只含源码（约 65 个文件），构建产物一律不入库：

- `firmware/build/`、`firmware/managed_components/`、`firmware/sdkconfig`
- `android/.gradle/`、`android/app/build/`、`android/local.properties`

> `firmware/managed_components/` 不入库是正确的：ESP-IDF 会按
> `components/bsp/idf_component.yml` + `dependencies.lock` 自动还原。
> 首次 `idf.py build` 需要联网拉取约 180 MB 依赖。

## 国内网络下的 Git 推送

若 `git push` 报 `Failed to connect to github.com:443`，说明命令行没走
系统代理（浏览器能开 GitHub 但 git 不能，就是这个原因）。先查系统代理端口：

```powershell
Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' |
    Select-Object ProxyEnable, ProxyServer
```

再把查到的端口（这里假设 `4780`）配给本仓库：

```bash
git config --local http.proxy  http://127.0.0.1:4780
git config --local https.proxy http://127.0.0.1:4780
```

`--local` 只影响本仓库，不污染全局设置。换网络环境后若代理失效，
用 `git config --local --unset http.proxy` 撤销即可。

## 许可


MIT，见 [LICENSE](LICENSE)。
