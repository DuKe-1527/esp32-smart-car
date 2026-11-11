#include "aivisiondialog.h"

#include <QBuffer>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

AiVisionDialog::AiVisionDialog(QWidget *parent)
    : QDialog(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_autoTimer(new QTimer(this))
{
    setWindowTitle(QStringLiteral("AI 视觉识别配置"));
    setMinimumSize(780, 700);
    resize(820, 730);
    setModal(false);
    buildInterface();

    m_autoTimer->setInterval(3500);
    connect(m_autoTimer, &QTimer::timeout,
            this, &AiVisionDialog::analyseCurrentFrame);
    connect(this, &QDialog::finished, this, [this] {
        m_autoTimer->stop();
        m_continuousButton->setChecked(false);
        m_continuousButton->setText(QStringLiteral("连续检测"));
    });
}

void AiVisionDialog::buildInterface()
{
    setStyleSheet(QStringLiteral(R"QSS(
        QDialog {
            background: qlineargradient(
                x1: 0, y1: 0, x2: 1, y2: 1,
                stop: 0 #fff7e8, stop: 0.5 #f0fbf7, stop: 1 #e4f5f4
            );
            color: #19363c;
            font-family: "Microsoft YaHei UI";
        }
        QLabel {
            background: transparent;
            color: #19363c;
        }
        QLabel#dialogTitle {
            color: #123b43;
            font-size: 24px;
            font-weight: 700;
        }
        QLabel#dialogSubtitle {
            color: #6f8d8e;
            font-size: 12px;
        }
        QLabel#fieldLabel {
            color: #537772;
            font-size: 11px;
            font-weight: 600;
        }
        QLineEdit, QComboBox, QPlainTextEdit {
            background: #ffffff;
            border: 1px solid #c9e0da;
            border-radius: 9px;
            color: #21464c;
            font-size: 12px;
            padding: 6px 9px;
            selection-background-color: #c9eee5;
        }
        QLineEdit:focus, QComboBox:focus, QPlainTextEdit:focus {
            border-color: #25a996;
        }
        QPushButton {
            background: #149f8d;
            border: 1px solid #0f8f80;
            border-radius: 9px;
            color: #ffffff;
            font-size: 12px;
            font-weight: 700;
            padding: 7px 15px;
        }
        QPushButton:hover {
            background: #19b09c;
        }
        QPushButton:disabled {
            background: #a8c8c2;
            border-color: #93b9b2;
            color: #edf8f5;
        }
        QPushButton#secondaryButton {
            background: #f7fbfa;
            border-color: #c8e1db;
            color: #47766f;
        }
        QPushButton#secondaryButton:hover {
            background: #e9f8f3;
        }
        QPushButton#continuousButton:checked {
            background: #dff5ed;
            border-color: #72c9b9;
            color: #126d60;
        }
        QFrame#infoPanel {
            background: rgba(255, 255, 255, 0.86);
            border: 1px solid #d7e9e4;
            border-radius: 16px;
        }
        QLabel#statusLabel {
            color: #5d7e7a;
            font-size: 11px;
        }
        QLabel#frameStateLabel {
            color: #16806e;
            font-size: 11px;
            font-weight: 600;
        }
        QLabel#privacyLabel {
            color: #8b6b57;
            font-size: 10px;
        }
    )QSS"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(26, 22, 26, 22);
    root->setSpacing(13);

    auto *title = new QLabel(QStringLiteral("AI 视觉识别"));
    title->setObjectName(QStringLiteral("dialogTitle"));
    auto *subtitle = new QLabel(
        QStringLiteral("使用云端或本地 OpenAI-compatible 视觉模型分析当前摄像头画面"));
    subtitle->setObjectName(QStringLiteral("dialogSubtitle"));
    root->addWidget(title);
    root->addWidget(subtitle);

    auto *configPanel = new QFrame;
    configPanel->setObjectName(QStringLiteral("infoPanel"));
    auto *form = new QFormLayout(configPanel);
    form->setContentsMargins(18, 16, 18, 16);
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(11);

    m_protocolCombo = new QComboBox;
    m_protocolCombo->addItem(QStringLiteral("OpenAI Responses API"),
                             QStringLiteral("responses"));
    m_protocolCombo->addItem(QStringLiteral("OpenAI-compatible Chat"),
                             QStringLiteral("chat"));
    form->addRow(QStringLiteral("接口格式"), m_protocolCombo);

    m_endpointEdit = new QLineEdit(
        QStringLiteral("https://api.openai.com/v1/responses"));
    m_endpointEdit->setPlaceholderText(
        QStringLiteral("完整请求地址或本地模型服务地址"));
    form->addRow(QStringLiteral("接口地址"), m_endpointEdit);

    m_modelCombo = new QComboBox;
    m_modelCombo->setEditable(true);
    m_modelCombo->addItems({
        QStringLiteral("gpt-4.1-mini"),
        QStringLiteral("gpt-4o-mini"),
        QStringLiteral("gpt-4.1"),
        QStringLiteral("qwen2.5vl:7b"),
        QStringLiteral("llava:latest"),
        QStringLiteral("gemma3:12b"),
    });
    form->addRow(QStringLiteral("模型"), m_modelCombo);

    m_apiKeyEdit = new QLineEdit;
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText(
        QStringLiteral("本地模型通常可以留空"));
    form->addRow(QStringLiteral("API Key"), m_apiKeyEdit);

    m_colorCombo = new QComboBox;
    m_colorCombo->addItem(QStringLiteral("红色和绿色都检测"),
                          QStringLiteral("auto"));
    m_colorCombo->addItem(QStringLiteral("只检测红色"),
                          QStringLiteral("red"));
    m_colorCombo->addItem(QStringLiteral("只检测绿色"),
                          QStringLiteral("green"));
    form->addRow(QStringLiteral("颜色约束"), m_colorCombo);

    m_targetPrompt = new QPlainTextEdit;
    m_targetPrompt->setMinimumHeight(92);
    m_targetPrompt->setPlainText(
        QStringLiteral("请找出画面中的红/绿色标识物或用户指定物品，"
                       "说明颜色、左右位置、远近和是否存在障碍。"));
    form->addRow(QStringLiteral("目标描述"), m_targetPrompt);

    root->addWidget(configPanel);

    auto *privacy = new QLabel(
        QStringLiteral("API Key 仅保存在当前程序内存中，不写入配置文件。"
                       "点击检测会把当前画面发送到所配置的接口。"));
    privacy->setObjectName(QStringLiteral("privacyLabel"));
    privacy->setWordWrap(true);
    root->addWidget(privacy);

    auto *actions = new QHBoxLayout;
    m_frameStateLabel = new QLabel(QStringLiteral("尚未收到视频帧"));
    m_frameStateLabel->setObjectName(QStringLiteral("frameStateLabel"));
    m_analyseButton = new QPushButton(QStringLiteral("检测当前画面"));
    m_continuousButton = new QPushButton(QStringLiteral("连续检测"));
    m_continuousButton->setObjectName(QStringLiteral("continuousButton"));
    m_continuousButton->setCheckable(true);
    m_continuousButton->setProperty("kind", QStringLiteral("secondary"));
    actions->addWidget(m_frameStateLabel, 1);
    actions->addWidget(m_analyseButton);
    actions->addWidget(m_continuousButton);
    root->addLayout(actions);

    m_statusLabel = new QLabel(QStringLiteral("等待配置"));
    m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_resultView = new QPlainTextEdit;
    m_resultView->setReadOnly(true);
    m_resultView->setPlaceholderText(QStringLiteral("AI 检测结果将在这里显示"));
    root->addWidget(m_resultView, 1);

    auto *closeButton = new QPushButton(QStringLiteral("关闭"));
    closeButton->setObjectName(QStringLiteral("secondaryButton"));
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton);
    root->addLayout(buttonRow);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    connect(m_analyseButton, &QPushButton::clicked,
            this, &AiVisionDialog::analyseCurrentFrame);
    connect(m_continuousButton, &QPushButton::toggled, this, [this](bool enabled) {
        if (enabled) {
            m_autoTimer->start();
            m_continuousButton->setText(QStringLiteral("停止连续检测"));
            analyseCurrentFrame();
        } else {
            m_autoTimer->stop();
            m_continuousButton->setText(QStringLiteral("连续检测"));
        }
    });
    connect(m_protocolCombo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int) {
        const QString protocol =
            m_protocolCombo->currentData().toString();
        if (protocol == QStringLiteral("chat")
            && m_endpointEdit->text()
                   == QStringLiteral("https://api.openai.com/v1/responses")) {
            m_endpointEdit->setText(
                QStringLiteral("https://api.openai.com/v1/chat/completions"));
        } else if (protocol == QStringLiteral("responses")
                   && m_endpointEdit->text()
                          == QStringLiteral("https://api.openai.com/v1/chat/completions")) {
            m_endpointEdit->setText(
                QStringLiteral("https://api.openai.com/v1/responses"));
        }
    });
    connect(this, &QDialog::finished, this, [this] {
        if (m_reply) {
            m_reply->abort();
        }
    });
}

