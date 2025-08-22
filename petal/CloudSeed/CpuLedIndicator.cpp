#include "CpuLedIndicator.h"

using namespace daisy;

CpuLedIndicator::CpuLedIndicator() {}

void CpuLedIndicator::Init(DaisyPetal& hw) {
    hw_ = &hw;
}

void CpuLedIndicator::UpdateFromLoad(float loadPercent, uint32_t nowMs) {
    if (loadPercent >= 95.0f) {
        latched_ = true;
        if (hw_) hw_->seed.SetLed(true);
        return;
    }
    if (latched_) return; // already latched

    int newTarget = 0;
    if (loadPercent >= 80.0f) newTarget = 4;
    else if (loadPercent >= 60.0f) newTarget = 3;
    else if (loadPercent >= 40.0f) newTarget = 2;
    else if (loadPercent >= 20.0f) newTarget = 1;
    else newTarget = 0;

    if (newTarget != target_) {
        target_ = newTarget;
        index_ = 0;
        state_ = IDLE;
        phase_start_ = nowMs; // restart gap timing
    }
}

void CpuLedIndicator::Tick(uint32_t nowMs) {
    if (!hw_) return;
    if (latched_) return;

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
