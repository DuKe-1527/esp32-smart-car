#include "networkpage.h"

#include <QApplication>
#include <QByteArray>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHideEvent>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPaintEvent>
#include <QPair>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QTcpSocket>
#include <QTimer>
#include <QTime>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

// 本文件是 Qt 主界面的核心，包含四类逻辑：
// 1. 自绘控件：超声波曲线和摄像头画面；
// 2. 界面构建与状态显示；
// 3. TCP 控制、JSON 状态和 MJPEG 视频接收；
// 4. Qt 本地红绿颜色识别与安全输入处理。
namespace {

// JSON 数值统一转成便于显示的字符串：
// 整数不带小数点，小数保留 1 位，缺失值返回空字符串。
QString jsonText(const QJsonValue &value)
{
    if (value.isString()) {
        return value.toString();
    }
    if (value.isDouble()) {
        const double number = value.toDouble();
        if (std::abs(number - std::round(number)) < 0.001) {
            return QString::number(qRound64(number));
        }
        return QString::number(number, 'f', 1);
    }
    return {};
}

QString numberText(double value, int digits = 0)
{
    if (value < 0 || !std::isfinite(value)) {
        return QStringLiteral("--");
    }
    return QString::number(value, 'f', digits);
}

QString formatDuration(qint64 seconds)
{
    seconds = std::max<qint64>(0, seconds);
    if (seconds < 60) {
        return QStringLiteral("%1 秒").arg(seconds);
    }
    if (seconds < 3600) {
        return QStringLiteral("%1 分钟").arg(seconds / 60);
    }
    return QStringLiteral("%1 小时 %2 分")
        .arg(seconds / 3600)
        .arg((seconds % 3600) / 60);
}

QString motionName(const QString &action)
{
    if (action == QStringLiteral("F")) return QStringLiteral("forward");
    if (action == QStringLiteral("B")) return QStringLiteral("backward");
    if (action == QStringLiteral("L")) return QStringLiteral("left");
    if (action == QStringLiteral("R")) return QStringLiteral("right");
    if (action == QStringLiteral("FL")) return QStringLiteral("forward-left");
    if (action == QStringLiteral("FR")) return QStringLiteral("forward-right");
    if (action == QStringLiteral("BL")) return QStringLiteral("backward-left");
    if (action == QStringLiteral("BR")) return QStringLiteral("backward-right");
    return QStringLiteral("stopped");
}

QString motionGlyph(const QString &action)
{
    if (action == QStringLiteral("F")) return QStringLiteral("↑");
    if (action == QStringLiteral("B")) return QStringLiteral("↓");
    if (action == QStringLiteral("L")) return QStringLiteral("↶");
    if (action == QStringLiteral("R")) return QStringLiteral("↷");
    if (action == QStringLiteral("FL")) return QStringLiteral("↖");
    if (action == QStringLiteral("FR")) return QStringLiteral("↗");
    if (action == QStringLiteral("BL")) return QStringLiteral("↙");
    if (action == QStringLiteral("BR")) return QStringLiteral("↘");
    return QStringLiteral("•");
}

QString motionLabel(const QString &action)
{
    if (action == QStringLiteral("F")) return QStringLiteral("前进中");
    if (action == QStringLiteral("B")) return QStringLiteral("后退中");
    if (action == QStringLiteral("L")) return QStringLiteral("左转中");
    if (action == QStringLiteral("R")) return QStringLiteral("右转中");
    if (action == QStringLiteral("FL")) return QStringLiteral("前进左转");
    if (action == QStringLiteral("FR")) return QStringLiteral("前进右转");
    if (action == QStringLiteral("BL")) return QStringLiteral("后退左转");
    if (action == QStringLiteral("BR")) return QStringLiteral("后退右转");
    return QStringLiteral("已停止");
}

QString motionHint(const QString &action)
{
    if (action == QStringLiteral("N")) {
        return QStringLiteral("电机会保持停止，按住方向键开始行驶");
    }
    return QStringLiteral("指令持续发送中，松开按键后立即停车");
}

} // namespace

// 超声波距离曲线控件：
// 不依赖 QtCharts，直接用 QPainter 绘制曲线。setValues() 接收到新数据后，
// 用 16 ms 定时器在旧值和新值之间做插值，使曲线变化看起来更平滑。
class DistanceChart final : public QWidget
{
public:
    explicit DistanceChart(QWidget *parent = nullptr)
        : QWidget(parent)
        , m_timer(new QTimer(this))
    {
        setMinimumHeight(130);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        m_timer->setInterval(16);
        connect(m_timer, &QTimer::timeout, this, [this] {
            // m_progress 从 0 增加到 1，最终显示 m_targetValues。
            m_progress = std::min(1.0, m_progress + 0.12);
            QList<qreal> frame;
            frame.reserve(m_targetValues.size());
            for (qsizetype i = 0; i < m_targetValues.size(); ++i) {
                const qreal oldValue = i < m_previousValues.size()
                                           ? m_previousValues.at(i)
                                           : m_targetValues.at(i);
                frame.append(oldValue + (m_targetValues.at(i) - oldValue) * m_progress);
            }
            m_values = frame;
            update();
            if (m_progress >= 1.0) {
                m_timer->stop();
            }
        });
    }

    void setValues(const QList<qreal> &values)
    {
        // 保存本次动画的起点和终点；空数据表示超声波关闭或还没有读数。
        m_previousValues = m_values;
        m_targetValues = values;
        m_progress = 0.0;
        if (m_targetValues.isEmpty()) {
            m_values.clear();
            m_timer->stop();
            update();
            return;
        }
        m_timer->start();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        // 留出左侧刻度和底部文字空间，再把距离映射到绘图区高度。
        const QRectF plot = rect().adjusted(34, 12, -12, -22);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(246, 251, 249, 208));
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 14, 14);

        painter.setPen(QPen(QColor(169, 207, 198, 52), 1));
        for (int row = 0; row < 4; ++row) {
            const qreal y = plot.top() + plot.height() * row / 3.0;
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }

        if (m_values.isEmpty()) {
            painter.setPen(QColor(95, 129, 125));
            painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("等待超声波测距数据"));
            return;
        }

        // 动态计算纵轴最大值，至少保留 60 cm 的显示范围。
        qreal maximum = 60.0;
        for (const qreal value : std::as_const(m_values)) {
            maximum = std::max(maximum, value);
        }
        maximum *= 1.15;

        QList<QPointF> points;
        points.reserve(m_values.size());
        for (qsizetype i = 0; i < m_values.size(); ++i) {
            const qreal x = m_values.size() == 1
                                ? plot.center().x()
                                : plot.left() + plot.width() * i / (m_values.size() - 1);
            const qreal y = plot.bottom() - plot.height() * m_values.at(i) / maximum;
            points.append(QPointF(x, y));
        }

        // 绘制默认的 15 cm 停车线和 30 cm 预警线，方便观察趋势。
        const auto thresholdY = [&](qreal distance) {
            return plot.bottom() - plot.height() * distance / maximum;
        };
        painter.setPen(QPen(QColor(221, 100, 94, 150), 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), thresholdY(15)),
                         QPointF(plot.right(), thresholdY(15)));
        painter.setPen(QPen(QColor(223, 165, 73, 155), 1, Qt::DashLine));
        painter.drawLine(QPointF(plot.left(), thresholdY(30)),
                         QPointF(plot.right(), thresholdY(30)));

        QPainterPath area(points.first());
        // 曲线下方使用渐变填充，增强数据变化的视觉可见性。
        for (qsizetype i = 1; i < points.size(); ++i) {
            area.lineTo(points.at(i));
        }
        area.lineTo(points.last().x(), plot.bottom());
        area.lineTo(points.first().x(), plot.bottom());
        area.closeSubpath();

        QLinearGradient areaGradient(plot.topLeft(), plot.bottomLeft());
        areaGradient.setColorAt(0.0, QColor(68, 191, 171, 112));
        areaGradient.setColorAt(1.0, QColor(68, 191, 171, 4));
        painter.fillPath(area, areaGradient);

        QPainterPath line(points.first());
        for (qsizetype i = 1; i < points.size(); ++i) {
            line.lineTo(points.at(i));
        }
        painter.setPen(QPen(QColor(20, 159, 141), 2.4, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(line);

        painter.setBrush(QColor(22, 128, 110));
        painter.setPen(QPen(QColor(246, 251, 249), 2));
        painter.drawEllipse(points.last(), 4.5, 4.5);

        painter.setPen(QColor(95, 129, 125));
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 8));
        painter.drawText(QRectF(4, plot.top() - 6, 28, 16),
                         Qt::AlignRight, QString::number(qRound(maximum)));
        painter.drawText(QRectF(plot.left() + 5, thresholdY(30) - 16, 64, 14),
                         Qt::AlignLeft, QStringLiteral("预警 30 cm"));
        painter.drawText(QRectF(plot.left() + 5, thresholdY(15) - 16, 64, 14),
                         Qt::AlignLeft, QStringLiteral("停止 15 cm"));
        painter.drawText(QRectF(plot.left(), plot.bottom() + 4, 60, 16),
                         Qt::AlignLeft, QStringLiteral("较早"));
        painter.drawText(QRectF(plot.right() - 40, plot.bottom() + 4, 40, 16),
                         Qt::AlignRight, QStringLiteral("现在"));
    }

private:
    QTimer *m_timer = nullptr;
    QList<qreal> m_values;
    QList<qreal> m_previousValues;
    QList<qreal> m_targetValues;
    qreal m_progress = 1.0;
};

// 视频显示控件：
// 根据窗口大小等比例绘制 QImage，并把视频原始坐标中的检测框
// 映射到当前显示区域，避免缩放后识别框与图像错位。
class CameraView final : public QWidget
{
public:
    explicit CameraView(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(320, 185);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setImage(const QImage &image)
    {
        // 收到新帧时清除占位提示并请求重绘。
        m_image = image;
        m_message.clear();
        update();
    }

    void setDetection(const QString &color, const QRect &box,
                      const QSize &frameSize)
    {
        // box 使用摄像头原始帧坐标；frameSize 用于计算显示缩放比例。
        m_detectionColor = color;
        m_detectionBox = box;
        m_detectionFrameSize = frameSize;
        update();
    }

    void clearDetection()
    {
        m_detectionColor.clear();
        m_detectionBox = QRect();
        update();
    }

    void setMessage(const QString &message)
    {
        m_message = message;
        m_image = QImage();
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

        const QRectF frame = rect().adjusted(1, 1, -1, -1);
        painter.setPen(QPen(QColor(0xcf, 0xe7, 0xdf), 1));
        painter.setBrush(QColor(0xf6, 0xfb, 0xf9));
        painter.drawRoundedRect(frame, 15, 15);

        if (!m_image.isNull()) {
            const QRectF target = frame.adjusted(6, 6, -6, -6);
            QSize scaled = m_image.size();
            // KeepAspectRatio 防止视频被拉伸，剩余区域由背景色填充。
            scaled.scale(target.size().toSize(), Qt::KeepAspectRatio);
            const QRectF imageRect(
                target.center().x() - scaled.width() / 2.0,
                target.center().y() - scaled.height() / 2.0,
                scaled.width(), scaled.height());

            QPainterPath clip;
            clip.addRoundedRect(target, 11, 11);
            painter.save();
            painter.setClipPath(clip);
            painter.fillRect(target, QColor(0xea, 0xf9, 0xf4));
            painter.drawImage(imageRect, m_image);
            painter.restore();

            if (!m_detectionColor.isEmpty()
                && m_detectionFrameSize.width() > 0
                && m_detectionFrameSize.height() > 0) {
                // 将原始画面坐标按显示比例换算为屏幕坐标。
                const qreal scaleX = imageRect.width()
                                     / m_detectionFrameSize.width();
                const qreal scaleY = imageRect.height()
                                     / m_detectionFrameSize.height();
                const QRectF detected(
                    imageRect.left() + m_detectionBox.x() * scaleX,
                    imageRect.top() + m_detectionBox.y() * scaleY,
                    m_detectionBox.width() * scaleX,
                    m_detectionBox.height() * scaleY);
                const QColor color = m_detectionColor == QStringLiteral("red")
                                         ? QColor(230, 72, 74)
                                         : QColor(55, 174, 109);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(color, 3));
                painter.drawRoundedRect(detected, 5, 5);
                painter.setPen(Qt::white);
                painter.setBrush(color);
                painter.drawRoundedRect(
                    QRectF(detected.left(), detected.top() - 20,
                           std::max<qreal>(70, detected.width() * 0.55), 20),
                    4, 4);
                painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9,
                                      QFont::DemiBold));
                // 在识别框上方绘制颜色标签，便于直观确认目标类型。
                painter.drawText(
                    QRectF(detected.left() + 5, detected.top() - 20,
                           std::max<qreal>(60, detected.width() * 0.5), 20),
                    Qt::AlignVCenter | Qt::AlignLeft,
                    m_detectionColor == QStringLiteral("red")
                        ? QStringLiteral("红色目标")
                        : QStringLiteral("绿色目标"));
            }
            return;
        }

        painter.setPen(QColor(0x66, 0x8b, 0x86));
        painter.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
        painter.drawText(frame.adjusted(24, 20, -24, -20),
                         Qt::AlignCenter | Qt::TextWordWrap,
                         m_message.isEmpty()
                             ? QStringLiteral("连接设备后加载摄像头画面")
                             : m_message);
    }

private:
    QImage m_image;
    QString m_message;
    QString m_detectionColor;
    QRect m_detectionBox;
    QSize m_detectionFrameSize;
};

