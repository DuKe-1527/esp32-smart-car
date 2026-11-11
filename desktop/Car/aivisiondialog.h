#ifndef AIVISIONDIALOG_H
#define AIVISIONDIALOG_H

#include <QDialog>
#include <QImage>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;
class QTimer;

class AiVisionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AiVisionDialog(QWidget *parent = nullptr);

    void setLatestFrame(const QImage &image);

signals:
    void resultReady(const QString &summary);

private:
    void buildInterface();
    void analyseCurrentFrame();
    void sendRequest(const QByteArray &body);
    QByteArray buildRequestBody() const;
    QByteArray encodeFrame() const;
    QString buildPrompt() const;
    QString extractResponseText(const QByteArray &payload) const;
    void setBusy(bool busy);
    void updateConfigurationUi();

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_autoTimer = nullptr;
    QImage m_latestFrame;
    bool m_busy = false;

    QComboBox *m_protocolCombo = nullptr;
    QLineEdit *m_endpointEdit = nullptr;
    QComboBox *m_modelCombo = nullptr;
    QLineEdit *m_apiKeyEdit = nullptr;
    QComboBox *m_colorCombo = nullptr;
    QPlainTextEdit *m_targetPrompt = nullptr;
    QPlainTextEdit *m_resultView = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_frameStateLabel = nullptr;
    QPushButton *m_analyseButton = nullptr;
    QPushButton *m_continuousButton = nullptr;
};

#endif // AIVISIONDIALOG_H
