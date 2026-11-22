#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace regions {
using Frame = std::int64_t;
struct Interval { Frame first, end; }; // Half-open, in original source frames.
struct Settings { double thresholdDb = -48, gapMs = 180, preMs = 8, postMs = 25; };

class Detector {
    Frame total_, gap_, pre_, post_, first_ = -1, last_ = -1;
    double open_, hold_;
    Frame framesPerBin_;
    void finishRegion() {
        if (first_ < 0) return;
        Interval next{std::max<Frame>(0, first_ - pre_), std::min(total_, last_ + 1 + post_)};
        if (!intervals.empty() && next.first <= intervals.back().end)
            intervals.back().end = next.end;
        else
            intervals.push_back(next);
        first_ = last_ = -1;
    }
public:
    Frame position = 0;
    std::vector<Interval> intervals;
    std::vector<float> peaks;
    Detector(Frame total, double rate, Settings s) : total_(total) {
        auto frames = [rate](double ms) { return Frame(std::llround(ms * rate / 1000)); };
        gap_ = std::max<Frame>(1, frames(s.gapMs));
        pre_ = frames(s.preMs);
        post_ = frames(s.postMs);
        open_ = std::pow(10., s.thresholdDb / 20);
        hold_ = open_ * 0.5011872336; // 6 dB hysteresis for decaying tails.
        framesPerBin_ = std::max<Frame>(1, (total + 4095) / 4096);
        peaks.resize(static_cast<size_t>((total + framesPerBin_ - 1) / framesPerBin_));
    }
    Frame total() const { return total_; }
    Frame binWidth() const { return framesPerBin_; }
    void append(const double* interleaved, int frames, int channels) {
        for (int i = 0; i < frames && position < total_; ++i, ++position) {
            double level = 0;
            for (int ch = 0; ch < channels; ++ch)
                level = std::max(level, std::abs(*interleaved++));
            float& bin = peaks[static_cast<size_t>(position / framesPerBin_)];
            bin = std::max(bin, static_cast<float>(std::min(1., level)));
            if (first_ >= 0 && position - last_ > gap_) finishRegion();
            if (level >= (first_ < 0 ? open_ : hold_)) {
                if (first_ < 0) first_ = position;
                last_ = position;
            }
        }
    }
    void finish() { finishRegion(); }
};
}
