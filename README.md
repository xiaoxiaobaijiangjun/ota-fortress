# FOTA_STM32_Resume

STM32F407VET6 固件在线升级（FOTA）项目 —— 移植自
[EhabMagdyy/FOTA-STM32](https://github.com/EhabMagdyy/FOTA-STM32)，用于简历项目与面试演示。

包含：

- `Bootloader/` —— 自定义 Bootloader（Keil MDK 工程，0x08000000，6.8 KB，0 错误 0 警告）
- `App/` —— 示例用户应用（A/B 双槽：Slot A 0x0800C000 / Slot B 0x08040000，VTOR 重定位，LED + 串口心跳）
- `Host/fota_host.py` —— PC 串口调试工具 + 双槽协议模拟器自测
- `docs/` —— 中文说明文档与简历素材

快速开始：

```text
cd Host
python fota_host.py selftest                  # 无硬件协议自测
python fota_host.py --port COM5 id            # 真机读芯片 ID
python fota_host.py --port COM5 status        # 查看激活槽与升级状态
python fota_host.py --port COM5 --slot B update demo_firmware\Application_B.bin   # 一键升级到 Slot B
```

详细文档见 [docs/README.md](docs/README.md)，简历素材见 [docs/简历项目描述.md](docs/简历项目描述.md)。

面试学习笔记（Obsidian 原版在 `D:\App\Obsidian\学习\嵌入式\项目\FOTA固件升级\`）：
- [docs/项目从零理解教学笔记.md](docs/项目从零理解教学笔记.md) —— 从零讲解 Bootloader/FOTA/协议/CRC/跳转，每节带自测题
- [docs/调试与排错实战记录.md](docs/调试与排错实战记录.md) —— 移植与调试踩坑实录 + 面试回答模板
