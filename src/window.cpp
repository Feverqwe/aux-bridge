#include "audio_bridge.h"
#include "launcher.h"

#include <CoreFoundation/CoreFoundation.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
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
#include <cstdio>
#include <cstdlib>
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
        auto* devices = new QFormLayout;
        devices->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        setupDevice(input_, "Input");
        setupDevice(output_, "Output");
        setupChannels(inputChannels_, "Input channels");
        setupChannels(outputChannels_, "Output channels");
        auto* inputControls = new QHBoxLayout;
        inputControls->addWidget(&input_, 1);
        inputControls->addWidget(&inputChannels_);
        auto* inputLabel = new QLabel("&Input", this);
        inputLabel->setBuddy(&input_);
        devices->addRow(inputLabel, inputControls);
        auto* outputControls = new QHBoxLayout;
        outputControls->addWidget(&output_, 1);
        outputControls->addWidget(&outputChannels_);
        outputControls->addWidget(&addOutput_);
        auto* outputLabel = new QLabel("&Output", this);
        outputLabel->setBuddy(&output_);
        devices->addRow(outputLabel, outputControls);
        auto* volumeControls = new QHBoxLayout;
        auto* volumeLabel = new QLabel("&Volume", this);
        volumeLabel->setBuddy(&volume_);
        volume_.setAccessibleName("Volume");
        volume_.setAttribute(Qt::WA_MacSmallSize);
        volume_.setMaximumWidth(190);
        volume_.setRange(0, 100);
        volume_.setValue(100);
        volumeValue_.setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        volumeValue_.setMinimumWidth(volumeValue_.sizeHint().width());
        volumeControls->addWidget(&volume_, 1);
        volumeControls->addWidget(&volumeValue_);
        volumeControls->addStretch(1);
        devices->addRow(volumeLabel, volumeControls);
        content->addLayout(devices);
        connect(&volume_, &QSlider::valueChanged, this, [this](int percent) {
            volumeValue_.setText(QString::number(percent) + "%");
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
        connect(&input_, &QComboBox::currentIndexChanged, this, [this] {
            populateChannels(inputChannels_, channelCount(input_), 0);
            selectionChanged();
        });
        connect(&output_, &QComboBox::currentIndexChanged, this, [this] {
            populateChannels(outputChannels_, channelCount(output_), 0);
            selectionChanged();
        });
        connect(&inputChannels_, &QComboBox::currentIndexChanged, this, [this] { selectionChanged(); });
        connect(&outputChannels_, &QComboBox::currentIndexChanged, this, [this] { selectionChanged(); });
    }

    QPushButton& removeButton() { return remove_; }
    QPushButton& addOutputButton() { return addOutput_; }
    AudioDeviceID inputDevice() const { return selected(input_); }
    int inputChannel() const { return selectedChannel(inputChannels_); }
    void selectInput(AudioDeviceID id, int firstChannel) {
        input_.setCurrentIndex(input_.findData(id));
        if (inputChannels_.count())
            inputChannels_.setCurrentIndex(inputChannels_.findData(firstChannel));
        else if (firstChannel != 0)
            input_.setCurrentIndex(-1);
    }
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
                populate(input_, inputChannels_, *devices, true, !populated_);
                populate(output_, outputChannels_, *devices, false, !populated_ && selectDefaultOutput_);
                devices_ = *devices;
                populated_ = true;
            }
        }
        const bool ready = devices && readyToStart();
        input_.setEnabled(devices && input_.count() > 0);
        output_.setEnabled(devices && output_.count() > 0);
        inputChannels_.setEnabled(devices.has_value());
        outputChannels_.setEnabled(devices.has_value());
        addOutput_.setEnabled(devices && selected(input_) && inputChannel() >= 0);
        toggle_.setText("Start");
        toggle_.setEnabled(ready);
        if (!devices)
            status_.setText(QString("Could not list audio devices (Core Audio %1).").arg(devices.error()));
        else if (!message_.isEmpty())
            status_.setText(message_);
        else if (!input_.count() || !output_.count())
            status_.setText("Connect an audio input and output to start.");
        else if (!selected(input_) || !selected(output_))
            status_.setText("Choose an input and output to start.");
        else if (!ready)
            status_.setText("Choose input and output channels to start.");
        else
            status_.setText("Ready to route audio.");
    }

