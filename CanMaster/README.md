# CanMaster — F103C8T6 CAN 升级主站（C2 真机）

> **定位**：蓝图 C2「CAN 刷写真机」的自建主站。架构 = **纯透传桥**：PC 串口上跑的
> 就是 FOTA 原版 `[LEN][cmd][args][crc32]` 命令流，主站把它按 ISO-TP 拆包发到
> 0x6E0、收 0x6E1 重组后原样回 PC——**`fota_host.py` 一行不改，只换 `--port`**。
> "换物理层不动协议层"从 T1（蓝牙）延伸到第二条腿（CAN），且主从两端都是自研。
>
> **不是 CubeMX 生成工程**（无 .ioc）：骨架裁自检测仪仓库（同芯片同晶振），
> CAN/USART 为寄存器级裸驱动，HAL 只管时钟/GPIO。

## 引脚

| 外设 | 引脚 | 去向 |
|---|---|---|
| USART1 TX/RX | PA9 / PA10 | CH340（交叉：PA9→RXD，PA10←TXD），115200 |
| CAN1 RX/TX | PA11 / PA12 | TJA1050 ①：PA11←RXD，PA12→TXD，模块 VCC=**5V** |
| SWD | PA13 / PA14 | ST-Link 烧录 |
| LED | PC13 | 板载灯（有则闪）：1Hz 心跳；保活被 ACK 时快闪 |

500 kbps：APB1 36 MHz ÷4 = 9 MHz，1+15+2=18 tq，采样点 88.9%（F407 侧 87.5%，同标称速率互操作）。

## 构建 / 烧录（CubeIDE）

1. File → Import → Existing Projects into Workspace → 选 `CanMaster` 目录
2. Project → Build（期望 0 错；首次构建会编译整个 HAL 树，稍慢）
3. Run → 用 ST-Link 下载（或 OpenOCD/STM32CubeProgrammer）

本机无 ARM 工具链，首次构建若有编译错误把贴报错给 AI 修。

## C2 联调流程（前置：F407 侧也要重烧一次）

**前置 A（F407 真机构建）**：`Bootloader/Core/Inc/can_transport.h` 的
`BL_CAN_LOOPBACK_TEST` 已翻 **0**（NORMAL 模式，AutoRetransmission=ON）。
用 Keil Rebuild Bootloader → 烧录 F407（只写扇区 0-1，应用区不动）→ 手动 RESET。

**前置 B（接线）**：见 `02-实验记录\接线图-三项目.html` ③ 节。
主站、F407 各自供电；两块 TJA1050 的 CANH↔CANH、CANL↔CANL、两板 GND 共地。

**跑**（CH340 接主站，假设 COM3）：

```
# 1. F407 按 RESET 进 bootloader（主站每 300ms 发保活帧，窗口不会超时，随时按都行）
python Host/fota_host.py --port COM3 --slot B erase        # 先单步验证链路
python Host/fota_host.py --port COM3 --slot B upload Host/demo_firmware/FOTA_App_B_signed.bin
python Host/fota_host.py --port COM3 --slot B activate Host/demo_firmware/FOTA_App_B_signed.bin
# 或一把梭（0xAA 魔法字节主站会吞掉，不影响）：
python Host/fota_host.py --port COM3 --slot B update Host/demo_firmware/FOTA_App_B_signed.bin
```

**取证**（验收数字：耗时/总帧数/错误帧数）：

```
python tools/canm_stats.py --port COM3    # 跑前跑后各一次，差值即本次会话
```

**错误注入**：upload 进行中拔掉 CANH（或 CANL）→ 主站静默 → fota_host 超时中止，
F407 侧不写 Flash（半包 CRC 不过）。插回后重跑即恢复。
错序号 CF 注入在真机总线上无法经由纯透传桥构造（主站只会发正确序号）——
该行为已由 C1 loopback 证据覆盖（同一段重组状态机），C2 口径如实这样讲。

## 主站本地行为备忘

- **吞 0xAA**：reboot 魔法字节只对 UART 版 App 有意义，CAN 上无 App 监听；
  进 bootloader 的方式 = F407 NRST（保活帧会一直续窗口，无 1s 竞态）。
- **保活**：空闲时每 300ms 发一帧 SF(len=0)——F407 重组器静默丢弃但它算
  "CAN activity"，bootloader 的 1s 自启动窗口因此一直续期。总线上没有应答方时
  自动降到 3s 一发。
- **0x5A 本地命令**：主站自己回统计行，不上总线（fota_host 永远不会发 0x5A）。
- **升级结束后**：F407 跳去 App（无 CAN），主站保活帧无 ACK → 降速，正常现象。
