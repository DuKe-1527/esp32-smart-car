# 当前主界面只使用 Qt Widgets 和 Network：
# - widgets：窗口、按钮、布局、QPainter、动画等桌面界面功能
# - network：QTcpSocket 控制连接和 QNetworkAccessManager 视频流
QT += widgets network

# 使用 C++17，便于使用标准库中的 clamp、as_const 等能力
CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
    main.cpp \
    networkpage.cpp

# 主界面、视频控件、图表控件和网络控制逻辑都声明在 networkpage.h
HEADERS += \
    networkpage.h

RESOURCES += \
    resources.qrc

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
