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

## 测试

```sh
python3 -m unittest discover -s tests
python3 bootloader_components/bmc_rescue/tests/test_rescue.py
```

主机测试中的 BLE/HTTP/寄存器模拟不替代实板验证。可选的 FEL/uopbridge 恢复
下载需要另行提供 uopbridge，并通过工具的 `--uopbridge` 指定路径。
