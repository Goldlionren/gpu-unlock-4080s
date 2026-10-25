# 硬盘启动与 Secure Boot：0.2.0 候选方案

日期：2026-10-25。目标为这台 Windows 11 Pro / NUC12 / RTX 4080 SUPER 16GB。

当前完成了源码适配、编译、主机模拟测试和硬盘目录打包；**尚未安装到 ESP、创建固件启动项、登记证书，也没有 Secure Boot 开启状态的实机成功报告。** 已测 0.1.2 的性能与报告不能证明本候选版成功。

## 硬盘启动的布局

候选输出为本仓库 `Disk\EFI\GPUUnlock`：

```text
EFI/
  Microsoft/Boot/bootmgfw.efi   原 Windows 引导程序，保留
  GPUUnlock/
    GPUUnlock.efi
    ASSETS/                   五个固定大小与 SHA256 的资产
    manifest.json
    assets-manifest.json
    LICENSES/
    RUN00/REPORT.TXT           首次实际启动时创建，后续 RUN01…RUN99
```

将完整 GPUUnlock 目录放入固件能读取的 FAT32 EFI 系统分区（ESP），再创建指向 `\EFI\GPUUnlock\GPUUnlock.efi` 的独立固件启动项。不是把它放到普通 NTFS 的 F: 目录就能启动。

首轮采用一次性启动候选项，保留现有 Windows Boot Manager 默认项。成功并核验后，才考虑将候选项设为默认。程序仍要求每次按 **U** 才执行 GPU 事务；“不用 U 盘”不等于“取消人工确认”。正常完成后加载原 Windows 引导程序。

资产总计约 85 MB（十进制），还有 EFI、许可证和运行日志。安装前按实际目录大小加至少 32 MiB 余量检查可用空间；该余量不是 100 次运行的容量保证。分区容量不足时先决定另一个固件可读 ESP 或单独规划分区调整，不能直接清理微软引导文件腾空间。此次尚未提供自动分区/安装脚本，因为当前进程没有读到真实 ESP 与剩余空间。

## 源码变化

1. `src/boot_media.h` 对有界设备路径分类：保留 USB/SD，新增 SATA/NVMe + GPT 分区路径；拒绝网络路径、多实例和不完整结构。GPT 路径不是 ESP 类型的证明，安装前另行检查分区类型 GUID。
2. `src/app_io.c`：硬盘模式从同卷 `\EFI\GPUUnlock` 读取资产并写日志，USB/SD 仍用 `\RTX4080S-UNLOCK`。100 个 RUN 目录全部占用时拒绝继续，需在 Windows 下归档。
3. `src/app.c`：硬盘模式允许原 Windows 引导程序位于同一 ESP；仍要求唯一候选、检查加载后的卷和代码类型，使用固件 `LoadImage` 正常验签加载。
4. 入口允许 `SecureBoot=1` 且 `SetupMode=0`；变量错误、非法值、Setup Mode 不符均在 GPU 写入前停止。不登记密钥、不修改验签协议、不绕过固件的签名检查。
5. GPU 固件载荷、板卡身份/PROM 校验、事务、DMA 隔离、FLR、选择器验证、最终配置恢复检查未更改。

支持范围仅为上述路径形态；VMD/RAID/特殊固件的设备路径仍可能被拒绝，不能在没有报告的情况下宣称所有内置硬盘均兼容。

## Secure Boot 签名路线

可研究的路线是 **给 EFI 做 Authenticode 签名，并让固件 db 信任签名证书**。Windows 的证书库和 UEFI db 是不同的存储；仅在 Windows 导入证书不能使固件信任它。PK 管理平台所有权，KEK 授权更新 db/dbx，db 是允许库，dbx 是撤销库。见 [Microsoft 密钥管理说明](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/windows-secure-boot-key-creation-and-management-guidance?view=windows-10)。

