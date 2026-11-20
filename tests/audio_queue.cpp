#include "../src/audio_queue.h"

#include <array>
#include <cassert>
#include <cmath>
#include <numbers>
#include <vector>

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

    {
        constexpr unsigned rate = 48000;
        std::array<float, 128> input;
        std::array<float, 128> output;
        const auto cycle = [&] {
            queue.push(input.data(), input.size(), 1);
            return queue.pull(output.data(), output.size(), 1);
        };

        // A brief capture stall must not raise the queue delay for the rest of the route.
        assert(queue.reset(rate, rate, input.size(), output.size()));
        input.fill(0.25f);
        for (unsigned i = 0; i < rate / input.size() * 2; ++i)
            cycle();
        const auto normalFill = queue.fill();
        for (unsigned i = 0; i < 38; ++i)
            queue.pull(output.data(), output.size(), 1);
        assert(queue.drops() > 0);
        for (unsigned i = 0; i < rate / input.size() * 15; ++i)
            cycle();
        assert(near(output.back(), 0.25f));
        assert(queue.fill() <= normalFill + rate / 1000);

        // Resume from the live signal after both a short pause and a queue overflow.
        for (const unsigned pausedBlocks : {38u, 1125u}) {
            assert(queue.reset(rate, rate, input.size(), output.size()));
            input.fill(-0.5f);
            for (unsigned i = 0; i < rate / input.size(); ++i)
                cycle();
            for (unsigned i = 0; i < pausedBlocks; ++i) {
                if (i == pausedBlocks / 2)
                    input.fill(0.25f);
                queue.push(input.data(), input.size(), 1);
            }
            bool resumed = false;
            for (unsigned i = 0; i < 10; ++i) {
                if (cycle()) {
                    resumed = true;
                    for (const auto sample : output)
                        assert(near(sample, 0.25f));
                }
            }
            assert(resumed);
            assert(queue.fill() < rate / 100);
        }
    }

    // Route the selected pair, leaving every other output channel silent.
    {
        std::array<float, 64 * 5> input;
        std::array<float, 64 * 5> output;
        for (unsigned i = 0; i < input.size(); i += 5) {
            input[i] = 0.75f;
            input[i + 1] = 0.75f;
            input[i + 2] = 0.25f;
            input[i + 3] = -0.5f;
            input[i + 4] = 0.5f;
        }
        for (const unsigned firstChannel : {2u, 4u}) {
            assert(queue.reset(48000, 48000, 64, 64));
            for (unsigned i = 0; i < 8; ++i) {
                queue.push(input.data(), 64, 5, firstChannel);
                queue.pull(output.data(), 64, 5, firstChannel);
            }
            for (unsigned i = 0; i < output.size(); i += 5) {
                assert(output[i] == 0 && output[i + 1] == 0);
                if (firstChannel == 2) {
                    assert(near(output[i + 2], 0.25f) && near(output[i + 3], -0.5f));
                    assert(output[i + 4] == 0);
                } else {
                    assert(output[i + 2] == 0 && output[i + 3] == 0);
                    assert(near(output[i + 4], 0.5f));
                }
            }
        }
    }

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

    // Regular but differently sized capture and playback blocks must not cause dropouts.
    struct RouteTiming {
        unsigned inputRate, outputRate, inputFrames, outputFrames;
        double phase, drift;
    };
    for (const auto [inputRate, outputRate, inputFrames, outputFrames, phase, drift] :
         {RouteTiming{44100, 48000, 128, 128, 0.49, -0.0001},
          RouteTiming{48000, 44100, 512, 1024, 0.49, -0.0001},
          RouteTiming{44100, 48000, 512, 32, 0, -0.0001},
          RouteTiming{48000, 48000, 128, 1024, 0, 0}}) {
        assert(queue.reset(inputRate, outputRate, inputFrames, outputFrames));
        std::vector<float> input(inputFrames, 0.25f);
        std::vector<float> output(outputFrames);
        double inputTime = 0, outputTime = phase * inputFrames / inputRate;
        bool started = false;
        while (outputTime < 8) {
            if (inputTime <= outputTime) {
                queue.push(input.data(), inputFrames, 1);
                inputTime += inputFrames / (inputRate * (1.0 + drift));
            } else {
                if (queue.pull(output.data(), outputFrames, 1)) {
                    if (started)
                        assert(near(output.front(), 0.25f) && near(output.back(), 0.25f));
                    started = true;
                } else {
                    assert(!started);
                }
                outputTime += static_cast<double>(outputFrames) / outputRate;
            }
        }
        assert(started && queue.drops() == 0);
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
