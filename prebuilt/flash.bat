@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

set "BIN=%~dp0WhaleLink-1.0-full.bin"

if not exist "%BIN%" (
    echo [错误] 找不到固件文件：
    echo         %BIN%
    echo         请确认 flash.bat 和 bin 在同一个目录里。
    pause
    exit /b 1
)

if "%~1"=="" (
    echo.
    echo 用法：flash.bat COM3
    echo.
    echo 不知道板子是哪个口？看下面列表（插拔一次对比哪个多出来）：
    echo.
    powershell -NoProfile -Command "Get-CimInstance Win32_SerialPort | Select-Object DeviceID,Description | Format-Table -AutoSize" 2>nul
    powershell -NoProfile -Command "Get-PnpDevice -Class Ports -Status OK | Select-Object FriendlyName | Format-Table -AutoSize" 2>nul
    echo 提示：ESP32-C3 的原生 USB 口通常显示为 USB 串行设备 / USB JTAG serial。
    echo.
    pause
    exit /b 1
)

set "PORT=%~1"
echo.
echo ================================================
echo   WhaleLink 固件刷写
echo   端口：%PORT%
echo   文件：%BIN%
echo ================================================
echo.
echo [注意] 从 0x0 整片写入，会清空设备里的照片与配对信息。
echo        这是正常行为，刷完等于出厂状态。
echo.
choice /C YN /M "继续刷写"
if errorlevel 2 exit /b 0

echo.
echo [1/2] 检查 Python 与 esptool ...

set "PY=python"
%PY% -c "import esptool" >nul 2>&1
if errorlevel 1 (
    set "PY=py"
    !PY! -c "import esptool" >nul 2>&1
    if errorlevel 1 (
        echo.
        echo [错误] 没找到可用的 esptool。
        echo.
        echo 分两种情况：
        echo   A. 电脑上根本没装 Python
        echo      -^> 去 https://www.python.org/downloads/ 装一个
        echo         安装时务必勾选 "Add python.exe to PATH"
        echo   B. 装了 Python 但没有 esptool
        echo      -^> 打开命令提示符，运行：  pip install esptool
        echo.
        echo 装完关掉这个窗口，重新双击 flash.bat。
        pause
        exit /b 1
    )
)
echo        使用 !PY!，esptool 已就绪

echo [2/2] 开始刷写，大约 30-60 秒，过程中不要拔线 ...
echo.
!PY! -m esptool --chip esp32c3 -p %PORT% -b 460800 ^
    --before default_reset --after hard_reset ^
    write_flash 0x0 "%BIN%"

if errorlevel 1 (
    echo.
    echo ================================================
    echo   刷写失败
    echo ================================================
    echo.
    echo 常见原因，按顺序排查：
    echo   1. USB 线只能充电不能传数据 -- 换一根线
    echo   2. 端口号写错 -- 重新运行 flash.bat 不带参数看列表
    echo   3. 端口被别的程序占用 -- 关掉串口助手 / Arduino IDE / 另一个 idf.py
    echo   4. 板子没进下载模式 -- 按住 BOOT 键再点一下 RESET，然后松开 BOOT
    echo   5. 降速试试 -- 把命令里的 460800 改成 115200
    echo.
    pause
    exit /b 1
)

echo.
echo ================================================
echo   刷写完成
echo ================================================
echo.
echo 设备应该已经自动重启。屏幕出现四块卡片（LINK/TIME/MOTION/FILE）
echo 就说明成功了。
echo.
echo 接下来：手机上装 WhaleLink-1.0.apk，打开后会自动搜索 WhaleBT-XXXX。
echo.
pause
