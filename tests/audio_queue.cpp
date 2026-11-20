#include "../src/audio_queue.h"

#include <array>
#include <cassert>
#include <cmath>

int main() {
    AudioQueue queue;
    assert(queue.reset(48000, 48000, 64, 64));

    std::array<float, 128> stereo;
    stereo.fill(1);
    assert(!queue.pull(stereo.data(), 64, 2));
    for (const float sample : stereo)
        assert(sample == 0);

    std::array<float, 192> mono;
    mono.fill(0.25f);
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    for (const float sample : stereo)
        assert(sample == 0.25f);

    // A slow output start must play recent samples and discard startup backlog.
    assert(queue.reset(48000, 48000, 64, 64));
    std::array<float, 4800> oldAudio;
    oldAudio.fill(-0.5f);
    queue.push(oldAudio.data(), oldAudio.size(), 1);
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    assert(stereo.front() == 0.25f);
    assert(queue.fill() < 192);

    // After a late input packet, deliver samples that are already available.
    assert(queue.reset(48000, 48000, 64, 64));
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    assert(queue.pull(stereo.data(), 64, 2));
    assert(queue.pull(stereo.data(), 64, 2));
    assert(stereo.front() == 0.25f && stereo.back() == 0);
    queue.push(mono.data(), mono.size(), 1);
    assert(queue.pull(stereo.data(), 64, 2));
    assert(stereo.back() == 0.25f);
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
        while (outputTime < 120) {
            if (inputTime <= outputTime) {
                queue.push(input.data(), inputFrames, 2);
                inputTime += inputFrames / (96000 * (1 + drift));
            } else {
                const bool played = queue.pull(output.data(), outputFrames, 2);
                if (played) {
                    assert(output.front() == 0.5f && output.back() == -0.25f);
                    started = true;
                    if (outputTime > 5) {
                        fillSum += queue.fill();
                        ++observations;
                    }
                } else {
                    assert(!started);
                }
                outputTime += outputFrames / 44100.0;
            }
        }
        assert(queue.drops() == 0);
        assert(observations > 0);
        assert(fillSum / observations / 96.0 < 2.5);
    }
}
