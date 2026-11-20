#include "audio_bridge.h"
#include "audio_queue.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <bit>
#include <cstddef>
#include <map>
#include <memory>
#include <utility>

namespace {
constexpr UInt32 outputBus = 0;
constexpr UInt32 inputBus = 1;

auto address(AudioObjectPropertySelector selector,
             AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

template<class T>
std::expected<T, OSStatus> property(AudioObjectID device, AudioObjectPropertyAddress key) {
    T value{};
    UInt32 bytes = sizeof(value);
    const auto status = AudioObjectGetPropertyData(device, &key, 0, nullptr, &bytes, &value);
    if (status != noErr)
        return std::unexpected(status);
    if (bytes != sizeof(value))
        return std::unexpected(kAudioHardwareBadPropertySizeError);
    return value;
}

unsigned channels(AudioDeviceID device, AudioObjectPropertyScope scope) {
    const auto key = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(device, &key, 0, nullptr, &bytes) != noErr ||
        bytes < offsetof(AudioBufferList, mBuffers))
        return 0;

    std::vector<std::byte> memory(bytes);
    auto* list = reinterpret_cast<AudioBufferList*>(memory.data());
    if (AudioObjectGetPropertyData(device, &key, 0, nullptr, &bytes, list) != noErr ||
        bytes < offsetof(AudioBufferList, mBuffers) ||
        list->mNumberBuffers > (bytes - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer))
        return 0;

    unsigned count = 0;
    const auto* buffers = reinterpret_cast<const AudioBuffer*>(
        memory.data() + offsetof(AudioBufferList, mBuffers));
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        count += buffers[i].mNumberChannels;
    return count;
}

std::string deviceName(AudioDeviceID device) {
    auto value = property<CFStringRef>(device, address(kAudioObjectPropertyName));
    if (!value || !*value)
        return "Audio device " + std::to_string(device);
    const auto length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(*value), kCFStringEncodingUTF8);
    std::string name(static_cast<std::size_t>(length) + 1, '\0');
    if (CFStringGetCString(*value, name.data(), length + 1, kCFStringEncodingUTF8))
        name.resize(name.find('\0'));
    else
        name = "Audio device " + std::to_string(device);
    CFRelease(*value);
    return name;
}

struct DeviceFormat {
    Float64 rate;
    UInt32 channels;
    UInt32 period;
};