// 构造主界面并建立三条并行数据通道：
// QTcpSocket 控制通道、QNetworkAccessManager 视频通道和两个 UI 定时器。
NetworkPage::NetworkPage(QWidget *parent)
    : QWidget(parent)
    , m_socket(new QTcpSocket(this))
    , m_cameraNetwork(new QNetworkAccessManager(this))
    , m_refreshTimer(new QTimer(this))
    , m_heartbeatTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("NetworkPage"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(980, 660);
    buildInterface();

    // 顶层按钮和网络地址栏的事件绑定。
    connect(m_connectButton, &QPushButton::clicked,
            this, &NetworkPage::connectToDevice);
    connect(m_refreshButton, &QPushButton::clicked,
            this, &NetworkPage::refreshWeather);
    connect(m_cameraButton, &QPushButton::clicked,
            this, &NetworkPage::toggleCameraStream);
    connect(m_ipEdit, &QLineEdit::returnPressed, this, [this] {
        if (!m_connectionRequested) {
            connectToDevice();
        }
    });
    connect(m_socket, &QTcpSocket::readyRead,
            this, &NetworkPage::handleSocketData);
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        // 连接成功后开启低延迟选项，减少小控制包被延迟发送的概率。
        m_online = true;
        m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        updateConnection(true, QStringLiteral("TCP 长连接已建立"));
        appendLog(QStringLiteral("设备连接成功：%1:%2")
                      .arg(m_tcpHost)
                      .arg(m_tcpPort));
        setFocus(Qt::OtherFocusReason);
        sendSocketLine("DATA");
        m_socket->flush();
        updateVisionColorFilter();
        startCameraStream();
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        // TCP 断开时必须同时停小车和停视频，不能保留最后一次动作。
        m_online = false;
        stopCar();
        stopCameraStream(QStringLiteral("网络断开，视觉画面已停止"));
        updateConnection(false, m_connectionRequested
                                    ? QStringLiteral("连接已断开，正在自动重连")
                                    : QStringLiteral("TCP 网络控制已断开"));
        if (m_connectionRequested) {
            QTimer::singleShot(2000, this, [this] {
                if (m_connectionRequested
                    && m_socket->state() == QAbstractSocket::UnconnectedState) {
                    m_socket->connectToHost(m_tcpHost, m_tcpPort);
                }
            });
        }
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError error) {
        // 远端主动关闭由 disconnected 处理，这里避免重复记录日志。
        if (error == QAbstractSocket::RemoteHostClosedError) {
            return;
        }
        appendLog(QStringLiteral("TCP 错误：%1").arg(m_socket->errorString()));
        if (m_connectionRequested
            && m_socket->state() == QAbstractSocket::UnconnectedState) {
            updateConnection(false, QStringLiteral("连接失败，2 秒后自动重试"));
            QTimer::singleShot(2000, this, [this] {
                if (m_connectionRequested
                    && m_socket->state() == QAbstractSocket::UnconnectedState) {
                    m_socket->connectToHost(m_tcpHost, m_tcpPort);
                }
            });
        }
    });

    // 天气和完整状态每 3 秒主动拉取一次；车辆运动另有 100 ms 心跳。
    m_refreshTimer->setInterval(3000);
    connect(m_refreshTimer, &QTimer::timeout, this, &NetworkPage::fetchData);

    // 只有仍有活动输入且车辆不是停止状态时才重发动作，
    // 这样 ESP32 的超时停车不会把长按操作误判为断线。
    m_heartbeatTimer->setInterval(100);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this] {
        if (isVisible() && m_online
            && !m_activeCarInputs.isEmpty()
            && m_currentAction != QStringLiteral("N")) {
            sendCarAction(m_currentAction);
        }
    });

    qApp->installEventFilter(this);
    updateConnection(false, QStringLiteral("连接 ESP32-CAM-9 热点后开始"));
    updateMotion(QStringLiteral("N"));
    appendLog(QStringLiteral("网络控制台已就绪，等待连接 ESP32"));
}

NetworkPage::~NetworkPage()
{
    // 销毁前先停止定时器、释放视频请求，并向车辆发送最后一条 N。
    qApp->removeEventFilter(this);
    m_connectionRequested = false;
    m_refreshTimer->stop();
    m_heartbeatTimer->stop();
    m_activeCarInputs.clear();
    stopCameraStream();
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        m_socket->write("N\n");
        m_socket->flush();
        m_socket->waitForBytesWritten(120);
        m_socket->abort();
    }
}

