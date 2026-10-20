# ota-fortress

STM32F4 固件升级（FOTA）系统，基于上游开源项目二次开发。

要解决的问题很直接：设备装到现场之后固件难免要修 bug、加功能，拆机烧录成本高，而升级本身又有风险——传到一半断电、传了个坏包、新固件跑不起来，任何一种都可能把设备变成砖。这套系统围绕"升级失败也不能变砖"来设计，真机验证过各种失败场景。

## 设计思路

**A/B 双分区。** 单分区升级是"先擦后写"，中断就没了退路。这里把 512KB Flash 分成四块：Bootloader 32KB、元数据 16KB、Slot A 208KB、Slot B 256KB。升级永远写非激活槽，写完激活，旧固件原封不动躺在另一个槽里——升级失败的成本被压到"重传一次"。元数据单独占一个扇区，擦任何一个应用槽都不会破坏升级状态。

**激活门槛。** 新固件不是写进去就算数。Activate 命令触发一整条校验链：整包 CRC32 → 解析 TLV → SHA-256 摘要 → ECDSA-P256 验签（micro-ecc）→ security_version 比较。任何一步不过都拒绝激活。验签放在 Bootloader 而不是 App，因为信任锚不能是被验证的对象本身。带 security_version 是防降级攻击：就算攻击者拿到合法签名密钥签一个有漏洞的旧版本，版本号低于地板值照样拒。

**双保险回滚。** 激活后固件以 TRIAL 状态试运行，App 健康跑满 3 秒才写确认（IDLE）；没确认就断电，下次上电自动切回旧槽。软件确认之外再加 IWDG 看门狗（约 10 秒，按 LSI 最坏情况标定）：新固件挂死会被狗复位，同样触发回滚。两个机制都单独真机验证过。

**写保护。** Flash 写命令只允许落在应用槽内，Bootloader 扇区和元数据扇区写不进去，协议层面的越界地址直接 NACK——升级会话本身没法破坏升级能力。

## 三条物理层

同一套升级协议（8 条命令、64B 分包、每包 CRC32），主机工具不改一行代码就能走三种通道：

| 通道 | 硬件 | 说明 |
|---|---|---|
| 串口 | USB-TTL ↔ USART1（PA9/PA10） | 有线基线 |
| 蓝牙 | JDY-31 SPP 透传 | 120 包签名固件零重传 |
| CAN | F103 主站（寄存器级 bxCAN）+ 简化 ISO-TP | 刷写语义对标 UDS 的请求下载/传输数据/结束传输 |

CAN 这条腿做了一个 F103 主站：寄存器级 bxCAN、500kbps 位时序定标、ISO-TP 分段重组（SF/FF/CF/FC），加上 UART↔CAN 纯透传桥，把 CAN 总线变成主机工具眼里的一根串口线。选寄存器级而不是 HAL，是因为排查双节点"互不认帧"问题时 TSR 判决位要看得清楚。

## 工具链

主机端 `fota_host.py` 带协议模拟器：Python 里模拟一个双槽 Bootloader，无硬件跑通上传、比对、激活、回滚全状态机。签名链是 `tools/keygen.py`（密钥对，公钥编进 Bootloader，私钥不进仓库）+ `tools/sign.py`（镜像尾部追加 TLV：security_version、SHA-256、ECDSA 签名）。

## 验证情况

- 串口通道全流程真机通过：读 ID、擦除、上传、激活、跳转、心跳，以及四类攻击包（坏 CRC、越界地址、伪命令、坏签名）全部拒绝
- 断电回滚、IWDG 挂死自愈均真机复现
- 蓝牙通道全流程 120 包零重传
- CAN 通道双节点组网完成，收发器故障已定位，待换件复测

## 使用

USB-TTL 接 PA9/PA10，115200-8-N-1。Bootloader 和 App（Slot A/B 两个工程）都用 Keil MDK 编译。

```bash
python tools/keygen.py
python tools/sign.py app.bin --sv 1 -o app_signed.bin

cd Host
python fota_host.py selftest                              # 无硬件全链路自测
python fota_host.py --port COM5 --slot B update app_signed.bin
```

## 目录

| 目录 | 内容 |
|---|---|
| `Bootloader/` | F407 Bootloader（Keil 工程，含 CAN 传输层） |
| `CanMaster/` | F103 CAN 主站 |
| `App/` | 示例应用（Slot A / Slot B 两个工程） |
| `Host/` | 主机工具与协议模拟器 |
| `tools/` | 签名工具、Keil 工程生成脚本、主站诊断 |
