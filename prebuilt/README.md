# 免编译安装包

不想装 ESP-IDF / Android SDK？直接下这里两个文件就行。

> **推荐走 Release 页面下载**：<https://github.com/NOMIEVO/whale-link-open/releases/latest>
> —— 附件区是一个大按钮，不用在仓库里找 Raw / Download。
> 下面的文件与本目录内容完全一致（同一份构建产物，双份存档）。

| 文件 | 用途 | 大小 |
|---|---|---|
| `WhaleLink-1.0-full.bin` | ESP32-C3 整片固件镜像（从 `0x0` 开始） | 约 1 MB |
| `WhaleLink-1.0.apk` | Android 手机端 App（debug 签名） | 约 3 MB |
| `SHA256SUMS.txt` | 校验和，用来确认下载完整 | — |

> 这些文件由仓库当前源码编译而来。想自己改代码，见根目录
> [`README.md`](../README.md) 的「快速开始」。

---

## 一、刷固件（ESP32-C3）

### 准备

- 一根**能传数据**的 USB 线（很多线只能充电，是最常见的失败原因）
- Windows / macOS / Linux 均可
- 设备是 ESP32-C3 + 8 MB Flash + 240×320 SPI 屏

### 方法 A：图形界面（推荐给非开发者）

用乐鑫官方的 **Flash Download Tool**：

1. 下载：<https://www.espressif.com/en/support/download/other-tools>
   → `Flash Download Tool`
2. 解压后运行 `flash_download_tool_*.exe`，按提示选：
   - ChipType：**ESP32-C3**
   - WorkMode：**Develop**
   - LoadMode：**UART**
3. 界面里只填一行：

   | 勾选 | 文件 | 地址 |
   |---|---|---|
   | ✅ | `WhaleLink-1.0-full.bin` | `0x0` |

4. 右下角 **COM** 选你板子的端口（不认识就先插拔一次看哪个多出来），
   **BAUD** 选 `460800`
5. 点 **START**，等进度条走完显示 `FINISH`，然后按一下板子上的复位键

### 方法 B：命令行

装一次 esptool：

```bash
pip install esptool
```

Windows 直接用目录里的 `flash.bat`（把 `COM3` 换成你的口）：

```bat
flash.bat COM3
```

或者手敲：

```bash
esptool.py --chip esp32c3 -p COM3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash 0x0 WhaleLink-1.0-full.bin
```

macOS 常见的口是 `/dev/cu.usbserial-*` 或 `/dev/cu.usbmodem*`，
Linux 是 `/dev/ttyACM0` 或 `/dev/ttyUSB0`（需要把用户加进 `dialout` 组）。

### ⚠️ 整片写入会清空数据

从 `0x0` 写是整片覆盖，会擦掉：

- `nvs`（0x9000）—— 蓝牙配对名、存储区元数据
- `phy_init`（0xF000）—— RF 校准参数

**后果**：刷完第一次开机是出厂状态，之前传的照片全部消失，
设备名会变回默认的 `WhaleBT-XXXX`。这是正常的，不是刷坏了。

### 判据：怎么知道刷成功了

刷完按复位，屏幕应该出现四块卡片（LINK / TIME / MOTION / MIRROR / FILE），
底部一行 `OK=REQ HOLD-OK=IMG DN=CLOSE`。

如果屏幕全黑或花屏 → 大概率是**只烧了 app 分区、没烧完整镜像**，
或者板子不是 240×320 这块屏。重新按上面步骤整片烧一次。

---

## 二、装 App（Android）

1. 把 `WhaleLink-1.0.apk` 传到手机（微信传文件 / 数据线 / 网盘都行）
2. 点开安装，系统提示「未知来源应用」时选**允许**
   （这个 APK 是 debug 签名，不是从应用商店装的，所以会被拦一次）
3. 打开 App，按提示给权限：
   - **蓝牙**（必需，Android 12+ 是「附近的设备」）
   - **相册 / 照片和视频**（点「要下一张照片」时才申请）

### 如果一直搜不到设备

按顺序排查，别跳步：

| 现象 | 原因 | 处理 |
|---|---|---|
| App 里设备列表一直空 | 设备没在广播 | 设备上**长按 DN 键**重新广播 |
| 能看到设备但连上就断 | GATT 缓存了旧服务 | App 已自动刷新 2 次；仍不行就关开手机蓝牙，或在系统设置里「忽略此设备」 |
| 连上了但按键没反应 | 通知没订阅上 | 断开重连一次（订阅是连接时做的） |
| 能连不能传文件 | MTU 没协商成 | 靠近设备（1 米内），远离 2.4G Wi-Fi 路由器 |

设备名格式是 `WhaleBT-XXXX`，XXXX 是 MAC 后两字节。

---

## 三、刷完怎么用

| 操作 | 结果 |
|---|---|
| 手机连上设备 | 屏幕 LINK 卡片显示 MTU 与连接状态 |
| App 点「要下一张照片」 | 手机扫相册 → 转码 → 推给设备 → 设备**约 2 秒**后显示 |
| 设备**长按 OK** | 同样效果，从设备侧发起（人不在手机旁时用） |
| 设备**短按 OK** | 让手机查一次 DeepSeek 余额 |
| 设备**短按 UP** | 统计数字清零 |
| 设备**长按 UP** | 显示内建自检图 |
| 设备**短按 DN** | 收起资源面板，回到四卡片界面 |
| 设备**长按 DN** | 重新开始广播 |

存储区约 **4.9 MB**，最多放 **16 个条目**。同一张照片重复传会原地覆盖，
不占新空间。刷固件会全部清空（如上文所述）。

---

## 四、校验下载完整性

下载后确认没坏：

```bash
sha256sum -c SHA256SUMS.txt      # macOS / Linux
certutil -hashfile WhaleLink-1.0.apk SHA256   # Windows 单文件查
```

Windows 上想批量校验，用 PowerShell：

```powershell
Get-Content SHA256SUMS.txt | ForEach-Object {
    $hash, $file = $_ -split '\s+\*?', 2
    $actual = (Get-FileHash $file -Algorithm SHA256).Hash.ToLower()
    "{0}  {1}" -f $(if ($actual -eq $hash) { "OK  " } else { "BAD " }), $file
}
```

---

## 版本

| 项 | 值 |
|---|---|
| 版本 | 1.0 |
| 固件目标 | ESP-IDF v5.5.3 / esp32c3 |
| App minSdk | 26（Android 8.0） |
| 构建日期 | 2026-11-06 |

固件与 App **必须配套**：协议改过之后新旧版本混用会连不上。
升级时两个文件一起换。
