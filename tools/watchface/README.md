# 表盘资源、播放和双缓冲

当前工程按参考照片排列时间、日期、天气、电量和四组随机短句。短句使用站酷快乐体；时间字体与之前确认的版本相同。底部中间为 `hjc->dy`，动画在短句上层。底部导航提示条和最近任务页关闭，应用内短上滑返回保留。

## 双缓冲与内部内存预算

表盘使用屏幕原生竖屏，工程中没有运行时旋转调用。旧配置即使角度一直为 0，也会为软件旋转预留一块与绘图缓冲同样大的内部 DMA 内存。

| 配置 | 绘图缓冲 | 旋转备用缓冲 | 合计 |
| --- | ---: | ---: | ---: |
| 能正常启动的单缓冲版 | 16,400 B | 16,400 B | 32,800 B |
| 曾导致 AIChats 安装失败的双缓冲版 | 32,800 B | 16,400 B | 49,200 B |
| 当前双缓冲版 | 32,800 B | 0 B | 32,800 B |

以上是像素缓冲大小，不含分配器的少量元数据。BSP 配置新增 `flags.sw_rotate`，主程序将其关闭；通用 `bsp_display_start()` 仍保留软件旋转。当前表盘配置只支持原生方向。主程序核对 LVGL 实际处于双缓冲模式，并在启动日志打印缓冲和内部内存数据。没有把 AIChats 的任务栈迁到 PSRAM。

用户提供的单缓冲实机日志中，AIChats 安装成功；`WatchGifPerf` 解码约 15ms，`FacePerf` 渲染平均约 100ms、5 秒更新 30 次。双缓冲能重叠渲染与传输，但不足以独自解决实时缩放的全部计算开销，因此本轮还预处理了动画。

## 动画预处理

源目录 `../手表素材` 只读，11 个源 GIF 保持不动。`prepare_gifs.py` 用 Pillow 完成原帧的透明叠加，按全动画可见边界裁掉透明外圈，再提前缩放到角色最大边长约 260 像素。固件直接显示目标大小，不再每帧对整块透明画布执行 LVGL 缩放。

- 保留全部 373 帧，包括视觉相同的重复帧；使用逐帧写入避免 Pillow 自动合并重复帧。
- 每帧原始延时保持，包含 40ms、80ms 和 710ms 等不同停留时间；使用全帧 disposal 2，避免依赖原来的增量帧形状。
- 每个 GIF 使用同一张 128 色调色板，并预留独立透明索引。与源图预缩放结果的颜色存在少量量化差异；不声称逐像素颜色一致。透明蒙版逐帧完全一致。
- `gif-manifest.json` 分别记录源文件和生成文件的 SHA256、尺寸、原帧延时、调色误差。生成后的 GIF 与源文件字节不同。
- 当前生成 GIF 合计 2,815,599 字节。运行时只将随机选中的一个压缩 GIF 预读到 PSRAM，退出/熄屏先删除解码对象再释放数据。

播放仍使用累计墙钟截止时间；若负载过高，最多合成四个到期帧，只显示最后一帧，以免整段慢放。这是保底策略，不能通过关掉它来保证既不掉帧又不慢放。是否能显示所有帧需要实机确认。

## 结构与再生成

- `layout.json` / `generate.py`：界面坐标、颜色、四组短句、背景、天气图标和五个 LVGL 位图字库。
- `components/watch_shell/watch_face_view.c`：固件和电脑预览共用的界面代码。
- `patches/lvgl-gif/lv_gif.c`：基于 LVGL 9.5.0 的透明合成与播放时钟修复；CMake 替换上游文件，依赖刷新不会覆盖。升级 LVGL 时重新审查。
- `verify_gif_frames.py` / `preview_lvgl.c` / `render_lvgl.py`：Pillow 独立参考帧与真实 LVGL 解码/渲染对照。

使用装有 Pillow、numpy 的 Python：

```powershell
python tools/watchface/prepare_gifs.py
python tools/watchface/generate.py
python tools/watchface/render_lvgl.py
```

电脑原生预览使用本机 Qt Clang、VS2013、Windows SDK；固件常规构建不依赖这些预览工具。字体源文件在 `fonts/`。未取得原表盘字体，不声称与原照片逐像素一比一。

在 ESP-IDF 6.1 终端执行：

```powershell
idf.py build
idf.py -p COM7 flash monitor
```

本轮应用和 SPIFFS 动画都更新了，必须使用完整 `flash`，不能只用 `app-flash`。按实际设备修改串口。

