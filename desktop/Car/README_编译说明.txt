Qt 小车控制台源码说明
=====================

开发环境：
- Qt 6.8.3
- MinGW 64-bit
- qmake

打开方式：
1. 启动 Qt Creator。
2. 打开 Car.pro。
3. 选择 Qt 6.8.3 MinGW 64-bit 构建套件。
4. 执行“执行 qmake”，然后“重新构建”。

当前 Car.pro 实际编译以下文件：
- main.cpp
- networkpage.cpp
- networkpage.h
- resources.qrc

networkpage.cpp 是主要实现文件，包含：
- Qt 界面布局和控件；
- QTcpSocket 网络连接、状态 JSON 解析和命令发送；
- MJPEG 视频接收及 JPEG 拆帧；
- Qt 本地红绿颜色识别与识别框稳定；
- 鼠标、键盘组合驾驶；
- 超声波开关、安全距离配置和距离曲线；
- 断线、失焦、松开按键时的安全停车。

widget.cpp、widget.h、widget.ui、aivisiondialog.cpp 和
aivisiondialog.h 是历史开发文件，为便于追溯一并保留，但当前
Car.pro 没有编译它们，不影响运行。