void NetworkPage::buildInterface()
{
    // 样式表集中定义页面、卡片、按钮、输入框和状态颜色。
    // 使用 objectName / 动态属性区分不同控件，避免为每个控件重复样式。
    setStyleSheet(QStringLiteral(R"QSS(
        QWidget#NetworkPage {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #fff7e8,
                stop: 0.48 #f0fbf7,
                stop: 1 #e4f5f4
            );
            color: #19363c;
            font-family: "Microsoft YaHei UI";
        }
        QLabel {
            background: transparent;
            color: #19363c;
        }
        QLabel#netBrandIcon {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #ffffff, stop: 1 #e3f4ef
            );
            border: 1px solid #f4d79b;
            border-radius: 17px;
            font-family: "Segoe UI Emoji";
            font-size: 26px;
        }
        QLabel#netEyebrow {
            color: #e39a26;
            font-size: 10px;
            font-weight: 700;
            letter-spacing: 2px;
        }
        QLabel#netTitle {
            color: #123b43;
            font-size: 29px;
            font-weight: 700;
        }
        QLabel#netSubtitle {
            color: #6f8d8e;
            font-size: 13px;
        }
        QLabel#netBadge {
            background: #fff7e6;
            border: 1px solid #f0d39b;
            border-radius: 15px;
            color: #b77a16;
            font-size: 11px;
            font-weight: 700;
            padding: 8px 14px;
        }
        QLabel#netBadge[online="true"] {
            background: #def5ed;
            border-color: #73cbb6;
            color: #16806e;
        }
        QFrame#networkBar {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 0,
                stop: 0 #fffdf9, stop: 1 #eaf9f4
            );
            border: 1px solid #cfe7df;
            border-radius: 17px;
        }
        QLabel#fieldCaption {
            color: #698987;
            font-size: 10px;
            font-weight: 600;
        }
        QLineEdit {
            background: #ffffff;
            border: 1px solid #c9e0da;
            border-radius: 9px;
            color: #21464c;
            font-family: Consolas;
            font-size: 12px;
            padding: 6px 10px;
            selection-background-color: #c9eee5;
        }
        QLineEdit:focus {
            border-color: #25a996;
        }
        QComboBox#visionColorCombo {
            background: #ffffff;
            border: 1px solid #c9e0da;
            border-radius: 8px;
            color: #21464c;
            font-size: 10px;
            padding: 3px 20px 3px 8px;
        }
        QComboBox#visionColorCombo:hover {
            border-color: #62c5b4;
        }
        QComboBox#visionColorCombo::drop-down {
            border: none;
            width: 20px;
        }
        QComboBox#visionColorCombo QAbstractItemView {
            background: #ffffff;
            border: 1px solid #c9e0da;
            color: #21464c;
            selection-background-color: #dff5ed;
            selection-color: #126d60;
        }
        QSpinBox#distanceLimitSpin {
            background: #ffffff;
            border: 1px solid #c9e0da;
            border-radius: 7px;
            color: #21464c;
            font-family: Consolas;
            font-size: 10px;
            padding: 2px 5px;
        }
        QSpinBox#distanceLimitSpin:focus {
            border-color: #25a996;
        }
        QSpinBox#distanceLimitSpin::up-button,
        QSpinBox#distanceLimitSpin::down-button {
            width: 11px;
            border: none;
            background: #e8f5f1;
        }
        QFrame#ultrasonicLimitEditor {
            background: #f2faf7;
            border: 1px solid #d3e8e1;
            border-radius: 9px;
        }
        QPushButton#applyUltrasonicLimits {
            font-size: 9px;
            padding: 2px;
        }
        QPushButton[kind="connect"] {
            background: #149f8d;
            border: 1px solid #0f8f80;
            border-radius: 9px;
            color: #ffffff;
            font-size: 11px;
            font-weight: 700;
            padding: 7px 13px;
        }
        QPushButton[kind="connect"]:hover {
            background: #19b09c;
        }
        QPushButton[kind="refresh"] {
            background: #f7fbfa;
            border: 1px solid #c8e1db;
            border-radius: 9px;
            color: #47766f;
            font-size: 11px;
            font-weight: 600;
            padding: 7px 12px;
        }
        QPushButton[kind="refresh"]:hover {
            background: #e9f8f3;
            border-color: #7ccdbd;
            color: #225d55;
        }
        QPushButton[kind="refresh"]:checked {
            background: #ddf3ed;
            border-color: #78c8b9;
            color: #146f66;
        }
        QPushButton[kind="approach"] {
            background: #149f8d;
            border: 1px solid #0f8f80;
            border-radius: 9px;
            color: #ffffff;
            font-size: 11px;
            font-weight: 700;
            padding: 5px 12px;
        }
        QPushButton[kind="approach"]:hover {
            background: #19b09c;
        }
        QPushButton[kind="approach"]:disabled {
            background: #e8efed;
            border-color: #d4dfdc;
            color: #a5b4b0;
        }
        QLabel#networkHint {
            color: #759391;
            font-size: 11px;
        }
        QFrame[card="true"] {
            background: rgba(255, 255, 255, 240);
            border: 1px solid #d7e9e4;
            border-radius: 21px;
        }
        QFrame#carPanel {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #fffdf9, stop: 1 #eaf9f4
            );
            border: 1px solid #cfe7df;
            border-radius: 23px;
        }
        QLabel#cardCaption {
            color: #668b86;
            font-size: 10px;
            font-weight: 700;
            letter-spacing: 1px;
        }
        QLabel#cardIcon {
            font-family: "Segoe UI Emoji";
            font-size: 20px;
        }
        QLabel#panelTitle {
            color: #153e45;
            font-size: 19px;
            font-weight: 700;
        }
        QLabel#panelSubtitle {
            color: #779491;
            font-size: 11px;
        }
        QLabel#netCarMotion {
            color: #149f8d;
            font-size: 54px;
            font-weight: 700;
        }
        QLabel#netCarMotion[motion="stopped"] {
            color: #bd4d57;
        }
        QLabel#netStatusValue {
            color: #16434a;
            font-size: 25px;
            font-weight: 700;
        }
        QLabel#netStatusHint {
            color: #718f8c;
            font-size: 11px;
        }
        QPushButton[kind="netAction"] {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 0, y2: 1,
                stop: 0 #ffffff, stop: 1 #edf8f5
            );
            border: 1px solid #c5e0d9;
            border-radius: 14px;
            color: #24525a;
            font-size: 13px;
            font-weight: 700;
            padding: 7px;
        }
        QPushButton[kind="netAction"]:hover {
            background: #e5f7f2;
            border-color: #4bc3b0;
        }
        QPushButton[kind="netAction"]:checked {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #20b9a4, stop: 1 #52d2bd
            );
            border-color: #159f8d;
            color: #ffffff;
        }
        QPushButton[kind="netStop"] {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 0, y2: 1,
                stop: 0 #fff7f5, stop: 1 #ffebe9
            );
            border: 1px solid #efc7c5;
            border-radius: 14px;
            color: #bd4d57;
            font-size: 13px;
            font-weight: 700;
            padding: 7px;
        }
        QPushButton[kind="netStop"]:hover {
            background: #ffe3df;
            border-color: #ee8e92;
        }
        QPushButton[kind="netStop"]:checked {
            background: #ed606b;
            border-color: #dc4957;
            color: #ffffff;
        }
        QPushButton[kind="netAction"]:disabled,
        QPushButton[kind="netStop"]:disabled {
            background: #f2f5f4;
            border-color: #dce6e2;
            color: #a8b8b3;
        }
        QLabel#keyboardHint {
            color: #6f8f8a;
            font-size: 10px;
        }
        QPlainTextEdit#networkLog {
            background: #f6fbf9;
            border: 1px solid #d5e8e2;
            border-radius: 12px;
            color: #376a70;
            font-family: Consolas;
            font-size: 10px;
            padding: 9px;
            selection-background-color: #c9eee5;
        }
        QLabel#temperatureValue {
            color: #16806e;
            font-size: 40px;
            font-weight: 700;
        }
        QLabel#weatherState {
            color: #16434a;
            font-size: 17px;
            font-weight: 700;
        }
        QLabel#weatherMood {
            color: #668783;
            font-size: 10px;
        }
        QLabel#weatherMeta {
            color: #759391;
            font-size: 10px;
        }
        QLabel#weatherError {
            color: #bd4d57;
            font-size: 10px;
        }
        QLabel#visionResult {
            color: #255a5f;
            font-size: 10px;
            font-weight: 600;
        }
        QLabel#obstacleStatus {
            color: #61847f;
            font-size: 10px;
            font-weight: 600;
        }
        QLabel#obstacleStatus[warning="true"] {
            color: #bd4d57;
        }
        QLabel#distanceValue {
            color: #14927f;
            font-size: 34px;
            font-weight: 700;
        }
        QLabel#distanceState {
            color: #24525a;
            font-size: 14px;
            font-weight: 600;
        }
        QLabel#distanceState[warning="true"] {
            color: #bd4d57;
        }
        QLabel#metricText {
            color: #718f8c;
            font-size: 10px;
        }
        QLabel#networkDetails,
        QLabel#deviceDetails {
            color: #5f7f7c;
            font-size: 11px;
            line-height: 1.45;
        }
        QFrame#weatherCard[weatherTheme="clear"] {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #eef9f4, stop: 1 #dcf0e8
            );
            border-color: #afd9ce;
        }
        QFrame#weatherCard[weatherTheme="cloudy"] {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #f2f8f7, stop: 1 #dfebea
            );
            border-color: #c5d9d5;
        }
        QFrame#weatherCard[weatherTheme="rain"] {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #e8f0f5, stop: 1 #d7e4ed
            );
            border-color: #b8cddd;
        }
        QFrame#weatherCard[weatherTheme="thunder"] {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #eceae5, stop: 1 #dedde4
            );
            border-color: #c6c1d3;
        }
        QFrame#weatherCard[weatherTheme="snow"] {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #f9fcfb, stop: 1 #e6f1ef
            );
            border-color: #c7ded8;
        }
        QFrame#weatherCard[weatherTheme="fog"] {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #f0f4f2, stop: 1 #e2e9e5
            );
            border-color: #c4d2cb;
        }
        QToolTip {
            background: #173f46;
            border: 1px solid #568a87;
            color: #ffffff;
            padding: 6px 9px;
        }
        QScrollBar:vertical {
            background: transparent;
            width: 7px;
        }
        QScrollBar::handle:vertical {
            background: #b7d8cf;
            border-radius: 3px;
            min-height: 24px;
        }
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical {
            height: 0;
        }
    )QSS"));

    // 顶部区域：页面标题、连接状态和 TCP 地址栏。
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(34, 30, 34, 20);
    root->setSpacing(14);

    auto *header = new QHBoxLayout;
    header->setSpacing(16);
    auto *brandIcon = new QLabel(QStringLiteral("🛰️"));
    brandIcon->setObjectName(QStringLiteral("netBrandIcon"));
    brandIcon->setFixedSize(54, 54);
    brandIcon->setAlignment(Qt::AlignCenter);
    header->addWidget(brandIcon);

    auto *titleLayout = new QVBoxLayout;
    titleLayout->setSpacing(4);
    auto *eyebrow = new QLabel(QStringLiteral("AI VISION & NETWORK CONSOLE"));
    eyebrow->setObjectName(QStringLiteral("netEyebrow"));
    auto *title = new QLabel(QStringLiteral("智能小车控制台"));
    title->setObjectName(QStringLiteral("netTitle"));
    auto *subtitle = new QLabel(
        QStringLiteral("ESP32 TCP 网络驾驶 · 视觉识别 · 超声波避障 · 安全停车"));
    subtitle->setObjectName(QStringLiteral("netSubtitle"));
    titleLayout->addWidget(eyebrow);
    titleLayout->addWidget(title);
    titleLayout->addWidget(subtitle);
    header->addLayout(titleLayout);
    header->addStretch();

    m_connectionState = new QLabel(QStringLiteral("●  网络离线"));
    m_connectionState->setObjectName(QStringLiteral("netBadge"));
    m_connectionState->setAlignment(Qt::AlignCenter);
    m_connectionState->setProperty("online", false);
    m_connectionState->setMinimumWidth(118);
    header->addWidget(m_connectionState);

    root->addLayout(header);

    auto *networkBar = new QFrame;
    networkBar->setObjectName(QStringLiteral("networkBar"));
    auto *networkBarLayout = new QHBoxLayout(networkBar);
    networkBarLayout->setContentsMargins(16, 10, 16, 10);
    networkBarLayout->setSpacing(10);
    auto *fieldCaption = new QLabel(QStringLiteral("ESP32 TCP"));
    fieldCaption->setObjectName(QStringLiteral("fieldCaption"));
    networkBarLayout->addWidget(fieldCaption);
    m_ipEdit = new QLineEdit(QStringLiteral("192.168.4.1:9000"));
    m_ipEdit->setObjectName(QStringLiteral("networkAddressEdit"));
    m_ipEdit->setPlaceholderText(QStringLiteral("IP:端口，例如 192.168.4.1:9000"));
    m_ipEdit->setMinimumWidth(175);
    m_ipEdit->setMaximumWidth(210);
    m_ipEdit->setMinimumHeight(30);
    networkBarLayout->addWidget(m_ipEdit);
    m_connectButton = new QPushButton(QStringLiteral("连接设备"));
    m_connectButton->setObjectName(QStringLiteral("networkConnectButton"));
    m_connectButton->setProperty("kind", QStringLiteral("connect"));
    m_connectButton->setMinimumHeight(32);
    networkBarLayout->addWidget(m_connectButton);
    m_refreshButton = new QPushButton(QStringLiteral("更新天气"));
    m_refreshButton->setProperty("kind", QStringLiteral("refresh"));
    m_refreshButton->setMinimumHeight(32);
    networkBarLayout->addWidget(m_refreshButton);
    m_networkHint = new QLabel(QStringLiteral("连接 ESP32-CAM-9 热点后开始"));
    m_networkHint->setObjectName(QStringLiteral("networkHint"));
    networkBarLayout->addStretch();
    networkBarLayout->addWidget(m_networkHint);
    root->addWidget(networkBar);

    // 主体分为左侧驾驶面板和右侧监控面板。
    auto *body = new QHBoxLayout;
    body->setSpacing(16);

    // 左侧驾驶面板：车辆状态、方向按钮、快捷键提示和操作日志。
    auto *carPanel = new QFrame;
    carPanel->setObjectName(QStringLiteral("carPanel"));
    carPanel->setMinimumWidth(326);
    carPanel->setMaximumWidth(360);
    auto *carLayout = new QVBoxLayout(carPanel);
    carLayout->setContentsMargins(20, 18, 20, 18);
    carLayout->setSpacing(9);

    auto *carHeader = new QHBoxLayout;
    auto *carTitleLayout = new QVBoxLayout;
    carTitleLayout->setSpacing(2);
    auto *carTitle = new QLabel(QStringLiteral("车辆驾驶"));
    carTitle->setObjectName(QStringLiteral("panelTitle"));
    auto *carSubtitle = new QLabel(QStringLiteral("按住执行 · 松开即停 · 支持组合按键"));
    carSubtitle->setObjectName(QStringLiteral("panelSubtitle"));
    carTitleLayout->addWidget(carTitle);
    carTitleLayout->addWidget(carSubtitle);
    carHeader->addLayout(carTitleLayout);
    carHeader->addStretch();
    auto *manualBadge = new QLabel(QStringLiteral("MANUAL"));
    manualBadge->setObjectName(QStringLiteral("cardCaption"));
    carHeader->addWidget(manualBadge);
    carLayout->addLayout(carHeader);

    m_carMotion = new QLabel(QStringLiteral("•"));
    m_carMotion->setObjectName(QStringLiteral("netCarMotion"));
    m_carMotion->setAlignment(Qt::AlignCenter);
    m_carMotion->setMinimumHeight(76);
    carLayout->addWidget(m_carMotion);
    m_statusValue = new QLabel(QStringLiteral("已停止"));
    m_statusValue->setObjectName(QStringLiteral("netStatusValue"));
    m_statusValue->setAlignment(Qt::AlignCenter);
    carLayout->addWidget(m_statusValue);
    m_statusHint = new QLabel(QStringLiteral("按住方向键开始行驶"));
    m_statusHint->setObjectName(QStringLiteral("netStatusHint"));
    m_statusHint->setAlignment(Qt::AlignCenter);
    m_statusHint->setWordWrap(true);
    carLayout->addWidget(m_statusHint);

    auto *pad = new QGridLayout;
    pad->setHorizontalSpacing(8);
    pad->setVerticalSpacing(8);
    pad->setColumnStretch(0, 1);
    pad->setColumnStretch(1, 1);
    pad->setColumnStretch(2, 1);
    auto *forward = createControlButton(QStringLiteral("↑\n前进"),
                                        QStringLiteral("F"), QStringLiteral("W"));
    auto *left = createControlButton(QStringLiteral("←\n左转"),
                                     QStringLiteral("L"), QStringLiteral("A"));
    auto *stop = createControlButton(QStringLiteral("■\n停止"),
                                     QStringLiteral("N"), QStringLiteral("SPACE"));
    auto *right = createControlButton(QStringLiteral("→\n右转"),
                                      QStringLiteral("R"), QStringLiteral("D"));
    auto *backward = createControlButton(QStringLiteral("↓\n后退"),
                                         QStringLiteral("B"), QStringLiteral("S"));
    pad->addWidget(forward, 0, 1);
    pad->addWidget(left, 1, 0);
    pad->addWidget(stop, 1, 1);
    pad->addWidget(right, 1, 2);
    pad->addWidget(backward, 2, 1);
    carLayout->addLayout(pad);

    auto *keyboardHint = new QLabel(
        QStringLiteral("W/A/S/D 驾驶  ·  W+A 前进左转  ·  松开任一键自动恢复"));
    keyboardHint->setObjectName(QStringLiteral("keyboardHint"));
    keyboardHint->setAlignment(Qt::AlignCenter);
    keyboardHint->setWordWrap(true);
    carLayout->addWidget(keyboardHint);

    auto *logCaption = new QLabel(QStringLiteral("操作记录"));
    logCaption->setObjectName(QStringLiteral("cardCaption"));
    carLayout->addWidget(logCaption);
    m_log = new QPlainTextEdit;
    m_log->setObjectName(QStringLiteral("networkLog"));
    m_log->setReadOnly(true);
    m_log->setFocusPolicy(Qt::NoFocus);
    m_log->setMinimumHeight(86);
    m_log->setMaximumBlockCount(100);
    carLayout->addWidget(m_log, 1);
    body->addWidget(carPanel);

    // 右侧监控区域：摄像头、天气、超声波、网络和设备信息。
    auto *rightColumn = new QVBoxLayout;
    rightColumn->setSpacing(12);
    auto *topRow = new QHBoxLayout;
    topRow->setSpacing(12);
    auto *sideMetricsColumn = new QVBoxLayout;
    sideMetricsColumn->setSpacing(10);

    // 创建统一风格的卡片：标题、说明、图标和内容布局由调用方继续填充。
    auto makeCard = [](const QString &title, const QString &icon,
                       const QString &caption, const QString &objectName) {
        auto *card = new QFrame;
        card->setProperty("card", true);
        card->setObjectName(objectName);
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(17, 14, 17, 14);
        layout->setSpacing(5);
        auto *head = new QHBoxLayout;
        auto *label = new QLabel(title);
        label->setObjectName(QStringLiteral("cardCaption"));
        auto *iconLabel = new QLabel(icon);
        iconLabel->setObjectName(QStringLiteral("cardIcon"));
        head->addWidget(label);
        head->addStretch();
        head->addWidget(iconLabel);
        layout->addLayout(head);
        auto *captionLabel = new QLabel(caption);
        captionLabel->setObjectName(QStringLiteral("weatherMeta"));
        layout->addWidget(captionLabel);
        return qMakePair(card, layout);
    };

    // 视觉卡片：视频画面、颜色过滤下拉框、识别结果和“靠近色块”按钮。
    auto *cameraCard = new QFrame;
    cameraCard->setProperty("card", true);
    cameraCard->setObjectName(QStringLiteral("cameraCard"));
    auto *cameraLayout = new QVBoxLayout(cameraCard);
    cameraLayout->setContentsMargins(15, 12, 15, 13);
    cameraLayout->setSpacing(7);
    auto *cameraHeader = new QHBoxLayout;
    auto *cameraTitle = new QLabel(QStringLiteral("视觉画面"));
    cameraTitle->setObjectName(QStringLiteral("cardCaption"));
    m_cameraStatus = new QLabel(QStringLiteral("等待连接"));
    m_cameraStatus->setObjectName(QStringLiteral("weatherMeta"));
    m_cameraButton = new QPushButton(QStringLiteral("开启视觉"));
    m_cameraButton->setProperty("kind", QStringLiteral("refresh"));
    m_cameraButton->setCheckable(true);
    m_cameraButton->setEnabled(false);
    m_cameraButton->setFixedHeight(28);
    m_visionColorCombo = new QComboBox;
    m_visionColorCombo->setObjectName(QStringLiteral("visionColorCombo"));
    m_visionColorCombo->addItem(QStringLiteral("自动识别"), QStringLiteral("auto"));
    m_visionColorCombo->addItem(QStringLiteral("只找红色"), QStringLiteral("red"));
    m_visionColorCombo->addItem(QStringLiteral("只找绿色"), QStringLiteral("green"));
    m_visionColorCombo->setFixedWidth(96);
    m_visionColorCombo->setFixedHeight(28);
    cameraHeader->addWidget(cameraTitle);
    cameraHeader->addWidget(m_cameraStatus);
    cameraHeader->addStretch();
    cameraHeader->addWidget(m_visionColorCombo);
    cameraHeader->addWidget(m_cameraButton);
    cameraLayout->addLayout(cameraHeader);
    m_cameraView = new CameraView;
    m_cameraView->setMessage(QStringLiteral("连接 ESP32 后自动加载摄像头画面"));
    cameraLayout->addWidget(m_cameraView, 1);
    auto *visionTelemetry = new QHBoxLayout;
    visionTelemetry->setSpacing(8);
    m_visionResult = new QLabel(QStringLiteral("识别：等待画面"));
    m_visionResult->setObjectName(QStringLiteral("visionResult"));
    m_obstacleStatus = new QLabel(QStringLiteral("距离：已关闭"));
    m_obstacleStatus->setObjectName(QStringLiteral("obstacleStatus"));
    m_obstacleStatus->setProperty("warning", false);
    m_approachButton = new QPushButton(QStringLiteral("靠近色块"));
    m_approachButton->setObjectName(QStringLiteral("approachButton"));
    m_approachButton->setProperty("kind", QStringLiteral("approach"));
    m_approachButton->setFocusPolicy(Qt::NoFocus);
    m_approachButton->setEnabled(false);
    m_approachButton->setFixedHeight(28);
    visionTelemetry->addWidget(m_visionResult, 1);
    visionTelemetry->addWidget(m_obstacleStatus);
    visionTelemetry->addWidget(m_approachButton);
    cameraLayout->addLayout(visionTelemetry);
    connect(m_approachButton, &QPushButton::clicked,
            this, &NetworkPage::startColorApproach);
    connect(m_visionColorCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int index) {
        // 切换颜色过滤时清空旧识别框和确认状态，避免旧颜色残留。
        m_visionColorFilter =
            m_visionColorCombo->itemData(index).toString();
        m_localVisionDetected = false;
        m_localVisionPendingColor.clear();
        m_localVisionPendingColorFrames = 0;
        m_localVisionPendingBox = QRect();
        m_localVisionPendingBoxFrames = 0;
        m_localVisionLastSeenAt = 0;
        m_cameraView->clearDetection();
        updateVisionColorFilter();
        setFocus(Qt::OtherFocusReason);
    });
    topRow->addWidget(cameraCard, 3);

    // 天气卡片：文本、图标和温度由 updateData() 从 ESP32 JSON 更新。
    const auto weatherParts =
        makeCard(QStringLiteral("室外天气"), QStringLiteral("🌤️"),
                 QStringLiteral("心知天气 · 南通"), QStringLiteral("weatherCard"));
    m_weatherCard = weatherParts.first;
    auto *weatherLayout = weatherParts.second;
    m_weatherCard->setProperty("weatherTheme", QStringLiteral("cloudy"));
    auto *weatherMain = new QHBoxLayout;
    auto *weatherLeft = new QVBoxLayout;
    m_weatherIcon = new QLabel(QStringLiteral("🌤️"));
    m_weatherIcon->setAlignment(Qt::AlignLeft);
    m_weatherIcon->setMinimumHeight(42);
    m_weatherIcon->setStyleSheet(QStringLiteral(
        "font-family:'Segoe UI Emoji'; font-size:31px; background:transparent;"));
    m_weatherText = new QLabel(QStringLiteral("等待同步"));
    m_weatherText->setObjectName(QStringLiteral("weatherState"));
    m_weatherMood = new QLabel(QStringLiteral("连接设备后自动获取天气"));
    m_weatherMood->setObjectName(QStringLiteral("weatherMood"));
    m_weatherMood->setWordWrap(true);
    weatherLeft->addWidget(m_weatherIcon);
    weatherLeft->addWidget(m_weatherText);
    weatherLeft->addWidget(m_weatherMood);
    weatherMain->addLayout(weatherLeft, 1);
    auto *temperatureColumn = new QVBoxLayout;
    temperatureColumn->setAlignment(Qt::AlignTop | Qt::AlignRight);
    m_temperature = new QLabel(QStringLiteral("--°C"));
    m_temperature->setObjectName(QStringLiteral("temperatureValue"));
    m_temperature->setAlignment(Qt::AlignRight);
    temperatureColumn->addWidget(m_temperature);
    m_weatherObserved = new QLabel(QStringLiteral("尚未获取"));
    m_weatherObserved->setObjectName(QStringLiteral("weatherMeta"));
    m_weatherObserved->setAlignment(Qt::AlignRight);
    m_weatherObserved->setWordWrap(true);
    temperatureColumn->addWidget(m_weatherObserved);
    weatherMain->addLayout(temperatureColumn);
    weatherLayout->addLayout(weatherMain, 1);
    m_weatherError = new QLabel;
    m_weatherError->setObjectName(QStringLiteral("weatherError"));
    m_weatherError->setWordWrap(true);
    m_weatherError->hide();
    weatherLayout->addWidget(m_weatherError);
    sideMetricsColumn->addWidget(m_weatherCard, 1);

    // 超声波卡片：实时距离、平均值、范围、趋势和安全距离编辑区。
    auto *ultrasonicCard = new QFrame;
    ultrasonicCard->setProperty("card", true);
    ultrasonicCard->setObjectName(QStringLiteral("ultrasonicCard"));
    auto *ultrasonicLayout = new QVBoxLayout(ultrasonicCard);
    ultrasonicLayout->setContentsMargins(15, 11, 15, 13);
    ultrasonicLayout->setSpacing(5);

    // 右上角编辑区只申请最小宽度，避免窗口缩放或高 DPI 时两个输入框重叠。
    auto *ultrasonicHeader = new QHBoxLayout;
    ultrasonicHeader->setSpacing(6);
    auto *ultrasonicTitleColumn = new QVBoxLayout;
    ultrasonicTitleColumn->setSpacing(2);
    auto *ultrasonicTitle =
        new QLabel(QStringLiteral("📡 超声波避障"));
    ultrasonicTitle->setObjectName(QStringLiteral("cardCaption"));
    auto *ultrasonicCaption =
        new QLabel(QStringLiteral("HC-SR04 · GPIO16 / GPIO2"));
    ultrasonicCaption->setObjectName(QStringLiteral("weatherMeta"));
    ultrasonicTitleColumn->addWidget(ultrasonicTitle);
    ultrasonicTitleColumn->addWidget(ultrasonicCaption);
    ultrasonicHeader->addLayout(ultrasonicTitleColumn, 1);

    auto *limitEditor = new QFrame;
    limitEditor->setObjectName(QStringLiteral("ultrasonicLimitEditor"));
    limitEditor->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    limitEditor->setMaximumWidth(210);
    auto *limitGrid = new QGridLayout(limitEditor);
    limitGrid->setContentsMargins(6, 3, 6, 3);
    limitGrid->setHorizontalSpacing(4);
    limitGrid->setVerticalSpacing(3);
    limitGrid->setSizeConstraint(QLayout::SetMinimumSize);
    auto *warningLimitLabel = new QLabel(QStringLiteral("预警"));
    warningLimitLabel->setObjectName(QStringLiteral("weatherMeta"));
    warningLimitLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    auto *stopLimitLabel = new QLabel(QStringLiteral("停车"));
    stopLimitLabel->setObjectName(QStringLiteral("weatherMeta"));
    stopLimitLabel->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
    m_warningDistanceSpin = new QSpinBox;
    m_warningDistanceSpin->setObjectName(QStringLiteral("distanceLimitSpin"));
    m_warningDistanceSpin->setRange(10, 200);
    m_warningDistanceSpin->setValue(30);
    m_warningDistanceSpin->setSuffix(QStringLiteral("cm"));
    m_warningDistanceSpin->setMinimumWidth(68);
    m_warningDistanceSpin->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_stopDistanceSpin = new QSpinBox;
    m_stopDistanceSpin->setObjectName(QStringLiteral("distanceLimitSpin"));
    m_stopDistanceSpin->setRange(5, 195);
    m_stopDistanceSpin->setValue(15);
    m_stopDistanceSpin->setSuffix(QStringLiteral("cm"));
    m_stopDistanceSpin->setMinimumWidth(68);
    m_stopDistanceSpin->setSizePolicy(
        QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_applyUltrasonicLimitsButton =
        new QPushButton(QStringLiteral("应用"));
    m_applyUltrasonicLimitsButton->setObjectName(
        QStringLiteral("applyUltrasonicLimits"));
    m_applyUltrasonicLimitsButton->setProperty(
        "kind", QStringLiteral("refresh"));
    m_applyUltrasonicLimitsButton->setFocusPolicy(Qt::NoFocus);
    m_applyUltrasonicLimitsButton->setToolTip(
        QStringLiteral("应用预警距离和停车距离"));
    m_applyUltrasonicLimitsButton->setMinimumWidth(40);
    m_applyUltrasonicLimitsButton->setSizePolicy(
        QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_applyUltrasonicLimitsButton->setEnabled(false);
    limitGrid->addWidget(warningLimitLabel, 0, 0);
    limitGrid->addWidget(m_warningDistanceSpin, 0, 1);
    limitGrid->addWidget(stopLimitLabel, 1, 0);
    limitGrid->addWidget(m_stopDistanceSpin, 1, 1);
    limitGrid->addWidget(m_applyUltrasonicLimitsButton, 0, 2, 2, 1);
    limitGrid->setColumnStretch(1, 1);
    ultrasonicHeader->addWidget(limitEditor, 0, Qt::AlignTop);
    ultrasonicLayout->addLayout(ultrasonicHeader);

    auto *distanceRow = new QHBoxLayout;
    m_distanceValue = new QLabel(QStringLiteral("--"));
    m_distanceValue->setObjectName(QStringLiteral("distanceValue"));
    auto *distanceUnit = new QLabel(QStringLiteral("cm"));
    distanceUnit->setObjectName(QStringLiteral("weatherMeta"));
    distanceUnit->setAlignment(Qt::AlignBottom);
    distanceRow->addWidget(m_distanceValue);
    distanceRow->addWidget(distanceUnit);
    distanceRow->addStretch();
    ultrasonicLayout->addLayout(distanceRow);
    m_distanceState = new QLabel(QStringLiteral("超声波已关闭"));
    m_distanceState->setObjectName(QStringLiteral("distanceState"));
    m_distanceState->setProperty("warning", false);
    ultrasonicLayout->addWidget(m_distanceState);
    m_distanceAverage = new QLabel(QStringLiteral("平均值  --"));
    m_distanceAverage->setObjectName(QStringLiteral("metricText"));
    m_distanceRange = new QLabel(QStringLiteral("范围  --"));
    m_distanceRange->setObjectName(QStringLiteral("metricText"));
    m_distanceTrend = new QLabel(QStringLiteral("趋势  --"));
    m_distanceTrend->setObjectName(QStringLiteral("metricText"));
    ultrasonicLayout->addWidget(m_distanceAverage);
    ultrasonicLayout->addWidget(m_distanceRange);
    ultrasonicLayout->addWidget(m_distanceTrend);

    // 预警距离变化时同步限制停车距离，保证两者至少相差 5 cm。
    connect(m_warningDistanceSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        m_stopDistanceSpin->setMaximum(value - 5);
        m_ultrasonicLimitsDirty = true;
        m_applyUltrasonicLimitsButton->setEnabled(m_online);
    });
    connect(m_stopDistanceSpin,
            qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int) {
        m_ultrasonicLimitsDirty = true;
        m_applyUltrasonicLimitsButton->setEnabled(m_online);
    });
    connect(m_applyUltrasonicLimitsButton, &QPushButton::clicked,
            this, &NetworkPage::applyUltrasonicLimits);

    // 卡片底部显示当前生效阈值，并提供超声波开启/关闭按钮。
    auto *ultrasonicFooter = new QHBoxLayout;
    m_ultrasonicThreshold = new QLabel(
        QStringLiteral("预警 ≤ 30 cm · 停车 ≤ 15 cm"));
    m_ultrasonicThreshold->setObjectName(QStringLiteral("weatherMeta"));
    m_ultrasonicButton = new QPushButton(QStringLiteral("开启超声波"));
    m_ultrasonicButton->setObjectName(QStringLiteral("ultrasonicToggle"));
    m_ultrasonicButton->setProperty("kind", QStringLiteral("refresh"));
    m_ultrasonicButton->setCheckable(true);
    m_ultrasonicButton->setFocusPolicy(Qt::NoFocus);
    m_ultrasonicButton->setFixedHeight(28);
    m_ultrasonicButton->setEnabled(false);
    ultrasonicFooter->addWidget(m_ultrasonicThreshold, 1);
    ultrasonicFooter->addWidget(m_ultrasonicButton);
    ultrasonicLayout->addLayout(ultrasonicFooter);
    connect(m_ultrasonicButton, &QPushButton::clicked,
            this, &NetworkPage::toggleUltrasonic);
    sideMetricsColumn->addWidget(ultrasonicCard, 2);
    topRow->addLayout(sideMetricsColumn, 2);
    rightColumn->addLayout(topRow, 3);

    // 下方区域：距离曲线和网络/设备信息。
    auto *bottomRow = new QHBoxLayout;
    bottomRow->setSpacing(12);

    const auto distanceChartParts =
        makeCard(QStringLiteral("超声波距离曲线"), QStringLiteral("📈"),
                 QStringLiteral("实时回波 · 最近 24 次测距"),
                 QStringLiteral("distanceChartCard"));
    auto *distanceChartCard = distanceChartParts.first;
    auto *distanceChartLayout = distanceChartParts.second;
    m_distanceChart = new DistanceChart;
    m_distanceChart->setMinimumHeight(150);
    distanceChartLayout->addWidget(m_distanceChart, 1);
    bottomRow->addWidget(distanceChartCard, 3);

    // 网络与设备信息来自 ESP32 状态 JSON，用于判断连接环境和运行时间。
    auto *detailsColumn = new QVBoxLayout;
    detailsColumn->setSpacing(10);
    const auto networkParts =
        makeCard(QStringLiteral("网络状态"), QStringLiteral("📶"),
                 QStringLiteral("ESP32 热点与外网"), QStringLiteral("builtinNetworkCard"));
    auto *networkCard = networkParts.first;
    auto *networkLayout = networkParts.second;
    m_networkDetails = new QLabel(
        QStringLiteral("外网 WiFi  --\n信号强度  --\n热点客户端  --\n热点地址  --"));
    m_networkDetails->setObjectName(QStringLiteral("networkDetails"));
    networkLayout->addWidget(m_networkDetails, 1);
    detailsColumn->addWidget(networkCard, 1);

    const auto deviceParts =
        makeCard(QStringLiteral("设备信息"), QStringLiteral("⏱️"),
                 QStringLiteral("连接与运行状态"), QStringLiteral("deviceCard"));
    auto *deviceCard = deviceParts.first;
    auto *deviceLayout = deviceParts.second;
    m_deviceDetails = new QLabel(
        QStringLiteral("设备地址  --\n外网 IP  --\n运行时间  --\n数据更新  --"));
    m_deviceDetails->setObjectName(QStringLiteral("deviceDetails"));
    deviceLayout->addWidget(m_deviceDetails, 1);
    detailsColumn->addWidget(deviceCard, 1);
    bottomRow->addLayout(detailsColumn, 2);
    rightColumn->addLayout(bottomRow, 2);

    body->addLayout(rightColumn, 1);
    root->addLayout(body, 1);

    m_connectButton->setFocusPolicy(Qt::NoFocus);
    m_refreshButton->setFocusPolicy(Qt::NoFocus);
    m_cameraButton->setFocusPolicy(Qt::NoFocus);
    setControlButtonsEnabled(false);
}

