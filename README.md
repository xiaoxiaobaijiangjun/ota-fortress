# FOTA_STM32_Resume — STM32F4 安全固件升级系统

基于开源 [EhabMagdyy/FOTA-STM32](https://github.com/EhabMagdyy/FOTA-STM32) 二次开发的安全固件升级系统：**A/B 双槽 + ECDSA-P256 验签 + 防降级 + 三物理层刷写通道（串口/蓝牙/CAN）**。用于简历项目与面试演示。

> **二次开发边界（诚实声明）**：核心跳转沿用原项目。本项目新增：A/B 双分区状态机（元数据扇区/TRIAL-IDLE 确认/断电回滚）、IWDG 挂死自愈、ECDSA-P256+SHA-256 验签链（micro-ecc，TLV 格式对标 MCUboot）、security_version 防降级、Python 主机工具+协议模拟器、蓝牙（JDY-31 SPP）与 CAN（F103 主站+简化 ISO-TP）两条新物理层、检测与排查工具链。详见 `docs/简历项目描述.md` Q8 归属清单。

## 架构

```text
[PC] fota_host.py（命令流+签名工具，三通道共用，零改动换物理层）
      │            │                │
   USART1       JDY-31 SPP       CAN 500kbps
   （有线）      （蓝牙透传）      F103 主站（寄存器级 bxCAN + ISO-TP + UART↔CAN 桥）
      └────────────┴────────────────┘
                   │  同一组升级命令（8 条 · 64B 分包 · CRC32 · TLV 签名包）
                   ▼
        F407 Bootloader（32KB，写保护）
          ├─ 传输层收包/校验（CRC32 每包）
          ├─ A/B 双槽写入（208K/256K，只写非激活槽）
          ├─ 激活：整包 CRC → TLV 解析 → SHA-256 → ECDSA 验签 → min_sv 地板 → TRIAL
          └─ 跳转 + IWDG 自愈（挂死 ~10s 自动回滚）
```

## 核心设计取舍

- **A/B 双分区 vs 三分区/单分区**：单分区升级中断=变砖风险；A/B 的代价是 Flash 预算（512KB 才切得下），换来"升级失败成本=一次重传"。元数据独立扇区（16KB）保证擦任一槽不破坏升级状态。
- **TLV 镜像格式**：`[image][FTLV magic][sv 条目][SHA-256 条目][ECDSA 签名条目]` 追加在 bin 尾——上传路径不变（主机把 image+TLV 当一个文件传），验签数据随镜像走。security_version 在 digest 覆盖范围内（防篡改降级标）。
- **验签在 Bootloader 而非 App**：信任锚不能是被验对象；App 可能已被整体替换。当前时机=激活时必验（固件进槽唯一入口），TRIAL 首启加验是已知改进项。验签耗时 1592ms@16MHz HSI（配 PLL 可缩至 ~150ms，未做）。
- **防降级 ≠ A/B 回滚**：回滚是 TRIAL 未确认的内部状态处理，不走激活；min_sv 拦的是重新激活的签名合法旧包。写地板时机在激活而非 App 确认。
- **CAN 传输层**：经典 CAN 8B 载荷 → 简化 ISO-TP（SF/FF/CF/FC，BS=8/STmin=1ms）；刷写语义对标 UDS 三服务（请求下载/传输数据/结束传输）。F103 主站选寄存器级 bxCAN——TSR 判决位看得清楚，排查时救过命。

## 目录

| 目录 | 内容 |
|---|---|
| `Bootloader/` | F407 Bootloader（Keil，0x08000000，17.56KB，0 错 0 警；含 CAN 传输层与上电自检判决） |
| `CanMaster/` | F103 CAN 主站（寄存器级 bxCAN + ISO-TP + 透传桥；`CANM_SNIFFER` 开关=纯监听诊断固件） |
| `App/` | 示例应用（A/B 双槽，VTOR 重定位，LED+串口心跳+3s 确认） |
| `Host/` | `fota_host.py` 主机工具（id/erase/upload/activate/monitor + 协议模拟器自测） |
| `tools/` | keygen.py / sign.py（签名链）/ canm_stats.py（主站诊断） |
| `docs/` | 简历项目描述 · 调试与排错实战记录（坑 1-9）· 项目从零理解教学笔记（0-15 节） |

## 快速开始

```bash
cd Host
python fota_host.py selftest                                        # 无硬件协议自测
python fota_host.py --port COM8 id                                  # 串口腿：读芯片 ID（0x0413）
python fota_host.py --port COM4 --slot B update demo_firmware/FOTA_App_B_signed.bin   # 蓝牙腿（JDY-31）
```

CAN 腿：CanMaster 烧 F103 后 `--port COM8` 同命令走总线（500kbps；真机联调记录与收发器故障定位见 docs 坑 9，全流程验收清单见简历项目描述 C2 节）。

## 真机验收状态

串口腿全流程（含四类攻击包全拒、断电回滚、IWDG 自愈）2026-08-05 验收；蓝牙腿全流程 10.3s/120 包零重传 2026-10-18 验收；CAN 腿代码+双节点组网完成、收发器故障已定位（2026-10-19），验收清单待换件复测。证据链均归档 `D:\文件\找工作\秋招备战\02-实验记录\`。

## 文档

- [docs/简历项目描述.md](docs/简历项目描述.md) — 投递版描述、真机清单、面试问答（含 C2 节与 Q-CAN）
- [docs/调试与排错实战记录.md](docs/调试与排错实战记录.md) — 坑 1-9 实录（坑 9=CAN 双节点疑难四层判决）
- [docs/项目从零理解教学笔记.md](docs/项目从零理解教学笔记.md) — 0-15 节（第 15 节=CAN 通道互动精读）
