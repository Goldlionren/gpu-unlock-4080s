# 本机签名与 Secure Boot 登记

最新验收（2026-10-26 10:50）：[RUN09](evidence/selector-state-run09/README.md) 已确认修复版在 Secure Boot 开启时从初始选择器 0/0 成功执行事务、恢复并返回 Windows，随后两次现场测量确认 Tensor 提升。当前已安装签名 EFI SHA256 为 `13ce504d6639614a7fe73ea94228079993274dcf6a282cf27f73ff28f5b260a2`，沿用原证书与固件信任，无需重复登记。下文 00:59 的哈希和 RUN03 属于首次签名版本的历史记录。

2026-10-26 00:59 状态：**本机 Secure Boot 开启后的硬盘 EFI 启动、GPU 事务与返回 Windows 已实测成功。** 已在 Admin 账户生成 RSA-2048 / SHA-256 本机签名证书，私钥以不可导出方式保存在 CurrentUser\My。已签署实测 0.2.0 EFI，安装到原 ESP 的 `\EFI\GPUUnlock\GPUUnlock.efi`，并完成固件信任登记。证据见 [RUN03 报告与验收说明](evidence/secureboot-stage3-0.2.0-run03/README.md)。不需要再次登记密钥。

00:43 历史状态：用户完成 BIOS Custom / Reset to Setup Mode 并执行登记后，已独立重新读取固件。原 PK、KEK、dbx 与最新备份逐字节一致，db 与“原库加一张本机签名证书”逐字节一致。当时 SetupMode 仍为 1、SecureBoot 为 0，状态为 KEYS_VERIFIED_MODE_PENDING，因此先保持关闭并重启后只读复核。

00:49 历史状态：重启后的只读检查确认四库全部精确匹配、SetupMode=0、SecureBoot=0，状态为 `KEYS_VERIFIED_USER_MODE_SECURE_BOOT_OFF`，模式待确认的问题解决。随后用户在 BIOS 保留 Custom Mode 和现有密钥，开启 Secure Boot，启动 GPUUnlock 并按 U。

00:58–00:59 验收：Windows `Confirm-SecureBootUEFI=True`；只读检查为 `SECURE_BOOT_ENABLED_KEYS_VERIFIED`、SetupMode=0、SecureBoot=1、四库全部匹配；RUN03 记录 `secure_boot=1 internal_volume=1 firmware_authentication_not_overridden=1`。GPU 事务 result=0、required_plm=1、FLR 前后 SS0/SS1 匹配、recovered=1、retained=0、maps=0、最终 status=0。all_plm=0 仍是已知非致命 FBPA 不匹配。用户确认本次进入 Windows，采集器记录 Windows 启动 UTC 为 2026-10-25T13:57:05.5000000Z（悉尼 00:57:05.5）。该关联由报告、采集上下文和用户操作确认共同支持，不是独立的启动证明或远程证明。

| 项目 | 实际值 |
|---|---|
| 证书指纹 | `49561A2E65953174248986EE16E639C51000798F` |
| 原 EFI SHA256 | `cd506a77b942009e55daad312aa6bfdcbcfabfedfcd851e09466317aecd32513` |
| 签名 EFI SHA256 | `32d3e756af38f66a1a5b1f68790d4058aed93164dac39f0af4562e71bf8af128` |
| Authenticode 映像 SHA256 | `8ae951c67c657c5c0f7527a660204a99496978e541cc126f05786c2b55c2d723` |
| 最新固件检查 | `SECURE_BOOT_ENABLED_KEYS_VERIFIED`，四库匹配、SetupMode=0、SecureBoot=1 |
| Secure Boot 实测报告 SHA256 | `bd2474d7144579ca71a07f74d776b2a2f53c9eb29700724a8948d44ba9361d78` |

私钥不在仓库中，也没有导出 PFX。签名副本、公开证书、最新信任库备份和登记计划位于：

