# ePass BMC

基于 ESP32-C3 / ESP-IDF 6.0.2 的板级管理固件，负责 D1s 启动服务、
电源与电池管理、串口调试和固件更新。

## 功能与接口

- **SPI**：在 D1s SPI0 上提供正常 SPL，并承担 APP 电量、版本、READY、关机
  握手和 OTA 数据传输；D1s 的系统镜像存放在独立 SPI1 NAND。
- **UART**：转发 D1s 控制台，BLE 可分别读取 APP 串口与 BMC 日志。
- **I²C / GPIO**：连接 BQ25601、采样电池电压，控制 APP 复位、电源和实体按键。
- **BLE GATT**：设备名 `EPASS-BMC`，服务 UUID
  `6e400001-b5a3-f393-e0a9-e50e24dcca9e`。同一 UUID 模板的 `0002/0003` 为
  UART 写入/通知，`0004` 为控制，`0005` 为日志，`0006/0007` 为 OTA 上传/状态。
- **Wi-Fi HTTP**：BLE 配置 STA/AP 后，使用会话 Bearer token 访问
  `/v1/control`、`/v1/channels/ota`、`/v1/status`；
  `/v1/bmc/firmware` 用于查询或上传 ESP 应用。ESP 重启后需重新配网获取 token。

控制命令包括 `status`、`power-status`、`boot`、`reset`、`hold`、`fel`、
`wifi-sta`、`wifi-ap`、`wifi-stop`、`ota-status`、`ota-finish`、`ota-reboot`。
BLE 调试为单连接服务，未启用配对认证。引脚配置见
[board_pins.h](main/board_pins.h) 和 [Kconfig](main/Kconfig.projbuild)。

OTA 支持 D1s `boot/rootfs/uboot`、WCH `touch` 和 ESP `bmc`。
系统/WCH 更新前需要上传完整 U-Boot FIT；只更新 ESP 不需要 FIT。
ESP 应用写入备用槽，校验后暂存，`ota-finish` 提交，`ota-reboot` 重启。
不支持跨芯片原子更新或自动健康回滚；ESP OTA 不更新 bootloader、分区表或 SPL。

## 电池充电

BMC 在启动 BLE 和 APP 前配置 BQ25601：输入限流 500 mA、充电电流
480 mA、恒压 4.208 V、预充 120 mA、终止 60 mA，开启充电终止及
10 小时安全定时器，关闭芯片 I²C 看门狗。此配置用于首次充电验证，
不代表 4.5 V 电池的满充配置，也不代表已完成实测电流或温升验证。

配置过程先禁止充电，写入并回读后才开启充电；每两秒检查配置，修复
芯片复位或 PSEL 检测导致的参数变化。I²C 错误时尝试禁止充电并回读，
无法确认关闭时明确记录 `charge disable UNCONFIRMED`。ADC 初始化不影响
充电配置。芯片冷启动到 BMC 执行之间仍可能使用硬件默认参数。

`tools/bmc_ble.py charger-status` 通过现有 BLE 控制通道读取实时配置及
充电/故障状态；`verified=1` 表示配置寄存器匹配，`enabled` 表示软件充电
使能位，实际是否充电还取决于 CE、输入及保护条件。电流字段是设定值，
不是实测电流。`fault` 是当前故障，`latched` 是本次首次读取的历史故障。

首次升级优先在电量充足时使用纯电池供电进行 BLE OTA；确认新固件运行
且配置回读正确后再接 USB。电量不足时先断开电池，USB 供电升级。

## APP 供电交接

APP 断电时板级供电交接会短暂降低 BMC 电压。当前配置关闭 ESP32-C3 brownout，
应用入口同时关闭 detector、analog reset 和 interrupt，避免 bootloader 遗留的
模拟复位位在正常交接中触发复位。此改动配合现有板级时序使用，不表示低电压下
所有外设与 Flash 操作均已验证；日志会回读三项开关状态。

## APP 关机与 BMC 深睡

APP READY/epoch 和 Linux poweroff 协议不变。正常关机或五秒强制关机完成后，
等电源键释放稳定 40 ms，停止 BLE/Wi-Fi 并进入 ESP32-C3 Deep-sleep，
不保留调试窗口。关机采样周期与无线收尾会带来数秒以内的正常入睡延迟。
OTA 未完成、BMC 自身升级及救援状态不自动入睡。

GPIO18/19 保持高电平切断 APP 电源；APP_RESET 保持有效，跨域 SPI/UART/IRQ
置为无上下拉输入并 hold。深睡期间 BLE 不可连接，按电源键才启动 APP。
GPIO2 充电中断只唤醒最小 I²C/充电维护路径，不启动 APP 或无线。
GPIO2 持续为低时仅保留按键唤醒，加 60 秒定时重查；恢复为高后重新启用中断。
充电配置核验失败则保持 APP 断电并重试；新的按键开机请求仍可重新启动 BMC。

