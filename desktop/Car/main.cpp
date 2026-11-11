// 程序入口：创建 Qt 应用和主窗口。
// 主界面由 NetworkPage 自己创建，main.cpp 只负责启动窗口及自动化预览参数。
#include "networkpage.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTimer>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 当前工程只保留网络控制页：摄像头、TCP、颜色识别、超声波和驾驶都在这里。
    NetworkPage window;
    window.setWindowTitle(QStringLiteral("智能小车 AI 视觉控制台"));
    window.resize(1180, 820);
    window.show();

    // 以下参数仅用于开发阶段的自动截图：
    // --ui-preview <目录> 启用预览模式
    // --tcp-preview、--vision-color、--ultrasonic-preview、--track-preview
    // 可以分别模拟连接设备、选择颜色、开启超声波和启动自动靠近。
    const QStringList arguments = app.arguments();
    const int previewIndex =
        arguments.indexOf(QStringLiteral("--ui-preview"));
    if (previewIndex >= 0 && previewIndex + 1 < arguments.size()) {
        const QDir output(arguments.at(previewIndex + 1));
        QDir().mkpath(output.absolutePath());

        const int tcpPreviewIndex =
            arguments.indexOf(QStringLiteral("--tcp-preview"));
        const QString tcpPreview =
            tcpPreviewIndex >= 0 && tcpPreviewIndex + 1 < arguments.size()
                ? arguments.at(tcpPreviewIndex + 1)
                : QString();
        const int colorIndex =
            arguments.indexOf(QStringLiteral("--vision-color"));
        const QString visionColor =
            colorIndex >= 0 && colorIndex + 1 < arguments.size()
                ? arguments.at(colorIndex + 1)
                : QString();
        const bool trackPreview =
            arguments.contains(QStringLiteral("--track-preview"));
        const bool ultrasonicPreview =
            arguments.contains(QStringLiteral("--ultrasonic-preview"));

        // 延迟执行可以等待界面完成布局、控件对象名注册和初始状态刷新。
        QTimer::singleShot(700, &window, [&window, tcpPreview] {
            if (tcpPreview.isEmpty()) {
                return;
            }
            if (auto *address = window.findChild<QLineEdit *>(
                    QStringLiteral("networkAddressEdit"))) {
                address->setText(tcpPreview);
            }
            if (auto *button = window.findChild<QPushButton *>(
                    QStringLiteral("networkConnectButton"))) {
                button->click();
            }
        });
        QTimer::singleShot(1100, &window, [&window, visionColor] {
            if (visionColor.isEmpty()) {
                return;
            }
            if (auto *combo = window.findChild<QComboBox *>(
                    QStringLiteral("visionColorCombo"))) {
                const int index = combo->findData(visionColor);
                if (index >= 0) {
                    combo->setCurrentIndex(index);
                }
            }
        });
        QTimer::singleShot(1250, &window, [&window, ultrasonicPreview] {
            if (!ultrasonicPreview) {
                return;
            }
            if (auto *button = window.findChild<QPushButton *>(
                    QStringLiteral("ultrasonicToggle"))) {
                button->click();
            }
        });
        QTimer::singleShot(1350, &window, [&window, trackPreview] {
            if (!trackPreview) {
                return;
            }
            if (auto *button = window.findChild<QPushButton *>(
                    QStringLiteral("approachButton"))) {
                button->click();
            }
        });
        QTimer::singleShot(2300, &window, [&window, output] {
            window.grab().save(output.filePath(QStringLiteral("ai-console.png")));
            qApp->quit();
        });
    }

    return app.exec();
}
