# 第一版使用与移植指南

适用 Lotus Mini / OpenWrt 25.12、AArch64 musl、现有 ALSA `LotusMini` 声卡。
服务 0.1.0-r7；客户端为 SDK 导出的 C++ header-only 接口。
没有网络端口、HA 实体、PCM 或提示音素材；业务由调用方实现。

## 构建与安装

```sh
make test
sh openwrt/build-sdk.sh /path/to/matching-openwrt-sdk
```

输出在 `openwrt/artifacts/`，不构建内核/固件。应用 feed 使用
`PKG_BUILD_DEPENDS:=lotus-hardware-service`、运行依赖 `+lotus-hardware-service`，
include SDK 的 `usr/include/lotus-hardware`。首次构建应用前先构建硬件服务。

```sh
apk --no-network add --allow-untrusted /tmp/lotus-hardware-service-0.1.0-r7.apk
```

正式发布应签名；`--allow-untrusted` 仅用于已核对来源及 SHA-256 的开发包。
首次安装禁用自启。升级前先停服务并把 `main.enabled` 暂设 0，防止 APK 的
默认升级钩子提前启动；部署核对后再恢复配置。备份旧二进制、init 及配置，
备份可能含密钥，必须私有保存，不能提交 Git。

## 完成交接

1. 停止直接操作硬件的旧应用，检查录放 PCM 均 closed。
2. 安装服务接口版应用，设置该应用 `main.hardware_service=1`。
3. 检查 `.apk-new`；保留旧配置不等于新版 init 生效。核对差异并保留用户
   自定义内容后部署新版脚本，不能盲目覆盖所有配置。
4. 启动硬件服务，再明确启动一个应用；不支持同时占用声卡。

```sh
uci set lotus_hardware.main.enabled='1'
uci set lotus_hardware.main.handoff_confirmed='1'
uci commit lotus_hardware
/etc/init.d/lotus_hardware start
lotus-hwctl --socket /var/run/lotus-hardware/control.sock state.get
```

确认后可 `/etc/init.d/lotus_hardware enable`。服务 START=90，应用 START=95；
保留应用原有自启选择和开麦授权。解除全局静音不授权或启动应用。
缺失/损坏记录默认静音；解除后等待保存成功才能开麦。
从 r7 起，静音灯的颜色、亮度与夜间策略由应用通过独占灯租约提交，服务不覆盖灯效。
应用退出或租约失效时熄灯。旧应用适配器需要同步更新，不能继续跳过静音时的灯效提交。

```sh
lotus-hwctl --socket /var/run/lotus-hardware/control.sock privacy.set '{"muted":false}'
lotus-hwctl --socket /var/run/lotus-hardware/control.sock state.get
lotus-hwctl --socket /var/run/lotus-hardware/control.sock volume.adjust '{"delta":-5}'
logread -e lotus-hardwared
```

首次迁移读取当前 mixer，不在应用连接时恢复旧偏好。明确设置已核实值并等
`volume_persistence.status=saved` 才跨重启保存。`--initialize-volume` 仅在
`HA Volume` 不存在时创建兼容控件，起始 35%，随后恢复有效全局记录；不打开
PCM、不覆盖已有控件。它复用现有 softvol 和 0.15 倍保护缩放。

## 新应用接入

设置 `LOTUS_HARDWARE_SOCKET=/var/run/lotus-hardware/control.sock`，使用
`hw::ApplicationClient.start()`。取得快照、成组业务租约及音频登记；业务租约
15 秒，每 5 秒续租。采集还必须有应用本身的用户许可。
服务模式不同时运行直接后端，失联不自动回退、重连或抢占。

- 灯：`client.led()` 执行效果；主题、业务状态、待机 150 秒周期属于应用。
- 物理音量/静音由服务执行，应用仅用 `button.feedback` 播放提示音。
- 只有 owner 的 `button.action` 才能执行业务；observed 仅观察。
- 读取 PCM 前后核对隐私/音频登记；静音或失联停止采集、KWS 和上行。
- 音频子进程用 `start(path,error,false)` 观察，不抢租约；采集须检查
  audio.capture=true。输出使用公共 `playback.conf`，不重复软件调音量。
- 传感器缺失明确降级，不把 unavailable/null 显示成 0。

Connection 是单线程接口；ApplicationClient 提供受锁保护的工作线程。
状态每 500 ms 核对，命令超时 1 秒，有界事件队列；不是硬实时保护。

## 故障与回滚

服务不自动 respawn，应用失联停录，不自动重新接管。先查日志、磁盘、节点及
进程，再启动服务并明确重启应用。SIGKILL 遗留 socket 时，确认无存活实例后
只删除已核实的 `/var/run/lotus-hardware/control.sock`，不清空运行目录。

回滚必须成套：停应用、停/禁用硬件服务，恢复备份的应用二进制、配置和 init，
关闭该应用 hardware_service，再按原选择启动。不能并行运行旧直接后端和
新服务；不要恢复整个 overlay 或触碰 NAND 分区。

## 验收边界

目前 profile 仅 Lotus Mini，缺节点
明确降级，不声称其他主板开箱即用。同 UID 访问，不提供组授权或 root 隔离。
软件隐私不是物理断电，音频登记不禁止绕过接口访问 ALSA。
实际断电/满盘、完整语音对话和 24–72 小时长稳须另行验收。