// 创建可复用的车辆按钮，并把动作代码存入动态属性，
// 后续 refreshCarButtons() 可以据此同步按钮高亮状态。
QPushButton *NetworkPage::createControlButton(const QString &text,
                                              const QString &command,
                                              const QString &shortcut)
{
    auto *button = new QPushButton(text);
    button->setProperty("kind", command == QStringLiteral("N")
                                    ? QStringLiteral("netStop")
                                    : QStringLiteral("netAction"));
    button->setProperty("carCommand", command);
    button->setCheckable(true);
    button->setMinimumSize(84, 58);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(QStringLiteral("快捷键：%1").arg(shortcut));
    m_carButtons.append(button);
    connectCarButton(button, command);
    return button;
}

// 鼠标/触控按钮按“按下开始、松开停止”处理：
// pointer 输入使用独立 id，因此多个按钮可以组合成 FL/FR/BL/BR。
void NetworkPage::connectCarButton(QPushButton *button, const QString &command)
{
    if (command == QStringLiteral("N")) {
        connect(button, &QPushButton::clicked, this, &NetworkPage::stopCar);
        return;
    }

    const QString inputId = QStringLiteral("pointer:%1").arg(command);
    connect(button, &QPushButton::pressed, this, [this, inputId, command] {
        startCarInput(inputId, command);
    });
    connect(button, &QPushButton::released, this, [this, inputId] {
        endCarInput(inputId);
    });
}