[ASUS 官方 NUC 说明](https://www.asus.com/support/faq/1052602/) 表示 Aptio V NUC 支持自定义密钥，包括第八代及更新型号。这为本机提供了研究依据，但不是对 EDADL579.0065 固件菜单和实际执行结果的验证。[另一份官方 NUC FAQ](https://zentalk.asus.com/t5/faq/nuc-how-to-enroll-the-custom-secure-boot-key/ta-p/466140) 描述 Custom Mode / Setup Mode 后在 Windows 登记密钥的路径，并指出 NUC BIOS 不提供密钥登记菜单。**不能据此直接清空本机 PK/KEK/db。**

需要先核对现有 PK、KEK、db、dbx 与默认库，确定能否追加信任以及确切恢复方法。如果必须进入 Setup Mode，再单独制定完整的信任库恢复/登记顺序，保留微软与 OEM 所需证书及撤销数据。现有公开变量的二进制备份不一定能直接作为 authenticated update 写回；不能把“已导出”当成“已验证可恢复”。微软 2023 系列证书迁移也需要纳入检查，不能按旧教程把所有信任替换成 2011 版本。

签名只解决主机固件对 EFI 的身份验证；不会直接让 NVIDIA 固件解锁过程变得可信或兼容。开启 Secure Boot 后的 DMA/IOMMU 行为、GPU 事务与 Windows 接续必须重新实测。固件显示 Secure Boot 已启用，也不能据此承诺所有反作弊、设备证明或组织安全策略都接受新增自定义信任。

## 当前先执行：只读采集

以管理员身份打开 Windows PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File F:\AI\gpu-unlock-4080s\Get-BootPreflight.ps1
```

输出 `LOCAL-boot-preflight\<时间>\preflight.json`、BCD 文本及公开 Secure Boot 变量。采集磁盘/分区/可见卷剩余空间、BIOS、Secure Boot 状态和 BitLocker 保护状态；不采集恢复密码，不挂载 ESP，不写 NVRAM，不登记证书，不重启。管理员权限是 Windows 读取固件变量和分区信息所需。

该目录已被 Git 忽略。读不到某个默认变量不等于硬件失败；需要查看具体 error。若隐藏 ESP 的剩余空间未返回，下一步再按明确分区进行临时只读检查。BitLocker 若启用，在改变引导路径前必须确认用户持有恢复密钥；脚本不会替用户关闭保护或公开恢复密钥。

## 签名工具已准备，尚未生成或登记证书

`Sign-EfiCandidate.ps1` 读取已构建 manifest，核对候选 EFI 哈希，从 `Cert:\CurrentUser\My` 使用已有、有效且有私钥的 RSA 证书，调用 Windows SDK SignTool：

```powershell
# 证书准备及 db 登记方案确认后再使用；替换为实际证书 thumbprint。
.\Sign-EfiCandidate.ps1 -CertificateThumbprint '0123456789ABCDEF0123456789ABCDEF01234567'
```

脚本仅在新的 `LOCAL-signed\<时间>` 目录生成签名副本、公钥 CER 和签名 manifest，不导出私钥、不覆盖 unsigned 构建、不安装。命令使用 `/fd SHA256 /nph`；`/sha1` 参数用于按指纹选择证书，不代表使用 SHA-1 签名文件。参数依据 [Microsoft SignTool 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/signtool)。

匹配到 SignerCertificate 只是签名者检查，Windows Authenticode 状态也不等于固件 db 验证结果。脚本记录状态，不宣称固件已信任。签名后 EFI 哈希会变化，应使用 signing-manifest 的 signed hash，不能继续用 unsigned manifest 的哈希冒充最终镜像。

私钥应留在本机证书存储或专门离线保管，不能提交 Git；`*.pfx / *.p12 / *.key` 和 `LOCAL-*` 已忽略。当前没有创建或导出私钥。

## 验证与下一轮顺序

本次构建已运行 boot-media、port、app、admission 测试，并使用本机精确匹配的 PROM fixture 跑完 43 个 core 事务模拟场景。测试不执行真实 GPU；主机 Secure Boot 场景模拟的是入口与分支逻辑，不模拟固件密码学验证。构建 manifest 标记 `hardware_tested=false`。

1. 取得上述只读报告，选定实际 ESP，确认空间、保留原启动项与恢复路径。
2. Secure Boot 保持关闭，安装独立目录/启动项，先一次性从硬盘启动候选版，采集新的 REPORT 与同启动周期基准。
3. 确定证书登记方式并准备可审核的变更/恢复方案，再签名、登记；不得因启动失败而盲目清空所有固件密钥。
4. 重新开启 Secure Boot，核验固件变量、候选实际执行及 Windows 状态，再核验 GPU 报告和性能。未受信任或被篡改 EFI 应被固件拒绝；该验证须保留原 Windows 可启动项。

源码在本地 `feature/internal-efi-secureboot` 分支，旧实测 `artifacts/stage3-0.1.2/BOOTX64.EFI` 保留。该旧版本仍要求 USB/SD 与 Secure Boot 关闭，不能拿旧二进制验证新功能。
