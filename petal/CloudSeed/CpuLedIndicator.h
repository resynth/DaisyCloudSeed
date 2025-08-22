#pragma once
#include "daisy_petal.h"

class CpuLedIndicator {
  public:
  CpuLedIndicator() {}
  void Init(daisy::DaisyPetal& hw) { hw_ = &hw; }
  // Call when you have a new averaged CPU load sample (percent 0..100)
  void UpdateFromLoad(float loadPercent, uint32_t nowMs) {
    if (loadPercent >= 91.0f) {
      if (hw_) hw_->seed.SetLed(true);
      return;
    }
    int newTarget = 0;
    if (loadPercent >= 71.0f) newTarget = 4;
    else if (loadPercent >= 51.0f) newTarget = 3;
    else if (loadPercent >= 31.0f) newTarget = 2;
    else if (loadPercent >= 11.0f) newTarget = 1;
    else newTarget = 0;

    if (newTarget != target_) {
      target_ = newTarget;
      index_ = 0;
      state_ = IDLE;
      phase_start_ = nowMs; // restart gap timing
    }
  }
  // Call frequently from your main loop to advance non-blocking flash state
  void Tick(uint32_t nowMs) {
    if (!hw_) return;

    switch (state_) {
      case IDLE:
        if (target_ > 0) {
          if (phase_start_ == 0 || (nowMs - phase_start_) >= GAP_MS) {
            state_ = ON;
            phase_start_ = nowMs;
            index_ = 1;
            hw_->seed.SetLed(true);
          } else {
            hw_->seed.SetLed(false);
          }
        } else {
          hw_->seed.SetLed(false);
        }
        break;
      case ON:
        if ((nowMs - phase_start_) >= ON_MS) {
          state_ = OFF;
          phase_start_ = nowMs;
          hw_->seed.SetLed(false);
        }
        break;
      case OFF:
        if ((nowMs - phase_start_) >= OFF_MS) {
          if (index_ < target_) {
            index_++;
            state_ = ON;
            phase_start_ = nowMs;
            hw_->seed.SetLed(true);
          } else {
            state_ = IDLE;
            phase_start_ = nowMs; // start gap
            index_ = 0;
            hw_->seed.SetLed(false);
          }
        }
        break;
    }
  }

  private:
  daisy::DaisyPetal* hw_ = nullptr;
  int target_ = 0;
  int index_ = 0;
  uint32_t phase_start_ = 0;

  enum State { IDLE = 0, ON, OFF } state_ = IDLE;

  // timing (ms)
  static constexpr uint32_t ON_MS  = 120;
  static constexpr uint32_t OFF_MS = 120;
  static constexpr uint32_t GAP_MS = 600;
};
