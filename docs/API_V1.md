# 本机 API v1

## 模式与能力

`--mock`、`--read-only`、`--controls` 三选一；controls 必须显式
`--handoff-confirmed`，由操作者先停止旧控制者，有真实硬件副作用。
只读模式只查询传感器，不打开 GPIO、evdev、ALSA，不提供控制或租约。
当前真实 profile 为 Lotus Mini；不是任意主板自动适配。

- `--volume-alsa`：controls 的 ALSA CTL 音量，绝不打开 PCM。
- `--initialize-volume`：显式创建缺失的已知 softvol 控件，保留已有值。
- `--state-dir /私有绝对目录`：可选全局音量保存，mock/ALSA controls 可用。
- `--privacy`：必须同时 state-dir，mock/controls 可用；异步持久软件隐私。
- `--global-keys`：仅 controls+privacy+volume；物理音量/静音统一执行。
- `--sysfs-root /sys --input-root /dev/input`：显式设备根，测试可给 fixture。

未启用 privacy 的 mock 为内存模拟，controls 隐私不可用，不应据此允许录音。
缺传感器/设备返回 unavailable/null 或 not_supported/device_error，不能补成 0。
电池 five_step；external_power_connected 在状态 Unknown/缺失时为 null。
温度按 thermal type 匹配，单位 millidegrees_celsius，同 type 歧义明确拒绝。

## 传输与安全

Unix SOCK_STREAM，UTF-8 JSONL。每帧 ≤16 KiB、嵌套 ≤16，拒绝重复键、非法
UTF-8、尾部垃圾；≤16 客户端。socket 0600，私有父目录，不替换已有路径。
仅服务同 UID，通过 SO_PEERCRED 核验；没有组授权，不能隔离同 UID/root。
最大输出队列 64 KiB，慢客户端断开；不会重放事件或无限积压。

连接即收到 snapshot。请求必须含 version=1、id（0..2^53−1 整数）、method、
params 对象。响应按 id 匹配，事件可能在响应前到达。解析/ID 错误 id=null。

```json
{"version":1,"id":1,"method":"state.get","params":{}}
{"id":1,"ok":true,"result":{"microphone_muted":true}}
{"event":"privacy.changed","sequence":12,"data":{"muted":true,"source_connection":1}}
```

示例响应仅示意字段，实际 state 包含完整能力相关状态。

| 方法 | params | 语义 |
|---|---|---|
| capabilities.get | `{}` | backend、可用能力与约束 |
| state.get | `{}` | 当前缓存状态、owner、保存结果 |
| events.subscribe | `{}` | 开始订阅全部事件，返回 sequence+snapshot |
| events.unsubscribe | `{}` | 停止订阅 |
| lease.acquire / renew | `{"ttl_ms":15000}` | led/action_buttons 成组排他租约，TTL 1000..60000 |
| lease.release | `{}` | 释放业务与关联音频登记 |
| audio.acquire | `{"capture":false,"playback":true}` | 仅业务 owner；至少一项 true，capture 要求未静音 |
| audio.release | `{}` | 仅音频 owner |
| led.set | 见下 | 仅业务 owner；包括静音状态下的灯效 |
| volume.set | `{"percent":35}` | 全局整数 0..100，写后读回 |
| volume.adjust | `{"delta":-5}` | 非零整数 −100..100，夹紧到 0..100 |
| privacy.set | `{"muted":true}` | 全局软件隐私，不授予应用许可 |

错误：invalid_request、unsupported_version、not_supported、busy、
permission_denied、device_error。未知方法或写入失败不假装成功。

## 租约与录音

先到先得，绑定连接；退出、断线、释放、TTL 到期清除业务和音频登记，尝试
关闭灯。麦克风静音不改变灯租约、不覆盖灯效；静音指示的颜色、亮度和夜间策略由当前应用决定。
应用失联或租约释放时熄灯，硬件服务不自行生成静音红灯。静音仍拒绝采集登记并撤销已有采集登记。
不自动重连/抢占/恢复录音，不阻止绕过接口直接访问 ALSA 的进程。
audio.enforcement 和 privacy_enforcement 都是 cooperative。
录音应用必须同时检查自身许可、有效隐私状态及 audio.capture；每次读取前后
检查，静音/失联立即停止 KWS、PCM 上行。服务不拥有麦克风电源或音频流。

