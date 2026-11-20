#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

class AudioQueue {
public:
    bool reset(double inputRate, double outputRate, unsigned inputPeriod, unsigned outputPeriod) noexcept {
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
        overflows_.store(0, std::memory_order_relaxed);
        seenOverflows_ = 0;
        fill_.store(0, std::memory_order_relaxed);
        drops_.store(0, std::memory_order_relaxed);
        ratio_ = inputRate / outputRate;
        inputPeriod_ = inputPeriod;
        reserve_ = std::max(inputPeriod * 0.5 + 4.0, inputRate * 0.002);
        maxReserve_ = std::max(reserve_, inputRate * 0.020);
        averageFill_ = reserve_;
        smoothing_ = std::min(1.0, outputPeriod / (outputRate * 0.5));
        phase_ = 0;
        primed_ = false;
        return outputPeriod * ratio_ + maxReserve_ + inputPeriod + 2 < capacity;
    }

    void push(const float* samples, unsigned frames, unsigned channels) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (frames > capacity - (write - read)) {
            overflows_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        for (unsigned i = 0; i < frames; ++i) {
            const auto* source = samples + i * channels;
            frames_[(write + i) & (capacity - 1)] = {source[0], source[channels > 1 ? 1 : 0]};
        }
        write_.store(write + frames, std::memory_order_release);
    }

    bool pull(float* samples, unsigned frames, unsigned channels) noexcept {
        auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        auto available = write - read;
        const double demand = frames * ratio_;
        const auto startFill = static_cast<std::uint64_t>(std::ceil(demand + reserve_));
        const auto overflows = overflows_.load(std::memory_order_relaxed);
        if (overflows != seenOverflows_) {
            seenOverflows_ = overflows;
            primed_ = false;
            drops_.fetch_add(1, std::memory_order_relaxed);
        }

        if (!primed_ && available < startFill) {
            fill_.store(static_cast<unsigned>(available), std::memory_order_relaxed);
            std::fill_n(samples, static_cast<std::size_t>(frames) * channels, 0.0f);
            return false;
        }

        // A device can take several I/O periods to begin pulling audio.
        // Start close to the live signal even when capture is already ahead.
        if (!primed_ && available > startFill + inputPeriod_) {
            available = startFill + inputPeriod_ / 2;
            read = write - available;
            phase_ = 0;
        }
        primed_ = true;

        const double remainder = std::max(0.0, static_cast<double>(available) - demand);
        averageFill_ += (remainder - averageFill_) * smoothing_;
        const double correction = std::clamp((averageFill_ - reserve_) * 0.00002, -0.003, 0.003);
        const double step = ratio_ * (1.0 + correction);
        const double advance = phase_ + frames * step;
        unsigned playable = frames;
        if (available < static_cast<std::uint64_t>(std::floor(advance)) + 2) {
            const double usable = static_cast<double>(available) - 2.0 - phase_;
            playable = static_cast<unsigned>(std::clamp(std::floor(usable / step), 0.0, double(frames)));
            if (playable < frames) {
                drops_.fetch_add(1, std::memory_order_relaxed);
                reserve_ = std::min(maxReserve_, reserve_ + inputPeriod_ * 0.5);
                std::fill_n(samples + std::size_t{playable} * channels,
                            std::size_t{frames - playable} * channels, 0.0f);
            }
        }

        for (unsigned i = 0; i < playable; ++i) {
            const auto offset = static_cast<std::uint64_t>(phase_);
            const auto& a = frames_[(read + offset) & (capacity - 1)];
            const auto& b = frames_[(read + offset + 1) & (capacity - 1)];
            const auto fraction = static_cast<float>(phase_ - static_cast<double>(offset));
            const float left = std::lerp(a.left, b.left, fraction);
            const float right = std::lerp(a.right, b.right, fraction);
            if (channels == 1) {
                samples[i] = (left + right) * 0.5f;
            } else {
                auto* output = samples + i * channels;
                output[0] = left;
                output[1] = right;
                std::fill(output + 2, output + channels, 0.0f);
            }
            phase_ += step;
        }
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
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<unsigned>::is_always_lock_free);

    std::array<Frame, capacity> frames_;
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    std::atomic<unsigned> overflows_{0};
    std::atomic<unsigned> fill_{0};
    std::atomic<unsigned> drops_{0};
    unsigned seenOverflows_ = 0;
    double ratio_ = 1;
    unsigned inputPeriod_ = 0;
    double reserve_ = 0;
    double maxReserve_ = 0;
    double phase_ = 0;
    double averageFill_ = 0;
    double smoothing_ = 0;
    bool primed_ = false;
};
