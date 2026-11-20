#pragma once

#include <CoreAudio/CoreAudio.h>

#include <expected>
#include <memory>
#include <string>
#include <vector>

struct AudioDevice {
    AudioDeviceID id;
    std::string name;
    unsigned inputChannels;
    unsigned outputChannels;
};

std::expected<std::vector<AudioDevice>, OSStatus> audioDevices();
AudioDeviceID defaultAudioDevice(bool input);

struct AudioError {
    const char* stage;
    OSStatus status;
};

class AudioBridge {
public:
    AudioBridge();
    ~AudioBridge();

    std::expected<void, AudioError> start(AudioDeviceID input, AudioDeviceID output);
    void stop();
    void setVolume(float volume) noexcept;
    bool running() const noexcept;
    double queueMilliseconds() const noexcept;
    unsigned dropouts() const noexcept;
    OSStatus streamError() const noexcept;
    bool routeChanged() const;

private:
    struct Stream;
    std::unique_ptr<Stream> stream_;
    float volume_ = 1.0f;
};
