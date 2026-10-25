# 硬盘启动与 Secure Boot：0.2.0 本机实测

日期：2026-10-25。目标为这台 Windows 11 Pro / NUC12 / RTX 4080 SUPER 16GB。

更新至 2026-10-26 00:59：已安装到本机 ESP、创建独立启动项，完成本机证书签名与固件信任登记。**RUN03 已验证 Secure Boot 开启时从硬盘执行 GPU 事务，用户确认按 U 后成功返回 Windows。** Windows Confirm-SecureBootUEFI=True、SetupMode=0，PK/KEK/db/dbx 全部符合登记计划；报告包含 `secure_boot=1 internal_volume=1`。见 [RUN03 原始报告与验收说明](evidence/secureboot-stage3-0.2.0-run03/README.md)。本次没有新性能数据，0.1.2 的历史性能不能直接作为本次性能结果。

签名 EFI 已通过密码学和内容一致性校验；本机采用 Custom / Setup Mode 登记，恢复原 Intel PK、原 KEK 与 dbx，db 保留原库并增加一张本机签名证书。重启后进入 User Mode，再开启 Secure Boot。最新步骤与实际哈希见 [本机签名与 Secure Boot 登记](SECUREBOOT-SIGNING.zh-CN.md)。这是本机自定义信任路径，不能把本机证书当作其他机器默认信任的微软签名。

此前 Secure Boot 关闭时的 [RUN00 报告](evidence/disk-boot-stage3-0.2.0-run00/REPORT.TXT) 和 [核验记录](evidence/disk-boot-stage3-0.2.0-run00/verification.json) 表明：internal_volume=1、事务 result=0、required_plm=1、FLR 前后选择器匹配、recovered=1、retained=0、maps=0、最终 status=0。RUN03 在 Secure Boot 开启时也通过这些检查。两次 all_plm=0 均对应已知非致命 FBPA 未匹配，不能描述成四个 PLM 全部成功。RUN00 报告 SHA256 为 `cd48ed65bd14489879551f3a9837bb4d30519b2f3acc37f98f9cce0c0c5dd26f`；RUN03 为 `bd2474d7144579ca71a07f74d776b2a2f53c9eb29700724a8948d44ba9361d78`。报告 build ID、对应 EFI 哈希、当前源码与原候选 manifest 均已交叉核对。

原构建 manifest 的 hardware_tested=false 及程序头部未实测提示是构建时状态，保留作为历史记录；上述新增证据记录这次后续实机结果。采集器没有独立证明报告与 Windows 同一次启动，返回 Windows 的关联同时依赖用户本次操作确认。

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

资产总计约 85 MB（十进制），还有 EFI、许可证和运行日志。安装前按实际目录大小加至少 32 MiB 余量检查可用空间；该余量不是 100 次运行的容量保证。分区容量不足时先决定另一个固件可读 ESP 或单独规划分区调整，不能直接清理微软引导文件腾空间。现已提供 `Install-InternalEfi.ps1`，按采集的分区 GUID 重新核对实际系统 ESP；不调整分区。

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

## 首次部署：只读采集（本机已完成）