// 连接或断开 ESP32 TCP 服务。地址支持带协议前缀、IP、主机名和自定义端口。
void NetworkPage::connectToDevice()
{
    if (m_connectionRequested) {
        // 主动断开时先停止车辆和定时刷新，再中止 socket。
        m_connectionRequested = false;
        m_refreshTimer->stop();
        stopCar();
        m_socket->abort();
        m_socketBuffer.clear();
        updateConnection(false, QStringLiteral("网络控制已断开"));
        appendLog(QStringLiteral("已断开设备连接"));
        return;
    }

    QString address = m_ipEdit->text().trimmed();
    if (address.isEmpty()) {
        updateConnection(false, QStringLiteral("设备地址格式不正确"));
        appendLog(QStringLiteral("连接失败：设备地址格式不正确"));
        return;
    }
    static const QRegularExpression protocolPrefix(
        QStringLiteral("^(tcp|http|https)://"),
        QRegularExpression::CaseInsensitiveOption);
    address.remove(protocolPrefix);
    while (address.endsWith('/')) {
        address.chop(1);
    }

    const qsizetype colon = address.lastIndexOf(':');
    bool portOk = false;
    quint16 port = 9000;
    if (colon > 0) {
        // 只创建一次端口字符串，避免对同一子串重复生成临时对象。
        const QString portText = address.mid(colon + 1);
        if (!portText.contains('.')) {
            port = portText.toUShort(&portOk);
            if (!portOk || port == 0) {
                updateConnection(false, QStringLiteral("端口号无效"));
                appendLog(QStringLiteral("连接失败：端口号无效"));
                return;
            }
            address.truncate(colon);
        }
    }
    QUrl url(QStringLiteral("tcp://%1").arg(address));
    if (!url.isValid() || url.host().isEmpty()) {
        updateConnection(false, QStringLiteral("设备地址格式不正确"));
        appendLog(QStringLiteral("连接失败：设备地址格式不正确"));
        return;
    }

    m_tcpHost = url.host();
    m_tcpPort = port;
    m_connectionRequested = true;
    m_socketBuffer.clear();
    updateConnection(false, QStringLiteral("正在连接 %1:%2")
                                .arg(m_tcpHost)
                                .arg(m_tcpPort));
    appendLog(QStringLiteral("正在建立 TCP 连接 %1:%2")
                  .arg(m_tcpHost)
                  .arg(m_tcpPort));
    m_socket->connectToHost(m_tcpHost, m_tcpPort);
}