private:
    static void setupDevice(QComboBox& combo, const QString& accessibleName) {
        combo.setAccessibleName(accessibleName);
        combo.setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo.setMinimumContentsLength(22);
    }

    static AudioDeviceID selected(const QComboBox& combo) { return combo.currentData().toUInt(); }

    static void setupChannels(QComboBox& combo, const QString& label) {
        combo.setAccessibleName(label);
        combo.setToolTip(label);
        combo.setPlaceholderText("Channels");
        combo.hide();
    }

    static unsigned channelCount(const QComboBox& combo) {
        return combo.currentData(Qt::UserRole + 1).toUInt();
    }

    static int selectedChannel(const QComboBox& combo) {
        return combo.count() && combo.currentIndex() < 0 ? -1 : combo.currentData().toInt();
    }

    static void populateChannels(QComboBox& combo, unsigned count, int firstChannel) {
        const QSignalBlocker blocker(&combo);
        combo.clear();
        if (count > 2) {
            for (unsigned channel = 0; channel < count; channel += 2) {
                const QString name = channel + 1 < count
                    ? QString("Ch. %1–%2").arg(channel + 1).arg(channel + 2)
                    : QString("Ch. %1").arg(channel + 1);
                combo.addItem(name, static_cast<int>(channel));
            }
            combo.setCurrentIndex(combo.findData(firstChannel));
        }
        combo.setVisible(count > 2);
    }

    static void populate(QComboBox& combo, QComboBox& channelCombo, const std::vector<AudioDevice>& devices,
                         bool input, bool selectDefault) {
        const auto previous = selected(combo);
        const auto firstChannel = selectedChannel(channelCombo);
        const QSignalBlocker blocker(&combo);
        combo.clear();
        for (const auto& info : devices) {
            const auto count = input ? info.inputChannels : info.outputChannels;
            if (count) {
                combo.addItem(QString::fromStdString(info.name), info.id);
                combo.setItemData(combo.count() - 1, count, Qt::UserRole + 1);
            }
        }
        const auto kept = combo.findData(previous);
        const auto system = selectDefault ? combo.findData(defaultAudioDevice(input)) : -1;
        const auto first = selectDefault && combo.count() > 0 ? 0 : -1;
        combo.setCurrentIndex(kept >= 0 ? kept : system >= 0 ? system : first);
        if (kept >= 0 && channelCount(combo) <= 2 && firstChannel != 0)
            combo.setCurrentIndex(-1);
        populateChannels(channelCombo, channelCount(combo), kept >= 0 ? firstChannel : 0);
        if (combo.count() == 0)
            combo.setPlaceholderText(input ? "No audio inputs" : "No audio outputs");
        else
            combo.setPlaceholderText(input ? "Choose input" : "Choose output");
    }

    bool readyToStart() const {
        return selected(input_) && selected(output_) && inputChannel() >= 0 &&
               selectedChannel(outputChannels_) >= 0;
    }

    void selectionChanged() {
        message_.clear();
        refresh(audioDevices());
    }

    void disableSelections() {
        input_.setEnabled(false);
        output_.setEnabled(false);
        inputChannels_.setEnabled(false);
        outputChannels_.setEnabled(false);
    }

    static void permissionCompleted(bool allowed) noexcept {
        try {
            auto requests = std::exchange(pendingPermission_, {});
            for (const auto& route : requests) {
                if (route && route->waiting_)
                    route->start(allowed);
            }
        } catch (...) {
            std::fputs("Aux Bridge: could not process microphone permission.\n", stderr);
            QApplication::exit(EXIT_FAILURE);
        }
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
        disableSelections();
        toggle_.setEnabled(false);
        status_.setText("Checking microphone access…");
        pendingPermission_.push_back(this);
        if (pendingPermission_.size() == 1)
            requestMicrophoneAccess(permissionCompleted);
    }

    void start(bool allowed) {
        waiting_ = false;
        if (!allowed) {
            message_ = "Allow Aux Bridge in System Settings → Privacy & Security → Microphone.";
            refresh(audioDevices());
            return;
        }
        const auto devices = audioDevices();
        refresh(devices);
        if (!devices || !readyToStart())
            return;
        auto result = bridge_.start(selected(input_), selected(output_), static_cast<unsigned>(inputChannel()),
                                    static_cast<unsigned>(selectedChannel(outputChannels_)));
        if (!result) {
            message_ = QString("%1 failed (Core Audio %2).")
                .arg(QString::fromLatin1(result.error().stage)).arg(result.error().status);
            refresh(audioDevices());
            return;
        }
        disableSelections();
        toggle_.setText("Stop");
        toggle_.setEnabled(true);
        refresh(devices);
    }

    static inline std::vector<QPointer<Route>> pendingPermission_;
    AudioBridge bridge_;
    QComboBox input_;
    QComboBox inputChannels_;
    QComboBox output_;
    QComboBox outputChannels_;
    QLabel status_;
    QPushButton toggle_{"Start"};
    QPushButton remove_{"Remove"};
    QPushButton addOutput_{"Add Output"};
    QSlider volume_{Qt::Horizontal};
    QLabel volumeValue_{"100%"};
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
        panels_->installEventFilter(this);
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
        resize(610, sizeHint().height());

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

    void addRoute(AudioDeviceID input = kAudioObjectUnknown, int firstChannel = 0) {
        auto* route = new Route(panels_, routes_.empty());
        routes_.push_back(route);
        routesLayout_->insertWidget(routesLayout_->count() - 1, route);
        scroll_.show();
        empty_.hide();
        route->refresh(audioDevices());
        if (input != kAudioObjectUnknown)
            route->selectInput(input, firstChannel);
        connect(&route->addOutputButton(), &QPushButton::clicked, this, [this, route] {
            addRoute(route->inputDevice(), route->inputChannel());
        });
        connect(&route->removeButton(), &QPushButton::clicked, this, [this, route] {
            const int extraHeight = std::max(0, height() - fittedRouteHeight());
            route->deactivate();
            std::erase(routes_, route);
            routesLayout_->removeWidget(route);
            route->hide();
            route->deleteLater();
            scroll_.setVisible(!routes_.empty());
            empty_.setVisible(routes_.empty());
            const int fitted = fittedRouteHeight() + extraHeight;
            if (fitted < height())
                resize(width(), fitted);
            add_.setFocus();
        });
        if (input == kAudioObjectUnknown)
            route->focusInput();
        else
            route->focusOutput();
        QTimer::singleShot(0, this, [this, route = QPointer<Route>(route)] {
            if (!route || route->isHidden())
                return;
            updateMinimumWidth();
            const int fitted = fittedRouteHeight();
            if (fitted > height())
                resize(width(), fitted);
            scroll_.ensureWidgetVisible(route, 0, 0);
        });
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == panels_ && event->type() == QEvent::LayoutRequest)
            updateMinimumWidth();
        return QDialog::eventFilter(object, event);
    }

private:
    int fittedRouteHeight() {
        layout()->activate();
        if (routes_.empty())
            return layout()->sizeHint().height();
        routesLayout_->activate();
        const int required = routesLayout_->sizeHint().height() + height() - scroll_.viewport()->height();
        return std::min(required, screen()->availableGeometry().height() * 3 / 4);
    }

    void updateMinimumWidth() {
        const auto margins = layout()->contentsMargins();
        setMinimumWidth(panels_->minimumSizeHint().width() + margins.left() + margins.right() +
                        scroll_.verticalScrollBar()->sizeHint().width());
    }

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

int runAuxBridge(int argc, char** argv) try {
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
    QObject::connect(&app, &QApplication::applicationStateChanged, &settings, [&](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && !window.isVisible() && !settings.isVisible())
            showWindow();
    });
    window.show();
    return app.exec();
} catch (...) {
    std::fputs("Aux Bridge: unexpected application error.\n", stderr);
    return EXIT_FAILURE;
}
