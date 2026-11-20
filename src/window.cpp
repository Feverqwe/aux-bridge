#include "audio_bridge.h"
#include "launcher.h"

#include <CoreFoundation/CoreFoundation.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {
using DeviceList = std::expected<std::vector<AudioDevice>, OSStatus>;

void reveal(QWidget& window) {
    if (window.isMinimized())
        window.showNormal();
    else
        window.show();
    window.raise();
    window.activateWindow();
}

class Settings final : public QDialog {
public:
    explicit Settings(QSystemTrayIcon& tray)
        : tray_(tray), menuBarAvailable_(QSystemTrayIcon::isSystemTrayAvailable()),
          preferences_(QSettings::NativeFormat, QSettings::UserScope, preferencesDomain()) {
        setWindowTitle("Aux Bridge Settings");
        auto* layout = new QVBoxLayout(this);
        layout->addWidget(&menuBar_);
        layout->addWidget(&dock_);
        layout->addWidget(&status_);
        apply(preferences_.value("showInMenuBar", true).toBool(),
              preferences_.value("showInDock", false).toBool());

        const auto change = [this] { apply(menuBar_.isChecked(), dock_.isChecked()); };
        connect(&menuBar_, &QCheckBox::clicked, this, change);
        connect(&dock_, &QCheckBox::clicked, this, change);
    }

private:
    static QString preferencesDomain() {
        const auto identifier = CFBundleGetIdentifier(CFBundleGetMainBundle());
        return identifier ? QString::fromCFString(identifier) : QString("app.auxbridge.desktop");
    }

    void apply(bool menuBar, bool dock) {
        if (!menuBarAvailable_) {
            menuBar = false;
            dock = true;
        } else if (!menuBar && !dock) {
            menuBar = true;
        }

        // Show the replacement before hiding the current way back into the app.
        if (menuBar)
            tray_.show();
        if (dock != dockIconVisible())
            setDockIconVisible(dock);
        const bool dockShown = dockIconVisible();
        menuBar = menuBar || (menuBarAvailable_ && !dockShown);
        tray_.setVisible(menuBar);
        QApplication::setQuitOnLastWindowClosed(!menuBar && !dockShown);

        menuBar_.setChecked(menuBar);
        menuBar_.setEnabled(menuBarAvailable_ && (!menuBar || dockShown));
        dock_.setChecked(dockShown);
        dock_.setEnabled(!dockShown || menuBar);
        status_.setText(dock != dockShown
            ? "macOS could not change the Dock icon."
            : "Keep at least one icon visible to reopen Aux Bridge.");
        preferences_.setValue("showInMenuBar", menuBar);
        preferences_.setValue("showInDock", dockShown);
    }

