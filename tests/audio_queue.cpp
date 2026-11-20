#include "../src/audio_queue.h"

#include <array>
#include <cassert>
#include <cmath>
#include <numbers>

namespace {
bool near(float sample, float expected) {
    return std::abs(sample - expected) < 0.0001f;
}

double toneGain(AudioQueue& queue, double inputRate, double outputRate, double frequency) {
    std::array<float, 128> input;
    std::array<float, 256> output;
    assert(queue.reset(inputRate, outputRate, input.size(), output.size()));
    unsigned sent = 0;
    unsigned requested = 0;
    unsigned measured = 0;
    double energy = 0;
    while (requested < outputRate) {
        if (sent / inputRate <= requested / outputRate) {
            for (float& sample : input)
                sample = static_cast<float>(std::sin(2 * std::numbers::pi * frequency * sent++ / inputRate));
            queue.push(input.data(), input.size(), 1);
        } else {
            if (queue.pull(output.data(), output.size(), 1) && requested >= outputRate / 4) {
                for (float sample : output)
                    energy += sample * sample;
                measured += output.size();
            }
            requested += output.size();
        }
    }
    assert(queue.drops() == 0);
    assert(measured > outputRate / 2);
    return 10 * std::log10(2 * energy / measured);
}
}

int main() {
    AudioQueue queue;
    assert(queue.reset(48000, 48000, 64, 64));

    std::array<float, 128> stereo;
    stereo.fill(1);
    assert(!queue.pull(stereo.data(), 64, 2));
    for (const float sample : stereo)
        assert(sample == 0);

    std::array<float, 256> mono;
    mono.fill(0.25f);
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    for (unsigned i = 0; i < stereo.size(); i += 2)
        assert(near(stereo[i], stereo[i + 1]));
    assert(near(stereo.back(), 0.25f));

    queue.setVolume(0.5f);
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    assert(near(stereo.back(), 0.125f));
    queue.setVolume(0);
    assert(queue.pull(stereo.data(), 64, 2));
    assert(queue.pull(stereo.data(), 64, 2));
    for (const float sample : stereo)
        assert(sample == 0);
    queue.setVolume(1);

    // A slow output start must play recent samples and discard startup backlog.
    assert(queue.reset(48000, 48000, 64, 64));
    std::array<float, 4800> oldAudio;
    oldAudio.fill(-0.5f);
    queue.push(oldAudio.data(), oldAudio.size(), 1);
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    assert(near(stereo.back(), 0.25f));
    assert(queue.fill() < 480);

    // After a late input packet, deliver samples that are already available.
    assert(queue.reset(48000, 48000, 64, 64));
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    assert(queue.pull(stereo.data(), 64, 2));
    assert(queue.pull(stereo.data(), 64, 2));
    assert(queue.pull(stereo.data(), 64, 2));
    assert(near(stereo.front(), 0.25f) && stereo.back() == 0);
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    assert(near(stereo.back(), 0.25f));
    assert(queue.drops() == 1);

    // A USB input at 96 kHz and a Bluetooth output at 44.1 kHz arrive in
    // independent blocks. Run long enough for clock drift to exceed a packet.
    for (const double drift : {-0.00015, 0.00015}) {
        constexpr unsigned inputFrames = 128;
        constexpr unsigned outputFrames = 512;
        assert(queue.reset(96000, 44100, inputFrames, outputFrames));
        std::array<float, inputFrames * 2> input;
        for (unsigned i = 0; i < inputFrames; ++i) {
            input[2 * i] = 0.5f;
            input[2 * i + 1] = -0.25f;
        }
        std::array<float, outputFrames * 2> output;
        double inputTime = 0;
        double outputTime = 0;
        bool started = false;
        double fillSum = 0;
        unsigned observations = 0;
        unsigned outputCallbacks = 0;
        while (outputTime < 120) {
            // A starting output can pull a block early while capture settles.
            const double early = outputTime < 0.5 && outputCallbacks % 2 ? 0.002 : 0;
            if (inputTime <= outputTime - early) {
                queue.push(input.data(), inputFrames, 2);
                inputTime += inputFrames / (96000 * (1 + drift));
            } else {
                const bool played = queue.pull(output.data(), outputFrames, 2);
                if (played) {
                    if (started)
                        assert(near(output.front(), 0.5f) && near(output.back(), -0.25f));
                    started = true;
                    if (outputTime > 5) {
                        fillSum += queue.fill();
                        ++observations;
                    }
                } else {
                    assert(!started);
                }
                outputTime += outputFrames / 44100.0;
                ++outputCallbacks;
            }
        }
        assert(queue.drops() == 0);
        assert(observations > 0);
        assert(fillSum / observations / 96.0 < 2.5);
    }

    // High frequencies must stay at the right level or be rejected before
    // downsampling, so they don't reappear as lower-frequency tones.
    for (double inputRate : {48000.0, 96000.0, 192000.0}) {
        assert(std::abs(toneGain(queue, inputRate, 44100, 1000)) < 0.1);
        assert(std::abs(toneGain(queue, inputRate, 44100, 20000)) < 0.3);
        assert(toneGain(queue, inputRate, 44100, 23000) < -60);
    }
    assert(std::abs(toneGain(queue, 44100, 48000, 20000)) < 0.3);
}
