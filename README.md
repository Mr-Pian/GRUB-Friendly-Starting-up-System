<div align="left">
  <a href="README_en.md"><img src="https://img.shields.io/badge/Language-English-lightgrey?style=flat-square" alt="English"></a>
  <img src="https://img.shields.io/badge/语言-简体中文-blue?style=flat-square" alt="简体中文">
</div>

# GRUB-Friendly-Starting-up-System (GRUB 友好型远程开机系统)

本项目是一个软硬件结合的智能开机网关系统，专为多系统用户（特别是使用 GRUB 引导程序的用户）设计。它通过 **ESP32-S3** 构成的带屏网关和 **EFR32BG22** 构成的低功耗蓝牙 (BLE) 钥匙，为你提供优雅、无缝且支持远程控制的电脑开机与系统切换体验。

## ✨ 核心特性

- **多主控协同**：
  - **ESP32-S3 网关**：负责网络通信、Mesh 组网，并搭载 **LVGL** 图形界面进行状态显示与交互。
  - **EFR32BG22 蓝牙模块**：作为低功耗蓝牙 (BLE) 节点/钥匙，实现无感配对或远程指令接收。
- **友好的多系统支持**：针对包含 GRUB 引导程序的双系统/多系统环境，提供更智能的启动项选择方案。
- **Web 端控制支持**：配套了基于 Node.js 的轻量级 Web 服务器，支持局域网内通过浏览器直接控制开机状态。
- **全套开源**：包含完整的设备固件（Firmware）、硬件 PCB 生产文件（Hardware/Gerber）及配套资料。

## 📸 项目展示 (实物图)

| ESP32-S3 开机网关 | EFR32BG22 蓝牙钥匙 | Web 端/系统界面 |
| :---: | :---: | :---: |
| <img src="images/Esp32_Mesh.png" height="200" alt="Gateway"> | <img src="images/Grub_Key.png" height="220" alt="BLE Key"> | <img src="images/Web_UI.png" height="220" alt="UI"> |
## 📂 目录结构

Plaintext

```c
GRUB-Friendly-Starting-up-System/
├── Documents/            # 项目相关的参考文档、芯片数据手册（ESP32-S3, EFR32BG22, TPS62132 等）及字体/图标素材
├── Firmware/             # 硬件设备的固件源码
│   ├── ESP32_S3_Mesh/    # 基于 ESP-IDF 开发的网关固件，包含 BLE 客户端、LVGL UI 及网状网络功能
│   └── GRUB_Key/         # 基于 Silicon Labs SDK 开发的 EFR32BG22 蓝牙端点/按键固件
├── Hardware/             # 硬件电路设计与生产制造文件
│   ├── Gerber/           # 可直接发给板厂打样的 Gerber 打板文件 (含 ESP32S3_Mesh 与 EFR32BG22)
│   └── PCB/              # EasyEDA (立创EDA) 原理图与 PCB 源工程文件 (.eprj2)
├── Web/                  # 配套的 Web 后台控制服务
│   ├── index.html        # 前端控制面板
│   ├── package.json      # Node.js 依赖配置
│   └── server.js         # 后端服务脚本
├── LICENSE               # 开源许可证文件
└── README.md             # 项目说明文件 (本文档)
```

## 🛠️ 快速开始

### 1. 硬件准备与打板

- 进入 `Hardware/Gerber` 目录，分别下载 `ESP32S3_Mesh_xxxx.zip` 和 `EFR32BG22_xxxx.zip`。
- 将压缩包直接上传至嘉立创 (JLC) 或其他 PCB 制造商进行打样。
- 根据 `Documents` 中的硬件规格书准备对应物料（如 ESP32-S3-MINI-1 模组、EFR32 芯片、TPS62132 电源芯片等）并完成焊接。

### 2. 网关固件编译与烧录 (ESP32-S3)

- 推荐环境：[ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html) (推荐 v5.x 版本)

- 路径：`Firmware/ESP32_S3_Mesh`

- 配置与编译：

  Bash

  ```
  cd Firmware/ESP32_S3_Mesh
  idf.py build
  idf.py -p (你的串口号) flash monitor
  ```

### 3. 蓝牙钥匙编译与烧录 (EFR32BG22)

- 推荐环境：[Simplicity Studio 5](https://www.silabs.com/developers/simplicity-studio) + Gecko SDK
- 路径：`Firmware/GRUB_Key`
- 导入项目 `GRUB_Key.slcp`，编译并使用 J-Link 或其他兼容调试器烧录 `.hex` 或 `.bin` 文件。

### 4. 运行 Web 控制端

- 确保已安装 [Node.js](https://nodejs.org/)。

- 路径：`Web/`

- 运行命令：

  Bash

  ```
  cd Web
  npm install
  npm start
  ```

- 服务启动后，在浏览器访问控制台即可（默认端口以终端输出为准）。

## 📄 许可证 (License)

本项目遵循 [MIT / GPL 等许可证，请根据实际 https://www.google.com/search?q=LICENSE 文件修改] 开源协议。详情请参阅项目根目录下的 [LICENSE](https://www.google.com/search?q=LICENSE) 文件。