    QSystemTrayIcon& tray_;
    const bool menuBarAvailable_;
    QSettings preferences_;
    QCheckBox menuBar_{"Show in Menu Bar"};
    QCheckBox dock_{"Show in Dock"};
    QLabel status_;
};

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
        setupDevice(input_, "Input");
        setupDevice(output_, "Output");
        devices->addRow("&Input", &input_);
        auto* outputControls = new QHBoxLayout;
        outputControls->addWidget(&output_, 1);
        outputControls->addWidget(&addOutput_);
        auto* outputLabel = new QLabel("&Output", this);
        outputLabel->setBuddy(&output_);
        devices->addRow(outputLabel, outputControls);
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
        addOutput_.setAutoDefault(false);
        addOutput_.setToolTip("Add another output for this input");
        connect(&toggle_, &QPushButton::clicked, this, [this] { toggle(); });
        const auto deviceSelected = [this] {
            message_.clear();
            refresh(audioDevices());
        };
        connect(&input_, &QComboBox::currentIndexChanged, this, deviceSelected);
        connect(&output_, &QComboBox::currentIndexChanged, this, deviceSelected);
    }

    QPushButton& removeButton() { return remove_; }
    QPushButton& addOutputButton() { return addOutput_; }
    AudioDeviceID inputDevice() const { return selected(input_); }
    void selectInput(AudioDeviceID id) { input_.setCurrentIndex(input_.findData(id)); }
    void focusOutput() { output_.setFocus(); }
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
        addOutput_.setEnabled(devices && selected(input_));
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
    static void setupDevice(QComboBox& combo, const QString& accessibleName) {
        combo.setAccessibleName(accessibleName);
        combo.setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo.setMinimumContentsLength(22);
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
    QPushButton addOutput_{"Add Output"};
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
        scroll_.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        panels_ = new QWidget;
        routesLayout_ = new QVBoxLayout(panels_);
        routesLayout_->setContentsMargins(0, 0, 0, 0);
        routesLayout_->addStretch();
        scroll_.setWidget(panels_);
        content->addWidget(&scroll_, 1);
        content->addWidget(&empty_);
        empty_.setAlignment(Qt::AlignCenter);

        auto* footer = new QHBoxLayout;
        footer->addWidget(&settings_);
        footer->addStretch();
        footer->addWidget(&add_);
        content->addLayout(footer);
        settings_.setAutoDefault(false);
        add_.setAutoDefault(false);
        connect(&add_, &QPushButton::clicked, this, [this] { addRoute(); });
        addRoute();
        resize(610, routes_.front()->sizeHint().height() + 70);

        connect(&timer_, &QTimer::timeout, this, [this] {
            if (routes_.empty())
                return;
            const auto devices = audioDevices();
            for (auto* route : routes_)
                route->refresh(devices);
        });
        timer_.start(350);
    }

    QPushButton& settingsButton() { return settings_; }

    void addRoute(AudioDeviceID input = kAudioObjectUnknown) {
        auto* route = new Route(panels_, routes_.empty());
        routes_.push_back(route);
        routesLayout_->insertWidget(routesLayout_->count() - 1, route);
        scroll_.show();
        empty_.hide();
        route->refresh(audioDevices());
        if (input != kAudioObjectUnknown)
            route->selectInput(input);
        connect(&route->addOutputButton(), &QPushButton::clicked, this, [this, route] {
            addRoute(route->inputDevice());
        });
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
        if (input == kAudioObjectUnknown)
            route->focusInput();
        else
            route->focusOutput();
        QTimer::singleShot(0, this, [this, route = QPointer<Route>(route)] {
            if (!route)
                return;
            routesLayout_->activate();
            const auto margins = layout()->contentsMargins();
            setMinimumWidth(panels_->minimumSizeHint().width() + margins.left() + margins.right() +
                            scroll_.verticalScrollBar()->sizeHint().width());
            const int required = routesLayout_->sizeHint().height() + height() - scroll_.viewport()->height();
            const int limit = screen()->availableGeometry().height() * 3 / 4;
            const int fitted = std::min(required, limit);
            if (fitted > height())
                resize(width(), fitted);
            scroll_.ensureWidgetVisible(route, 0, 0);
        });
    }

private:
    QScrollArea scroll_;
    QLabel empty_{"No routes"};
    QPushButton settings_{"Settings…"};
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
    painter.setPen(QPen(Qt::black, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    QPainterPath signal(QPointF(2.5, 9));
    signal.lineTo(7.4, 9);
    signal.cubicTo(10, 9, 10.25, 5, 13, 5);
    signal.lineTo(15.5, 5);
    signal.moveTo(7.4, 9);
    signal.cubicTo(10, 9, 10.25, 13, 13, 13);
    signal.lineTo(15.5, 13);
    painter.drawPath(signal);
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
    QMenu dockMenu;
    QSystemTrayIcon tray(menuBarIcon());
    tray.setContextMenu(&menu);
    tray.setToolTip("Aux Bridge");
    Settings settings(tray);

    const auto showWindow = [&window] { reveal(window); };
    const auto showSettings = [&settings] { reveal(settings); };
    QObject::connect(&window.settingsButton(), &QPushButton::clicked, &settings, showSettings);
    QShortcut settingsShortcut(QKeySequence::Preferences, &window, showSettings, Qt::ApplicationShortcut);
    menu.addAction("Open Aux Bridge", &window, showWindow);
    menu.addAction("Add Route", &window, [&window, showWindow] {
        showWindow();
        window.addRoute();
    });
    menu.addAction("Settings…", &settings, showSettings);
    menu.addSeparator();
    menu.addAction("Quit Aux Bridge", &app, &QApplication::quit);
    dockMenu.addAction("Open Aux Bridge", &window, showWindow);
    dockMenu.addAction("Settings…", &settings, showSettings);
    dockMenu.setAsDockMenu();
    QObject::connect(&app, &QApplication::applicationStateChanged, &window, [&](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && !window.isVisible() && !settings.isVisible())
            showWindow();
    });
    window.show();
    return app.exec();
}
