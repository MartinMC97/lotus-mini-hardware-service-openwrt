# 可选音量状态持久化

`--state-dir /绝对路径` 必须显式启用，只接受已有、服务UID所有、0700式私有
目录，不自动创建。支持mock和已启用volume-alsa的controls；只读模式拒绝。
默认不写文件、不从旧应用导入音量、不自动创建默认音量。

目录中volume.lock采用flock排他锁，防止不同socket实例同时保存同一目录。
volume.json格式为 `{"version":1,"percent":42}`，0600、同UID、单硬链接的
常规文件；不跟随符号链接，拒绝FIFO、超长文件、重复键、错误版本/字段。

启动读取有效文件：mock恢复模拟音量；controls显式把保存的全局音量应用到
后端。只有指定state-dir才会做该写入；不指定时仍只读当前mixer。missing或
损坏文件不覆盖当前音量，也不改写文件；状态/日志明确unsaved或error。保存
文件不是旧应用偏好，不能在每次应用连接时恢复应用自己的音量。

## 保存与失败语义

- 只有成功的volume.set/adjust安排保存；后台读到未迁移程序的外部写入不会
  偷偷持久化。频繁命令在2秒窗口合并，只保留最后值，同值已保存时不重复写。
- 写文件和fsync在独立线程，不阻塞socket主循环；队列只有一个最新值槽位。
- 同目录独占新建临时文件、写入、file fsync、renameat、directory fsync。
  正常退出处理待保存值，不重试失败写入形成闪存/日志风暴。
- 失败不回滚已经生效的输出音量。API成功代表硬件/模拟值应用成功，响应
  persistence和状态中的volume_persistence分别报告pending/saved/error。
  日志与volume.persistence事件报告阶段；失败后须新命令才再尝试保存。
  failure_count/last_failure保留历史失败，即使新请求已在排队或随后保存成功，
  也能在下一次控制循环报告，避免快速状态切换掩盖失败；它们不等于当前值未保存。
- rename前失败保留旧文件；目录fsync失败时新值可能已可见，但持久性不确定，
  报directory_sync_uncertain，不伪装成功。saved_percent是最后确认同步的值。
- 断电/SIGKILL可能丢失未保存的最近2秒调节；正常退出最后一次保存可能早于
  2秒窗口。底层fsync可能卡住，退出等待不是硬实时有界保证。

state.persistent 现仅表示是否显式启用持久隐私，不用于判断音量保存。
麦克风需要另加 --privacy；音量保存不隐式解除静音。

OpenWrt 第一版启用 --initialize-volume：缺失 HA Volume 才创建兼容用户
控件和 dB scale TLV，初始 35%；已有控件校验后保留现值，再应用有效保存。
此过程只打开 CTL，不打开 PCM，不能导入旧应用的历史音量。

## 验证记录

2026-10-05：23项socket/fixture测试通过，覆盖合并、同值不写、重启恢复、
退出排空、损坏状态、显式修复、符号链接保护、FIFO及不安全目录、独占锁。
ALSA-enabled daemon的23项ASan/UBSan回归通过，AArch64/musl编译通过。

额外store-fault-test用链接器注入fsync失败，不提供生产故障开关：文件同步
失败旧值仍在；目录同步失败新值可见但saved_percent保持旧值；显式重试成功，
无残留临时文件。这不是实际NAND断电测试或ENOSPC验证。

音箱RAM `/tmp/lotus-hw-state.aNOhom` 中用**mock后端**验证41%调到42%、保存、
重启恢复42%；实际mixer保持179,179，卫星idle。不把tmpfs测试声称为NAND
生产持久化，不把mock重启声称为真实mixer/冷启动恢复验收。

后续第一版已部署 /etc/lotus-hardware 保存并验证 mixer 恢复。
仍未执行实际断电/满盘与 24–72 小时长稳；
没有刷写 Boot/Rootfs/Userdata 分区。
