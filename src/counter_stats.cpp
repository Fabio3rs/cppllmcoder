#include "counter_stats.hpp"

void CounterStatsRecorder::incrementTokenCount(int delta) {
    if (delta > 0) {
        total_tokens_ += static_cast<size_t>(delta);
    }
}

size_t CounterStatsRecorder::totalTokens() const { return total_tokens_; }
