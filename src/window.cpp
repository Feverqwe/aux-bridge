#include "audio_bridge.h"
#include "launcher.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
using DeviceList = std::expected<std::vector<AudioDevice>, OSStatus>;

class Route final : public QFrame {
public:
    Route(QWidget* parent, bool selectDefaultOutput)
        : QFrame(parent), selectDefaultOutput_(selectDefaultOutput) {
        setFrameShape(QFrame::StyledPanel);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
        auto* content = new QVBoxLayout(this);
        auto* controls = new QHBoxLayout;
        auto* devices = new QFormLayout;
        devices->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        setupDevice(*devices, input_, "&Input", "Input");
        setupDevice(*devices, output_, "&Output", "Output");
        controls->addLayout(devices, 1);

        auto* volume = new QVBoxLayout;
        auto* volumeLabel = new QLabel("&Volume", this);
        volumeLabel->setBuddy(&volume_);
        volume_.setAccessibleName("Volume");
        volume_.setRange(0, 100);
        volume_.setValue(100);
        volume_.setToolTip("100%");
        volume->addWidget(volumeLabel);
        volume->addWidget(&volume_);
        controls->addLayout(volume);
        content->addLayout(controls);
        connect(&volume_, &QSlider::valueChanged, this, [this](int percent) {
            volume_.setToolTip(QString::number(percent) + "%");
            bridge_.setVolume(percent / 100.0f);
        });

        status_.setWordWrap(true);
        status_.setAccessibleName("Playback status");
        status_.setToolTip("The bridge queue excludes the delay of the audio devices.");

        auto* footer = new QHBoxLayout;
        footer->addWidget(&status_, 1);
        footer->addWidget(&remove_);
        footer->addWidget(&toggle_);
        content->addLayout(footer);
        remove_.setAutoDefault(false);
        toggle_.setAutoDefault(false);
        connect(&toggle_, &QPushButton::clicked, this, [this] { toggle(); });
        const auto deviceSelected = [this] {
            message_.clear();
            refresh(audioDevices());
        };
        connect(&input_, &QComboBox::currentIndexChanged, this, deviceSelected);
        connect(&output_, &QComboBox::currentIndexChanged, this, deviceSelected);
    }

    QPushButton& removeButton() { return remove_; }
    void focusInput() { input_.setFocus(); }
    void deactivate() {
        waiting_ = false;
        bridge_.stop();
    }

