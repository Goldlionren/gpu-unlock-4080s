# RUN09：零初态解锁、Secure Boot 与性能验收

2026-10-26 10:50（Australia/Sydney）：修复版从 **初始 SS0=0、SS1=0** 成功执行事务，使用硬盘上的签名 EFI、Secure Boot 开启，用户进入 Windows 后完成两次现场性能测量。这次验证直接覆盖了此前 RUN08 被入口拒绝的零初态。

## 证据与结果

- [REPORT.TXT](REPORT.TXT)：原始 RUN09，SHA256 `4e54fad3a03912d53651c24b66d0fadae600261ae0902389a73a7c050cb3fc88`。
- [verification.json](verification.json)：报告标记、文件哈希、构建与安装关联及验证范围。
- [benchmark-1.json](benchmark-1.json)、[benchmark-2.json](benchmark-2.json)：两次现场测量原始数据。
- [comparison.json](comparison.json)：与两次历史原版数据的逐项计算。
- [performance-status.json](performance-status.json)：性能状态检查原始输出。

初始 SS0/SS1 三次读取均为 0/0，其余初始寄存器及 PROM 双读/精确哈希通过。随后 SS0 从 0 写为 0x88888888、SS1 从 0 写为 8，两者在写后及 FLR 前后均匹配。`required_plm=1`、`recovered=1`、`retained=0`、`maps=0`、`FINAL status=0`。`all_plm=0` 仍表示已知非致命 FBPA 不匹配，不宣称全部 PLM 都匹配。

EFI 报告 `secure_boot=1 internal_volume=1 firmware_authentication_not_overridden=1`；Windows 采集器确认 Secure Boot=True。签名 EFI SHA256 为 `13ce504d6639614a7fe73ea94228079993274dcf6a282cf27f73ff28f5b260a2`，build ID 为 `0cbbea2b718f0616b78ca023c67d692a4cb567234a3970435bbe4951a5864f4d`。已交叉核对源文件与 [该构建 manifest](../selector-state-run08/manifest.json)、签名/未签名映像哈希和安装记录。

## 本次实际性能

驱动 581.57，原 ComfyUI Python 3.13.12 / PyTorch 2.12.1+cu130 / CUDA 13.0，原基准只运行 8192 尺寸，两次均为 5 trials × 15 iterations。

| 类型 | 历史原版均值 TFLOPS | 本次第 1 次 | 本次第 2 次 | 本次均值 | 相对历史原版 |
|---|---:|---:|---:|---:|---:|
| FP32_STRICT | 35.7495 | 35.938 | 35.687 | 35.8125 | +0.18% |
| FP16 | 108.2125 | 177.947 | 177.283 | 177.6150 | +64.14% |
| BF16 | 108.2415 | 184.481 | 183.475 | 183.9780 | +69.97% |
| FP8 E4M3 | 199.4165 | 313.154 | 307.721 | 310.4375 | +55.67% |

FP32 基本持平，不将 +0.18% 解释为可归因于解锁的提升。FP16/BF16 对应状态输出 UNLOCKED_PERFORMANCE_LIKELY，与本次 UEFI 目标值回读证据相互支持。该状态脚本不直接读取 Windows 下的 SS0/SS1，因此其 `hardware_unlock_state_verified=false` 不代表 UEFI 事务失败。

历史原版测试运行完整尺寸序列，本次只测 8192；相同环境、精度设置及 API 标签不证明机器内核完全相同，背景负载、温度和频率未严格控制。这是两次实测结果，不是数值正确性、ComfyUI 工作流加速或长期稳定性证明。原始基准的 STOCK 标题/文件名是历史命名，不表示当前处于原版状态。

## 初值为什么不再固定，为什么仍读取

初始 SS0/SS1 不参与显存布局或 DMA 地址计算，也不是识别这张固定板卡的依据。当前修复已取消 `…35/36/37` 和 SS1=2 的初值白名单；日志中的 historical_reference 只是旧采样值，不控制准入。身份仍由原有 PCI/拓扑/PROM 等检查确认。

现在保留的初始读取有效性和三次一致性检查是保守的状态检查，用于发现 MMIO 错误值或采集期间的变化，不是解锁计算所必需的固定初值。真正判断解锁是否生效的仍是最终 SS0=0x88888888、SS1=8 的读回及恢复检查。本次没有进一步修改已通过实测的 EFI。

## 范围与历史记录

本次 Windows 启动时间由采集器记录为 `2026-10-25T23:49:01.5000000Z`，测量开始于悉尼 10:50:05。报告与当前启动的关联基于用户操作、报告及采集上下文；没有独立的启动绑定或 TPM 证明，保留 `same_boot_independently_verified=false`。

这是新修复构建的一次零初态成功，不等于每次重启、所有可能初态或其他板卡均已验证。初值变化的硬件原因仍未确定。原 manifest 和报告头部的“构建时尚未实测”字段保持原样，后续成功由本目录新增记录表达。之前失败和中间补丁的证据保留，见 [RUN08 与修复说明](../selector-state-run08/README.md)。
