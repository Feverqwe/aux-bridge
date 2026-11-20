#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

class AudioQueue {
public:
    bool reset(double inputRate, double outputRate, unsigned inputPeriod, unsigned outputPeriod) {
        ratio_ = inputRate / outputRate;
        const double downsampling = std::max(1.0, ratio_ * (1.0 + maxCorrection));
        radius_ = static_cast<unsigned>(std::ceil(45 * downsampling));
        // Keep a capture packet's worth of headroom when callback order varies.
        reserve_ = std::max(inputPeriod + radius_ + 2.0, inputRate * 0.002);
        maxReserve_ = std::max(reserve_, inputRate * 0.020);
        const double demand = outputPeriod * ratio_;
        if (demand * (1.0 + maxCorrection) + maxReserve_ + inputPeriod + 2 * radius_ + 2 >= capacity)
            return false;

        makeFilter(0.477 / downsampling);
        std::fill_n(frames_.begin(), radius_, Frame{});
        write_.store(radius_, std::memory_order_relaxed);
        read_.store(radius_, std::memory_order_relaxed);
        overflows_.store(0, std::memory_order_relaxed);
        overflowing_ = false;
        seenOverflows_ = 0;
        fill_.store(0, std::memory_order_relaxed);
        drops_.store(0, std::memory_order_relaxed);
        inputPeriod_ = inputPeriod;
        nominalDemand_ = demand;
        averageFill_ = reserve_;
        baseReserve_ = reserve_;
        smoothing_ = std::min(1.0, outputPeriod / (outputRate * 0.2));
        correctionGain_ = 1.8 / inputRate;
        recoveryDelay_ = static_cast<unsigned>(std::ceil(outputRate));
        recoveryFrames_ = 0;
        reserveDecay_ = 0.002 * ratio_;
        phase_ = 0;
        primed_ = false;
        needsHistory_ = false;
        volume_ = targetVolume_.load(std::memory_order_relaxed);
        return true;
    }

    void setVolume(float volume) noexcept {
        targetVolume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void push(const float* samples, unsigned frames, unsigned channels, unsigned firstChannel = 0) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        // Keep the samples before the consumer cursor available to the filter.
        if (frames > capacity - radius_ - (write - read)) {
            if (!overflowing_) {
                overflows_.fetch_add(1, std::memory_order_release);
                overflowing_ = true;
            }
            return;
        }
        overflowing_ = false;
        const unsigned rightChannel = firstChannel + (firstChannel + 1 < channels);
        for (unsigned i = 0; i < frames; ++i) {
            const auto* source = samples + i * channels;
            frames_[(write + i) & (capacity - 1)] = {source[firstChannel], source[rightChannel]};
        }
        write_.store(write + frames, std::memory_order_release);
    }

    bool pull(float* samples, unsigned frames, unsigned channels, unsigned firstChannel = 0) noexcept {
        auto read = read_.load(std::memory_order_relaxed);
        const auto overflows = overflows_.load(std::memory_order_acquire);
        const auto write = write_.load(std::memory_order_acquire);
        auto available = write - read;
        const double demand = frames * ratio_;
        if (overflows != seenOverflows_) {
            seenOverflows_ = overflows;
            // The oldest data survived the overflow. Wait for fresh samples and filter history.
            read = write;
            read_.store(read, std::memory_order_release);
            available = 0;
            needsHistory_ = true;
            primed_ = false;
            recoveryFrames_ = 0;
            drops_.fetch_add(1, std::memory_order_relaxed);
        } else if (primed_ && available >
                   std::max(demand, nominalDemand_) * (1.0 + maxCorrection) + maxReserve_ + 2 * inputPeriod_) {
            // Allow packet bursts during startup before treating a backlog as a playback pause.
            primed_ = false;
            recoveryFrames_ = 0;
            drops_.fetch_add(1, std::memory_order_relaxed);
        }

        if (!primed_) {
            // A starting device can request several blocks close together.
            const double startupReserve = std::min(maxReserve_, reserve_ + std::min(demand, 2.0 * inputPeriod_));
            const auto startFill = static_cast<std::uint64_t>(std::ceil(demand + startupReserve));
            const unsigned history = needsHistory_ ? radius_ : 0;
            if (available < startFill + history) {
                fill_.store(static_cast<unsigned>(available), std::memory_order_relaxed);
                std::fill_n(samples, static_cast<std::size_t>(frames) * channels, 0.0f);
                return false;
            }

            // Start close to the live signal even when capture is already ahead.
            available -= history;
            if (available > startFill + inputPeriod_)
                available = startFill + inputPeriod_ / 2;
            read = write - available;
            phase_ = 0;
            averageFill_ = reserve_;
            primed_ = true;
            needsHistory_ = false;
        }

        const double remainder = std::max(0.0, static_cast<double>(available) - demand);
        averageFill_ += (remainder - averageFill_) * smoothing_;
        const double correction = std::clamp((averageFill_ - reserve_) * correctionGain_,
                                             -maxCorrection, maxCorrection);
        const double step = ratio_ * (1.0 + correction);
        const double advance = phase_ + frames * step;
        unsigned playable = frames;
        if (available < static_cast<std::uint64_t>(advance) + radius_ + 1) {
            const double usable = static_cast<double>(available) - radius_ - 1.0 - phase_;
            playable = static_cast<unsigned>(std::clamp(std::floor(usable / step), 0.0, double(frames)));
            if (playable < frames) {
                drops_.fetch_add(1, std::memory_order_relaxed);
                reserve_ = std::min(maxReserve_, reserve_ + inputPeriod_ * 0.5);
                recoveryFrames_ = 0;
                std::fill_n(samples + std::size_t{playable} * channels,
                            std::size_t{frames - playable} * channels, 0.0f);
            }
        }
        if (playable == frames && reserve_ > baseReserve_) {
            const auto graceFrames = std::min(frames, recoveryDelay_ - recoveryFrames_);
            recoveryFrames_ += graceFrames;
            reserve_ = std::max(baseReserve_, reserve_ - (frames - graceFrames) * reserveDecay_);
        }

        if (channels > 2 || firstChannel != 0)
            std::fill_n(samples, std::size_t{playable} * channels, 0.0f);
        const bool stereo = firstChannel + 1 < channels;
        for (unsigned i = 0; i < playable; ++i) {
            const auto offset = static_cast<std::uint64_t>(phase_);
            const auto [left, right] = interpolate(read + offset, phase_ - static_cast<double>(offset));
            auto* output = samples + i * channels + firstChannel;
            if (stereo) {
                output[0] = left;
                output[1] = right;
            } else {
                output[0] = (left + right) * 0.5f;
            }
            phase_ += step;
        }

        const float target = targetVolume_.load(std::memory_order_relaxed);
        if (playable && (volume_ != 1.0f || target != 1.0f)) {
            for (unsigned i = 0; i < playable; ++i) {
                const auto gain = std::lerp(volume_, target, static_cast<float>(i + 1) / playable);
                auto* output = samples + i * channels + firstChannel;
                output[0] *= gain;
                if (stereo)
                    output[1] *= gain;
            }
        }
        volume_ = target;

        const auto consumed = static_cast<std::uint64_t>(phase_);
        phase_ -= static_cast<double>(consumed);
        read_.store(read + consumed, std::memory_order_release);
        fill_.store(static_cast<unsigned>(available - consumed), std::memory_order_relaxed);
        return playable > 0;
    }