DeviceFormat formatOf(AudioDeviceID id, bool input) {
    const auto rate = property<Float64>(id, address(kAudioDevicePropertyNominalSampleRate));
    const auto period = property<UInt32>(id, address(kAudioDevicePropertyBufferFrameSize));
    auto count = channels(id, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
    if (!rate || !period || !std::isfinite(*rate) || *rate < 8000 || *rate > 768000 ||
        *period == 0 || *period > 8192 || count == 0 || count > 256)
        throw AudioError{input ? "Read input format" : "Read output format",
                         !rate ? rate.error() : !period ? period.error() : kAudioDeviceUnsupportedFormatError};
    return {*rate, count, *period};
}

class CapturePeriod {
public:
    CapturePeriod(AudioDeviceID device, bool capture) : device_(device) {
        if (!capture)
            return;
        const auto used = property<UInt32>(device_, address(kAudioDevicePropertyDeviceIsRunningSomewhere));
        const auto rate = property<Float64>(device_, address(kAudioDevicePropertyNominalSampleRate));
        const auto current = property<UInt32>(device_, key_);
        const auto range = property<AudioValueRange>(device_, address(kAudioDevicePropertyBufferFrameSizeRange));
        Boolean writable = false;
        if (!used || *used || !rate || !std::isfinite(*rate) || *rate < 8000 || *rate > 768000 ||
            !current || !range || !std::isfinite(range->mMinimum) ||
            range->mMinimum < 1 || range->mMinimum >= *current ||
            AudioObjectIsPropertySettable(device_, &key_, &writable) != noErr || !writable)
            return;

        const auto preferred = std::bit_floor(static_cast<UInt32>(*rate * 0.002));
        const auto requested = std::max(preferred, static_cast<UInt32>(std::ceil(range->mMinimum)));
        if (requested >= *current || requested > range->mMaximum)
            return;
        if (write(requested) == noErr) {
            original_ = *current;
            chosen_ = property<UInt32>(device_, key_).value_or(requested);
        }
    }

    ~CapturePeriod() {
        if (original_ && original_ != chosen_ && property<UInt32>(device_, key_) == chosen_)
            write(original_);
    }

    CapturePeriod(const CapturePeriod&) = delete;
    CapturePeriod& operator=(const CapturePeriod&) = delete;

private:
    OSStatus write(UInt32 frames) {
        return AudioObjectSetPropertyData(device_, &key_, 0, nullptr, sizeof(frames), &frames);
    }

    const AudioDeviceID device_;
    const AudioObjectPropertyAddress key_ = address(kAudioDevicePropertyBufferFrameSize);
    UInt32 original_ = 0;
    UInt32 chosen_ = 0;
};

std::shared_ptr<CapturePeriod> retainDevice(AudioDeviceID id, bool capture) {
    // Route starts and stops are serialized on the UI thread.
    static std::map<AudioDeviceID, std::weak_ptr<CapturePeriod>> periods;
    std::erase_if(periods, [](const auto& entry) { return entry.second.expired(); });
    auto& entry = periods[id];
    if (auto period = entry.lock())
        return period;
    auto period = std::make_shared<CapturePeriod>(id, capture);
    entry = period;
    return period;
}

void check(OSStatus status, const char* stage) {
    if (status != noErr)
        throw AudioError{stage, status};
}

template<class T>
void set(AudioUnit unit, AudioUnitPropertyID key, AudioUnitScope scope,
         AudioUnitElement bus, const T& value, const char* stage) {
    check(AudioUnitSetProperty(unit, key, scope, bus, &value, sizeof(value)), stage);
}

struct HalUnit {
    AudioUnit unit = nullptr;
    bool initialized = false;
    bool started = false;

    ~HalUnit() {
        if (started)
            AudioOutputUnitStop(unit);
        if (initialized)
            AudioUnitUninitialize(unit);
        if (unit)
            AudioComponentInstanceDispose(unit);
    }

    void open(AudioDeviceID id, bool input, DeviceFormat format) {
        const AudioComponentDescription description{
            kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0
        };
        const auto component = AudioComponentFindNext(nullptr, &description);
        if (!component)
            throw AudioError{"Find Core Audio unit", kAudioUnitErr_FailedInitialization};
        check(AudioComponentInstanceNew(component, &unit), "Create Core Audio unit");

        if (input) {
            set(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, inputBus,
                UInt32{1}, "Enable audio input");
            set(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, outputBus,
                UInt32{0}, "Disable unused audio output");
        }
        set(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, outputBus, id,
            input ? "Select input device" : "Select output device");

        const UInt32 bytesPerFrame = sizeof(float) * format.channels;
        const AudioStreamBasicDescription pcm{
            format.rate, kAudioFormatLinearPCM, kAudioFormatFlagsNativeFloatPacked,
            bytesPerFrame, 1, bytesPerFrame, format.channels, 32, 0
        };
        set(unit, kAudioUnitProperty_StreamFormat, input ? kAudioUnitScope_Output : kAudioUnitScope_Input,
            input ? inputBus : outputBus, pcm, input ? "Set input format" : "Set output format");
    }

    void initialize() {
        check(AudioUnitInitialize(unit), "Initialize audio device");
        initialized = true;
    }

    void start() {
        check(AudioOutputUnitStart(unit), "Start audio device");
        started = true;
    }
};
}

std::expected<std::vector<AudioDevice>, OSStatus> audioDevices() {
    const auto key = address(kAudioHardwarePropertyDevices);
    UInt32 bytes = 0;
    auto status = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &key, 0, nullptr, &bytes);
    if (status != noErr)
        return std::unexpected(status);
    if (bytes == 0)
        return std::vector<AudioDevice>{};
    if (bytes % sizeof(AudioDeviceID) != 0)
        return std::unexpected(kAudioHardwareBadPropertySizeError);

    std::vector<AudioDeviceID> ids(bytes / sizeof(AudioDeviceID));
    status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &key, 0, nullptr, &bytes, ids.data());
    if (status != noErr)
        return std::unexpected(status);

    std::vector<AudioDevice> devices;
    for (std::size_t i = 0; i < bytes / sizeof(AudioDeviceID); ++i) {
        const auto id = ids[i];
        const auto input = channels(id, kAudioObjectPropertyScopeInput);
        const auto output = channels(id, kAudioObjectPropertyScopeOutput);
        if (input || output)
            devices.push_back({id, deviceName(id), input, output});
    }
    std::ranges::sort(devices, {}, &AudioDevice::name);
    return devices;
}

AudioDeviceID defaultAudioDevice(bool input) {
    return property<AudioDeviceID>(kAudioObjectSystemObject,
        address(input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice))
        .value_or(kAudioObjectUnknown);
}

struct AudioBridge::Stream {
    static constexpr UInt32 maxInputFrames = 8192;