以管理员身份打开 Windows PowerShell：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File F:\AI\gpu-unlock-4080s\Get-BootPreflight.ps1
```

输出 `LOCAL-boot-preflight\<时间>\preflight.json`、BCD 文本及公开 Secure Boot 变量。采集磁盘/分区/可见卷剩余空间、BIOS、Secure Boot 状态和 BitLocker 保护状态；不采集恢复密码，不挂载 ESP，不写 NVRAM，不登记证书，不重启。管理员权限是 Windows 读取固件变量和分区信息所需。

该目录已被 Git 忽略。读不到某个默认变量不等于硬件失败；需要查看具体 error。若隐藏 ESP 的剩余空间未返回，下一步再按明确分区进行临时只读检查。BitLocker 若启用，在改变引导路径前必须确认用户持有恢复密钥；脚本不会替用户关闭保护或公开恢复密钥。

## 签名工具

`Sign-EfiCandidate.ps1` 读取已构建 manifest，核对候选 EFI 哈希，从 `Cert:\CurrentUser\My` 使用已有、有效且有私钥的 RSA 证书，调用 Windows SDK SignTool：

```powershell
# 证书准备及 db 登记方案确认后再使用；替换为实际证书 thumbprint。
.\Sign-EfiCandidate.ps1 -CertificateThumbprint '0123456789ABCDEF0123456789ABCDEF01234567'
```

脚本仅在新的 `LOCAL-signed\<时间>` 目录生成签名副本、公钥 CER 和签名 manifest，不导出私钥、不覆盖 unsigned 构建、不安装。命令使用 `/fd SHA256 /nph`；`/sha1` 参数用于按指纹选择证书，不代表使用 SHA-1 签名文件。参数依据 [Microsoft SignTool 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/signtool)。

匹配到 SignerCertificate 只是签名者检查，Windows Authenticode 状态也不等于固件 db 验证结果。脚本记录状态，不宣称固件已信任。签名后 EFI 哈希会变化，应使用 signing-manifest 的 signed hash，不能继续用 unsigned manifest 的哈希冒充最终镜像。

私钥应留在本机证书存储或专门离线保管，不能提交 Git；`*.pfx / *.p12 / *.key` 和 `LOCAL-*` 已忽略。现已在 Admin 的 CurrentUser\My 中创建不可导出的本机签名私钥；没有导出私钥。此处为工具说明，实际签名安装与固件登记状态见上方最新记录。

## 首次部署的验证顺序

本次构建已运行 boot-media、port、app、admission 测试，并使用本机精确匹配的 PROM fixture 跑完 43 个 core 事务模拟场景。测试不执行真实 GPU；主机 Secure Boot 场景模拟的是入口与分支逻辑，不模拟固件密码学验证。构建 manifest 标记 `hardware_tested=false`。

1. 取得上述只读报告，选定实际 ESP，确认空间、保留原启动项与恢复路径。
2. Secure Boot 保持关闭，安装独立目录/启动项，先一次性从硬盘启动候选版，采集新的 REPORT 与同启动周期基准。
3. 确定证书登记方式并准备可审核的变更/恢复方案，再签名、登记；不得因启动失败而盲目清空所有固件密钥。
4. 重新开启 Secure Boot，核验固件变量、候选实际执行及 Windows 状态，再核验 GPU 报告和性能。未受信任或被篡改 EFI 应被固件拒绝；该验证须保留原 Windows 可启动项。

源码在本地 `feature/internal-efi-secureboot` 分支，旧实测 `artifacts/stage3-0.1.2/BOOTX64.EFI` 保留。该旧版本仍要求 USB/SD 与 Secure Boot 关闭，不能拿旧二进制验证新功能。

## 管理员安装命令

```powershell
# 替换成实际采集文件。缺省仅打印计划，不挂载 ESP 或改启动项。
.\Install-InternalEfi.ps1 -PreflightReport 'C:\path\to\preflight.json'

