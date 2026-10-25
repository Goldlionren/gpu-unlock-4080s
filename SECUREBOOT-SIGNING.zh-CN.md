# 本机签名与 Secure Boot 登记

2026-10-26 状态：已在 Admin 账户生成 RSA-2048 / SHA-256 本机签名证书，私钥以不可导出方式保存在 CurrentUser\My。已签署实测 0.2.0 EFI，并安装到原 ESP 的 `\EFI\GPUUnlock\GPUUnlock.efi`。尚未登记固件信任，Secure Boot 仍关闭；没有开启后的成功报告。

| 项目 | 实际值 |
|---|---|
| 证书指纹 | `49561A2E65953174248986EE16E639C51000798F` |
| 原 EFI SHA256 | `cd506a77b942009e55daad312aa6bfdcbcfabfedfcd851e09466317aecd32513` |
| 签名 EFI SHA256 | `32d3e756af38f66a1a5b1f68790d4058aed93164dac39f0af4562e71bf8af128` |
| Authenticode 映像 SHA256 | `8ae951c67c657c5c0f7527a660204a99496978e541cc126f05786c2b55c2d723` |
| 固件预检查 | `WAITING_FOR_FIRMWARE_SETUP_MODE`，没有写入密钥 |

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

## 现在需要的 BIOS 操作

当前 PK 仍是 Intel SBoot1、SetupMode=0。按 [UEFI 规范](https://uefi.org/specs/UEFI/2.10/32_Secure_Boot_and_Driver_Signing.html)，User Mode 的 db 更新需要已有 PK/KEK 授权，新生成的代码签名私钥没有该授权。[ASUS NUC 官方流程](https://zentalk.asus.com/t5/faq/nuc-how-to-enroll-the-custom-secure-boot-key/ta-p/466140) 指向 Custom Mode、Reset to Setup Mode，然后回 Windows 登记。不能靠 Windows 管理员权限代替固件授权。

1. 保存工作，进入 BIOS，继续保持 Secure Boot **Disabled**。
2. Secure Boot Mode 选 **Custom Mode**。
3. 选择 **Reset to Setup Mode**。这一步会暂时清除平台 PK，使固件允许登记；本流程随后恢复原 Intel PK，不创建自己的 PK。
4. 保存退出，通过原 **Windows Boot Manager** 回到 Windows，运行下面的登记脚本。

不要选择 Restore Factory Keys：本机当前 db 含默认库没有的两张 2023 CA，dbx 比默认库多 124 条记录。若没有上述菜单、或提示删除全部密钥，先保留设置并报告菜单原文，不能假设不同操作等价。此说明基于官方 NUC 流程，本机具体菜单行为尚待用户实际确认。

## Windows 登记命令

管理员 PowerShell：

```powershell
cd F:\AI\gpu-unlock-4080s
.\Enroll-SecureBootCertificate.ps1 -Bundle "F:\AI\gpu-unlock-4080s\LOCAL-secureboot\20261026-002446-de1ebf01\bundle.json" -Apply
```

该脚本先验证 bundle 哈希、签名和 ESP 中实际文件，再检查 SecureBoot=0、SetupMode=1、PK 已为空。它不会主动清除 PK，也不会自动开启 Secure Boot。

具体变更顺序：

1. dbx、KEK 若保持原样则不写；若已为空则恢复最新备份原值。出现非空且与备份不同的内容时停止，避免覆盖新更新。
2. db 保留原值并追加一张证书；若已为空，写入完整原 db 加新证书。已符合预期时跳过。
3. 对上述三项逐字节核验后，最后恢复原 Intel PK；再确认 SetupMode=0。

每次写入前再次读取，防止忽略并发变化。每次写入后立即核对；失败时停止，不以删除所有密钥作为自动回退。逻辑经过 10 个模拟场景、34 项检查，包括保留旧库、空库恢复、部分完成后继续、错误模式、内容变化、写入失败和回读不符。**这些不是本机固件写入的成功证明。**

使用的 [Set-SecureBootUEFI](https://learn.microsoft.com/en-us/powershell/module/secureboot/set-securebootuefi?view=windowsserver2025-ps) 接收原始格式化内容并生成认证变量写入封装；只有 Setup Mode 下才执行此处未使用旧 PK/KEK 私钥签名的更新。原备份作为本流程 Setup Mode 恢复内容使用，不能当作在 User Mode 任意写回的授权。

成功会输出 `ENROLLED: ... SetupMode=0`，并在 bundle 下生成新的 enrollment 目录、写入意图和 after 采集。把输出路径交回核验，**此时仍先保持 Secure Boot 关闭**。确认原 PK/KEK/dbx 保留、db 仅增加预定证书后，再进 BIOS 开启 Secure Boot 并重新测试 GPUUnlock。

若未成功，先保持 Disabled，通过原 Windows Boot Manager 返回系统，保留 ERROR 和写入记录。不要恢复出厂库或尝试移除剩余密钥。

## 最后一次验证

开启后的验收需要同时满足：Windows `Confirm-SecureBootUEFI` 为 True、SetupMode=0、固件信任库符合预期；新 GPUUnlock 报告记录 `secure_boot=1 internal_volume=1`，GPU 事务/FLR/选择器/恢复均成功，并实际返回 Windows。之后再重新测量性能。当前尚未完成这一阶段。