    unsigned fill() const noexcept { return fill_.load(std::memory_order_relaxed); }
    unsigned drops() const noexcept { return drops_.load(std::memory_order_relaxed); }

private:
    struct Frame { float left; float right; };
    static constexpr unsigned capacity = 32768;
    static constexpr unsigned filterPhases = 128;
    static constexpr double maxCorrection = 0.003;
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    static_assert(std::atomic<float>::is_always_lock_free);

    void makeFilter(double cutoff) {
        // A Blackman-windowed sinc limits the bandwidth before downsampling.
        const unsigned taps = 2 * radius_;
        coefficients_.resize((filterPhases + 1) * taps);
        for (unsigned phase = 0; phase <= filterPhases; ++phase) {
            const double fraction = static_cast<double>(phase) / filterPhases;
            auto* weights = coefficients_.data() + phase * taps;
            double total = 0;
            for (unsigned tap = 0; tap < taps; ++tap) {
                const double distance = static_cast<double>(tap) - radius_ + 1 - fraction;
                const double angle = std::numbers::pi * distance;
                const double window = 0.42 + 0.5 * std::cos(angle / radius_) +
                                      0.08 * std::cos(2 * angle / radius_);
                const double sinc = distance == 0 ? 2 * cutoff : std::sin(2 * cutoff * angle) / angle;
                weights[tap] = static_cast<float>(sinc * window);
                total += weights[tap];
            }
            for (unsigned tap = 0; tap < taps; ++tap)
                weights[tap] = static_cast<float>(weights[tap] / total);
        }
    }

    Frame interpolate(std::uint64_t center, double fraction) const noexcept {
        const unsigned taps = 2 * radius_;
        const double phase = fraction * filterPhases;
        const auto index = static_cast<unsigned>(phase);
        const auto mix = static_cast<float>(phase - index);
        const auto* lower = coefficients_.data() + index * taps;
        const auto* upper = lower + taps;
        const auto start = static_cast<unsigned>((center - radius_ + 1) & (capacity - 1));
        const unsigned first = std::min(taps, capacity - start);
        Frame result{};
        const auto accumulate = [&](const Frame* source, unsigned begin, unsigned end) {
            for (unsigned tap = begin; tap < end; ++tap, ++source) {
                const float weight = lower[tap] + mix * (upper[tap] - lower[tap]);
                result.left += source->left * weight;
                result.right += source->right * weight;
            }
        };
        accumulate(frames_.data() + start, 0, first);
        accumulate(frames_.data(), first, taps);
        return result;
    }

    std::array<Frame, capacity> frames_;
    std::vector<float> coefficients_;
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    std::atomic<unsigned> overflows_{0};
    std::atomic<unsigned> fill_{0};
    std::atomic<unsigned> drops_{0};
    std::atomic<float> targetVolume_{1.0f};
    bool overflowing_ = false;
    float volume_ = 1.0f;
    unsigned seenOverflows_ = 0;
    unsigned radius_ = 0;
    double ratio_ = 1;
    double nominalDemand_ = 0;
    unsigned inputPeriod_ = 0;
    unsigned recoveryDelay_ = 0;
    unsigned recoveryFrames_ = 0;
    double baseReserve_ = 0;
    double reserve_ = 0;
    double reserveDecay_ = 0;
    double maxReserve_ = 0;
    double phase_ = 0;
    double averageFill_ = 0;
    double smoothing_ = 0;
    double correctionGain_ = 0;
    bool primed_ = false;
    bool needsHistory_ = false;
};