# 同一套检查通过后，执行安装；不会关机、重启或设置 BootNext。
.\Install-InternalEfi.ps1 -PreflightReport 'C:\path\to\preflight.json' -Apply
```

安装脚本核对当前分区 GUID、GPT/NVMe 或 SATA、FAT32、空间、Secure Boot 关闭及未加密的 Windows 状态，核对候选 EFI 和五个资产。临时挂载系统 ESP 后再次核对分区身份；已有 GPUUnlock 目录或启动项时停止，不覆盖。复制后逐文件校验，核对原 Windows EFI 哈希，再克隆 `{bootmgr}` 为独立项并更改新项的 device/path，把新项放在固件列表末尾。

原 Windows 项保持首位，原设备/path/其他设置不变；临时挂载造成的盘符显示变化在比较中单独处理。登记失败时尝试删除本次新项并恢复原顺序；复制文件保留供分析。结果与命令日志写在 `LOCAL-disk-install\<时间>\`，最后撤销临时挂载。脚本要求英文 BCDEdit 输出；当前本机符合。BCD export 是 Windows 配置备份，不是完整 NVRAM/固件密钥备份。

登记方式依据 [Microsoft 对 NVRAM 中 Windows Boot Manager 克隆的说明](https://learn.microsoft.com/en-us/troubleshoot/windows-server/backup-and-storage/set-up-dynamic-boot-partition-mirroring)；本项目只借用独立引导项的复制/设置机制，不执行该文的磁盘转换、格式化或镜像操作。固件启动顺序设置依据 [Microsoft UEFI BCD 说明](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcd-system-store-settings-for-uefi)。

只有输出 `INSTALLED: {GUID}` 且无错误后才进行首次测试：保存工作并完全关机，再开机进入固件一次性启动菜单，选择 `GPUUnlock 4080S (experimental)`，按 U。若失败，不改默认项；从同一菜单选择原 Windows Boot Manager 返回 Windows。脚本不承诺失效固件一定自动恢复或一定能完成 GPU 事务。保留本次安装状态和新生成的 RUN 报告，不能用 USB 上旧 RUN05 作为此次结果。

回到 Windows 后，以管理员身份运行 `.\Collect-InternalEfiReport.ps1`。它按最近安装状态核对系统 ESP 和已安装 EFI 哈希，临时挂载并导出所有 RUN 的 REPORT.TXT 到新的 `LOCAL-disk-reports` 目录，然后撤销挂载。输出最高 RUN 编号的报告路径；最高编号本身不证明属于当前启动，因此还记录 Windows 启动时间并明确 `same_boot_verified=false`，需结合本次操作核对。无报告也会明确报错，不自动跑基准或把旧成功报告视为本次成功。

若需要禁用新项，使用安装输出里带实际 GUID 的 `bcdedit /delete "{GUID}"`，只删除新项，文件和日志保留。默认启动顺序调整及 Secure Boot 密钥登记放到各自实测通过之后。

新增工具验证：`tests/disk_install_tests.ps1` 覆盖 16 个登记/回读/失败回退场景，130 项断言；`tests/disk_plan_tests.ps1` 使用实际候选包、模拟系统状态运行 6 个计划路径场景。均在 Windows PowerShell 5.1 下通过，不代表已经在真实 ESP/NVRAM 上执行安装。

2026-10-26 首次安装在挂载校验处停止：`mountvol S: /S` 成功，但 `Get-Partition -DriveLetter S` 未找到对象。状态记录为 `entry_id=null`、`new_files_retained=false`、`unmount_exit_code=0`，未复制候选文件、未创建启动项，临时挂载已撤销，无需回滚候选安装。

修正后先通过已验证分区取得 `Get-Volume.Path`，再使用 [Microsoft QueryDosDeviceW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-querydosdevicew) 比较目标卷 GUID 名称和挂载盘符的当前 NT 设备映射。不能直接把分区 GUID 当作卷 GUID；也不会因盘符查询失败而跳过身份检查。安装与报告导出共用同一校验，查询失败或映射不一致均停止。新增 `tests/esp_mount_tests.ps1` 覆盖 11 种情况、13 项断言；并在本机 PowerShell 5.1 只读验证 C: 与其卷 GUID 映射一致，系统 ESP 的卷 GUID 可解析为 `\Device\HarddiskVolume1`。修正后的实际安装和后续 RUN00/RUN03 采集均已通过挂载身份检查。

## 本机信任库解析结果

2026-10-25 23:20 本机采集显示 SecureBoot=0、SetupMode=0；Intel SBoot1 平台密钥仍在。PK/KEK 与默认库一致，KEK 同时含微软 2011 与 2023 证书。当前 db 有五张证书：Microsoft Corporation UEFI CA 2011、Microsoft Windows Production PCA 2011、Windows UEFI CA 2023、Microsoft Option ROM UEFI CA 2023、Microsoft UEFI CA 2023。

相对 dbDefault，当前 db 新增后两张 2023 证书，且不含默认库中的 UEFIappl1。当前 dbx 有 495 个签名条目，默认库有 371 个，差异为新增 124 个、缺失 0 个。这里是离线解析得到的事实，不是对各条撤销内容的独立有效性验证。不能将 Restore Factory Keys 当成保持当前信任状态不变的操作。

`Inspect-SecureBootDb.ps1` 可离线复核公开变量：先核对采集 SHA256，再有界解析 EFI_SIGNATURE_LIST，读取 X.509 名称与指纹并比较当前/默认条目；不访问固件、不生成私钥、不登记证书。User Mode 下不能假设管理员权限足以直接向 db 追加证书。本机后续已经完成 Custom / Setup Mode 登记并恢复 User Mode；当前 db 为原五张证书加一张本机证书，原 PK/KEK/dbx 逐字节保留。不要重复清空或登记密钥。