bootloader 与应用共享 16 字节 custom RTC 区域：救援计数、休眠标记及反码、
每次启动的能力标记。充电维护唤醒不增加 APP 未 READY 计数；真实开机重新
生成 epoch 并等待 READY。原 bootloader 按键救援入口保留。
`power-status` 增加 `sleep`、`sleep_error`、`wake`（唤醒原因位图）、
`wake_pins` 和 `sleep_block`；它们是软件状态，不能替代电源波形和功耗测量。

首次启用必须一起部署本次的 `build/bootloader/bootloader.bin` 和
`build/epass_bmc.bin`。应用 OTA 不会更新 bootloader；检测不到新版 bootloader
时显示 `sleep=bootloader_required`，保留原来的清醒关机模式。
使用现有 ROM 烧录链路更新 bootloader 和确认过的应用槽位即可，不需要重分区
或重置 otadata、SPL 分区。后续仅修改应用时可继续使用现有 OTA。

主机验证：

```sh
uv run --project tools python -m unittest discover -s tests -p 'test_*.py'
python3 bootloader_components/bmc_rescue/tests/test_rescue.py
```

实板验收需另行完成：20 次关机/按键开机、插拔与充满、长按不松手、已连接 BLE
关机、救援入口；用示波器确认 GPIO18/19 和 APP 电源在充电唤醒期间无上电脉冲，
检查跨域倒灌。比较电池侧原关机与深睡的稳态电流、唤醒峰值和平均电流。
固件构建和主机测试不代表这些实测已经通过。

## 构建与使用

启用 ESP-IDF 6.0.2 环境，使用仓库内完整 `sdkconfig` 直接构建：

```sh
idf.py build
uv sync --project tools
uv run --project tools python tools/bmc_ble.py scan
uv run --project tools python tools/bmc_ota.py --address <BLE地址> bmc build/epass_bmc.bin --boot
```

`tools/bmc-terminal` 提供交互终端；可用 `BMC_ADDRESS` 指定设备，否则自动发现。
`web/index.html` 是离线控制台，`web/ota.html` 是升级页面，使用方法见
[网页说明](web/README.md)。初次安装需 USB；SPL 由
`tools/pack_spl.py` 打包到 ESP 的 `d1s_spl` 分区，`idf.py flash` 不自动安装它。
bootloader 和应用的 RTC 保留配置必须一致，见
[恢复组件](bootloader_components/bmc_rescue/README.md)。

## 运行功耗与实验接口

运行态默认使用 `modem`：BLE 事件间 modem sleep，CPU 固定 160 MHz。
SPI/GDMA 的 APB 保持 80 MHz；BootROM 启动至收到有效 APP READY 期间锁定
CPU 160 MHz。UART 使用 XTAL 时钟，所有模式均禁用 light sleep，因为 APP
SPI 请求和 UART 字节没有提前唤醒握手。BLE 低功耗时钟使用主晶振，不依赖外部 32 kHz。

可通过 BLE 或 HTTP 控制接口进行同一固件的功耗 A/B：

```sh
uv run --project tools python tools/bmc_ble.py --address <BLE地址> pm performance
uv run --project tools python tools/bmc_ble.py --address <BLE地址> pm modem
uv run --project tools python tools/bmc_ble.py --address <BLE地址> pm balanced
uv run --project tools python tools/bmc_ble.py --address <BLE地址> pm-status
```

`performance` 为 CPU 160 MHz、BLE modem sleep 关闭；`modem` 仅开启 BLE
modem sleep；`balanced` 再允许 CPU 空闲降频。模式不写入 NVS，重启恢复
`modem`。`pm-status` 中 `cpu_mhz` 是处理查询当时的频率，不是空闲占比。
每次切换后等稳定再用独立电池端仪表测量，固件配置本身不代表节电实测。

无连接时默认采用 NimBLE 原始快广播间隔（当前配置为 30–60 ms）。
`ble-adv fast` 恢复默认，`ble-adv slow` 尝试 250–500 ms 慢广播；均在下一次
开始广播时应用，连接中设置后断开即可，不会停止 BLE host 或打断 OTA。
`ble-adv-status` 报告实际已启动广播的间隔和下次间隔，单位为微秒；连接中
`advertising=0`、`active_min_us=0`、`active_max_us=0`。该开关与 PM 档位独立，
不写入 NVS。通过 HTTP 在正在广播时修改也仅设置下一次广播的间隔。

本轮同素材实验中，modem sleep 观察到约 74 mW 的主要收益；DFS 另有约
8 mW、慢广播另有约 13 mW 的观察差。但最终试验固件出现一次尚未归因的
INT_WDT，慢广播阶段还发生多次 20 秒重连超时。因此默认舍弃 DFS 和慢广播
的小收益，优先保持运行与重连表现，实验命令继续保留。此默认调整不代表
已查明或修复看门狗问题，稳定性仍以保守固件的板上复测为准。

## 测试

```sh
python3 -m unittest discover -s tests
python3 bootloader_components/bmc_rescue/tests/test_rescue.py
```

主机测试中的 BLE/HTTP/寄存器模拟不替代实板验证。可选的 FEL/uopbridge 恢复
下载需要另行提供 uopbridge，并通过工具的 `--uopbridge` 指定路径。
