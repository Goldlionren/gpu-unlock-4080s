# RUN08：取消初始选择器固定值检查

2026-10-26 09:15，用户正常重启后提供 RUN08 和当前性能检查。已安装的有限 SS0 允许列表版本正确加载，但仍在 `cold-prom-profile` 阶段停止：

```text
SS0 first=0 second=0 SS1 first=0 second=0 stable=1
cold profile rejected: non-SS0 mismatch; PROM not read; firmware not started
FINAL status=0x8000000000000006 phase=cold-prom-profile attempted=0 firmware_started=0 restored_entry_command=0 performance_validated=0
```

报告中的其余 19 个初始寄存器均匹配原参考。PCI Command 已恢复，配置 differences=0，资源释放成功。Secure Boot=1，安装/报告 build ID 匹配；不是签名拒绝，也不是固件事务运行中失败。PROM 尚未读取，不能称为该次 PROM 已验证。

用户随后两次 8192-only 测量均值 FP16 108.27、BF16 108.42 TFLOPS，状态为 STOCK_PERFORMANCE_LIKELY。原始报告、两次 benchmark JSON 和状态输出保存在本目录，原始文件哈希见 `RUN08-verification.json`。

## 为什么修改前一个 workaround

前一补丁仅把 SS0 初始允许值扩展为 `0x64113035/36/37`，仍把 SS1 固定为 2，因此不能处理本次稳定的 0/0 状态。把有限观测值继续加入列表不能解决入口将可变选择器状态当作板卡身份的问题。

本项目核心 `src/vendor/unlock_610.c` 中，初始选择器值用于记录和写入前读回，不参与显存布局或 DMA 地址计算。`write_selector()` 写入完整目标值并检查读回；`inaccessible()` 拒绝全一值及 `0xbadfxxxx`。这里不声称已解释寄存器为何在启动间变化，也不把 0/0 视为硬件事务已验证成功。

## 当前修复（diagnostics revision=3）

仅修改外层 `src/app.c` 的初始选择器准入规则：SS0/SS1 不再要求等于某个固定值或属于初值列表，改为拒绝 MMIO 读取错误、全一值和 `0xbadfxxxx`；两次初始读值必须相同，PROM 双读/哈希通过后的第三次读值也必须相同。初始 0/0 可通过这个条件，但不免除其他检查。

板卡 PCI 身份、拓扑、BAR、所有非选择器初始寄存器、PROM 精确哈希与固定资产仍严格检查。核心执行代码未修改；解锁后仍要求 SS0=0x88888888、SS1=8，FLR 前后均匹配，必要 PLM、DMA 清理与设备恢复成功。没有增加自动固件重试。

## 模拟验证与实际安装

- app：67 个场景、76519 项断言；包含稳定 0/0、SS1 变化、初始/复读哨兵值、PROM 前后漂移、读取故障、PROM 错误及非选择器状态错误。
- core：48 个完整事务模拟场景。新增 0/0 起始成功路径、0/0 起始的 SS0/SS1 写入拒绝路径，以及 `…36/2`、`…37/2` 起始路径。模拟写入被拒绝时 result 非零，不能因恢复成功误报解锁成功。
- port 14369 断言、admission 27 场景/2141 断言、boot-media 76 项检查及 PE/源码/资产检查通过。

所有这些测试都是模拟硬件；不能替代实际零初态下的下一次启动测试。

| 项目 | 值 |
|---|---|
| build ID | `0cbbea2b718f0616b78ca023c67d692a4cb567234a3970435bbe4951a5864f4d` |
| unsigned EFI SHA256 | `4aa33537785400e821b53cecfc4946695e12f359c1cd2d640bf56fb98f71ed22` |
| signed EFI SHA256 | `13ce504d6639614a7fe73ea94228079993274dcf6a282cf27f73ff28f5b260a2` |

09:19 已沿用现有证书签名并安装：核验系统 ESP、全部资产、原签名 EFI，备份后更换 GPUUnlock EFI/manifest，再回读哈希和验签；原 Windows EFI 未变，安装前后信任库精确匹配、SecureBoot=1、SetupMode=0，临时挂载撤销成功。未改密钥、启动顺序或重启。隔离构建保留旧 bundle 依赖的原 build 文件。

本机记录为 `LOCAL-disk-install/20261026-091913-selector-state-bb826bcf/install-state.json`，同目录保留旧签名 EFI 和 manifest。当前只完成修复版安装，实机执行及性能仍待下一次报告；此前 RUN07 的成功不覆盖此新构建。
