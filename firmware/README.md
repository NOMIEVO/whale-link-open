# WhaleLink 联调固件（WhaleLinkTest）

**独立的测试程序，只用来验证 `whale-link-android` 这个 App 能不能把数据送到设备。**

它不是产品固件，也不是产品固件的「换皮」：界面是全新的四块状态卡片，代码里
不链接产品侧的账本、音效、挂件界面，连中文字模和角色图都没有。

```
whale-link/
├── firmware/    ← 本工程（测试台固件）
└── android/     ← 配套 App
```

## 跟产品固件的区别

| | 产品固件 ai-passport | 本工程 |
|---|---|---|
| 界面 | 小鲸鱼挂件页 + 菜单 | **四块状态卡片**（LINK / TIME / MOTION / MIRROR） |
| 字体 | 自建中文子集 16/24 | LVGL 自带 Montserrat 14/16/24 |
| 账本 / 峰谷 / 音效 | 有 | **无**（不编译这些模块） |
| 深睡 / 息屏 | 有 | **无**（测试期间一直亮） |
| 按键 | 长按菜单、泡泡切换 | 只有三个动作（见下） |
| 串口 | 事件日志 | 事件日志 + 每 5 秒一行统计 |

两者共享的只有 `components/bsp`（板级驱动）和 `whale_ble.c`（BLE GATT 协议实现）。

## 屏幕

```
WHALE LINK TESTER          87%
┌ LINK ─────────────────┐   BLE 是否连上、协商的 MTU、收到的余额快照数、设备名
│ BLE   CONNECTED       │
│ MTU 185    SNAP 3     │
│ WhaleBT-DA8A          │
└───────────────────────┘
┌ TIME ─────────────────┐   手机对时后的北京时间，AGE = 距上次对时几秒
│ 17:48:02      AGE 2s  │
└───────────────────────┘
┌ MOTION ───────────────┐   最近一次加速度（1e-3 m/s²）；开了 GPS 就显示经纬度
│ ACC  12, -4, 998      │
│ GPS  39.9073,116.3912 │
└───────────────────────┘
┌ MIRROR ───────────────┐   投屏帧数 / 分辨率 / 编码后大小
│ 24 f  120x160  5.2KB  │
└───────────────────────┘
OK=REQ  UP=CLEAR  DN=ADV
```

收到投屏帧时，上面这整屏会被**画面本身盖住**：手机发来的 RGB565+RLE 流在设备
上解码成位图，按整数倍率铺满 240×320（120×160 正好 2 倍），左上角留一行角标
`120x160  5200 B  x2.0`。超过 5 秒没有新帧就收起，回到四块卡片。

解码缓冲按需分配（120×160 是 38.4 KB，160×213 是 66.6 KB），不做 RLE 的
逐块直绘 —— 那需要在 draw 回调里现解，LVGL 的 draw unit 不给你这个机会。

按键：

| 操作 | 作用 |
|---|---|
| 短按 OK | 让手机再查一次余额（发 Request notify） |
| 短按 UP | 统计清零，便于重新数 |
| 短按 DN / 长按 OK / 长按 DN | 重新广播（手机搜不到时用） |

## 怎么判读

- **`BLE WAITING` 但手机显示已连接** → 连接建了又立刻断，看串口断开原因码。
- **`MTU 23`** → 手机没谈 MTU。40 字节余额报文会被截断、投屏也会碎，先修这个。
- **`TIME --:--:--` + `no sync`** → App 的时间开关没开，或写特征失败。
- **`AGE` 一直涨到几百秒** → App 只在连上时发了一次，定时重发没工作。
- **`ACC none`** → 传感器开关是关的，或系统不给后台传感器事件。
- **`MIR rejected N`** → 帧到了但尺寸/长度不合法，属于 App 侧编码问题。
- **串口出现 `投屏帧没显示出来(WxH, N 字节, 堆 M)`** → 解码残帧或位图分配失败，
  看最后的堆余量：小于 40 KB 就是内存不够，调低手机端分辨率。
- **`MIR 24 f ...` 有帧数但屏幕不切画面** → 5 秒超时逻辑把它收掉了，
  或者 `test_ui_mirror_show()` 一直返回 false（串口会有上一条）。
- **`SNAP 0`** → 从没收到余额，先确认 App 里 API Key 填了。

## 串口

115200，USB-Serial-JTAG（C3 原生 USB）。开机第一行会打印
`WhaleLink 联调固件启动（独立测试程序，非产品固件）`，用来确认刷的是这一版。
每 5 秒一行：

```
I (12345) diag: BLE=1 MTU=185 snap=3 time=5(-2s) acc=120 gyr=0 gps=12 mir=24/0
```

`mir=成功帧数/被拒帧数`，其余为累计次数。

## 构建与刷写

```bash
idf.py build
# -o 必须给绝对路径，否则校验阶段会报 missing
idf.py merge-bin -o "$(pwd)/build/WhaleLinkTest-full.bin"

python -m esptool --chip esp32c3 -p COM4 -b 460800 --before default_reset \
  --after hard_reset write_flash 0x0 build/WhaleLinkTest-full.bin
```

⚠️ `write_flash 0x0` 会整片擦除，nvs（配对信息、存储元数据）全部清零。

⚠️ **只跑 `build` 不会更新 `*-full.bin`**，必须再跑 `merge-bin`，否则烧进去的
还是上一次的镜像。校验办法：在 bin 里搜你新加的中文字符串。

## 目录

```
main/main.c            主循环、按键、BLE 回调
main/test_ui.c         测试台画面（四块卡片，纯 ASCII）
main/whale_link_diag.c 统计模块（host 任务只做整数自增，主循环取快照）
main/whale_ble.c       NimBLE GATT Server：Balance / Request / Time / Sensor / Frame / File
main/whale_file.c      Flash 存储：变长分配 + CRC 校验 + mmap 零拷贝 + 碎片整理
components/bsp/        板级驱动（显示 / 按键 / 电池 / 音频，音频本工程没用）
docs/                  手机↔设备协议
```

## 已知边界

- 设备不联网，余额全靠手机查完写进来。
- **投屏是「看得见但不流畅」**：手机 1–2 fps，设备每帧要做 76.8 K 像素的缩放
  绘制（240×320 全屏），实测能跟上这个帧率，但再高就不行 —— BLE 带宽与刷屏
  时间都不够。
- 帧的 RLE 流超过 40 KB 会被丢弃（App 端直接不发，串口有 `帧太大` 日志）。
  UI 截图通常 3–8 倍压缩，只有照片类内容才可能碰到。
- 没有深睡与息屏：测试期间屏幕常亮，插 USB 用。
