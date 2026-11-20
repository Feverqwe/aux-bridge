#include "audio_bridge.h"
#include "launcher.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QMenu>
#include <QPainter>
#include <QSystemTrayIcon>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
class Window final : public QDialog {
public:
    Window() {
        setWindowTitle("Aux Bridge");
        setSizeGripEnabled(true);

        auto* content = new QVBoxLayout(this);
        auto* devices = new QFormLayout;
        devices->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        setupDevice(*devices, input_, "&Input", "Input");
        setupDevice(*devices, output_, "&Output", "Output");
        content->addLayout(devices);

        status_.setWordWrap(true);
        status_.setAccessibleName("Playback status");
        status_.setToolTip("The bridge queue excludes the delay of the audio devices.");

        auto* footer = new QHBoxLayout;
        footer->addWidget(&status_, 1);
        footer->addWidget(&toggle_);
        content->addLayout(footer);
        toggle_.setDefault(true);
        connect(&toggle_, &QPushButton::clicked, this, [this] { toggle(); });

        refresh();
        resize(460, sizeHint().height());
        input_.setFocus();

        connect(&timer_, &QTimer::timeout, this, [this] { refresh(); });
        timer_.start(350);
    }

private:
    static void setupDevice(QFormLayout& form, QComboBox& combo,
                            const QString& label, const QString& accessibleName) {
        combo.setAccessibleName(accessibleName);
        combo.setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo.setMinimumContentsLength(22);
        form.addRow(label, &combo);
    }

    static AudioDeviceID selected(const QComboBox& combo) { return combo.currentData().toUInt(); }

    static void populate(QComboBox& combo, const std::vector<AudioDevice>& devices, bool input) {
        const auto previous = selected(combo);
        const auto fallback = defaultAudioDevice(input);
        const QSignalBlocker blocker(&combo);
        combo.clear();
        for (const auto& info : devices) {
            if (input ? info.inputChannels : info.outputChannels)
                combo.addItem(QString::fromStdString(info.name), info.id);
        }
        const auto kept = combo.findData(previous);
        const auto system = combo.findData(fallback);
        combo.setCurrentIndex(kept >= 0 ? kept : system >= 0 ? system : combo.count() > 0 ? 0 : -1);
        combo.setPlaceholderText(input ? "No audio inputs" : "No audio outputs");
    }

    void refresh() {
        if (bridge_.running()) {
            const auto error = bridge_.streamError();
            if (error != noErr || bridge_.routeChanged()) {
                bridge_.stop();
                message_ = error != noErr
                    ? QString("Audio stopped (Core Audio %1).").arg(error)
                    : QString("An audio device changed. Select the route and start again.");
            } else {
                const auto queue = QString("Bridge queue %1 ms").arg(bridge_.queueMilliseconds(), 0, 'f', 1);
                const auto drops = bridge_.dropouts();
                status_.setText(drops ? queue + QString(" · Dropouts %1").arg(drops) : queue);
                return;
            }
        }
        if (waiting_)
            return;

        auto devices = audioDevices();
        if (devices) {
            const auto sameDevice = [](const AudioDevice& a, const AudioDevice& b) {
                return a.id == b.id && a.name == b.name &&
                       a.inputChannels == b.inputChannels && a.outputChannels == b.outputChannels;
            };
            if (!populated_ || !std::ranges::equal(*devices, devices_, sameDevice)) {
                populate(input_, *devices, true);
                populate(output_, *devices, false);
                devices_ = std::move(*devices);
                populated_ = true;
            }
        }
        const bool ready = devices && selected(input_) && selected(output_);
        input_.setEnabled(devices && input_.count() > 0);
        output_.setEnabled(devices && output_.count() > 0);
        toggle_.setText("Start");
        toggle_.setEnabled(ready);
        if (!devices)
            status_.setText(QString("Could not list audio devices (Core Audio %1).").arg(devices.error()));
        else if (!ready)
            status_.setText("Connect an audio input and output to start.");
        else
            status_.setText(message_.isEmpty() ? "Ready to route audio." : message_);
    }

    void toggle() {
        if (bridge_.running()) {
            bridge_.stop();
            message_.clear();
            refresh();
            return;
        }
        waiting_ = true;
        message_.clear();
        input_.setEnabled(false);
        output_.setEnabled(false);
        toggle_.setEnabled(false);
        status_.setText("Checking microphone access…");
        permissionWindow_ = this;
        requestMicrophoneAccess([](bool allowed) {
            if (auto* window = permissionWindow_.data())
                window->start(allowed);
        });
    }

    void start(bool allowed) {
        permissionWindow_.clear();
        waiting_ = false;
        if (!allowed) {
            message_ = "Allow Aux Bridge in System Settings → Privacy & Security → Microphone.";
            refresh();
            return;
        }
        auto result = bridge_.start(selected(input_), selected(output_));
        if (!result) {
            message_ = QString("%1 failed (Core Audio %2).")
                .arg(QString::fromLatin1(result.error().stage)).arg(result.error().status);
            refresh();
            return;
        }
        toggle_.setText("Stop");
        toggle_.setEnabled(true);
        refresh();
    }

    static inline QPointer<Window> permissionWindow_;
    AudioBridge bridge_;
    QComboBox input_;
    QComboBox output_;
    QLabel status_;
    QPushButton toggle_{"Start"};
    QTimer timer_;
    QString message_;
    std::vector<AudioDevice> devices_;
    bool populated_ = false;
    bool waiting_ = false;
};
}

QIcon menuBarIcon() {
    QPixmap image(36, 36);
    image.setDevicePixelRatio(2);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::black, 1.8, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(3, 8), QPointF(3, 10));
    painter.drawLine(QPointF(7, 5), QPointF(7, 13));
    painter.drawLine(QPointF(11, 3), QPointF(11, 15));
    painter.drawLine(QPointF(15, 6), QPointF(15, 12));
    painter.end();

    QIcon icon(image);
    icon.setIsMask(true);
    return icon;
}

int runAuxBridge(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setApplicationDisplayName("Aux Bridge");
    Window window;
    QMenu menu;
    QSystemTrayIcon tray(menuBarIcon());
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        app.setQuitOnLastWindowClosed(false);
        const auto showWindow = [&window] {
            window.showNormal();
            window.raise();
            window.activateWindow();
        };
        menu.addAction("Open Aux Bridge", &window, showWindow);
        menu.addSeparator();
        menu.addAction("Quit Aux Bridge", &app, &QApplication::quit);
        tray.setContextMenu(&menu);
        tray.setToolTip("Aux Bridge");
        tray.show();
    }
    window.show();
    return app.exec();
}