```text
F:\AI\gpu-unlock-4080s\LOCAL-secureboot\20261026-002446-de1ebf01\
  bundle.json
  before\                 原 PK、KEK、db、dbx 和其他公开采集
  signed\GPUUnlock.efi
  signed\GPUUnlock-public.cer
  signed\signing-manifest.json
  db-append.esl           仅新增本机签名证书
  db-expected-merged.esl  原 db 完整内容加该证书
```

原 unsigned EFI 备份及新安装记录在 `LOCAL-disk-install\20261026-002946-signed-97e62b27\`。签名安装检查了挂载身份、目标文件哈希、签名和原 Windows EFI 哈希，临时挂载已撤销。没有修改原 Windows 引导文件、固件启动顺序或密钥。用户此前设置的 GPUUnlock 默认启动顺序保留。

## 签名究竟验证了什么

`Verify-EfiSignature.ps1` / `tools/EfiSignature.cs` 针对本项目 PE32+ 布局检查：

- 签名前后除 PE checksum、证书表目录和追加签名外，原文件字节完全一致。
- 使用 .NET SignedCms 验证 PKCS#7 签名，核对证书指纹。
- 解析 Authenticode 的 SHA256 摘要，并与重新计算的映像摘要比较。
- 拒绝代码篡改、签名篡改、截断、错误证书及将别的有效签名移植到本 EFI 的情况。

已对本机真实微软签名 EFI 及篡改变体运行 9 项检查；也已对本机实际生成的签名 EFI 完成校验。验证器只接受所需布局，不是通用 PE 分析器。

Windows `Get-AuthenticodeSignature` 返回 UnknownError，具体消息是证书链终止于未被 Windows 信任的根。这不等于上述密码学检查失败。脚本没有把私有签名证书导入 Windows Root；Windows 根库和固件 db 是两回事。**签名校验通过仍不等于固件信任或 Secure Boot 开启后执行成功。**

## 首次登记的 BIOS 操作（本机已完成，不要重复）

首次登记前 PK 是 Intel SBoot1、SetupMode=0。按 [UEFI 规范](https://uefi.org/specs/UEFI/2.10/32_Secure_Boot_and_Driver_Signing.html)，User Mode 的 db 更新需要已有 PK/KEK 授权，新生成的代码签名私钥没有该授权。[ASUS NUC 官方流程](https://zentalk.asus.com/t5/faq/nuc-how-to-enroll-the-custom-secure-boot-key/ta-p/466140) 指向 Custom Mode、Reset to Setup Mode，然后回 Windows 登记。不能靠 Windows 管理员权限代替固件授权。

1. 保存工作，进入 BIOS，继续保持 Secure Boot **Disabled**。
2. Secure Boot Mode 选 **Custom Mode**。
3. 选择 **Reset to Setup Mode**。这一步会暂时清除平台 PK，使固件允许登记；本流程随后恢复原 Intel PK，不创建自己的 PK。
4. 保存退出，通过原 **Windows Boot Manager** 回到 Windows，运行下面的登记脚本。

不要选择 Restore Factory Keys：本机原 db 含默认库没有的两张 2023 CA，dbx 比默认库多 124 条记录。本机这次 Reset 后登记脚本实际恢复了 dbx、KEK、完整原 db 加新证书，再恢复原 PK，均已逐字节核验。其他机器若菜单不同，仍须先核对具体行为，不能假设不同操作等价。

## 首次 Windows 登记命令（本机已完成）

管理员 PowerShell：

```powershell
cd F:\AI\gpu-unlock-4080s
.\Enroll-SecureBootCertificate.ps1 -Bundle "F:\AI\gpu-unlock-4080s\LOCAL-secureboot\20261026-002446-de1ebf01\bundle.json" -Apply
```

该脚本先验证 bundle 哈希、签名和 ESP 中实际文件，再检查 SecureBoot=0、SetupMode=1、PK 已为空。它不会主动清除 PK，也不会自动开启 Secure Boot。

具体变更顺序：

1. dbx、KEK 若保持原样则不写；若已为空则恢复最新备份原值。出现非空且与备份不同的内容时停止，避免覆盖新更新。
2. db 保留原值并追加一张证书；若已为空，写入完整原 db 加新证书。已符合预期时跳过。
3. 对上述三项逐字节核验后，最后恢复原 Intel PK；再次核对四库及 SetupMode，尚未读到 User Mode 时返回模式待确认。

每次写入前再次读取，防止忽略并发变化。每次写入后立即核对；失败时停止，不以删除所有密钥作为自动回退。逻辑经过 10 个模拟场景、34 项检查，包括保留旧库、空库恢复、部分完成后继续、错误模式、内容变化、写入失败和回读不符。**这些不是本机固件写入的成功证明。**

后续修正增加了模式未即时切换的情况，共 14 个模拟场景、45 项检查。原脚本在成功恢复 PK 后要求立即读到 SetupMode=0，因本机仍返回 1 而报错。新逻辑只有在四项内容全部精确匹配时才返回“模式待确认”，不会把它标记为已启用；重复执行时不再次写入这些已验证的密钥。内容不符仍停止。该修正没有改变固件变量或跳过内容检查。

使用的 [Set-SecureBootUEFI](https://learn.microsoft.com/en-us/powershell/module/secureboot/set-securebootuefi?view=windowsserver2025-ps) 接收原始格式化内容并生成认证变量写入封装；只有 Setup Mode 下才执行此处未使用旧 PK/KEK 私钥签名的更新。原备份作为本流程 Setup Mode 恢复内容使用，不能当作在 User Mode 任意写回的授权。

成功会输出 `ENROLLED: ... SetupMode=0`，并在 bundle 下生成新的 enrollment 目录、写入意图和 after 采集。把输出路径交回核验，**此时仍先保持 Secure Boot 关闭**。确认原 PK/KEK/dbx 保留、db 仅增加预定证书后，再进 BIOS 开启 Secure Boot 并重新测试 GPUUnlock。

若输出 `KEYS_VERIFIED_MODE_PENDING`，不再 Reset 或重复登记，保持 Secure Boot Disabled，重启一次后在管理员 PowerShell 执行：

```powershell
F:\AI\gpu-unlock-4080s\Check-SecureBootEnrollment.ps1
```

该工具只读采集，不挂载 ESP、不写密钥、不改启动项；核对信任库与模式，保存 `LOCAL-secureboot-checks\<时间>\verification.json`。只有四库匹配且 SetupMode=0 才属于模式就绪；SecureBoot=1 的变量状态仍不独立证明 GPUUnlock 已在验证开启的启动链上成功执行。

若未成功，先保持 Disabled，通过原 Windows Boot Manager 返回系统，保留 ERROR 和写入记录。不要恢复出厂库或尝试移除剩余密钥。

## 最后一次验证

开启后的验收需要同时满足：Windows `Confirm-SecureBootUEFI` 为 True、SetupMode=0、固件信任库符合预期；新 GPUUnlock 报告记录 `secure_boot=1 internal_volume=1`，GPU 事务/FLR/选择器/恢复均成功，并实际返回 Windows。**本机 RUN03 已满足上述验收；尚未重新测量 Secure Boot 开启时的性能，也未进行固件拒绝篡改/未信任 EFI 的负向启动测试。** 历史 FP32/Tensor 数据仍属于 0.1.2、Secure Boot 关闭的测量。

在管理员 PowerShell 执行以下命令，保留两份脚本输出的路径：

```powershell
Confirm-SecureBootUEFI
& F:\AI\gpu-unlock-4080s\Check-SecureBootEnrollment.ps1
& F:\AI\gpu-unlock-4080s\Collect-InternalEfiReport.ps1
```

采集器会复制历史报告；最高 RUN 编号本身不是本次启动成功的证明，需要核对新报告内容。如果出现 Security Violation 或 GPUUnlock 未出现，先记录提示原文，关闭 Secure Boot 后通过 Windows Boot Manager 回到系统；保留现有密钥。

`Check-SecureBootEnrollment.ps1` 的 `secure_boot_efi_execution_verified=false` 表示该工具只验证密钥和模式，不读取 EFI 执行报告，并非本次事务失败。原始检查结果、原构建 manifest、签名时 manifest 和报告头部的构建时未实测文字均保持原样；后续成功结论单独写在 RUN03 验收记录中。
