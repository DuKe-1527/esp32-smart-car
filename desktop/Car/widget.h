#ifndef WIDGET_H
#define WIDGET_H

#include <QSerialPort>
#include <QSerialPortInfo>
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui {
class Widget;
}
QT_END_NAMESPACE

class QEvent;
class QKeyEvent;
class QCloseEvent;
class QPushButton;
class QTimer;
class QShowEvent;

class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = nullptr);
    ~Widget() override;

    QPushButton *modeSwitchButton() const;
    void prepareForModeSwitch();

signals:
    void carCommandIssued(const QString &command);
    void networkModeRequested();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void on_portCombo_activated(int index);

private:
    void connectControls();
    void populateSerialOptions();
    void refreshSerialPorts();
    void toggleSerialPort();
    void closeSerialPort();
    void issueCommand(const QString &command);
    void updateCommandIndicator(const QString &command);
    void updateSerialUi(bool connected);
    void appendLog(const QString &text);
    bool writeCommand(const QString &command);

    Ui::Widget *ui;
    QSerialPort *m_serialPort = nullptr;
    QTimer *m_commandHeartbeat = nullptr;
    QString m_currentCommand;
};

#endif // WIDGET_H