## 验证与实机日志

`output/watchface/gif-runtime-verification.txt`：373 帧连续两轮像素与延时核对，以及不均匀短回调、60–110ms 繁忙回调追赶测试。`gif-manifest.json` 还验证了源帧与目标帧透明蒙版、帧数、延时及源文件不变。`preview-*.png` 是实际 LVGL 渲染结果。

- `Display buffers`：确认两个缓冲、软件旋转关闭，以及内部空闲内存。
- `Installing AIChats` / `Installed AIChats`：核对内部内存和启动是否通过。
- `WatchGifPerf`：`source/wall/speed` 表示动画进度与真实时长的比值；`decode_avg/max` 单位微秒；`catchup_skipped` 是因追赶而省略的中间显示帧数量。
- `FacePerf`：LVGL 渲染次数、平均/最大工作耗时和空闲 DMA 内存。它不等于面板物理刷新率。

判断是否达到不慢且无追赶跳帧，应同时看速度接近 100%、`catchup_skipped=0`、渲染频率能覆盖对应源动画的帧率，以及实机观感。电脑测试不提供 ESP32 实测帧率，本轮没有烧录。

构建日志：`firmware-double-buffer-build.log`。本轮前的代码与原始烧录资源备份在 `before-budgeted-double-buffer/`。

## 熄屏预备下一帧与 TE 同步

正常熄屏时先将 AMOLED 亮度设为 0，再读取下一张随机 GIF，解码首帧并暂停。压缩资源和首帧画布保留在 PSRAM；没有另加工作任务或同时缓存所有 GIF。正常唤醒复用这张已准备的首帧，刷新当前时间、状态及完整表盘，调用 `lv_refr_now()` 后才发送亮度开启命令。ESP-IDF 的 `panel_io_spi_tx_param()` 会先回收队列中尚未完成的颜色传输，确保背景和首帧均进入屏幕 GRAM 后才点亮。之后恢复 GIF 计时，首帧按原始延时停留。文件读取移到熄屏后，减少开屏路径耗时；资源准备失败时唤醒会在暗屏下重试。

原生回归对 11 张动画新增熄屏一秒、保持首帧、恢复后完整首帧停留时间检查；均通过。性能计数在唤醒时重置，避免把熄屏时长算作慢放。串口 `Prepared ... load=...ms` 是暗屏准备耗时，`Wake: complete frame visible in ...ms` 是唤醒绘制与亮屏耗时。

板载同步信号依据本地 V1.0 原理图：`LCD_TE -> GPIO13`。原有屏幕初始化已包含 0x35（TE 输出）及 0x44（扫描线）命令。现在通过 GPIO13 上升沿，在每次 LVGL 刷新的第一个传输块之前等待一个新 TE 信号；后续 20 行 DMA 分块不重复等待，以保留双缓冲吞吐。亮度为 0 时不等待 TE，避免息屏信号停止误触发超时。每次等待最多 35ms；连续三次缺失自动取消同步等待并记录警告，不永久阻塞启动。GPIO ISR 使用系统已有服务或默认服务，不强行改变触摸中断的 cache 行为。

TE 用于对齐写入屏幕 GRAM 与面板扫描的相位，实际撕裂改善还需实机验证。日志 `TE sync active: GPIO13, period=...us` 用于确认检测到同步脉冲。当前保留两个 20 行绘图缓冲及零旋转备用缓冲，没有重新增加一块大 DMA 缓冲。