## 灯与按键

灯 effect：off / solid / breath / gradient。非 off 必须 color（0..16777215）
和 brightness_percent（0..100）；breath/gradient 还需 period_ms（100..60000），
gradient 需 color_2。可选 duration_ms（100..60000），到期自动关闭。
颜色为十进制整数，JSON 不接受 0x 语法。mock 不运行 PWM。

controls 用可取消的 100 Hz PWM worker；成功响应表示提交/初始输出成功，
后续 I/O 仍可能失败，推送 led.availability 并限频记录。不能保证故障节点熄灯。
主题色、待机定时和业务状态由应用决定；服务不含助手状态机。

按键按设备身份和能力识别，不固定 event 编号，无 EVIOCGRAB。
input_lost 取消手势；交接屏蔽旧时间戳，不将旧按键转交新 owner。

- 播放 164：short/double/long；button.action 仅投递业务 owner。
- 音量 114/115：global_keys 模式在新按下时 −5/+5，一次一档，忽略 repeat，
  使用内核单调时间戳抑制释放后 30 ms 内的抖动。
- 麦克风 248：按下切换全局请求状态；完成对应保存后才反馈最终状态。
- button.observed 发给订阅者，含 code、gesture、time_ms、owner_connection、
  action_authorized；time_ms 为单调服务运行时间。它不授权执行业务。
- button.feedback 只发给当前 owner，含 code、muted、percent；应用仅播放提示音，
  不再增减音量/切换静音。保存中若 owner 或请求改变则取消陈旧麦克风反馈。

未启用 global_keys 时音量/隐私仅观察，不能多个订阅者各执行一次。

## 状态保存与事件

事件含全局递增 sequence 和 data：volume.changed / persistence / availability、
privacy.changed / persistence、led.changed / availability、lease.changed、
audio.changed、battery.changed、temperature.changed，以及按键事件。
音量/隐私变化附 source_connection；不要收到变化再写回形成循环。
正常 lease.renew 不制造 INFO 日志/lease.changed 风暴，DEBUG 可跟踪续租。

state.persistent 表示是否启用了持久隐私（兼容字段），不表示所有字段都已保存。
必须分别检查 privacy_persistence、volume_persistence。
privacy.set 回应含 muted、persistent、persistence：请求解除时 muted 仍为 true、
pending=true；对应文件及目录同步成功后才真正 false。失败维持 true。
音量命令成功表示 mixer 已应用，不等于文件已保存；2 秒合并写，saved/error
分开报告。文件格式、锁、故障和断电窗口见
[隐私](SOFTWARE_PRIVACY.md)、[音量保存](VOLUME_PERSISTENCE.md)。

## 客户端与日志

CLI `--socket PATH METHOD [JSON_PARAMS]` 执行完断线，不能跨两次命令保留租约。
`session` 接收 method/params JSONL 并保留连接，无自动续租；watch 只订阅，不抢控制。
Connection 是单线程 API，事件队列 ≤64；请求超时或无效响应断开，普通事件
空闲 timeout 不断开。ApplicationClient 为线程安全适配，事件队列 ≤32、
每 500 ms 重核状态、每 5 秒续 15 秒业务租约；失联清除许可，无自动回退。
SDK 导出 protocol/connection/application_client/privacy/picojson 头文件。

stderr JSON 日志由 procd 收入 logd；默认 INFO，--debug 提供请求跟踪。
日志含 instance、单调 uptime、level、event、connection/result；不含原始
请求体、音频、语音文本或应用密钥。全局限频 100 条/秒，异步队列 ≤64，
单条 ≤2048 bytes，丢弃有汇总；不记录每帧 PWM 或每次正常传感器轮询。
正常退出尝试有限排空日志，接收端堵塞可丢最后日志；fsync 不承诺硬实时退出。

SIGTERM/SIGINT 清理自身 socket；SIGKILL 遗留路径须操作者确认后精确清理。
安装、交接、重启及回滚见 [使用指南](V1_INSTALLATION.md)。
