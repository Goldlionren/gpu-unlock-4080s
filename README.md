# RTX 4080 SUPER 16GB UEFI unlock experiment

Windows 11 Pro 上的一次真实实机适配与测量。成功版本 **stage3-0.1.2**：在 Windows 启动前运行自定义 UEFI 程序，核对 SS0/SS1 写入及 FLR 后读回，恢复设备后自动进入原 Windows。

**这是单块已测板卡的实验代码，不是通用 RTX 40 系列工具。** 入口固定为 AD103、`10de:2702 / 19da:4702`、16GB、特定 BAR/ROM 哈希与 Intel GOP 拓扑。Secure Boot 关闭；不刷写 VBIOS、不替换 NVIDIA Windows 驱动。其他板卡应被检查拒绝，不能靠删除检查获得兼容性。

## 实测结果

原驱动 581.57、PyTorch 2.12.1+cu130 / CUDA 13.0、相同基准和精度设置。8192×8192 GEMM 两次吞吐均值：

| 类型 | 原始 TFLOPS | 本次 TFLOPS | 变化 |
|---|---:|---:|---:|
| FP32_STRICT（TF32 关闭） | 35.7495 | 34.6020 | **−3.21%** |
| FP16 | 108.2125 | 173.9930 | **+60.79%** |
| BF16 | 108.2415 | 181.4745 | **+67.66%** |
| FP8 E4M3 | 199.4165 | 298.7015 | **+49.79%** |

完整 2048/4096/8192、四次数据、耗时、失败过程和补丁源码解释：[全过程与真实性能报告](PROCESS.zh-CN.md)。[成功 RUN05 日志](REPORT.TXT)；[数据与比较](results/20261025-220124-082/comparison.json)。

FP32 没有提升。未独立验证数值误差、ComfyUI 工作流速度、长期稳定性或跨重启保留；背景活动/温度/频率未严格控制。以上是这次启动的实际 GEMM 观测，不是理论峰值或所有应用的加速承诺。

## 获取资产并组装已实测版本

要求 Windows、Python 3.10+、网络连接。固件和原 ROM 不随 Git 分发；脚本直接从固定上游提交下载 5 个文件并校验大小/SHA256，约 85MB。工具不执行固件或改变启动设置。

```powershell
git clone https://github.com/Goldlionren/gpu-unlock-4080s.git
cd gpu-unlock-4080s
python tools\fetch_assets.py
powershell -NoProfile -ExecutionPolicy Bypass -File .\Assemble-TestedUSB.ps1
```

`USB\` 内生成 `EFI` 和 `RTX4080S-UNLOCK` 两个目录。此路径使用仓库内保留的原已实测 EFI；它的哈希和对应源码见 `evidence/tested-stage3-0.1.2/manifest.json`。不要混淆下面重新编译的产物与原已实测二进制。

本机实测启动步骤：

1. 备份 U 盘原启动文件，将上述两个目录复制到 FAT32 U 盘根目录，保留旧 RUN 日志。
2. 保存工作，保持 Secure Boot 关闭、Intel 核显承担 UEFI 输出，完整关机后从一次性启动菜单选择 U 盘。
3. READY 后 30 秒内按 U，执行一次事务；成功后自动进入 Windows。
4. STOP、挂起或启动异常时物理关机；不要从失败状态热重启继续使用。
5. 复制该次最新 `RTX4080S-UNLOCK\RUNxx\REPORT.TXT`。目录编号自动递增，不要把旧成功日志用作新启动证据。

## 从源码构建

要求 Visual Studio 2022 C++ x64 工具和 Windows SDK；脚本通过 vswhere 查找工具链，也可显式传 `-VcVarsPath`。

```powershell
python tools\fetch_assets.py
powershell -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1
```

该命令编译 EFI，并运行 port/app/admission 主机模拟测试、校验 PE/资产/源码，再生成 USB 目录。完整 core 事务测试需要本机实测 PROM fixture，因其包含固件字节而不公开；缺省时明确记录 **SKIPPED**，不会假报通过。已有完全匹配 fixture 时：

```powershell
.\Build.ps1 -PromPath 'C:\your-local-capture\physical_prom_prefix.bin'
```

fixture 仅接受 SHA256 `dd5a21fc1434583d3660de35f60799c2cce3b72cd5330dd1d7e5b042a0785f6f`。原实测构建的 43 个 core 场景日志已保留。`src/` 与已实测 manifest 一致；顶层构建/打包/路径工具为公开版调整，重新构建的 build ID/EFI 哈希会变化，不代表新增实机验证。

## 核验数据与性能采集

只核验公开证据，不运行 GPU：

```powershell
python tools\verify_results.py
```

成功自动进入 Windows 后，在**同一启动周期**退出其他 GPU 负载，传入当前实际报告路径和你自己的 Python：

```powershell
.\Collect-AfterBoot.ps1 -Report 'G:\RTX4080S-UNLOCK\RUN05\REPORT.TXT' -Python 'C:\your-venv\Scripts\python.exe'
```

这里 G:、RUN05 是例子，必须替换成此次启动的报告。脚本验证报告后跑原基准两次，结果存入新 `results/<timestamp>`。源码中的 STOCK 标题/stock 文件名为旧命名，后测状态由 context 和报告区分。不要提交新生成的本机路径或私人数据。

## 补丁与授权

- 适配 16GB 本机身份、PROM、布局、无独显 GOP 占用的 UEFI 路径。
- 选择器失配不能因清理成功而返回解锁成功；DMA 不确定状态保留资源。
- 分离工作态 Command=2 检查与 STOP 中已恢复 Command=0 的隔离。
- 仅在验证成功事务/FLR 后允许 `0x80: 0x00092930 → 0x00002930`，其他配置字节仍严格相等；不回写错误状态位。

上游 V67 载荷保持原样，核心研究归属与许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。GPL-2.0-only 适用于 GPL 派生应用/组合执行文件；保留各文件 MIT、EDK2 BSD 等第三方声明。原始固件不按应用许可证重新授权。