参考：本地 `../ESP32-S3-Touch-AMOLED-2.06/hardware/ESP32-S3-Touch-AMOLED-2.06-Schematic-V1.0.pdf`；[官方原理图](https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf)；[Espressif 关于 TE 同步的说明](https://docs.espressif.com/projects/esp-iot-solution/en/latest/display/lcd/lcd_screen_tearing.html)。

本轮构建日志：`firmware-wake-te-build.log`。本轮前的代码在 `before-wake-te-sync/`。未执行烧录，不能据电脑回归确认硬件撕裂程度或具体唤醒延时。

## 息屏省电（第二版）

`watch_power` 在亮屏或音频会话占用时锁定 240MHz、禁止自动浅睡眠；息屏且音频空闲时释放锁，最低频率为 20MHz，并允许自动 Light-sleep。20MHz 是最低档位，RTOS 或驱动仍可临时请求更高频率。保留 Wi-Fi 连接并启用 MIN_MODEM 省电，不进入 Deep-sleep。CPU 上下文掉电关闭。保留 SDK 的 GPIO 复位规避措施，显式让外部板载 GPIO 在浅睡眠时维持原配置；Flash/PSRAM 引脚仍由 SDK 管理。

息屏暂停显示刷新、触摸读取和表盘状态定时器，输入/倒计时处理保留 50ms 周期。按键空闲时等待 GPIO 电平中断及浅睡眠唤醒，按下后暂时屏蔽电平中断，使用原 10ms 扫描消抖和长按判定，松开稳定后重新布防。触觉反馈请求主动通知线程。中断配置失败会记录警告并回退到原扫描方式。按键和抬腕事件通知 LVGL 线程处理，抬腕读取仍为 50ms。亮屏先恢复性能，在暗屏中画完完整首帧，再开启面板和亮度。LVGL 使用系统单调时间，停用冗余的周期 tick 定时器。空闲音频关闭编解码器和实际 I2S RX/TX 通道，并关闭功放；应用使用时保留正常恢复路径。

QMI8658 由抬腕服务统一管理，通过互斥锁串行化寄存器模式切换与加速度读取：亮屏 125Hz，息屏低功耗约 21Hz，前台重力球 500Hz；只开启加速度计，关闭当前应用未使用的陀螺仪。关闭抬腕且无前台运动应用时关闭传感器采样。重力球不再重复初始化、更改同一芯片量程，校准和抬腕都通过共享入口读取 m/s²；息屏停止其运动计算。传感器模式失败保留重试路径，应用退出采用协作停止，避免强制删除持有传感器锁的任务。

运行 `python tools/watchface/verify_standby_power.py` 检查生产代码的电源锁、I2S 停启适配和息屏/唤醒流程。`verify_low_power_inputs.py` 使用生产按键工作循环和 IMU 模式函数，检查空闲不扫描、消抖/长按、入睡布防竞争、失败回退，以及传感器在亮/息屏与应用间切换。报告为 `output/watchface/standby-power-verification.txt`、`low-power-input-verification.txt`，本版最终构建日志为 `firmware-standby-power-v2-final-build.log`。这些电脑回归不能确认硬件实际进入浅睡眠或续航改善幅度。

烧录后核对 `WatchPower: Policy ready`、息屏时的 `20MHz minimum / auto Light-sleep allowed`、按键的 `interrupt + Light-sleep wake`、传感器的 `IMU: low-power 21Hz, gyro off`，以及闲置音频的 `Audio stopped: PA GPIO46=0`。这些日志表示策略与关闭流程已执行，实际睡眠占比仍受驱动锁和后台任务影响；需要在电池供电下测量电流，并验证反复抬腕、短按/长按 BOOT、PWR、语音及音乐的恢复，以及进入/退出重力球后的灵敏度。

### 抬腕启动与首次息屏修复

实机日志出现 `Wrist gesture unavailable: ESP_ERR_NO_MEM`，随后 `Register wrist listener failed: ESP_ERR_INVALID_STATE`。Wi-Fi 启动后内部 SRAM 最大连续块只有约 1.5KiB，原 3KiB 抬腕任务栈无法分配。任务栈改用 PSRAM，TCB 保留内部 SRAM；按键和抬腕服务提前到 Wi-Fi 状态服务前初始化。成功日志为 `QMI8658 wrist monitor is running (stack=3072 in PSRAM)`。仍保留 20MHz 最低频率与传感器低功耗策略。

同时修复开机处于落腕姿态时只更新内部状态、没有向屏幕发送息屏事件的问题。传感器任务启动后仍保留 4 秒保护期，之后完成稳定采样再通知屏幕；晚注册的监听器由同一任务补发当前状态，避免资源加载期间漏掉第一次变化。

`verify_wrist_startup.py` 在 1.5KiB 最大内部块模型下检查 PSRAM 栈、初始化各阶段失败清理与重试，并运行实际姿态循环检查开机落腕、晚注册监听、抬落腕、消抖、tick 回绕及采样失败。报告为 `output/watchface/wrist-startup-verification.txt`，本修复构建日志为 `firmware-wrist-startup-fix-build.log`；尚需实机验证息屏与唤醒。

### 天气任务内存与启动修复

实机 `Start weather service failed [ESP_ERR_NO_MEM]` 表示天气任务尚未启动，随后 `app_main` 返回使表盘也没有继续初始化。天气的 6KiB 栈改用 PSRAM，任务控制块在 Wi-Fi 启动前分配；天气启动失败降为警告，允许表盘继续启动并使用已有缓存。系统状态任务仍保留内部 4KiB 栈，改为先分配再恢复 Wi-Fi，工作循环等待启动完成。

天气请求任务不在 PSRAM 栈中写 NVS。天气/城市更新只标记待保存，已有内部栈的系统状态任务复制快照后写入 NVS；写入失败保留标记并延后重试，并发更新不会在提交后被清掉。缓存城市仍为宜昌（101200901）。正常日志为 `Weather: Started, city 101200901 (stack=6144 in PSRAM)`，随后联网成功时输出新的温度和宜昌天气。

`verify_weather_memory.py` 检查小块内部内存下的 PSRAM 任务、启动失败与重试、HTTP/解析/分配失败保留旧数据，以及缓存从内部栈提交和 NVS 失败重试。HTTP、解析、NVS 和 RTOS 使用适配器，不调用真实天气接口，也不能确认实机 Flash/联网行为。报告为 `output/watchface/weather-memory-verification.txt`，本修复构建日志为 `firmware-weather-memory-fix-build.log`。

### USB 串口与浅睡眠

COM7 为芯片原生 USB Serial/JTAG（VID/PID 303A:1001），配置为第二日志输出；主 UART0 在 GPIO43/44，当前实际构建波特率为 115200。原生 USB 连接在浅睡眠中停止响应，亮屏后电脑端也可能不自动恢复。开启 SDK 的 `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`：检测到电脑 USB 主机时持有禁止自动浅睡眠的锁，屏幕仍可正常熄灭；拔掉 USB 后 SDK 自动释放锁，恢复电池省电策略。关闭串口监视器但保留 USB 数据连接也可能仍持有锁，测电池待机需拔线。

已断开的监视器应先保持手表亮屏，重新插拔 USB，并重新打开当前 COM7。此配置预防后续自动浅睡眠打断连接，不能修复已卡住的电脑端串口会话，也不表示已排除其他串口问题。构建日志为 `firmware-usb-console-sleep-guard-build.log`。参见 [Espressif USB Serial/JTAG 睡眠限制](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/usb-serial-jtag-console.html#sleep-mode-considerations)。

### 拔线待机睡眠统计

仅有 `Screen off` 或 `auto Light-sleep allowed` 不能证明芯片实际睡眠，也不能据此推算续航。诊断版通过链接器包装现有 `esp_light_sleep_start()` 调用，在 IRAM/DRAM 中记录成功返回的调用时长和次数，单独统计被拒绝的调用；不改变原 API 的返回值或睡眠决策，不新增任务/定时器。现有状态任务在 USB 未连接时每秒采样 `ESP_PM_NO_LIGHT_SLEEP` 锁总数，并在 USB 重连后输出内存中保存的省电窗口统计。USB 主机恢复与监视器打开之间可能有延时，因此同一条重连记录最多重复 5 次，间隔 5 秒，`replay=1` 表示重复记录。

`PowerSleep` 字段：`off` 从暗屏 GIF 预备完成、释放性能锁之后开始，表示省电窗口；`sleep_call` 是成功的浅睡眠 API 调用耗时总计，包含进出睡眠开销；`sleep_est` 是该耗时与窗口时长的比例估算，非精确硬件睡眠占比或电流测量。`ok/rejected` 为成功/失败调用次数；`battery_lock_samples` 为 USB 未连接时的锁采样数；`no_sleep_locks=min..max` 为采样时持有的禁止浅睡眠锁总数，不能覆盖所有瞬间变化。

验证流程：烧录诊断版后，拔掉 USB，在不使用应用的情况下息屏约 10 分钟；接回 USB 后不要复位，用 `idf.py -p COM7 monitor --no-reset` 打开监视器，保存 `PowerSleep:` 行。统计只存 RAM，重启或耗尽电池后会丢失。若 `sleep_est` 接近零或 `rejected` 很多，优先检查软件睡眠阻塞；若比例较高但续航仍短，再实测电流并排查 Wi-Fi、屏幕控制器/音频供电、电池容量与电量估算。

`verify_sleep_measurement.py` 使用真实测量模块与 SDK 适配器检查成功/失败计数、原返回值、USB 拔插后记录保留、重放、零时长及重复息屏等 200 轮场景。报告为 `output/watchface/sleep-measurement-verification.txt`；最终构建日志为 `firmware-sleep-measurement-final-build.log`。尚无本版实机睡眠比例或待机电流结果。