// 定时请求完整状态；ESP32 另外也会按自己的周期主动推送状态。
void NetworkPage::fetchData()
{
    if (!m_connectionRequested || !m_online
        || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    sendSocketLine("DATA");
}

// 手动请求 ESP32 刷新天气，实际网络访问由 ESP32 的后台任务完成。
void NetworkPage::refreshWeather()
{
    if (!m_online || m_socket->state() != QAbstractSocket::ConnectedState) {
        appendLog(QStringLiteral("请先连接 ESP32"));
        return;
    }
    if (sendSocketLine("REFRESH_WEATHER")) {
        appendLog(QStringLiteral("已请求 ESP32 更新天气"));
    }
}

// 自动靠近是一个切换操作：第一次点击发送 TRACK_COLOR，
// 再次点击会调用 stopColorApproach() 退出。
void NetworkPage::startColorApproach()
{
    if (m_visionTracking) {
        stopColorApproach();
        return;
    }
    if (!m_online) {
        appendLog(QStringLiteral("请先连接 ESP32"));
        return;
    }
    if (!sendSocketLine("TRACK_COLOR")) {
        appendLog(QStringLiteral("无法发送视觉靠近指令"));
        return;
    }
    setVisionTracking(true);
    appendLog(QStringLiteral("已启动色块靠近模式"));
}

// 退出自动靠近时必须同时发送 TRACK_STOP 和 N，
// 前者关闭固件跟踪状态，后者立即停车。
void NetworkPage::stopColorApproach()
{
    if (!m_visionTracking) {
        return;
    }
    sendSocketLine("TRACK_STOP");
    sendSocketLine("N");
    setVisionTracking(false);
    updateMotion(QStringLiteral("N"));
    appendLog(QStringLiteral("已停止色块靠近模式"));
}

// 只更新 Qt 侧按钮和提示文字；真正状态由 ESP32 的 tracking.active 确认。
void NetworkPage::setVisionTracking(bool active)
{
    const bool changed = m_visionTracking != active;
    m_visionTracking = active;
    if (m_approachButton) {
        m_approachButton->setText(
            active ? QStringLiteral("停止靠近")
                   : QStringLiteral("靠近色块"));
    }
    if (m_statusHint && active) {
        m_statusHint->setText(
            active ? QStringLiteral("低速靠近中，到达后自动居中，障碍物立即停车")
                   : QStringLiteral("按住方向键开始行驶"));
    } else if (m_statusHint && changed) {
        m_statusHint->setText(motionHint(m_currentAction));
    }
}

// 超声波默认关闭，点击后发送 ULTRASONIC_ON/OFF。
void NetworkPage::toggleUltrasonic()
{
    if (!m_online) {
        m_ultrasonicButton->setChecked(m_ultrasonicEnabled);
        return;
    }
    const bool enable = !m_ultrasonicEnabled;
    const QByteArray command =
        enable ? QByteArray("ULTRASONIC_ON") : QByteArray("ULTRASONIC_OFF");
    if (!sendSocketLine(command)) {
        m_ultrasonicButton->setChecked(m_ultrasonicEnabled);
        appendLog(QStringLiteral("超声波开关指令发送失败"));
        return;
    }
    setUltrasonicEnabled(enable);
    appendLog(enable ? QStringLiteral("已开启超声波测距")
                     : QStringLiteral("已关闭超声波测距"));
}

// 根据开关状态清空或恢复距离显示；changed 判断避免每 200 ms 重复刷新样式。
void NetworkPage::setUltrasonicEnabled(bool enabled)
{
    const bool changed = m_ultrasonicEnabled != enabled;
    m_ultrasonicEnabled = enabled;
    if (m_ultrasonicButton) {
        m_ultrasonicButton->setChecked(enabled);
        m_ultrasonicButton->setText(
            enabled ? QStringLiteral("关闭超声波")
                    : QStringLiteral("开启超声波"));
        m_ultrasonicButton->setEnabled(m_online);
    }
    if (changed && !enabled) {
        m_distanceValue->setText(QStringLiteral("--"));
        m_distanceState->setText(QStringLiteral("超声波已关闭"));
        m_distanceState->setProperty("warning", false);
        m_distanceState->style()->unpolish(m_distanceState);
        m_distanceState->style()->polish(m_distanceState);
        m_distanceAverage->setText(QStringLiteral("平均值  --"));
        m_distanceRange->setText(QStringLiteral("范围  --"));
        m_distanceTrend->setText(QStringLiteral("趋势  --"));
        m_distanceChart->setValues({});
        m_obstacleStatus->setText(QStringLiteral("距离：已关闭"));
        m_obstacleStatus->setProperty("warning", false);
        m_obstacleStatus->style()->unpolish(m_obstacleStatus);
        m_obstacleStatus->style()->polish(m_obstacleStatus);
    } else if (changed && enabled) {
        m_distanceState->setText(QStringLiteral("等待测距数据"));
    }
}

// 把用户在 Qt 中填写的预警/停车距离发送给 ESP32。
// 停车距离必须比预警距离至少小 5 cm，防止两个阈值互相冲突。
void NetworkPage::applyUltrasonicLimits()
{
    if (!m_online) {
        appendLog(QStringLiteral("请先连接 ESP32 再设置安全距离"));
        return;
    }
    const int warningCm = m_warningDistanceSpin->value();
    const int stopCm = m_stopDistanceSpin->value();
    if (stopCm > warningCm - 5) {
        appendLog(QStringLiteral("停车距离必须比预警距离至少小 5 cm"));
        return;
    }

    const QByteArray command =
        QStringLiteral("ULTRASONIC_LIMITS:%1:%2")
            .arg(warningCm)
            .arg(stopCm)
            .toUtf8();
    if (!sendSocketLine(command)) {
        appendLog(QStringLiteral("安全距离设置发送失败"));
        return;
    }
    m_socket->flush();
    m_ultrasonicLimitsDirty = false;
    m_applyUltrasonicLimitsButton->setEnabled(false);
    setUltrasonicLimits(warningCm, stopCm, true);
    appendLog(
        QStringLiteral("安全距离已设置：预警 %1 cm，停车 %2 cm")
            .arg(warningCm)
            .arg(stopCm));
}

// 将服务端最新阈值同步到两个 QSpinBox。
// 用户正在编辑且尚未应用时（dirty=true）不覆盖输入值。
void NetworkPage::setUltrasonicLimits(int warningCm, int stopCm, bool force)
{
    m_warningDistanceCm = std::clamp(warningCm, 10, 200);
    m_stopDistanceCm =
        std::clamp(stopCm, 5, m_warningDistanceCm - 5);
    QString threshold =
        QStringLiteral("当前：预警 ≤ %1 cm · 停车 ≤ %2 cm")
            .arg(m_warningDistanceCm)
            .arg(m_stopDistanceCm);
    if (m_ultrasonicLimitsDirty) {
        threshold += QStringLiteral(" · 待应用");
    }
    m_ultrasonicThreshold->setText(threshold);

    if (!force && m_ultrasonicLimitsDirty) {
        return;
    }
    const QSignalBlocker warningBlocker(m_warningDistanceSpin);
    const QSignalBlocker stopBlocker(m_stopDistanceSpin);
    m_stopDistanceSpin->setMaximum(m_warningDistanceCm - 5);
    m_warningDistanceSpin->setValue(m_warningDistanceCm);
    m_stopDistanceSpin->setValue(m_stopDistanceCm);
    if (force) {
        m_ultrasonicLimitsDirty = false;
    }
}

// 手动控制视觉流开关；网络未连接时不发起视频请求。
void NetworkPage::toggleCameraStream()
{
    if (m_cameraRequested) {
        stopCameraStream(QStringLiteral("视觉画面已关闭"));
        return;
    }
    if (!m_online) {
        m_cameraButton->setChecked(false);
        m_cameraStatus->setText(QStringLiteral("设备未连接"));
        return;
    }
    startCameraStream();
}

// ESP32-CAM 的 MJPEG 流地址为 http://设备IP:81/stream。
// 这里使用与 TCP 控制不同的网络对象，避免视频大流量影响控制 socket。
void NetworkPage::startCameraStream()
{
    if (!m_online || m_tcpHost.isEmpty()) {
        m_cameraStatus->setText(QStringLiteral("等待设备"));
        return;
    }
    if (m_cameraRequested
        && m_cameraReply
        && m_cameraReply->isRunning()) {
        return;
    }

    stopCameraStream();
    m_cameraRequested = true;
    m_cameraBuffer.clear();
    m_cameraStatus->setText(QStringLiteral("正在连接"));
    m_cameraView->setMessage(QStringLiteral("正在连接 ESP32-CAM 视频流…"));
    m_cameraButton->setChecked(true);
    m_cameraButton->setText(QStringLiteral("关闭视觉"));

    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(m_tcpHost);
    url.setPort(81);
    url.setPath(QStringLiteral("/stream"));
    QNetworkRequest request(url);
    // 视频必须实时更新，因此禁止缓存，并设置连接超时。
    request.setRawHeader("Cache-Control", "no-store");
    request.setRawHeader("User-Agent", "QtCarVision/1.0");
    request.setTransferTimeout(8000);
    m_cameraReply = m_cameraNetwork->get(request);

    connect(m_cameraReply, &QNetworkReply::readyRead,
            this, &NetworkPage::handleCameraData);
    connect(m_cameraReply, &QNetworkReply::finished, this,
            [this, reply = m_cameraReply] {
        if (m_cameraReply != reply) {
            reply->deleteLater();
            return;
        }
        m_cameraReply = nullptr;
        if (!m_cameraRequested) {
            reply->deleteLater();
            return;
        }

        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString reason = status > 0
                                   ? QStringLiteral("HTTP %1").arg(status)
                                   : reply->errorString();
        m_cameraRequested = false;
        m_cameraButton->setChecked(false);
        m_cameraButton->setText(QStringLiteral("重新开启"));
        m_cameraStatus->setText(QStringLiteral("连接失败"));
        m_cameraView->setMessage(
            QStringLiteral("视觉流连接失败\n%1").arg(reason));
        appendLog(QStringLiteral("视觉流中断：%1").arg(reason));
        reply->deleteLater();
    });
}

// 停止视频时先解除信号连接，再 abort 和释放 reply，
// 防止旧请求在关闭后继续触发 readyRead 或 finished。
void NetworkPage::stopCameraStream(const QString &message)
{
    m_cameraRequested = false;
    m_cameraBuffer.clear();
    if (m_cameraReply) {
        QNetworkReply *reply = m_cameraReply;
        m_cameraReply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }

    if (m_cameraButton) {
        m_cameraButton->setChecked(false);
        m_cameraButton->setText(QStringLiteral("开启视觉"));
        m_cameraButton->setEnabled(m_online);
    }
    if (m_cameraStatus) {
        m_cameraStatus->setText(
            m_online ? QStringLiteral("视觉已暂停")
                     : QStringLiteral("等待连接"));
    }
    if (m_cameraView && !message.isEmpty()) {
        m_cameraView->setMessage(message);
    }
}

// readyRead 只说明“收到了若干字节”，不代表刚好收到一张 JPEG。
// 因此必须把数据放入缓冲区，并查找 FF D8（JPEG 开始）和 FF D9（结束）。
void NetworkPage::handleCameraData()
{
    if (!m_cameraReply) {
        return;
    }
    m_cameraBuffer.append(m_cameraReply->readAll());

    while (true) {
        // 丢弃帧头前的 HTTP/multipart 数据，定位 JPEG 起点。
        const qsizetype start =
            m_cameraBuffer.indexOf(QByteArray::fromHex("ffd8"));
        if (start < 0) {
            if (m_cameraBuffer.size() > 1
                && static_cast<unsigned char>(m_cameraBuffer.back()) == 0xFF) {
                m_cameraBuffer = m_cameraBuffer.right(1);
            } else {
                m_cameraBuffer.clear();
            }
            break;
        }
        if (start > 0) {
            m_cameraBuffer.remove(0, start);
        }
        // 如果暂时没有帧尾，保留缓冲区，等待下一次 readyRead 继续拼接。
        const qsizetype end =
            m_cameraBuffer.indexOf(QByteArray::fromHex("ffd9"), 2);
        if (end < 0) {
            if (m_cameraBuffer.size() > 4 * 1024 * 1024) {
                m_cameraBuffer.clear();
            }
            break;
        }

        const QByteArray jpeg = m_cameraBuffer.left(end + 2);
        m_cameraBuffer.remove(0, end + 2);
        QImage image;
        // 一张图解码失败不会影响下一张；继续循环处理缓冲中的其余帧。
        if (!image.loadFromData(jpeg, "JPG")) {
            continue;
        }
        m_cameraView->setImage(image);
        analyzeLocalFrame(image);
        m_cameraStatus->setText(
            QStringLiteral("实时 · %1×%2").arg(image.width()).arg(image.height()));
    }
}

// Qt 本地颜色识别的完整流程：
// 降采样 -> HSV 分类 -> 连通区域 -> 面积/形状过滤 -> 两帧稳定确认。
// 本地识别结果既用于绘制识别框，也会在自动靠近时发送给 ESP32。
void NetworkPage::analyzeLocalFrame(const QImage &image)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 约 120 ms 分析一次，避免每个视频帧都进行连通区域搜索占用 GUI 线程。
    if (now - m_localVisionUpdatedAt < 120 || image.isNull()) {
        return;
    }
    m_localVisionUpdatedAt = now;

    QImage rgb = image.convertToFormat(QImage::Format_RGB888);
    struct Stats {
        int count = 0;
        int minX = std::numeric_limits<int>::max();
        int minY = std::numeric_limits<int>::max();
        int maxX = -1;
        int maxY = -1;
    };
    // 每 2 像素取一个样本，兼顾速度和目标轮廓精度。
    constexpr int sampleStep = 2;
    constexpr int sampleOrigin = 1;
    const int gridWidth =
        (rgb.width() - sampleOrigin + sampleStep - 1) / sampleStep;
    const int gridHeight =
        (rgb.height() - sampleOrigin + sampleStep - 1) / sampleStep;
    // colorMask：0=背景，1=红色，2=绿色，供连通区域搜索使用。
    QList<quint8> colorMask(gridWidth * gridHeight, 0);
    int redPixels = 0;
    int greenPixels = 0;
    const bool allowRed = m_visionColorFilter != QStringLiteral("green");
    const bool allowGreen = m_visionColorFilter != QStringLiteral("red");

    for (int gridY = 0; gridY < gridHeight; ++gridY) {
        const int y = sampleOrigin + gridY * sampleStep;
        const uchar *row = rgb.constScanLine(y);
        for (int gridX = 0; gridX < gridWidth; ++gridX) {
            const int x = sampleOrigin + gridX * sampleStep;
            const int r = row[x * 3];
            const int g = row[x * 3 + 1];
            const int b = row[x * 3 + 2];
            const int maximumChannel = std::max({r, g, b});
            const int minimumChannel = std::min({r, g, b});
            const int chroma = maximumChannel - minimumChannel;
            const qreal saturation =
                maximumChannel > 0
                    ? static_cast<qreal>(chroma) / maximumChannel
                    : 0.0;
            qreal hue = 0.0;
            if (chroma > 0) {
                if (maximumChannel == r) {
                    hue = 60.0 * (g - b) / chroma;
                    if (hue < 0) hue += 360.0;
                } else if (maximumChannel == g) {
                    hue = 60.0 * (static_cast<qreal>(b - r) / chroma + 2.0);
                } else {
                    hue = 60.0 * (static_cast<qreal>(r - g) / chroma + 4.0);
                }
            }

            // 同时检查色相范围和RGB通道优势，避免偏白或偏暗像素被识别。
            const bool isRed =
                (maximumChannel >= 45 && saturation >= 0.34
                 && (hue <= 30.0 || hue >= 330.0))
                || (saturation >= 0.34
                    && r >= 70 && r >= g + 30 && r >= b + 20);
            const bool isGreen =
                (maximumChannel >= 40 && saturation >= 0.30
                 && hue >= 75.0 && hue <= 165.0)
                || (saturation >= 0.30
                    && g >= 60 && g >= r + 18 && g >= b + 10);
            const quint8 label =
                (isRed && allowRed) ? 1
                                    : ((isGreen && allowGreen) ? 2 : 0);
            colorMask[gridY * gridWidth + gridX] = label;
            if (label == 1) {
                redPixels++;
            } else if (label == 2) {
                greenPixels++;
            }
        }
    }

    // 使用四邻域广度优先搜索，把一个颜色的连续像素合并成一个目标区域。
    Stats red;
    Stats green;
    QList<quint8> visited(colorMask.size(), 0);
    for (int start = 0; start < colorMask.size(); ++start) {
        const quint8 label = colorMask.at(start);
        if (label == 0 || visited.at(start)) {
            continue;
        }

        QList<int> queue;
        queue.append(start);
        visited[start] = 1;
        int head = 0;
        Stats component;
        while (head < queue.size()) {
            const int index = queue.at(head++);
            const int gridX = index % gridWidth;
            const int gridY = index / gridWidth;
            const int x = sampleOrigin + gridX * sampleStep;
            const int y = sampleOrigin + gridY * sampleStep;
            component.count++;
            component.minX = std::min(component.minX, x);
            component.minY = std::min(component.minY, y);
            component.maxX = std::max(component.maxX, x);
            component.maxY = std::max(component.maxY, y);

            const int neighbors[4] = {
                gridX > 0 ? index - 1 : -1,
                gridX + 1 < gridWidth ? index + 1 : -1,
                gridY > 0 ? index - gridWidth : -1,
                gridY + 1 < gridHeight ? index + gridWidth : -1,
            };
            for (int neighbor : neighbors) {
                if (neighbor >= 0 && colorMask.at(neighbor) == label
                    && !visited.at(neighbor)) {
                    visited[neighbor] = 1;
                    queue.append(neighbor);
                }
            }
        }

        // 过滤松散噪点：连通像素数、填充率、最小宽高必须同时满足。
        const int boxArea =
            (component.maxX - component.minX + 1)
            * (component.maxY - component.minY + 1);
        const qreal fillRatio =
            boxArea > 0
                ? component.count * sampleStep * sampleStep
                      / static_cast<qreal>(boxArea)
                : 0.0;
        const int boxWidth = component.maxX - component.minX + 1;
        const int boxHeight = component.maxY - component.minY + 1;
        if (fillRatio < 0.14 || boxWidth < 12 || boxHeight < 12) {
            continue;
        }
        Stats &best = label == 1 ? red : green;
        if (component.count > best.count) {
            best = component;
        }
    }

    Stats *selected = nullptr;
    QString color;
    if (red.count >= 36 && red.count >= green.count) {
        selected = &red;
        color = QStringLiteral("red");
    } else if (green.count >= 28) {
        selected = &green;
        color = QStringLiteral("green");
    }

    if (!selected) {
        // 短暂漏检时保留上一帧目标，超过 260 ms 才清空识别框。
        m_localVisionPendingColor.clear();
        m_localVisionPendingColorFrames = 0;
        m_localVisionPendingBox = QRect();
        m_localVisionPendingBoxFrames = 0;
        if (m_localVisionDetected
            && m_localVisionLastSeenAt > 0
            && now - m_localVisionLastSeenAt <= 260) {
            m_visionResult->setText(
                QStringLiteral("识别（Qt）：目标短暂遮挡，保持上一位置"));
            return;
        }
        m_localVisionDetected = false;
        m_cameraView->clearDetection();
        m_visionResult->setText(
            QStringLiteral("识别（Qt）：未发现目标（红 %1 / 绿 %2）")
                .arg(redPixels)
                .arg(greenPixels));
        return;
    }

    const QRect candidateBox(
        selected->minX, selected->minY,
        selected->maxX - selected->minX + 1,
        selected->maxY - selected->minY + 1);
    // 颜色变化必须连续两帧一致，避免红绿小噪点互相抢占。
    const bool changingColor =
        !m_localVisionDetected || color != m_localVisionColor;
    if (changingColor) {
        if (m_localVisionPendingColor == color) {
            m_localVisionPendingColorFrames++;
        } else {
            m_localVisionPendingColor = color;
            m_localVisionPendingColorFrames = 1;
        }
        if (m_localVisionPendingColorFrames < 2) {
            m_visionResult->setText(
                QStringLiteral("识别（Qt）：正在确认新的颜色目标"));
            return;
        }
    }

    if (!changingColor
        && m_localVisionDetected
        && !m_localVisionBox.isEmpty()
        && !candidateBox.isEmpty()) {
        const QPoint oldCenter = m_localVisionBox.center();
        const QPoint newCenter = candidateBox.center();
        const int dx = newCenter.x() - oldCenter.x();
        const int dy = newCenter.y() - oldCenter.y();
        const int maxCenterJump = std::max(32, rgb.width() / 5);
        const qreal areaRatio =
            static_cast<qreal>(candidateBox.width() * candidateBox.height())
            / std::max(1, m_localVisionBox.width() * m_localVisionBox.height());
        // 相同颜色但中心突然远跳、面积突变时，也要求新位置连续确认。
        const bool jumped =
            dx * dx + dy * dy > maxCenterJump * maxCenterJump
            || areaRatio < 0.30 || areaRatio > 3.0;
        if (jumped) {
            bool samePendingBox = false;
            if (!m_localVisionPendingBox.isEmpty()) {
                const QPoint pendingCenter = m_localVisionPendingBox.center();
                const int pendingDx = newCenter.x() - pendingCenter.x();
                const int pendingDy = newCenter.y() - pendingCenter.y();
                const qreal pendingAreaRatio =
                    static_cast<qreal>(candidateBox.width()
                                       * candidateBox.height())
                    / std::max(1, m_localVisionPendingBox.width()
                                      * m_localVisionPendingBox.height());
                samePendingBox =
                    pendingDx * pendingDx + pendingDy * pendingDy
                        <= (maxCenterJump / 2) * (maxCenterJump / 2)
                    && pendingAreaRatio >= 0.40
                    && pendingAreaRatio <= 2.50;
            }
            if (samePendingBox) {
                m_localVisionPendingBoxFrames++;
            } else {
                m_localVisionPendingBox = candidateBox;
                m_localVisionPendingBoxFrames = 1;
            }
            if (m_localVisionPendingBoxFrames < 2) {
                m_visionResult->setText(
                    QStringLiteral("识别（Qt）：正在确认新的目标位置"));
                return;
            }
        } else {
            m_localVisionPendingBox = QRect();
            m_localVisionPendingBoxFrames = 0;
        }
    }
    m_localVisionPendingColor.clear();
    m_localVisionPendingColorFrames = 0;
    m_localVisionPendingBox = QRect();
    m_localVisionPendingBoxFrames = 0;
    m_localVisionDetected = true;
    m_localVisionColor = color;
    m_localVisionLastSeenAt = now;
    m_localVisionBox = candidateBox;
    m_localVisionFrame = rgb.size();
    const int centerX = m_localVisionBox.x() + m_localVisionBox.width() / 2;
    const int centerY = m_localVisionBox.y() + m_localVisionBox.height() / 2;
    m_cameraView->setDetection(
        color, m_localVisionBox, m_localVisionFrame);
    m_visionResult->setText(
        QStringLiteral("识别（Qt）：%1色块 · 中心(%2,%3)")
            .arg(color == QStringLiteral("red")
                     ? QStringLiteral("红色")
                     : QStringLiteral("绿色"))
            .arg(centerX)
            .arg(centerY));
    // 自动靠近期间，把确认后的目标坐标交给 ESP32 做运动决策。
    if (m_visionTracking) {
        sendLocalVisionHint();
    }
}

// 将 Qt 识别到的目标转换为 ESP32 可解析的 VISION_TARGET 文本命令。
// 20 Hz 上限足够控制小车，同时避免过密消息占用 TCP。
void NetworkPage::sendLocalVisionHint()
{
    if (!m_online || !m_visionTracking || !m_localVisionDetected) {
        return;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_localVisionHintSentAt < 160) {
        return;
    }
    m_localVisionHintSentAt = now;
    const qint64 centerX =
        m_localVisionBox.x() + m_localVisionBox.width() / 2;
    const qint64 centerY =
        m_localVisionBox.y() + m_localVisionBox.height() / 2;
    sendSocketLine(
        QStringLiteral("VISION_TARGET:%1:%2:%3:%4:%5:%6:%7:%8:%9")
            .arg(m_localVisionColor)
            .arg(centerX)
            .arg(centerY)
            .arg(m_localVisionBox.x())
            .arg(m_localVisionBox.y())
            .arg(m_localVisionBox.width())
            .arg(m_localVisionBox.height())
            .arg(m_localVisionFrame.width())
            .arg(m_localVisionFrame.height())
            .toUtf8());
}

