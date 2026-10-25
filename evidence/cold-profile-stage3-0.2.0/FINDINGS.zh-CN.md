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

08:27 时仅完成诊断版安装，尚待下一次实机运行；未修复或放行新 SS0 初值。预定下一次重启采集完整诊断，而不是重复清空密钥或以反复断电代替诊断。

## RUN07 实机结果（08:38–08:39）

诊断版成功执行：初始 SS0 回到 `0x64113035`，两次读取相同，SS1 两次均为 2；其余固定寄存器匹配，PROM 双读和精确哈希通过。Secure Boot=1，事务 result=0、required_plm=1，FLR 前后选择器匹配，recovered=1、retained=0、maps=0、FINAL status=0。FBPA 仍是已知非致命不匹配（all_plm=0）。用户报告本次进入 Windows，采集器确认 Secure Boot=True。

在 Windows 中运行 `Check-UnlockStatus.ps1`，原 ComfyUI Python 环境下复用原基准、仅测 8192 尺寸两次：

| 类型 | 本次两次均值 TFLOPS | 历史原版均值 | 相对历史原版 |
|---|---:|---:|---:|
| FP16 | 177.66 | 108.21 | +64.17% |
| BF16 | 184.03 | 108.24 | +70.02% |

原始报告、两次 benchmark JSON、性能检查结果和综合核验分别保存在本目录的 `RUN07-*` 文件中。原基准 STOCK 标题/文件名是历史命名，不表示本次未解锁。当前性能结果为 `UNLOCKED_PERFORMANCE_LIKELY`；这是实测性能证据，没有直接读取 Windows 下的 SS0/SS1。历史参考跑过完整尺寸序列，本次只跑 8192，未严格控制温度、频率或背景负载，也未验证数值正确性及长期稳定性。

**这次成功时 SS0 为 `…35`，未出现预测的 `…38`。** 记录不足以支持简单的每次启动加一计数器解释，也不足以判定 `…36/37` 是可接受的初始状态；诊断改动没有改变允许值，因此不能宣称初始值变化问题已经修复。后续再次出现该初值时，新报告才会给出完整的剩余状态与 PROM 证据。