    void refresh(const DeviceList& devices) {
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

        if (devices) {
            const auto sameDevice = [](const AudioDevice& a, const AudioDevice& b) {
                return a.id == b.id && a.name == b.name &&
                       a.inputChannels == b.inputChannels && a.outputChannels == b.outputChannels;
            };
            if (!populated_ || !std::ranges::equal(*devices, devices_, sameDevice)) {
                populate(input_, *devices, true);
                populate(output_, *devices, false, selectDefaultOutput_ || selected(output_));
                devices_ = *devices;
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
        else if (!input_.count() || !output_.count())
            status_.setText("Connect an audio input and output to start.");
        else if (!ready)
            status_.setText("Choose an input and output to start.");
        else
            status_.setText(message_.isEmpty() ? "Ready to route audio." : message_);
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

    static void populate(QComboBox& combo, const std::vector<AudioDevice>& devices, bool input,
                         bool selectDefault = true) {
        const auto previous = selected(combo);
        const auto fallback = defaultAudioDevice(input);
        const QSignalBlocker blocker(&combo);
        combo.clear();
        for (const auto& info : devices) {
            if (input ? info.inputChannels : info.outputChannels)
                combo.addItem(QString::fromStdString(info.name), info.id);
        }
        const auto kept = combo.findData(previous);
        const auto system = selectDefault ? combo.findData(fallback) : -1;
        const auto first = selectDefault && combo.count() > 0 ? 0 : -1;
        combo.setCurrentIndex(kept >= 0 ? kept : system >= 0 ? system : first);
        if (combo.count() == 0)
            combo.setPlaceholderText(input ? "No audio inputs" : "No audio outputs");
        else
            combo.setPlaceholderText(input ? "Choose input" : "Choose output");
    }

    void toggle() {
        if (bridge_.running()) {
            bridge_.stop();
            message_.clear();
            refresh(audioDevices());
            return;
        }
        waiting_ = true;
        message_.clear();
        input_.setEnabled(false);
        output_.setEnabled(false);
        toggle_.setEnabled(false);
        status_.setText("Checking microphone access…");
        pendingPermission_.push_back(this);
        if (pendingPermission_.size() == 1) {
            requestMicrophoneAccess([](bool allowed) {
                auto requests = std::exchange(pendingPermission_, {});
                for (const auto& route : requests) {
                    if (route && route->waiting_)
                        route->start(allowed);
                }
            });
        }
    }

    void start(bool allowed) {
        waiting_ = false;
        if (!allowed) {
            message_ = "Allow Aux Bridge in System Settings → Privacy & Security → Microphone.";
            refresh(audioDevices());
            return;
        }
        auto result = bridge_.start(selected(input_), selected(output_));
        if (!result) {
            message_ = QString("%1 failed (Core Audio %2).")
                .arg(QString::fromLatin1(result.error().stage)).arg(result.error().status);
            refresh(audioDevices());
            return;
        }
        toggle_.setText("Stop");
        toggle_.setEnabled(true);
        refresh(audioDevices());
    }

    static inline std::vector<QPointer<Route>> pendingPermission_;
    AudioBridge bridge_;
    QComboBox input_;
    QComboBox output_;
    QLabel status_;
    QPushButton toggle_{"Start"};
    QPushButton remove_{"Remove"};
    QSlider volume_{Qt::Horizontal};
    QString message_;
    std::vector<AudioDevice> devices_;
    bool populated_ = false;
    bool waiting_ = false;
    const bool selectDefaultOutput_;
};

class Window final : public QDialog {
public:
    Window() {
        setWindowTitle("Aux Bridge");
        setSizeGripEnabled(true);

        auto* content = new QVBoxLayout(this);
        scroll_.setWidgetResizable(true);
        scroll_.setFrameShape(QFrame::NoFrame);
        panels_ = new QWidget;
        routesLayout_ = new QVBoxLayout(panels_);
        routesLayout_->setContentsMargins(0, 0, 0, 0);
        routesLayout_->addStretch();
        scroll_.setWidget(panels_);
        content->addWidget(&scroll_, 1);
        content->addWidget(&empty_);
        empty_.setAlignment(Qt::AlignCenter);

        content->addWidget(&add_, 0, Qt::AlignRight);
        add_.setAutoDefault(false);
        connect(&add_, &QPushButton::clicked, this, [this] { addRoute(); });
        addRoute();
        resize(540, routes_.front()->sizeHint().height() + 70);

        connect(&timer_, &QTimer::timeout, this, [this] {
            if (routes_.empty())
                return;
            const auto devices = audioDevices();
            for (auto* route : routes_)
                route->refresh(devices);
        });
        timer_.start(350);
    }

    void addRoute() {
        auto* route = new Route(panels_, routes_.empty());
        routes_.push_back(route);
        routesLayout_->insertWidget(routesLayout_->count() - 1, route);
        scroll_.show();
        empty_.hide();
        route->refresh(audioDevices());
        connect(&route->removeButton(), &QPushButton::clicked, this, [this, route] {
            route->deactivate();
            std::erase(routes_, route);
            routesLayout_->removeWidget(route);
            route->hide();
            route->deleteLater();
            scroll_.setVisible(!routes_.empty());
            empty_.setVisible(routes_.empty());
            add_.setFocus();
        });
        if (isVisible() && routes_.size() > 1) {
            const auto limit = screen()->availableGeometry().height() * 3 / 4;
            const int extra = route->sizeHint().height() + routesLayout_->spacing();
            if (height() < limit)
                resize(width(), std::min(height() + extra, limit));
        }
        route->focusInput();
        QTimer::singleShot(0, this, [this, route = QPointer<Route>(route)] {
            if (route)
                scroll_.ensureWidgetVisible(route, 0, 0);
        });
    }

private:
    QScrollArea scroll_;
    QLabel empty_{"No routes"};
    QPushButton add_{"Add Route"};
    QTimer timer_;
    QWidget* panels_ = nullptr;
    QVBoxLayout* routesLayout_ = nullptr;
    std::vector<Route*> routes_;
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
        menu.addAction("Add Route", &window, [&window, showWindow] {
            showWindow();
            window.addRoute();
        });
        menu.addSeparator();
        menu.addAction("Quit Aux Bridge", &app, &QApplication::quit);
        tray.setContextMenu(&menu);
        tray.setToolTip("Aux Bridge");
        tray.show();
    }
    window.show();
    return app.exec();
}
