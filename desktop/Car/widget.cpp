#include "widget.h"
#include "ui_widget.h"

#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QEvent>
#include <QFile>
#include <QKeyEvent>
#include <QPushButton>
#include <QSerialPort>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
    , m_serialPort(new QSerialPort(this))
    , m_commandHeartbeat(new QTimer(this))
{
    ui->setupUi(this);
    setAttribute(Qt::WA_StyledBackground, true);
    setFocusPolicy(Qt::StrongFocus);

    QFile styleFile(QStringLiteral(":/style.qss"));
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStyleSheet(QString::fromUtf8(styleFile.readAll()));
    }

    populateSerialOptions();
    connectControls();
    refreshSerialPorts();
    updateSerialUi(false);
    ui->modeButton->setFixedSize(164, 38);

    connect(m_serialPort, &QSerialPort::readyRead, this, [this] {
        const QString received = QString::fromUtf8(m_serialPort->readAll()).trimmed();
        if (!received.isEmpty()) {
            appendLog(QStringLiteral("[接收] %1").arg(received));
        }
    });

    connect(m_serialPort, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::NoError) {
            return;
        }
        appendLog(QStringLiteral("[串口错误] %1").arg(m_serialPort->errorString()));
        if (error == QSerialPort::ResourceError
            || error == QSerialPort::DeviceNotFoundError
            || error == QSerialPort::PermissionError) {
            closeSerialPort();
        }
    });

    m_commandHeartbeat->setInterval(100);
    connect(m_commandHeartbeat, &QTimer::timeout, this, [this] {
        if (!m_serialPort->isOpen()
            || m_currentCommand.isEmpty()
            || m_currentCommand == QStringLiteral("S")) {
            return;
        }
        writeCommand(m_currentCommand);
    });
    m_commandHeartbeat->start();
}

Widget::~Widget()
{
    if (m_serialPort->isOpen()) {
        m_serialPort->write("S\n");
        m_serialPort->waitForBytesWritten(120);
        m_serialPort->close();
    }
    delete ui;
}

void Widget::populateSerialOptions()
{
    ui->baudCombo->clear();
    for (const qint32 baud : {9600, 19200, 38400, 57600, 115200, 230400}) {
        ui->baudCombo->addItem(QString::number(baud), baud);
    }
    ui->baudCombo->setCurrentIndex(ui->baudCombo->findData(115200));

    ui->dataBitsCombo->clear();
    ui->dataBitsCombo->addItem(QStringLiteral("5"), QSerialPort::Data5);
    ui->dataBitsCombo->addItem(QStringLiteral("6"), QSerialPort::Data6);
    ui->dataBitsCombo->addItem(QStringLiteral("7"), QSerialPort::Data7);
    ui->dataBitsCombo->addItem(QStringLiteral("8"), QSerialPort::Data8);
    ui->dataBitsCombo->setCurrentIndex(ui->dataBitsCombo->findData(QSerialPort::Data8));

    ui->stopBitsCombo->clear();
    ui->stopBitsCombo->addItem(QStringLiteral("1"), QSerialPort::OneStop);
    ui->stopBitsCombo->addItem(QStringLiteral("1.5"), QSerialPort::OneAndHalfStop);
    ui->stopBitsCombo->addItem(QStringLiteral("2"), QSerialPort::TwoStop);
    ui->stopBitsCombo->setCurrentIndex(ui->stopBitsCombo->findData(QSerialPort::OneStop));

    ui->parityCombo->clear();
    ui->parityCombo->addItem(QStringLiteral("无校验"), QSerialPort::NoParity);
    ui->parityCombo->addItem(QStringLiteral("偶校验"), QSerialPort::EvenParity);
    ui->parityCombo->addItem(QStringLiteral("奇校验"), QSerialPort::OddParity);
    ui->parityCombo->addItem(QStringLiteral("标记"), QSerialPort::MarkParity);
    ui->parityCombo->addItem(QStringLiteral("空格"), QSerialPort::SpaceParity);
    ui->parityCombo->setCurrentIndex(ui->parityCombo->findData(QSerialPort::NoParity));
}

void Widget::connectControls()
{
    const auto bindDirection = [this](QPushButton *button, const QString &command) {
        connect(button, &QPushButton::pressed, this, [this, command] {
            issueCommand(command);
        });
        connect(button, &QPushButton::released, this, [this] {
            issueCommand(QStringLiteral("S"));
        });
    };

    bindDirection(ui->forwardButton, QStringLiteral("F"));
    bindDirection(ui->backwardButton, QStringLiteral("B"));
    bindDirection(ui->leftButton, QStringLiteral("L"));
    bindDirection(ui->rightButton, QStringLiteral("R"));

    connect(ui->stopButton, &QPushButton::clicked, this, [this] {
        issueCommand(QStringLiteral("S"));
    });
    connect(ui->refreshPortsButton, &QPushButton::clicked,
            this, &Widget::refreshSerialPorts);
    connect(ui->openSerialButton, &QPushButton::clicked,
            this, &Widget::toggleSerialPort);
    connect(ui->modeButton, &QPushButton::clicked,
            this, &Widget::networkModeRequested);
}