// 把“自动/红/绿”过滤设置同步给 ESP32，
// 让固件本地识别与 Qt 端的选择保持一致。
void NetworkPage::updateVisionColorFilter()
{
    if (!m_online || !m_socket
        || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    sendSocketLine(
        QStringLiteral("VISION_COLOR_FILTER:%1")
            .arg(m_visionColorFilter)
            .toUtf8());
}

// TCP 是字节流，readyRead() 可能收到半行、一行或多行。
// 因此先累积，再只处理以 '\n' 结束的完整文本行。
void NetworkPage::handleSocketData()
{
    m_socketBuffer.append(m_socket->readAll());
    while (true) {
        const qsizetype newline = m_socketBuffer.indexOf('\n');
        if (newline < 0) {
            break;
        }
        QByteArray line = m_socketBuffer.left(newline).trimmed();
        m_socketBuffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }
        // JSON 行交给 updateData()；READY 等握手信息只写入日志。
        if (line.startsWith('{')) {
            updateData(line);
        } else if (line.startsWith("READY")) {
            appendLog(QString::fromUtf8(line));
        }
    }
}

// 统一为 TCP 文本协议追加换行符，ESP32 以换行作为一条命令的边界。
bool NetworkPage::sendSocketLine(const QByteArray &line)
{
    if (m_socket->state() != QAbstractSocket::ConnectedState) {
        return false;
    }
    const QByteArray payload = line + '\n';
    return m_socket->write(payload) == payload.size();
}

// 解析 ESP32 的完整状态 JSON，并按模块更新天气、超声波、
// 车辆动作、颜色识别、网络和设备信息等界面。
void NetworkPage::updateData(const QByteArray &payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        appendLog(QStringLiteral("设备返回的 TCP 数据格式无效"));
        return;
    }

    const QJsonObject root = document.object();
    // 天气模块：有效时更新时间、温度与图标；失败时显示错误信息。
    const QJsonObject weather = root.value(QStringLiteral("weather")).toObject();
    const QString weatherText = jsonText(weather.value(QStringLiteral("text")));
    const QString weatherCode = jsonText(weather.value(QStringLiteral("code")));
    const QString temperature = jsonText(weather.value(QStringLiteral("temperature")));
    const QString observedAt = jsonText(weather.value(QStringLiteral("observedAt")));
    const QString weatherError = jsonText(weather.value(QStringLiteral("error")));
    const bool weatherValid = weather.value(QStringLiteral("valid")).toBool();

    m_weatherText->setText(weatherText.isEmpty() ? QStringLiteral("等待同步") : weatherText);
    m_temperature->setText(temperature.isEmpty()
                               ? QStringLiteral("--°C")
                               : QStringLiteral("%1°C").arg(temperature));
    if (weatherValid) {
        const qint64 age =
            static_cast<qint64>(weather.value(QStringLiteral("ageSeconds")).toDouble());
        QString observedDisplay = observedAt;
        observedDisplay.replace('T', ' ');
        observedDisplay = observedDisplay.left(19);
        m_weatherObserved->setText(
            QStringLiteral("%1\n更新于 %2前")
                .arg(observedAt.isEmpty()
                         ? QStringLiteral("观测时间未知")
                         : QStringLiteral("观测 %1").arg(observedDisplay),
                     formatDuration(age)));
    } else {
        m_weatherObserved->setText(QStringLiteral("尚未获取有效天气"));
    }
    m_weatherError->setText(weatherError);
    m_weatherError->setVisible(!weatherError.isEmpty());
    applyWeatherTheme(weatherText, weatherCode);

    // 超声波模块：enabled 表示用户是否开启，valid 表示本次是否有有效回波。
    const QJsonObject ultrasonic =
        root.value(QStringLiteral("ultrasonic")).toObject();
    const bool ultrasonicEnabled =
        ultrasonic.value(QStringLiteral("enabled")).toBool();
    setUltrasonicEnabled(ultrasonicEnabled);
    const bool distanceValid =
        ultrasonicEnabled
        && ultrasonic.value(QStringLiteral("valid")).toBool();
    const double distanceCm =
        ultrasonic.value(QStringLiteral("distanceCm")).toDouble(-1);
    const bool obstacleWarning =
        ultrasonicEnabled
        && ultrasonic.value(QStringLiteral("warning")).toBool();
    const int warningDistanceCm =
        ultrasonic.value(QStringLiteral("warningCm"))
            .toInt(m_warningDistanceCm);
    const int stopDistanceCm =
        ultrasonic.value(QStringLiteral("stopCm"))
            .toInt(m_stopDistanceCm);
    setUltrasonicLimits(warningDistanceCm, stopDistanceCm, false);
    const double averageDistance =
        ultrasonicEnabled
            ? ultrasonic.value(QStringLiteral("average")).toDouble(-1)
            : -1;
    const double minimumDistance =
        ultrasonicEnabled
            ? ultrasonic.value(QStringLiteral("minimum")).toDouble(-1)
            : -1;
    const double maximumDistance =
        ultrasonicEnabled
            ? ultrasonic.value(QStringLiteral("maximum")).toDouble(-1)
            : -1;

    m_distanceValue->setText(
        distanceValid ? numberText(distanceCm, 1) : QStringLiteral("--"));
    m_distanceState->setProperty("warning", obstacleWarning);
    // 距离状态文字根据开关、回波有效性和障碍预警共同决定。
    if (!ultrasonicEnabled) {
        m_distanceState->setText(QStringLiteral("超声波已关闭"));
    } else if (!distanceValid) {
        m_distanceState->setText(QStringLiteral("未收到有效回波"));
    } else if (obstacleWarning && distanceCm <= stopDistanceCm) {
        m_distanceState->setText(QStringLiteral("危险距离 · 紧急停车"));
    } else if (obstacleWarning) {
        m_distanceState->setText(QStringLiteral("障碍预警 · 请停车或转向"));
    } else {
        m_distanceState->setText(QStringLiteral("安全距离"));
    }
    m_distanceState->style()->unpolish(m_distanceState);
    m_distanceState->style()->polish(m_distanceState);
    m_distanceAverage->setText(
        averageDistance < 0
            ? QStringLiteral("平均值  --")
            : QStringLiteral("平均值  %1 cm").arg(numberText(averageDistance, 1)));
    m_distanceRange->setText(
        minimumDistance < 0 || maximumDistance < 0
            ? QStringLiteral("范围  --")
            : QStringLiteral("范围  %1 / %2 cm")
                  .arg(numberText(minimumDistance, 1),
                       numberText(maximumDistance, 1)));

    // history 是 ESP32 保存的最近 24 个距离点，直接用于自绘曲线。
    QList<qreal> distanceHistory;
    const QJsonArray historyArray =
        ultrasonicEnabled
            ? ultrasonic.value(QStringLiteral("history")).toArray()
            : QJsonArray();
    for (const QJsonValue &value : historyArray) {
        if (value.isDouble()) {
            distanceHistory.append(value.toDouble());
        }
    }
    // 用最后两个采样点的差值显示变远、接近或稳定趋势。
    if (distanceHistory.size() >= 2) {
        const qreal delta =
            distanceHistory.last() - distanceHistory.at(distanceHistory.size() - 2);
        if (delta > 1.5) {
            m_distanceTrend->setText(QStringLiteral("趋势  ↑ 变远"));
        } else if (delta < -1.5) {
            m_distanceTrend->setText(QStringLiteral("趋势  ↓ 接近障碍"));
        } else {
            m_distanceTrend->setText(QStringLiteral("趋势  → 稳定"));
        }
    } else {
        m_distanceTrend->setText(QStringLiteral("趋势  --"));
    }
    m_distanceChart->setValues(distanceHistory);
    m_obstacleStatus->setText(
        !ultrasonicEnabled
            ? QStringLiteral("距离：超声波关闭")
            : (distanceValid
                   ? QStringLiteral("距离：%1 cm%2")
                         .arg(numberText(distanceCm, 1),
                              obstacleWarning
                                  ? QStringLiteral(" · 障碍预警")
                                  : QStringLiteral(" · 安全"))
                   : QStringLiteral("距离：--")));
    m_obstacleStatus->setProperty("warning", obstacleWarning);
    m_obstacleStatus->style()->unpolish(m_obstacleStatus);
    m_obstacleStatus->style()->polish(m_obstacleStatus);

    // 网络模块：STA 外网、AP 热点、RSSI、客户端数量和运行时间。
    const QJsonObject network = root.value(QStringLiteral("network")).toObject();
    const bool staConnected = network.value(QStringLiteral("staConnected")).toBool();
    const QString staIp = jsonText(network.value(QStringLiteral("staIp")));
    const int rssi = network.value(QStringLiteral("rssi")).toInt();
    const QString ssid = jsonText(network.value(QStringLiteral("ssid")));
    const QString apSsid = jsonText(network.value(QStringLiteral("apSsid")));
    const QString apIp = jsonText(network.value(QStringLiteral("apIp")));
    const int apClients = network.value(QStringLiteral("apClients")).toInt();
    const qint64 uptime =
        static_cast<qint64>(network.value(QStringLiteral("uptime")).toDouble());
    m_networkDetails->setText(
        QStringLiteral("外网 WiFi  %1\n信号强度  %2\n设备热点  %3\n热点客户端  %4")
            .arg(staConnected ? QStringLiteral("已连接 · %1").arg(ssid)
                              : QStringLiteral("未连接"),
                 staConnected ? QStringLiteral("%1 dBm").arg(rssi)
                              : QStringLiteral("--"),
                 apSsid.isEmpty() ? QStringLiteral("--") : apSsid,
                 QStringLiteral("%1 台").arg(apClients)));
    m_deviceDetails->setText(
        QStringLiteral("TCP 地址  %1:%2\n外网 IP  %3\n热点 IP  %4\n运行时间  %5")
            .arg(m_tcpHost)
            .arg(m_tcpPort)
            .arg(
                 staIp.isEmpty() ? QStringLiteral("--") : staIp,
                 apIp.isEmpty() ? QStringLiteral("--") : apIp,
                 formatDuration(uptime)));

    // 正在按键时以本地输入为准；没有按键时才采用固件报告的最终动作，
    // 防止周期性的旧状态覆盖用户刚刚按下的动作。
    const QJsonObject car = root.value(QStringLiteral("car")).toObject();
    if (m_activeCarInputs.isEmpty()) {
        updateMotion(jsonText(car.value(QStringLiteral("action"))));
    }

    // 识别状态优先采用最近 900 ms 内的 Qt 本地结果；
    // 本地结果不可用时，再使用 ESP32 自身分析的识别框。
    const QJsonObject vision = root.value(QStringLiteral("vision")).toObject();
    const bool targetDetected =
        vision.value(QStringLiteral("detected")).toBool();
    const QString targetColor =
        jsonText(vision.value(QStringLiteral("color")));
    const int frameWidth = vision.value(QStringLiteral("frameWidth")).toInt();
    const int frameHeight = vision.value(QStringLiteral("frameHeight")).toInt();
    const int redPixels =
        vision.value(QStringLiteral("redPixels")).toInt();
    const int greenPixels =
        vision.value(QStringLiteral("greenPixels")).toInt();
    const int analyzeAttempts =
        vision.value(QStringLiteral("analyzeAttempts")).toInt();
    const int decodeSuccesses =
        vision.value(QStringLiteral("decodeSuccesses")).toInt();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool recentLocalAnalysis =
        m_localVisionUpdatedAt > 0
        && nowMs - m_localVisionUpdatedAt < 900;
    const bool firmwareColorAllowed =
        m_visionColorFilter == QStringLiteral("auto")
        || targetColor == m_visionColorFilter;
    if (m_localVisionDetected && recentLocalAnalysis) {
        m_cameraView->setDetection(
            m_localVisionColor, m_localVisionBox, m_localVisionFrame);
        const int localCenterX =
            m_localVisionBox.x() + m_localVisionBox.width() / 2;
        const int localCenterY =
            m_localVisionBox.y() + m_localVisionBox.height() / 2;
        m_visionResult->setText(
            QStringLiteral("识别（Qt）：%1色块 · 中心(%2,%3)")
                .arg(m_localVisionColor == QStringLiteral("red")
                         ? QStringLiteral("红色")
                         : QStringLiteral("绿色"))
                .arg(localCenterX)
                .arg(localCenterY));
    } else if (targetDetected && firmwareColorAllowed
               && frameWidth > 0 && frameHeight > 0) {
        const QRect targetBox(
            vision.value(QStringLiteral("x")).toInt(),
            vision.value(QStringLiteral("y")).toInt(),
            vision.value(QStringLiteral("width")).toInt(),
            vision.value(QStringLiteral("height")).toInt());
        const int centerX = vision.value(QStringLiteral("centerX")).toInt();
        const int centerY = vision.value(QStringLiteral("centerY")).toInt();
        m_cameraView->setDetection(
            targetColor, targetBox, QSize(frameWidth, frameHeight));
        m_visionResult->setText(
            QStringLiteral("识别：%1色块 · 中心(%2,%3)")
                .arg(targetColor == QStringLiteral("red")
                         ? QStringLiteral("红色")
                         : QStringLiteral("绿色"))
                .arg(centerX)
                .arg(centerY));
    } else if (!recentLocalAnalysis) {
        m_cameraView->clearDetection();
        m_visionResult->setText(
            frameWidth <= 0 || frameHeight <= 0
                ? (analyzeAttempts > 0 && decodeSuccesses == 0
                       ? QStringLiteral("识别：图像解码失败")
                       : QStringLiteral("识别：等待摄像头分析"))
                : QStringLiteral("识别：未发现目标（红 %1 / 绿 %2）")
                      .arg(redPixels)
                      .arg(greenPixels));
    }

    // 自动靠近是否处于活动状态由固件确认，避免按钮显示与车辆实际状态不一致。
    const QJsonObject tracking =
        root.value(QStringLiteral("tracking")).toObject();
    if (tracking.contains(QStringLiteral("active"))) {
        setVisionTracking(tracking.value(QStringLiteral("active")).toBool());
    }

    // 摄像头未就绪时主动停止视频请求，并显示固件返回的原因。
    const QJsonObject camera = root.value(QStringLiteral("camera")).toObject();
    if (camera.contains(QStringLiteral("ready"))
        && !camera.value(QStringLiteral("ready")).toBool()) {
        const QString cameraError =
            jsonText(camera.value(QStringLiteral("error")));
        if (m_cameraRequested) {
            stopCameraStream(
                cameraError.isEmpty()
                    ? QStringLiteral("摄像头未就绪")
                    : QStringLiteral("摄像头未就绪\n%1").arg(cameraError));
        }
        if (m_cameraStatus
            && m_cameraStatus->text() != QStringLiteral("摄像头未就绪")) {
            m_cameraStatus->setText(QStringLiteral("摄像头未就绪"));
        }
    }

    m_networkHint->setText(
        QStringLiteral("数据同步于 %1")
            .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss"))));
}

