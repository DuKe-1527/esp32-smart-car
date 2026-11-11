#ifndef NETWORKPAGE_H
#define NETWORKPAGE_H

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QMap>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>

// 前置声明可以减少头文件依赖；具体实现只在 networkpage.cpp 中使用。
class QEvent;
class QComboBox;
class QHideEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTcpSocket;
class QTimer;

class DistanceChart;
class CameraView;

// NetworkPage 是当前 Qt 主界面，也是网络控制层。
// 它同时管理 TCP 控制、MJPEG 视频、颜色识别、超声波展示和键盘/鼠标驾驶。
class NetworkPage : public QWidget
{
    Q_OBJECT

public:
    explicit NetworkPage(QWidget *parent = nullptr);
    ~NetworkPage() override;

    // 页面重新显示时恢复定时器、焦点和摄像头连接。
    void activatePage();

protected:
    // 捕获应用/窗口失焦，确保车辆不会在后台继续保持运动。
    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    // ---------------- 界面与通用交互 ----------------
    void buildInterface();
    QPushButton *createControlButton(const QString &text,
                                     const QString &command,
                                     const QString &shortcut);
    void connectCarButton(QPushButton *button, const QString &command);
    void connectToDevice();
    void fetchData();
    void refreshWeather();
    void startColorApproach();
    void stopColorApproach();
    void setVisionTracking(bool active);
    void analyzeLocalFrame(const QImage &image);
    void sendLocalVisionHint();
    void updateVisionColorFilter();
    void toggleUltrasonic();
    void setUltrasonicEnabled(bool enabled);
    void applyUltrasonicLimits();
    void setUltrasonicLimits(int warningCm, int stopCm, bool force);
    void toggleCameraStream();
    void startCameraStream();
    void stopCameraStream(const QString &message = QString());
    void handleCameraData();
    void updateData(const QByteArray &payload);
    void handleSocketData();
    bool sendSocketLine(const QByteArray &line);
    void updateConnection(bool online, const QString &message);
    void setControlButtonsEnabled(bool enabled);
    void sendCarAction(const QString &action);
    void stopCar();
    void updateMotion(const QString &action);
    QString carWireAction() const;
    void startCarInput(const QString &id, const QString &command);
    void endCarInput(const QString &id);
    void refreshCarButtons();
    void applyWeatherTheme(const QString &text, const QString &code);
    void animateWeatherIcon();
    void appendLog(const QString &text);

    // ---------------- 网络连接与协议 ----------------
    QTcpSocket *m_socket = nullptr;
    QNetworkAccessManager *m_cameraNetwork = nullptr;
    QNetworkReply *m_cameraReply = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QTimer *m_heartbeatTimer = nullptr;
    QString m_tcpHost;
    quint16 m_tcpPort = 9000;
    QByteArray m_socketBuffer;
    QByteArray m_cameraBuffer;

    // ---------------- 车辆控制状态 ----------------
    // m_activeCarInputs 保存当前仍按下的鼠标/键盘输入，
    // 松开其中一个键时，其他输入仍然可以组合成新的车辆动作。
    QString m_currentAction = QStringLiteral("N");
    QString m_lastConnectionMessage;
    QMap<QString, QString> m_activeCarInputs;
    bool m_connectionRequested = false;
    bool m_online = false;
    bool m_cameraRequested = false;
    bool m_visionTracking = false;
    bool m_ultrasonicEnabled = false;
    bool m_ultrasonicLimitsDirty = false;

    // 当前生效的超声波安全距离，默认预警 30 cm、停车 15 cm。
    int m_warningDistanceCm = 30;
    int m_stopDistanceCm = 15;

    // ---------------- Qt 本地颜色识别状态 ----------------
    // 识别框并不是每帧立即切换：颜色和位置都需要短时间确认，
    // 用于抑制背景小色块和摄像头噪声造成的跳框。
    bool m_localVisionDetected = false;
    QString m_localVisionColor;
    QString m_localVisionPendingColor;
    QString m_visionColorFilter = QStringLiteral("auto");
    QRect m_localVisionBox;
    QRect m_localVisionPendingBox;
    QSize m_localVisionFrame;
    qint64 m_localVisionUpdatedAt = 0;
    qint64 m_localVisionLastSeenAt = 0;
    qint64 m_localVisionHintSentAt = 0;
    int m_localVisionPendingColorFrames = 0;
    int m_localVisionPendingBoxFrames = 0;

    // ---------------- 界面控件 ----------------
    QLineEdit *m_ipEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLabel *m_connectionState = nullptr;
    QLabel *m_networkHint = nullptr;
    QLabel *m_statusValue = nullptr;
    QLabel *m_statusHint = nullptr;
    QLabel *m_carMotion = nullptr;
    QLabel *m_weatherIcon = nullptr;
    QLabel *m_weatherText = nullptr;
    QLabel *m_weatherMood = nullptr;
    QLabel *m_temperature = nullptr;
    QLabel *m_weatherObserved = nullptr;
    QLabel *m_weatherError = nullptr;
    QLabel *m_distanceValue = nullptr;
    QLabel *m_distanceState = nullptr;
    QLabel *m_distanceAverage = nullptr;
    QLabel *m_distanceRange = nullptr;
    QLabel *m_distanceTrend = nullptr;
    QLabel *m_networkDetails = nullptr;
    QLabel *m_deviceDetails = nullptr;
    QLabel *m_cameraStatus = nullptr;
    QLabel *m_visionResult = nullptr;
    QLabel *m_obstacleStatus = nullptr;
    QPushButton *m_cameraButton = nullptr;
    QPushButton *m_approachButton = nullptr;
    QComboBox *m_visionColorCombo = nullptr;
    QPushButton *m_ultrasonicButton = nullptr;
    QPushButton *m_applyUltrasonicLimitsButton = nullptr;
    QSpinBox *m_warningDistanceSpin = nullptr;
    QSpinBox *m_stopDistanceSpin = nullptr;
    QLabel *m_ultrasonicThreshold = nullptr;
    CameraView *m_cameraView = nullptr;
    QPlainTextEdit *m_log = nullptr;
    DistanceChart *m_distanceChart = nullptr;
    QWidget *m_weatherCard = nullptr;

    QList<QPushButton *> m_carButtons;
};

#endif // NETWORKPAGE_H