QPushButton *Widget::modeSwitchButton() const
{
    return ui->modeButton;
}

void Widget::prepareForModeSwitch()
{
    if (m_serialPort->isOpen()) {
        issueCommand(QStringLiteral("S"));
    }
    closeSerialPort();
    setFocus(Qt::OtherFocusReason);
}

void Widget::refreshSerialPorts()
{
    const QString previousPort = ui->portCombo->currentData().toString();
    ui->portCombo->clear();

    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &port : ports) {
        const QString description = port.description().trimmed();
        const QString display = description.isEmpty()
                                    ? port.portName()
                                    : QStringLiteral("%1 · %2").arg(port.portName(), description);
        ui->portCombo->addItem(display, port.portName());
    }

    if (ui->portCombo->count() == 0) {
        ui->portCombo->addItem(QStringLiteral("未检测到串口"), QString());
        ui->openSerialButton->setEnabled(false);
        ui->portHint->setText(QStringLiteral("请连接 USB 串口设备后点击刷新"));
    } else {
        const int previousIndex = ui->portCombo->findData(previousPort);
        if (previousIndex >= 0) {
            ui->portCombo->setCurrentIndex(previousIndex);
        }
        ui->openSerialButton->setEnabled(true);
        ui->portHint->setText(QStringLiteral("检测到 %1 个串口").arg(ports.size()));
    }
}

void Widget::toggleSerialPort()
{
    if (m_serialPort->isOpen()) {
        closeSerialPort();
        return;
    }

    const QString portName = ui->portCombo->currentData().toString();
    if (portName.isEmpty()) {
        appendLog(QStringLiteral("[提示] 请选择一个可用串口"));
        return;
    }

    m_serialPort->setPortName(portName);
    m_serialPort->setBaudRate(ui->baudCombo->currentData().toInt());
    m_serialPort->setDataBits(
        static_cast<QSerialPort::DataBits>(ui->dataBitsCombo->currentData().toInt()));
    m_serialPort->setStopBits(
        static_cast<QSerialPort::StopBits>(ui->stopBitsCombo->currentData().toInt()));
    m_serialPort->setParity(
        static_cast<QSerialPort::Parity>(ui->parityCombo->currentData().toInt()));
    m_serialPort->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serialPort->open(QIODevice::ReadWrite)) {
        appendLog(QStringLiteral("[打开失败] %1").arg(m_serialPort->errorString()));
        updateSerialUi(false);
        return;
    }

    // ESP32-CAM auto-reset circuits use these modem-control lines.
    // Release them after opening so the uploaded sketch actually runs.
    m_serialPort->setDataTerminalReady(false);
    m_serialPort->setRequestToSend(false);

    updateSerialUi(true);
    m_currentCommand.clear();
    writeCommand(QStringLiteral("S"));
    QTimer::singleShot(1200, this, [this] {
        if (m_serialPort->isOpen()) {
            writeCommand(QStringLiteral("S"));
        }
    });
    appendLog(QStringLiteral("[串口] 已打开 %1 @ %2 bps")
                  .arg(portName, ui->baudCombo->currentText()));
}

void Widget::on_portCombo_activated(int index)
{
    if (index < 0 || m_serialPort->isOpen()) {
        return;
    }

    const QString portName = ui->portCombo->currentData().toString();
    ui->openSerialButton->setEnabled(!portName.isEmpty());
    ui->portHint->setText(
        portName.isEmpty()
            ? QStringLiteral("请连接 USB 串口设备后点击刷新")
            : QStringLiteral("已选择 %1，可以打开串口").arg(portName));
}

void Widget::closeSerialPort()
{
    if (m_serialPort->isOpen()) {
        m_serialPort->write("S\n");
        m_serialPort->waitForBytesWritten(120);
        appendLog(QStringLiteral("[串口] 已关闭，并发送停车指令 S"));
        m_serialPort->close();
    }
    m_currentCommand.clear();
    updateCommandIndicator(QStringLiteral("S"));
    updateSerialUi(false);
}

