#pragma once

#include "agent.hpp"

// In-memory token counter for the PoC.
class CounterStatsRecorder : public IStatsRecorder {
  public:
    void incrementTokenCount(int delta) override;
    size_t totalTokens() const override;

  private:
    size_t total_tokens_ = 0;
};
