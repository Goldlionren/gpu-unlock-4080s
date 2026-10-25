# Secure Boot 开启后的本机 RUN03 验收

2026-10-26（Australia/Sydney）：本机 Windows 11 Pro / Intel NUC12 / RTX 4080 SUPER 16GB 已完成 **Secure Boot 开启 → 硬盘上的签名 GPUUnlock EFI → 按 U 执行 GPU 事务 → 返回 Windows** 的实测。

这是一次本机自定义信任路径验证。EFI 由本机 RSA-2048 / SHA-256 证书签名，固件 db 登记该证书；不是微软签发的通用 EFI 签名，不代表其他板卡或固件已经验证。

## 保存的证据

- [REPORT.TXT](REPORT.TXT)：RUN03 原始字节，SHA256 `bd2474d7144579ca71a07f74d776b2a2f53c9eb29700724a8948d44ba9361d78`。
- [trust-check.json](trust-check.json)：Windows 只读密钥检查原始结果，`SECURE_BOOT_ENABLED_KEYS_VERIFIED`，四库匹配、SetupMode=0、SecureBoot=1。
- [verification.json](verification.json)：本次综合核验，包括报告/EFI 哈希、签名指纹、公开变量内容哈希、执行结果和证据范围。

原始预检查、卷 GUID、BCD 导出、完整固件信任库备份、签名私钥均没有复制到此目录。私钥仍在本机证书存储中；本目录不含 EFI 或 NVIDIA 固件二进制。

## 实际结果

| 检查 | 结果 |
|---|---|
| EFI 启动环境 | `secure_boot=1 internal_volume=1 firmware_authentication_not_overridden=1` |
| Windows Boot Manager 预加载 | 一个内部候选，`status=0` |
| GPU 事务 | `result=0 firmware_started=1 required_plm=1` |
| 全部 PLM | `all_plm=0`：FBPA 为已知非致命不匹配，不能宣称四项全匹配 |
| SS0 / SS1 | `0x88888888 / 0x8`，FLR 前后均匹配 |
| 恢复 | `recovered=1 quiesced=1 retained=0 maps=0` |
| PCI 配置 | 仅允许已验证 FLR 后的 CED/URD 清除；`configuration_accepted=1` |
| EFI 最终状态 | `FINAL status=0` |
| Windows | 用户确认本次按 U 后成功进入；采集器确认 Secure Boot=True |
| 固件信任 | 原 PK、KEK、dbx 逐字节不变；db 精确等于原库加预定证书 |

Windows 启动时间为 `2026-10-25T13:57:05.5000000Z`，即悉尼 2026-10-26 00:57:05.5；密钥采集 UTC 为 `2026-10-25T13:58:51.4480941Z`，报告在悉尼 00:59 采集。报告与这次启动的关联依据用户实际操作确认、RUN03 内容和采集上下文。采集器本身不能独立绑定一次启动，因此保留 `same_boot_independently_verified=false`；这不是 TPM 或远程证明。

签名 EFI SHA256 为 `32d3e756af38f66a1a5b1f68790d4058aed93164dac39f0af4562e71bf8af128`，unsigned EFI 为 `cd506a77b942009e55daad312aa6bfdcbcfabfedfcd851e09466317aecd32513`，build ID 为 `a779b1312b1c25f2daba214af72e14c9540470a67151b3624a87b080288a969b`。已在 Windows PowerShell 5.1 重新验证 CMS 签名、Authenticode 映像摘要及签名前后执行内容一致性，并核对候选 manifest 中的源码哈希。

## 尚未测量与历史字段

本次没有运行性能基准，不能把 0.1.2、Secure Boot 关闭条件下的 FP32/Tensor 测量当成本次数据。也没有进行本次长期稳定性、ComfyUI、数值正确性或固件拒绝未受信任/篡改 EFI 的负向启动测试。

`trust-check.json` 中的 `secure_boot_efi_execution_verified=false` 表示该只读工具不检查 GPU 报告。报告头部“尚未实测”、原构建 manifest 的 `hardware_tested=false` 和签名时的未登记字段均是创建时状态，不回改原始证据。后续成功结论由本目录的新综合验收记录表达。

完整操作和密钥登记逻辑见 [签名说明](../../SECUREBOOT-SIGNING.zh-CN.md)。本机当前已完成登记，不要重复 Reset to Setup Mode 或清空密钥。