void Widget::issueCommand(const QString &command)
{
    if (!m_serialPort->isOpen()) {
        appendLog(QStringLiteral("[提示] 请先打开串口"));
        return;
    }
    if (command == m_currentCommand) {
        updateCommandIndicator(command);
        return;
    }
    if (!writeCommand(command)) {
        return;
    }

    m_currentCommand = command;
    updateCommandIndicator(command);

    QString action;
    QString description;
    if (command == QStringLiteral("F")) {
        action = QStringLiteral("前进中");
        description = QStringLiteral("小车正在向前行驶");
    } else if (command == QStringLiteral("B")) {
        action = QStringLiteral("后退中");
        description = QStringLiteral("小车正在向后行驶");
    } else if (command == QStringLiteral("L")) {
        action = QStringLiteral("左转中");
        description = QStringLiteral("小车正在向左转向");
    } else if (command == QStringLiteral("R")) {
        action = QStringLiteral("右转中");
        description = QStringLiteral("小车正在向右转向");
    } else {
        action = QStringLiteral("已停止");
        description = QStringLiteral("电机已进入安全停车状态");
    }

    ui->statusValue->setText(action);
    ui->commandValue->setText(QStringLiteral("%1 · %2").arg(command, action));
    ui->statusHint->setText(description);
    appendLog(QStringLiteral("[发送] %1  %2").arg(command, action));
    emit carCommandIssued(command);
}

bool Widget::writeCommand(const QString &command)
{
    const QByteArray payload = command.toUtf8() + '\n';
    if (m_serialPort->write(payload) != payload.size()) {
        appendLog(QStringLiteral("[发送失败] %1").arg(m_serialPort->errorString()));
        return false;
    }
    return true;
}

void Widget::updateCommandIndicator(const QString &command)
{
    ui->forwardButton->setChecked(command == QStringLiteral("F"));
    ui->backwardButton->setChecked(command == QStringLiteral("B"));
    ui->leftButton->setChecked(command == QStringLiteral("L"));
    ui->rightButton->setChecked(command == QStringLiteral("R"));
    ui->stopButton->setChecked(command == QStringLiteral("S"));
}

void Widget::updateSerialUi(bool connected)
{
    const bool controlsEnabled = connected;
    for (QPushButton *button : {ui->forwardButton, ui->backwardButton,
                                ui->leftButton, ui->rightButton,
                                ui->stopButton}) {
        button->setEnabled(controlsEnabled);
    }

    ui->portCombo->setEnabled(!connected);
    ui->baudCombo->setEnabled(!connected);
    ui->dataBitsCombo->setEnabled(!connected);
    ui->stopBitsCombo->setEnabled(!connected);
    ui->parityCombo->setEnabled(!connected);
    ui->refreshPortsButton->setEnabled(!connected);
    ui->openSerialButton->setText(
        connected ? QStringLiteral("关闭串口") : QStringLiteral("打开串口"));
    ui->serialStateBadge->setText(
        connected ? QStringLiteral("ONLINE") : QStringLiteral("OFFLINE"));
    ui->serialStateBadge->setProperty("online", connected);

    if (!connected) {
        ui->statusValue->setText(QStringLiteral("串口未连接"));
        ui->commandValue->setText(QStringLiteral("-- · 等待连接"));
        ui->statusHint->setText(QStringLiteral("选择串口参数后打开串口"));
    } else {
        ui->statusValue->setText(QStringLiteral("设备已就绪"));
        ui->commandValue->setText(QStringLiteral("S · 等待控制指令"));
        ui->statusHint->setText(QStringLiteral("方向按钮已启用，松开即停车"));
    }

    ui->serialStateBadge->style()->unpolish(ui->serialStateBadge);
    ui->serialStateBadge->style()->polish(ui->serialStateBadge);
}

void Widget::appendLog(const QString &text)
{
    ui->commandLog->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), text));
}

void Widget::keyPressEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    if (!m_serialPort->isOpen()) {
        QWidget::keyPressEvent(event);
        return;
    }

    switch (event->key()) {
    case Qt::Key_W:
        issueCommand(QStringLiteral("F"));
        break;
    case Qt::Key_S:
        issueCommand(QStringLiteral("B"));
        break;
    case Qt::Key_A:
        issueCommand(QStringLiteral("L"));
        break;
    case Qt::Key_D:
        issueCommand(QStringLiteral("R"));
        break;
    case Qt::Key_Space:
    case Qt::Key_Escape:
        issueCommand(QStringLiteral("S"));
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void Widget::keyReleaseEvent(QKeyEvent *event)
{
    if (event->isAutoRepeat()) {
        event->accept();
        return;
    }
    if (!m_serialPort->isOpen()) {
        QWidget::keyReleaseEvent(event);
        return;
    }

    switch (event->key()) {
    case Qt::Key_W:
    case Qt::Key_S:
    case Qt::Key_A:
    case Qt::Key_D:
        issueCommand(QStringLiteral("S"));
        event->accept();
        return;
    default:
        QWidget::keyReleaseEvent(event);
    }
}

void Widget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::ActivationChange
        && !isActiveWindow()
        && m_serialPort->isOpen()) {
        issueCommand(QStringLiteral("S"));
    }
    QWidget::changeEvent(event);
}

void Widget::closeEvent(QCloseEvent *event)
{
    if (m_serialPort->isOpen()) {
        m_serialPort->write("S\n");
        m_serialPort->waitForBytesWritten(120);
        m_serialPort->close();
    }
    QWidget::closeEvent(event);
}

void Widget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    setFocus(Qt::OtherFocusReason);
}
