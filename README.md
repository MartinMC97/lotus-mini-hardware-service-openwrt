# Lotus Mini Hardware Service

面向 OpenWrt/Linux 的独立本机硬件服务。应用通过 Unix domain socket 调用
统一接口，不直接操作 GPIO、输入设备、ALSA mixer 或传感器节点。

当前状态：**第一版功能已实现，并已完成 SDK 打包与两个应用的实机接入**。
全局音量、软件隐私、资源租约、RGB 灯效、按键和传感器集中管理。
这是开发版；实物按键/语音交互复测、断电故障和长稳仍需另行验收。
不开放 TCP/WebSocket 端口；
不提供录音、音频解码、PCM 传输或网络配置功能。

已实现接口与限制见 [API v1](docs/API_V1.md)。主机可执行 `make test`。
软件隐私策略和协作式保护边界见 [软件隐私](docs/SOFTWARE_PRIVACY.md)。
安装、SDK 构建、应用接入和回滚见 [第一版使用指南](docs/V1_INSTALLATION.md)。

GitHub Actions 在推送到 `main`、提交 PR 或手动触发时运行主机测试，并用固定
SHA-256 的官方 OpenWrt 25.12.5 rockchip/armv8 SDK 构建 AArch64 musl APK。
构建会安装所需依赖；APK、构建来源和 SHA-256 校验文件保存在 Actions artifacts。
推送与软件包版本一致的 `v<版本>-r<修订>` 标签会自动发布 GitHub Release 附件。
开发 APK 未签名；此流程只构建用户空间包，不构建内核或整机固件。

目标组件：

- `lotus-hardwared`：硬件服务与设备后端。
- `lotus-hwctl`：命令行调用和事件观察工具。
- 轻量客户端接口与接入示例。
- OpenWrt SDK 软件包、UCI 配置与 procd 启动入口。

设计原则：本机访问、能力查询、资源控制权、明确失败结果、有限资源开销。
不绑定某个语音平台，缺少某项硬件时允许应用正常降级。