// 统一处理在线/离线界面状态。
// 离线时禁用控制按钮、退出自动靠近并关闭视频，避免用户继续操作失联车辆。
void NetworkPage::updateConnection(bool online, const QString &message)
{
    m_online = online;
    m_connectionState->setText(
        online ? QStringLiteral("●  网络在线")
               : (m_connectionRequested ? QStringLiteral("●  连接中")
                                        : QStringLiteral("●  网络离线")));
    m_connectionState->setProperty("online", online);
    m_connectionState->style()->unpolish(m_connectionState);
    m_connectionState->style()->polish(m_connectionState);

    m_connectButton->setText(
        m_connectionRequested ? (online ? QStringLiteral("断开连接")
                                       : QStringLiteral("取消连接"))
                              : QStringLiteral("连接设备"));
    m_refreshButton->setEnabled(online);
    m_cameraButton->setEnabled(online);
    m_approachButton->setEnabled(online);
    m_ultrasonicButton->setEnabled(online);
    m_applyUltrasonicLimitsButton->setEnabled(
        online && m_ultrasonicLimitsDirty);
    if (!online) {
        setVisionTracking(false);
        setUltrasonicEnabled(false);
    }
    if (!online && m_cameraRequested) {
        stopCameraStream(QStringLiteral("网络断开，视觉画面已停止"));
    }
    setControlButtonsEnabled(online);
    if (!message.isEmpty()) {
        m_networkHint->setText(message);
    }
    if (!message.isEmpty() && message != m_lastConnectionMessage) {
        appendLog(message);
        m_lastConnectionMessage = message;
    }
}

// 统一启用或禁用所有车辆按钮；停止按钮也由列表一起管理。
void NetworkPage::setControlButtonsEnabled(bool enabled)
{
    for (QPushButton *button : std::as_const(m_carButtons)) {
        button->setEnabled(enabled);
    }
}

// 发送标准化后的车辆动作，并立即 flush 小控制包，降低转向延迟。
// 发送成功后同步更新界面动作状态。
void NetworkPage::sendCarAction(const QString &action)
{
    const QString normalized = action.trimmed().toUpper();
    if (!m_online || m_socket->state() != QAbstractSocket::ConnectedState) {
        if (normalized == QStringLiteral("N")) {
            updateMotion(normalized);
        }
        return;
    }
    if (sendSocketLine(normalized.toUtf8())) {
        m_socket->flush();
        updateMotion(normalized);
    } else {
        appendLog(QStringLiteral("控制指令发送失败，请检查 TCP 连接"));
    }
}

// 停止操作会清空所有活动输入，并通过 TRACK_STOP + N 形成双重停止。
void NetworkPage::stopCar()
{
    m_activeCarInputs.clear();
    if (m_visionTracking) {
        sendSocketLine("TRACK_STOP");
        setVisionTracking(false);
    }
    refreshCarButtons();
    updateMotion(QStringLiteral("N"));
    sendCarAction(QStringLiteral("N"));
}

// 更新车辆动作箭头、文字、提示和停止按钮高亮。
// 相同动作直接返回，减少 100 ms 心跳引起的无意义重绘。
void NetworkPage::updateMotion(const QString &action)
{
    QString normalized = action.trimmed().toUpper();
    if (normalized.isEmpty()) {
        normalized = QStringLiteral("N");
    }
    if (m_currentAction == normalized
        && m_carMotion->property("motion").toString()
               == motionName(normalized)) {
        return;
    }
    m_currentAction = normalized;
    m_carMotion->setText(motionGlyph(normalized));
    m_carMotion->setProperty("motion", motionName(normalized));
    m_carMotion->style()->unpolish(m_carMotion);
    m_carMotion->style()->polish(m_carMotion);
    m_statusValue->setText(motionLabel(normalized));
    m_statusHint->setText(motionHint(normalized));

    for (QPushButton *button : std::as_const(m_carButtons)) {
        const QString command = button->property("carCommand").toString();
        if (command == QStringLiteral("N")) {
            button->setChecked(normalized == QStringLiteral("N"));
        }
    }
}

// 将当前所有活动输入合成为一个车辆动作。
// 例如 W+A -> FL；同时按下相反方向时相互抵消。
QString NetworkPage::carWireAction() const
{
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    for (const QString &command : m_activeCarInputs) {
        if (command == QStringLiteral("N")) {
            return QStringLiteral("N");
        }
        forward = forward || command == QStringLiteral("F");
        backward = backward || command == QStringLiteral("B");
        left = left || command == QStringLiteral("L");
        right = right || command == QStringLiteral("R");
    }

    if (forward && backward) {
        forward = false;
        backward = false;
    }
    if (left && right) {
        left = false;
        right = false;
    }

    if (forward && left) return QStringLiteral("FL");
    if (forward && right) return QStringLiteral("FR");
    if (backward && left) return QStringLiteral("BL");
    if (backward && right) return QStringLiteral("BR");
    if (forward) return QStringLiteral("F");
    if (backward) return QStringLiteral("B");
    if (left) return QStringLiteral("L");
    if (right) return QStringLiteral("R");
    return QStringLiteral("N");
}

// 记录一次新的按下输入。id 区分鼠标按钮和不同键盘按键，
// 同一时间可以保留多个输入用于组合转向。
void NetworkPage::startCarInput(const QString &id, const QString &command)
{
    if (!m_online) {
        appendLog(QStringLiteral("控制未启用：请先连接设备"));
        return;
    }
    if (m_visionTracking) {
        stopColorApproach();
    }
    m_activeCarInputs.insert(id, command);
    refreshCarButtons();
    sendCarAction(carWireAction());
}

// 移除一个输入后重新计算剩余组合；
// 如果已经没有任何输入，carWireAction() 会返回 N。
void NetworkPage::endCarInput(const QString &id)
{
    if (!m_activeCarInputs.contains(id)) {
        return;
    }
    m_activeCarInputs.remove(id);
    refreshCarButtons();
    sendCarAction(carWireAction());
}

// 只让当前仍有效的方向按钮保持高亮，停止按钮仅在所有输入为空时点亮。
void NetworkPage::refreshCarButtons()
{
    QList<QString> activeCommands;
    for (const QString &command : std::as_const(m_activeCarInputs)) {
        if (!activeCommands.contains(command)) {
            activeCommands.append(command);
        }
    }
    for (QPushButton *button : std::as_const(m_carButtons)) {
        const QString command = button->property("carCommand").toString();
        if (command == QStringLiteral("N")) {
            button->setChecked(m_activeCarInputs.isEmpty());
        } else {
            button->setChecked(activeCommands.contains(command));
        }
    }
}

// 根据天气文字和代码选择图标、提示语及卡片配色。
// 只有主题真正变化时才刷新样式，减少状态轮询造成的重绘。
void NetworkPage::applyWeatherTheme(const QString &text, const QString &code)
{
    QString theme = QStringLiteral("cloudy");
    QString icon = QStringLiteral("🌤️");
    QString mood = QStringLiteral("天气状态已同步");

    if (text.contains(QStringLiteral("雷")) || code == QStringLiteral("99")) {
        theme = QStringLiteral("thunder");
        icon = QStringLiteral("⛈️");
        mood = QStringLiteral("雷电活动 · 建议留在室内");
    } else if (text.contains(QStringLiteral("雪"))
               || text.contains(QStringLiteral("冰粒"))) {
        theme = QStringLiteral("snow");
        icon = QStringLiteral("🌨️");
        mood = QStringLiteral("降雪天气 · 注意防滑保暖");
    } else if (text.contains(QStringLiteral("雨"))
               || text.contains(QStringLiteral("阵雨"))) {
        theme = QStringLiteral("rain");
        icon = QStringLiteral("🌧️");
        mood = QStringLiteral("有降水 · 出门记得带伞");
    } else if (text.contains(QStringLiteral("雾"))
               || text.contains(QStringLiteral("霾"))) {
        theme = QStringLiteral("fog");
        icon = QStringLiteral("🌫️");
        mood = QStringLiteral("能见度较低 · 出行注意安全");
    } else if (text.contains(QStringLiteral("阴"))
               || text.contains(QStringLiteral("云"))) {
        theme = QStringLiteral("cloudy");
        icon = text.contains(QStringLiteral("多云")) ? QStringLiteral("⛅")
                                                      : QStringLiteral("☁️");
        mood = QStringLiteral("云层柔和 · 光线较为平稳");
    } else if (text.contains(QStringLiteral("晴"))) {
        theme = QStringLiteral("clear");
        icon = code == QStringLiteral("1") ? QStringLiteral("🌙")
                                           : QStringLiteral("☀️");
        mood = QStringLiteral("天空晴朗 · 适合户外活动");
    } else if (text.contains(QStringLiteral("风"))) {
        theme = QStringLiteral("clear");
        icon = QStringLiteral("🌬️");
        mood = QStringLiteral("风力较明显 · 注意行车安全");
    }

    const bool iconChanged = m_weatherIcon->text() != icon;
    const bool themeChanged =
        m_weatherCard->property("weatherTheme").toString() != theme;
    m_weatherIcon->setText(icon);
    m_weatherMood->setText(mood);
    if (themeChanged) {
        m_weatherCard->setProperty("weatherTheme", theme);
        m_weatherCard->style()->unpolish(m_weatherCard);
        m_weatherCard->style()->polish(m_weatherCard);
    }
    if (iconChanged) {
        animateWeatherIcon();
    }
}

// 天气图标变化时播放一次淡入动画，让天气切换更明显。
void NetworkPage::animateWeatherIcon()
{
    if (m_weatherIcon->graphicsEffect()) {
        m_weatherIcon->graphicsEffect()->deleteLater();
        m_weatherIcon->setGraphicsEffect(nullptr);
    }
    auto *effect = new QGraphicsOpacityEffect(m_weatherIcon);
    effect->setOpacity(0.15);
    m_weatherIcon->setGraphicsEffect(effect);
    auto *animation = new QPropertyAnimation(effect, "opacity", m_weatherIcon);
    animation->setDuration(500);
    animation->setStartValue(0.15);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

// 在操作日志前添加本地时间，方便回看连接、发送和错误的发生顺序。
void NetworkPage::appendLog(const QString &text)
{
    m_log->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), text));
}

// 页面重新显示时恢复定时器、焦点、状态请求和视频流。
void NetworkPage::activatePage()
{
    if (!m_refreshTimer->isActive()) {
        m_refreshTimer->start();
    }
    if (!m_heartbeatTimer->isActive()) {
        m_heartbeatTimer->start();
    }
    setFocus(Qt::OtherFocusReason);
    if (m_connectionRequested) {
        fetchData();
    }
    if (m_online) {
        startCameraStream();
    }
}

// 应用切换到后台或窗口失去激活时立即停车，
// 避免键盘释放事件被其他窗口截获后车辆仍继续运动。
bool NetworkPage::eventFilter(QObject *watched, QEvent *event)
{
    if (isVisible()
        && (event->type() == QEvent::ApplicationDeactivate
            || event->type() == QEvent::WindowDeactivate)) {
        stopCar();
    }
    return QWidget::eventFilter(watched, event);
}

// 键盘按下映射：W/↑=F，S/↓=B，A/←=L，D/→=R，空格/Esc=N。
// 忽略 autoRepeat，真正的连续运动由 100 ms 心跳负责。
void NetworkPage::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Escape) {
        stopCar();
        event->accept();
        return;
    }

    QString id;
    QString command;
    switch (event->key()) {
    case Qt::Key_W:
    case Qt::Key_Up:
        id = QStringLiteral("key:forward");
        command = QStringLiteral("F");
        break;
    case Qt::Key_S:
    case Qt::Key_Down:
        id = QStringLiteral("key:backward");
        command = QStringLiteral("B");
        break;
    case Qt::Key_A:
    case Qt::Key_Left:
        id = QStringLiteral("key:left");
        command = QStringLiteral("L");
        break;
    case Qt::Key_D:
    case Qt::Key_Right:
        id = QStringLiteral("key:right");
        command = QStringLiteral("R");
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }

    startCarInput(id, command);
    event->accept();
}

// 键盘松开时只移除对应输入，其他仍按住的按键继续组成动作。
void NetworkPage::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }

    QString id;
    switch (event->key()) {
    case Qt::Key_W:
    case Qt::Key_Up:
        id = QStringLiteral("key:forward");
        break;
    case Qt::Key_S:
    case Qt::Key_Down:
        id = QStringLiteral("key:backward");
        break;
    case Qt::Key_A:
    case Qt::Key_Left:
        id = QStringLiteral("key:left");
        break;
    case Qt::Key_D:
    case Qt::Key_Right:
        id = QStringLiteral("key:right");
        break;
    default:
        QWidget::keyReleaseEvent(event);
        return;
    }

    endCarInput(id);
    event->accept();
}

// 窗口激活状态改变时再次执行停车保护。
void NetworkPage::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange
        && window() && !window()->isActiveWindow() && isVisible()) {
        stopCar();
    }
    QWidget::changeEvent(event);
}

// 显示页面后主动取得键盘焦点，使 W/A/S/D 能立即工作。
void NetworkPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    setFocus(Qt::OtherFocusReason);
}

// 页面隐藏时停止状态轮询、心跳和视频，并清除尚未释放的控制输入。
void NetworkPage::hideEvent(QHideEvent *event)
{
    m_refreshTimer->stop();
    m_heartbeatTimer->stop();
    stopCameraStream();
    if (!m_activeCarInputs.isEmpty()) {
        stopCar();
    }
    QWidget::hideEvent(event);
}
