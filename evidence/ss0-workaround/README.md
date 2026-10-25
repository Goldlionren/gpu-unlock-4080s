# 初始 SS0 有限兼容补丁

2026-10-26 08:43：用户要求为反复出现的初始 SS0 不匹配提供 workaround。本机先后观察到完整值 `0x64113035`、`0x64113036`、`0x64113037`。诊断 RUN07 在 `…35` 时实测成功，但尚未在 `…36/37` 时通过完整事务；本补丁是有限范围的实验兼容调整，不是已确认的根因修复。

## 变化和依据

原入口把一次采集得到的 SS0 固定为 `0x64113035`。源码中该初始值用于准入、记录和选择器写入前的读回，不参与显存布局或 DMA 地址计算；核心事务最终写入完整目标 `0x88888888` 并验证，不是在初值上加减。

新增 `ColdSs0Allowed()`，仅允许上述三个**完整 32 位值**，不屏蔽低位、不接受任意值或已解锁值。要求初始读取、固定寄存器采集后的复读、PROM 双读校验后的第三次读取全部相同。SS1 仍必须为 2，其他初始寄存器、PCI 身份/拓扑、PROM 精确哈希和资产检查不变。

若初值为允许的 `…36/37`，其余所有检查通过后可进入现有固件事务。解锁后的 SS0=0x88888888、SS1=8、FLR 前后回读、必要 PLM、DMA 清理和 PCI 配置恢复仍必须通过。未知值、复读不一致、读取故障或其他状态差异仍停止，不增加固件自动重试。

日志包括 `cold diagnostics revision=2`、`cold selectors post-PROM` 和 `cold profile accepted: ... observed_variant=1`。这些表示准入和诊断，不单独证明事务成功；仍须核对最终结果并现场测量性能。

## 验证

- app：58 个应用流程场景、9896 项断言；稳定的 `…36/37` 进入模拟事务，未知 `…38`、复读变化、PROM 后变化/故障、PROM 错误、SS1 错误和其他初始状态错误不进入事务。
- 允许列表测试覆盖低字节全部 256 个值、每个允许值的高 24 位逐位变动、0、全一值和解锁目标值。
- core：43 个模拟事务场景。
- port：14369 项断言；admission：27 场景、2141 项断言；boot-media：76 项检查。
- EFI 格式、固定资产/头文件及源码边界检查通过。核心固件执行代码未改。

本目录的 manifest、verification 和测试日志保留原始字节。`hardware_tested=false` 是本补丁构建时事实；模拟测试不证明新增起始状态的真实固件兼容性。

## 签名与安装

| 项目 | 值 |
|---|---|
| build ID | `e49ef765604e53658702e6db4c96846b5ab8b181e1af049280c4f0bbcc44a418` |
| unsigned SHA256 | `c9bfe8025823f93a62a208b430ca3afe99cf94b7afa3f61d7cdf6fe1c323bb72` |
| signed SHA256 | `28a2d129a0efbf92550eac91b02b8c2b144a21801b4d6f00fae902ca43873a6f` |
| 原诊断版 signed SHA256 | `4f8dc5e53fecc34861a9c7259c5af48b634cc135849b52eceb9f34d070cb7ed2` |

隔离构建目录为本机 `LOCAL-ss0-workaround/20261026`，保留原 build 二进制以兼容历史签名 bundle 的核验。沿用已经登记的本机证书，验证 CMS、映像摘要与签名前后执行内容一致性后安装。安装前后验证原有固件信任精确匹配、SecureBoot=1、SetupMode=0；验证 ESP 身份与资产，备份旧签名 EFI 和 manifest，替换 GPUUnlock EFI 和 manifest 后回读哈希、验证签名并确认原 Windows EFI 未变。未改密钥/启动顺序，未重启，临时挂载已撤销。

本机安装记录位于 `LOCAL-disk-install/20261026-084354-ss0-workaround-143ee469/install-state.json`，同目录保留 `GPUUnlock.previous-signed.efi` 和 `manifest.previous.json`。本目录不发布私钥、完整本机信任库或固件资产。

## 下一次实机验证

正常重启，在提示时按 U，返回 Windows 后执行 `Collect-InternalEfiReport.ps1` 与 `Check-UnlockStatus.ps1`。初始值为 `…36/37` 且事务、恢复、性能均通过时，才新增相应状态的实机成功记录；若仅出现 `…35`，只能确认既有状态仍可用。当前没有新增实机报告。
