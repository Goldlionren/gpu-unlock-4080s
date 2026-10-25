# 初始 SS0 变化导致的拒绝与诊断更新

2026-10-26：先前 RUN03 在 Secure Boot 开启时完成过一次事务，随后 RUN04、RUN05 和彻底断电复测的 RUN06 均在 `cold-prom-profile` 阶段拒绝，未开始 FWSEC/SEC2 解锁事务。Secure Boot 仍开启，签名 EFI 正常进入；不是固件签名拒绝。

| 报告 | SS0 实际值 | 固定期望值 | 结果 |
|---|---|---|---|
| RUN04 / RUN05 | `0x64113036` | `0x64113035` | `attempted=0 firmware_started=0` |
| RUN06 | `0x64113037` | `0x64113035` | `attempted=0 firmware_started=0` |

三份原始报告保存在本目录。各次入口 PCI MEM 曾临时启用用于读取，然后恢复 Command=0；配置检查 differences=0，资源释放成功。不能称为全过程没有任何硬件写入。

变化原因尚未确定。旧代码遇到第一个差异即停止，因此无法证明余下寄存器/PROM 全部相同，更不能把 `…36` 或 `…37` 直接加入允许列表。当前性能检查也只观察到原版附近的吞吐，不覆盖 UEFI 寄存器验收。

## 诊断改动

`src/app.c` 的 `ColdAndProm()` 保留所有原始期望值。芯片身份不符或 MMIO 读取错误仍立即退出；其他值不符时继续记录固定白名单中的寄存器，随后复读 SS0/SS1。若存在非 SS0 不匹配，则跳过 PROM 并拒绝；若仅 SS0 不匹配，则完成原有 PROM 双读和两项哈希检查，最后仍返回 EFI_NOT_READY，绝不因此进入事务。变化发生在两次选择器读取之间也会拒绝。

额外修正控制台日志路径：内部硬盘提示 `EFI/GPUUnlock/RUNxx/REPORT.TXT`。初始状态拒绝会明确指出固件事务尚未开始。报告新增 `cold diagnostics revision=1`，由 build ID 区分该源码修订；原版本标题保持 stage3-0.2.0-disk-candidate，不代表二进制与原 RUN03 相同。

验证：应用测试 54 场景 / 8764 断言，覆盖 `…36`、`…37`、复读变化、同时 SS1 不匹配、PROM 错误和读取故障；这些场景均检查没有调用核心固件事务。核心 43 场景、port 14369 断言、admission 27 场景 / 2141 断言、boot-media 76 检查通过。测试使用模拟硬件；不将主机测试写成新增状态的硬件兼容证明。

诊断构建在隔离的 LOCAL 目录完成，避免覆盖历史 bundle 验证依赖的原 build 二进制；本目录保留新构建 manifest 和验证/测试日志。固件与私钥不随本目录分发。

## 本机安装状态（08:27）

- build ID：`07b47e3faa0774de3321b4f6f947b085285b9bc11917f24523e041262c426e17`
- unsigned EFI SHA256：`c42c6ffaaac717afde4fd13b65c6c48d8e146b25789b9658f4dde6a89fee5db6`
- signed EFI SHA256：`4f8dc5e53fecc34861a9c7259c5af48b634cc135849b52eceb9f34d070cb7ed2`

使用原已登记证书签署并验证 CMS/映像摘要/内容一致性。安装前后都核对 SecureBoot=1、SetupMode=0、原信任库加预定证书；核对 ESP 身份和全部资产，备份旧签名 EFI 与 manifest，再更新 GPUUnlock EFI 和构建 manifest，回读哈希、校验签名，确认原 Windows EFI 不变。未写密钥、未改启动顺序、未自动重启，临时挂载已撤销。

**当前仅完成诊断版安装，尚待下一次实机运行；未修复或放行新 SS0 初值。** 下一次正常重启后按 U，若仍为上述初值，预期仍拒绝事务并返回 Windows，但报告应包含剩余寄存器、选择器复读和 PROM 校验结果。之后运行 `Collect-InternalEfiReport.ps1` 获取新日志，不再重复清空密钥或以反复断电代替诊断。
