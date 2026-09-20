# BMC 网页客户端

`index.html` 是内嵌终端库和许可证的离线 BLE 控制台；`ota.html` / `ota.js`
支持 BLE、Wi-Fi 固件更新。`ota.html` 和 `ota.js` 同时被 BMC 固件嵌入。

在仓库根目录启动本地服务：

```sh
python3 -m http.server 8877 --bind 127.0.0.1 --directory web
```

使用支持 Web Bluetooth 的浏览器打开 `http://127.0.0.1:8877/`，选择
`EPASS-BMC`。远程访问需要 HTTPS；设备仅允许一个 BLE 客户端连接。
控制台提供 APP 终端、BMC 日志、状态与启动控制；升级页支持系统、WCH 和
ESP 应用。ESP 更新需先暂存、再提交和重启；重启后重新配网查询运行版本。

修改控制台源码后重新生成自包含页面：

```sh
python3 web/build_console.py
uv sync --project tools --group web
uv run --project tools --group web python tests/web_console_test.py
```

网页测试使用模拟 GATT，需要本地服务及 Chrome；输出位于 `build/web-test/`。
第三方终端库及其原始许可证保存在 `vendor/`。