void AiVisionDialog::setLatestFrame(const QImage &image)
{
    if (image.isNull()) {
        return;
    }
    m_latestFrame = image;
    m_frameStateLabel->setText(
        QStringLiteral("最新画面：%1×%2").arg(image.width()).arg(image.height()));
    updateConfigurationUi();
}

QString AiVisionDialog::buildPrompt() const
{
    const QString color = m_colorCombo->currentData().toString();
    QString colorInstruction = QStringLiteral("同时关注红色和绿色目标");
    if (color == QStringLiteral("red")) {
        colorInstruction = QStringLiteral("只关注红色目标");
    } else if (color == QStringLiteral("green")) {
        colorInstruction = QStringLiteral("只关注绿色目标");
    }
    return QStringLiteral(
               "你是智能小车的视觉识别助手。%1。用户目标：%2。"
               "请只依据图片实际内容回答，不要猜测；用简洁中文说明"
               "是否找到、颜色、水平位置、相对远近和明显障碍。")
        .arg(colorInstruction, m_targetPrompt->toPlainText().trimmed());
}

QByteArray AiVisionDialog::encodeFrame() const
{
    QImage image = m_latestFrame;
    if (image.width() > 768 || image.height() > 768) {
        image = image.scaled(768, 768, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPG", 82);
    return bytes.toBase64();
}

QByteArray AiVisionDialog::buildRequestBody() const
{
    const QString prompt = buildPrompt();
    const QByteArray imageData = encodeFrame();
    const QString imageUrl =
        QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(imageData);
    const QString model = m_modelCombo->currentText().trimmed();
    const QString protocol = m_protocolCombo->currentData().toString();

    QJsonObject root;
    root.insert(QStringLiteral("model"), model);
    if (protocol == QStringLiteral("responses")) {
        QJsonObject textPart;
        textPart.insert(QStringLiteral("type"), QStringLiteral("input_text"));
        textPart.insert(QStringLiteral("text"), prompt);
        QJsonObject imagePart;
        imagePart.insert(QStringLiteral("type"), QStringLiteral("input_image"));
        imagePart.insert(QStringLiteral("image_url"), imageUrl);
        QJsonArray content{textPart, imagePart};
        QJsonObject message;
        message.insert(QStringLiteral("role"), QStringLiteral("user"));
        message.insert(QStringLiteral("content"), content);
        root.insert(QStringLiteral("input"), QJsonArray{message});
    } else {
        QJsonObject textPart;
        textPart.insert(QStringLiteral("type"), QStringLiteral("text"));
        textPart.insert(QStringLiteral("text"), prompt);
        QJsonObject imageUrlObject;
        imageUrlObject.insert(QStringLiteral("url"), imageUrl);
        QJsonObject imagePart;
        imagePart.insert(QStringLiteral("type"), QStringLiteral("image_url"));
        imagePart.insert(QStringLiteral("image_url"), imageUrlObject);
        QJsonArray content{textPart, imagePart};
        QJsonObject message;
        message.insert(QStringLiteral("role"), QStringLiteral("user"));
        message.insert(QStringLiteral("content"), content);
        root.insert(QStringLiteral("messages"), QJsonArray{message});
        root.insert(QStringLiteral("max_tokens"), 400);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

void AiVisionDialog::analyseCurrentFrame()
{
    if (m_busy) {
        return;
    }
    if (m_latestFrame.isNull()) {
        m_statusLabel->setText(QStringLiteral("尚未收到视频帧，请先连接车辆并开启视频。"));
        return;
    }
    const QString endpoint = m_endpointEdit->text().trimmed();
    const QUrl url(endpoint);
    if (!url.isValid() || url.host().isEmpty()) {
        m_statusLabel->setText(QStringLiteral("接口地址无效。"));
        return;
    }
    if (m_modelCombo->currentText().trimmed().isEmpty()) {
        m_statusLabel->setText(QStringLiteral("请填写视觉模型名称。"));
        return;
    }
    if (m_apiKeyEdit->text().trimmed().isEmpty()
        && url.host() != QStringLiteral("localhost")
        && url.host() != QStringLiteral("127.0.0.1")) {
        m_statusLabel->setText(
            QStringLiteral("当前远程接口没有 API Key。请填写密钥，"
                           "或改用本地模型服务。"));
        return;
    }
    sendRequest(buildRequestBody());
}

void AiVisionDialog::sendRequest(const QByteArray &body)
{
    QNetworkRequest request(QUrl(m_endpointEdit->text().trimmed()));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Cache-Control", "no-store");
    request.setTransferTimeout(45000);
    const QString apiKey = m_apiKeyEdit->text().trimmed();
    if (!apiKey.isEmpty()) {
        request.setRawHeader("Authorization",
                             QByteArray("Bearer ") + apiKey.toUtf8());
    }

    setBusy(true);
    m_statusLabel->setText(QStringLiteral("正在发送当前画面到 AI 模型…"));
    m_reply = m_network->post(request, body);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        setBusy(false);
        if (!reply) {
            return;
        }
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray payload = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || status < 200
            || status >= 300) {
            QString detail = QString::fromUtf8(payload).trimmed();
            if (detail.size() > 600) {
                detail = detail.left(600) + QStringLiteral("…");
            }
            m_statusLabel->setText(
                QStringLiteral("AI 请求失败：HTTP %1  %2")
                    .arg(status)
                    .arg(reply->errorString()));
            m_resultView->setPlainText(
                detail.isEmpty() ? reply->errorString() : detail);
        } else {
            const QString result = extractResponseText(payload);
            if (result.isEmpty()) {
                m_statusLabel->setText(
                    QStringLiteral("接口已响应，但未找到可显示的文本结果。"));
                m_resultView->setPlainText(QString::fromUtf8(payload).left(1500));
            } else {
                m_statusLabel->setText(
                    QStringLiteral("AI 检测完成：%1")
                        .arg(QDateTime::currentDateTime()
                                 .toString(QStringLiteral("HH:mm:ss"))));
                m_resultView->setPlainText(result);
                emit resultReady(result);
            }
        }
        reply->deleteLater();
    });
}

QString AiVisionDialog::extractResponseText(const QByteArray &payload) const
{
    const QJsonDocument document = QJsonDocument::fromJson(payload);
    if (!document.isObject()) {
        return {};
    }
    const QJsonObject root = document.object();

    const QString direct = root.value(QStringLiteral("output_text")).toString();
    if (!direct.isEmpty()) {
        return direct;
    }

    QStringList parts;
    const QJsonArray output = root.value(QStringLiteral("output")).toArray();
    for (const QJsonValue &outputValue : output) {
        const QJsonObject outputObject = outputValue.toObject();
        const QJsonArray content =
            outputObject.value(QStringLiteral("content")).toArray();
        for (const QJsonValue &contentValue : content) {
            const QJsonObject contentObject = contentValue.toObject();
            const QString type =
                contentObject.value(QStringLiteral("type")).toString();
            if (type == QStringLiteral("output_text")
                || type == QStringLiteral("text")) {
                const QString text =
                    contentObject.value(QStringLiteral("text")).toString();
                if (!text.isEmpty()) {
                    parts.append(text);
                }
            }
        }
    }

    const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
    for (const QJsonValue &choiceValue : choices) {
        const QJsonObject message =
            choiceValue.toObject().value(QStringLiteral("message")).toObject();
        const QJsonValue content = message.value(QStringLiteral("content"));
        if (content.isString()) {
            parts.append(content.toString());
        } else if (content.isArray()) {
            for (const QJsonValue &partValue : content.toArray()) {
                const QString text =
                    partValue.toObject().value(QStringLiteral("text")).toString();
                if (!text.isEmpty()) {
                    parts.append(text);
                }
            }
        }
    }
    return parts.join('\n').trimmed();
}

void AiVisionDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_analyseButton->setEnabled(!busy && !m_latestFrame.isNull());
    updateConfigurationUi();
}

void AiVisionDialog::updateConfigurationUi()
{
    const bool hasFrame = !m_latestFrame.isNull();
    m_frameStateLabel->setText(
        hasFrame
            ? QStringLiteral("最新画面：%1×%2")
                  .arg(m_latestFrame.width())
                  .arg(m_latestFrame.height())
            : QStringLiteral("尚未收到视频帧"));
    if (!m_busy) {
        m_analyseButton->setEnabled(hasFrame);
        m_continuousButton->setEnabled(hasFrame);
    }
}
