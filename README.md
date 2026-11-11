# ESP32 智能小车多模式控制系统

一个面向嵌入式与物联网实践的综合项目，包含四路红外循迹、超声波避障、ESP32-CAM 图传、Qt 上位机、Web 控制台和本地 TCP 桥接服务。

## 功能

- 四路红外循迹与规定赛道行驶
- HC-SR04 超声波测距、分级减速和障碍绕行
- L298N 差速驱动与 PWM 调速
- ESP32-CAM 图传、红/绿目标识别和自动靠近
- Qt 桌面控制台：串口 / 网络控制、视频预览、状态显示
- Web 控制台：方向控制、组合键差速转向、Shift 加速
- 本地 TCP 桥接：浏览器 HTTP 与 ESP32 TCP 协议互通
- 300 ms 指令超时停车、连接断开保护、PWM 平滑渐变

## 目录

```text
firmware/
  esp32-cam/        ESP32-CAM 图传、色块识别与避障固件
  arduino/          Arduino 循迹与超声波避障示例
desktop/Car/        Qt 6 桌面控制台源码
web-controller/     Node.js 本地 TCP 桥接与网页控制台
```

## 快速开始

### 1. 固件

使用 Arduino IDE 打开对应的 `.ino` 文件，选择开发板和端口后编译烧录。ESP32-CAM 固件需要根据实际网络配置修改 Wi-Fi 名称、密码与服务地址。

### 2. Qt 上位机

使用 Qt 6 打开 `desktop/Car/Car.pro`，配置对应 Kit 后构建运行。编译说明见 `desktop/Car/README_编译说明.txt`。

### 3. Web 控制台

```bash
cd web-controller
npm install
npm test
npm start
```

浏览器打开本地控制台后，可按住方向键控制小车。组合键会生成差速转向指令，松开按键或页面失焦时自动停车。

## 技术栈

ESP32 / Arduino · L298N · HC-SR04 · 红外传感器 · ESP32-CAM · OpenCV · Qt 6 · C++ · Node.js · TCP / HTTP

## 说明

仓库只包含项目源码和必要资源，不包含编译产物、串口日志或本机路径配置。