    Stream(AudioDeviceID input, AudioDeviceID output, float volume)
        : inputId(input), outputId(output),
          inputLease(retainDevice(input, true)), outputLease(retainDevice(output, false)),
          inputFormat(formatOf(input, true)),
          outputFormat(formatOf(output, false)), inputSamples(maxInputFrames * inputFormat.channels) {
        queue.setVolume(volume);
        if (!queue.reset(inputFormat.rate, outputFormat.rate, inputFormat.period, outputFormat.period))
            throw AudioError{"Buffer the selected route", kAudioDeviceUnsupportedFormatError};
        capture.open(inputId, true, inputFormat);
        playback.open(outputId, false, outputFormat);

        const AURenderCallbackStruct inputCallback{captureAudio, this};
        const AURenderCallbackStruct outputCallback{playAudio, this};
        set(capture.unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global,
            outputBus, inputCallback, "Connect input callback");
        set(playback.unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input,
            outputBus, outputCallback, "Connect output callback");

        capture.initialize();
        playback.initialize();
        capture.start();
        playback.start();
    }

    static OSStatus captureAudio(void* context, AudioUnitRenderActionFlags* flags,
                                 const AudioTimeStamp* time, UInt32, UInt32 count, AudioBufferList*) noexcept {
        auto& stream = *static_cast<Stream*>(context);
        if (count > maxInputFrames) {
            stream.error.store(kAudioUnitErr_TooManyFramesToProcess, std::memory_order_relaxed);
            return kAudioUnitErr_TooManyFramesToProcess;
        }
        const auto channels = stream.inputFormat.channels;
        const auto bytes = static_cast<UInt32>(count * channels * sizeof(float));
        AudioBufferList buffers{1, {{channels, bytes, stream.inputSamples.data()}}};
        const auto status = AudioUnitRender(stream.capture.unit, flags, time, inputBus, count, &buffers);
        if (status == noErr)
            stream.queue.push(stream.inputSamples.data(), count, channels);
        else
            stream.error.store(status, std::memory_order_relaxed);
        return status;
    }

    static OSStatus playAudio(void* context, AudioUnitRenderActionFlags* flags,
                              const AudioTimeStamp*, UInt32, UInt32 count, AudioBufferList* buffers) noexcept {
        auto& stream = *static_cast<Stream*>(context);
        const auto channels = stream.outputFormat.channels;
        if (!buffers || buffers->mNumberBuffers != 1 || !buffers->mBuffers[0].mData ||
            buffers->mBuffers[0].mDataByteSize < std::size_t{count} * channels * sizeof(float)) {
            stream.error.store(kAudioUnitErr_FormatNotSupported, std::memory_order_relaxed);
            return kAudioUnitErr_FormatNotSupported;
        }
        if (!stream.queue.pull(static_cast<float*>(buffers->mBuffers[0].mData), count, channels))
            *flags |= kAudioUnitRenderAction_OutputIsSilence;
        return noErr;
    }

    bool routeChanged() const {
        for (const auto& [id, format] : {std::pair{inputId, inputFormat}, std::pair{outputId, outputFormat}}) {
            const auto alive = property<UInt32>(id, address(kAudioDevicePropertyDeviceIsAlive));
            const auto rate = property<Float64>(id, address(kAudioDevicePropertyNominalSampleRate));
            const auto period = property<UInt32>(id, address(kAudioDevicePropertyBufferFrameSize));
            if (!alive || !*alive || !rate || *rate != format.rate || !period || *period != format.period)
                return true;
        }
        return false;
    }

    const AudioDeviceID inputId;
    const AudioDeviceID outputId;
    const std::shared_ptr<CapturePeriod> inputLease;
    const std::shared_ptr<CapturePeriod> outputLease;
    const DeviceFormat inputFormat;
    const DeviceFormat outputFormat;
    std::vector<float> inputSamples;
    AudioQueue queue;
    std::atomic<OSStatus> error{noErr};
    static_assert(std::atomic<OSStatus>::is_always_lock_free);
    HalUnit capture;
    HalUnit playback;
};

AudioBridge::AudioBridge() = default;
AudioBridge::~AudioBridge() = default;

std::expected<void, AudioError> AudioBridge::start(AudioDeviceID input, AudioDeviceID output) {
    stop();
    try {
        stream_ = std::make_unique<Stream>(input, output, volume_);
    } catch (const AudioError& error) {
        return std::unexpected(error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(AudioError{"Allocate audio buffers", kAudio_MemFullError});
    }
    return {};
}

void AudioBridge::stop() { stream_.reset(); }
void AudioBridge::setVolume(float volume) noexcept {
    volume_ = volume;
    if (stream_)
        stream_->queue.setVolume(volume);
}
bool AudioBridge::running() const noexcept { return static_cast<bool>(stream_); }
double AudioBridge::queueMilliseconds() const noexcept {
    return stream_ ? stream_->queue.fill() * 1000.0 / stream_->inputFormat.rate : 0;
}
unsigned AudioBridge::dropouts() const noexcept { return stream_ ? stream_->queue.drops() : 0; }
OSStatus AudioBridge::streamError() const noexcept {
    return stream_ ? stream_->error.load(std::memory_order_relaxed) : noErr;
}
bool AudioBridge::routeChanged() const { return stream_ && stream_->routeChanged(); }
