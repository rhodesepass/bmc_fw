# BMC 独立恢复组件

在正常应用加载前检查 RTC 启动计数和按键手势，必要时进入 ESP ROM 下载模式。
恢复入口不依赖 BLE、Wi-Fi 或 APP 文件系统；GPIO 从同一 `sdkconfig` 读取。

bootloader 与应用首次安装时需使用相同 RTC 保留配置：

```text
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE=0x10
CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC=n
```

连续三次启动未获得 APP READY，第四次启动进入恢复；启动时按住至少两秒、
在五秒窗口内松开并稳定，也可请求恢复。计数仅在 APP READY 后清零。
实际进入 ROM、D1s FEL 和供电时序仍需结合具体硬件验证。

主机回归：`python3 bootloader_components/bmc_rescue/tests/test_rescue.py`。
