// Modified version of DaisyCloudSeed by Keith Bloemer.
// Intended for the Terrarrium guitar pedal hardware.
// Code has been modified for mono processing (originally stereo) to 
// allow up to 5 delay lines on the Daisy Seed.

#include "daisy_petal.h"
#include "daisysp.h"
#include "terrarium.h"

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <array>
#include "util/CpuLoadMeter.h"
#include "CpuLedIndicator.h"
#include <atomic>
#include <cstring>

// Fast LFSR PRNG for audio/ISR-safe randomness
static inline uint32_t lfsr_rand()
{
    static uint32_t lfsr = 0xACE1u;
    lfsr ^= lfsr << 13;
    lfsr ^= lfsr >> 17;
    lfsr ^= lfsr << 5;
    return lfsr;
}

#include "../../CloudSeed/Default.h"
#include "../../CloudSeed/ReverbController.h"
#include "../../CloudSeed/FastSin.h"
#include "../../CloudSeed/AudioLib/ValueTables.h"
#include "../../CloudSeed/AudioLib/MathDefs.h"

using namespace daisy;
using namespace daisysp;
using namespace terrarium;  // This is important for mapping the correct controls to the Daisy Seed on Terrarium PCB

DaisyPetal hw;
::daisy::Parameter dry, earlyOut, lateOut, lineDecay, diffusion, tapDecay;

// Use enum-indexed arrays for preset values and their ranges. This is
// simpler and type-safe compared to string-keyed maps.
static std::array<float, (int)::Parameter::Count> presetValues = {};
static std::array<float, (int)::Parameter::Count> valueRanges  = {};

bool updateParms;
bool bypassing;
bool bypassed;
bool pendingBypass;
std::atomic<int> preset{0};
Led led1, led2;

// Shared flags/commands set from main() (non-audio) and consumed in audio callback
static std::atomic<int> g_desiredNumLines{1};
static std::atomic<bool> g_cyclePresetRequested{false};
static std::atomic<bool> g_toggleBypassRequested{false};
// Precomputed switch indices for main loop
static const int g_delay_switches[4] = { Terrarium::SWITCH_1, Terrarium::SWITCH_2, Terrarium::SWITCH_3, Terrarium::SWITCH_4 };

// deadband threshold: ignore pot changes smaller than one step (0.002 ~= 1/500, so each pot has 500 steps)
constexpr float PARAM_EPS = 0.002f;
// update analog controls every N audio blocks to reduce audio-thread work
constexpr int CONTROL_UPDATE_BLOCKS = 4;

// For fade outs of controls when bypass pressed
constexpr int EARLY_BYPASS_BLOCKS = 500 / CONTROL_UPDATE_BLOCKS;  // 500ms when 48000hz Fs and 48 block size
constexpr int LATE_BYPASS_BLOCKS = 12000 / CONTROL_UPDATE_BLOCKS;

// Maximum audio block size we expect to handle; buffers live in BSS to avoid stack churn
constexpr size_t AUDIO_MAX_BLOCK = 128;

// Persistent reverb IO buffers (BSS)
static float g_reverbIn[AUDIO_MAX_BLOCK];
static float g_reverbOut[AUDIO_MAX_BLOCK];


daisy::CpuLoadMeter cpu_meter;
// Cpu LED indicator (extracted class)
CpuLedIndicator cpu_led_indicator;

CloudSeed::ReverbController* reverb = 0;


// For allocating delay line memory to SDRAM (64MB available on Daisy)
#define CUSTOM_POOL_SIZE (48*1024*1024)
DSY_SDRAM_BSS char custom_pool[CUSTOM_POOL_SIZE];
size_t pool_index = 0;
int allocation_count = 0;

void* custom_pool_allocate(size_t size)
{
    if (pool_index + size >= CUSTOM_POOL_SIZE) {
        return 0;
    }
    void* ptr = &custom_pool[pool_index];
    pool_index += size;
    return ptr;
}


static inline void applyPreset(int idx)
{
    switch (idx)
    {
        //case 0: reverb->initFactorySmallRoom(); break;        
        case 0: reverb->initFactorySmallRoom(); break;
        case 1: reverb->initFactoryMediumSpace(); break;
        case 2: reverb->initFactoryChorus(); break;
        case 3: reverb->initFactoryRubiKaFields(); break;
        
        default: break;

        /**
         * case 5: reverb->initFactoryHyperplane(); break;
         * case 4: reverb->initFactoryDullEchos(); break; 
         * case 3: reverb->initFactoryNoiseInTheHallway(); break;
         * case 1: reverb->initGpt5AiryWideChamber(); break;
         * case 8: reverb->initGpt5NearInfinitePad(); break;
         */
    }

    // The controls will have the preset value at pot mid point and scaled so
    // 0 is not reached before pot low and 1 is not reached before pot max.
    // Use the Parameter enum directly and store results in enum-indexed arrays.
    const ::Parameter paramsToRead[] = {
        ::Parameter::EarlyOut,
        ::Parameter::MainOut,
        ::Parameter::LineDecay,
        ::Parameter::TapDecay,
        ::Parameter::LateDiffusionFeedback
    };

    for (auto p : paramsToRead)
    {
        float v = reverb->GetParameter(p);
        presetValues[(int)p] = v;
        if (v <= 0.5f)
            valueRanges[(int)p] = v * 2.0f;
        else
            valueRanges[(int)p] = (1.0f - v) * 2.0f;
    }

    updateParms = true;
}

void cyclePreset()
{
    int p = preset.load();
    p += 1;
    if (p > 3) {
        p = 0;
    }
    preset.store(p);

    //reverb->ClearBuffers();
    applyPreset(p);
}


// This runs at a fixed rate, to prepare audio samples
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    cpu_meter.OnBlockStart();

    // NOTE: digital controls and LED updates are processed on the main loop
    // to keep the audio thread light. The main loop sets atomic flags for
    // events which the audio callback consumes here.

    // Handle requests from main loop
    if (g_cyclePresetRequested.exchange(false)) {
        if (reverb) cyclePreset();
    }
    if (g_toggleBypassRequested.exchange(false)) {
        // Simulate footswitch press behavior: toggle bypassing/bypassed states
        if (bypassing || bypassed) {
            bypassing = bypassed = false;
            led1.Set(0.9f);
            // reset fade counters
            // (these will be picked up next time analog controls are processed)
        }
        else {
            bypassing = true;
            led1.Set(0);
        }
    }

    static int earlyBypassCountdown = EARLY_BYPASS_BLOCKS;
    static int lateBypassCountdown = LATE_BYPASS_BLOCKS;

    static int control_block_ctr = 0;
    static float dryValue, earlyValue, lateValue, lineDecayValue, diffusionValue, tapDecayValue;
    static float prevEarlyOut, prevLateOut, prevLineDecay, prevDiffusion, prevTapDecay;
    static int prevNumLines = -1;

        

    // Downsample analog control reads to reduce ADC work and avoid calling SetParameter unnecessarily.
    if ((--control_block_ctr <= 0 && !bypassed) || updateParms)
    {
        led1.Update();
        led2.Update();
        control_block_ctr = CONTROL_UPDATE_BLOCKS;

        hw.ProcessAnalogControls();
        dryValue       = dry.Process();

        if (!bypassed) {
            earlyValue     = earlyOut.Process();
            lateValue      = lateOut.Process();
            lineDecayValue      = lineDecay.Process();
            diffusionValue = diffusion.Process();
            tapDecayValue       = tapDecay.Process();
        }

        // When bypassing allow trails then fade everything off before we stop processing reverb
        if (bypassing) {
            if (--earlyBypassCountdown <= 0 && earlyBypassCountdown > -EARLY_BYPASS_BLOCKS) {
                float earlyReduxFactor = 1.0f - ((float)(-earlyBypassCountdown) / (float)EARLY_BYPASS_BLOCKS);
                earlyValue *= earlyReduxFactor;     
            }
            else if (earlyBypassCountdown <= -EARLY_BYPASS_BLOCKS) {
                earlyValue = 0;

                if (--lateBypassCountdown > 0) {
                    float lateReduxFactor = (float)lateBypassCountdown / (float)LATE_BYPASS_BLOCKS;
                    // lateValue = lateValue * lateReduxFactor;
                    tapDecayValue *= lateReduxFactor;
                    lineDecayValue *= lateReduxFactor;
                }
                else {
                    lateValue = tapDecayValue = lineDecayValue = 0;
                    bypassing = false;
                    bypassed = true;
                    reverb->ClearBuffers();
                }
            }
        }

        if (fabsf(prevEarlyOut - earlyValue) > PARAM_EPS || updateParms) {
            float presetValue = presetValues[(int)::Parameter::EarlyOut];
            float factor      = valueRanges[(int)::Parameter::EarlyOut];
            float scaled      = presetValue + (earlyValue - 0.5f) * factor;
            reverb->SetParameter(::Parameter::EarlyOut, scaled);
            prevEarlyOut = earlyValue;
        }
        if (fabsf(prevLateOut - lateValue) > PARAM_EPS || updateParms) {
            float presetValue = presetValues[(int)::Parameter::MainOut];
            float factor      = valueRanges[(int)::Parameter::MainOut];
            float scaled      = presetValue + (lateValue - 0.5f) * factor;
            reverb->SetParameter(::Parameter::MainOut, scaled);
            prevLateOut = lateValue;
        }
        if (fabsf(prevLineDecay - lineDecayValue) > PARAM_EPS || updateParms) {
            float presetValue = presetValues[(int)::Parameter::LineDecay];
            float factor      = valueRanges[(int)::Parameter::LineDecay];
            float scaled      = presetValue + (lineDecayValue - 0.5f) * factor;
            reverb->SetParameter(::Parameter::LineDecay, scaled);
            prevLineDecay = lineDecayValue;
        }
        if (fabsf(prevDiffusion - diffusionValue) > PARAM_EPS || updateParms) {
            float presetValue = presetValues[(int)::Parameter::LateDiffusionFeedback];
            float factor      = valueRanges[(int)::Parameter::LateDiffusionFeedback];
            float scaled      = presetValue + (diffusionValue - 0.5f) * factor;
            reverb->SetParameter(::Parameter::LateDiffusionFeedback, scaled);
            prevDiffusion = diffusionValue;
        }
        if (fabsf(prevTapDecay - tapDecayValue) > PARAM_EPS || updateParms) {
            float presetValue = presetValues[(int)::Parameter::TapDecay];
            float factor      = valueRanges[(int)::Parameter::TapDecay];
            float scaled      = presetValue + (tapDecayValue - 0.5f) * factor;
            reverb->SetParameter(::Parameter::TapDecay, scaled);
            prevTapDecay = tapDecayValue;
        }
    }


    // Delay Line Switches
    // The main loop computes desired num lines and writes into g_desiredNumLines.
    // Read it here and update reverb if changed.
    int numDelayLines = g_desiredNumLines.load();
    if (prevNumLines != numDelayLines) {
    if (reverb) reverb->SetParameter(::Parameter::LineCount, numDelayLines);
        prevNumLines = numDelayLines;
    }


    // Footswitches and switches are processed on the main loop; the audio
    // thread reacts to requests via atomics. Use persistent BSS buffers to
    // avoid stack churn; cap size to AUDIO_MAX_BLOCK to avoid overruns.
    if (size > AUDIO_MAX_BLOCK) {
        size = AUDIO_MAX_BLOCK;
    }

    if (bypassing) {
        memset(g_reverbIn, 0, size * sizeof(float));
    }
    else if (!bypassed) {
        memcpy(g_reverbIn, in[0], size * sizeof(float));
    }

    if (!bypassed) {
        if (reverb) {
            reverb->Process(g_reverbIn, g_reverbOut, (int)size);
        } else {
            memset(g_reverbOut, 0, size * sizeof(float));
        }
        for (size_t i = 0; i < size; i++) {
            out[0][i] = (in[0][i] * dryValue) + g_reverbOut[i];
        }
    }
    else {
        memcpy(out[0], in[0], size * sizeof(float));
    }


    // LED2 handled in main loop to avoid audio-thread work.


    cpu_meter.OnBlockEnd();
}

int main(void)
{
    float samplerate;

    hw.Init();
    samplerate = hw.AudioSampleRate();

    dry.Init(hw.knob[Terrarium::KNOB_1], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    earlyOut.Init(hw.knob[Terrarium::KNOB_2], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    lateOut.Init(hw.knob[Terrarium::KNOB_3], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    diffusion.Init(hw.knob[Terrarium::KNOB_4], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 
    tapDecay.Init(hw.knob[Terrarium::KNOB_5], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 
    lineDecay.Init(hw.knob[Terrarium::KNOB_6], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 

    led1.Init(hw.seed.GetPin(Terrarium::LED_1), false);
    led1.Update();
    led2.Init(hw.seed.GetPin(Terrarium::LED_2), false);
    led2.Update();
    
    cpu_meter.Init(samplerate, hw.AudioBlockSize());
    cpu_led_indicator.Init(hw);

    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();
    
    reverb = new CloudSeed::ReverbController(samplerate);
    reverb->ClearBuffers();
    
    bypassed = true;
    applyPreset(0);
    updateParms = true;

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    // Main loop handles low-rate digital IO and indicators. Keep analog controls
    // processing in the audio thread to keep their timing tight.
    while(1) {
    // Process digital controls and LEDs here (non-audio thread)
    hw.ProcessDigitalControls();

        // Handle footswitch presses and delay-line switches here and communicate
        // desired actions to the audio thread via atomics.
        // Footswitch 1 toggles bypass; Footswitch 2 cycles preset.
        if (hw.switches[Terrarium::FOOTSWITCH_1].RisingEdge()) {
            g_toggleBypassRequested.store(true);
        }
        if (hw.switches[Terrarium::FOOTSWITCH_2].RisingEdge()) {
            g_cyclePresetRequested.store(true);
        }

        // Compute desired number of delay lines from the three switches. Keep
        // this calculation here so the audio thread only reads the atomic value.
        int desired = 2;
        for (int i = 0; i < 4; ++i) {
            if (hw.switches[g_delay_switches[i]].Pressed())
                desired += 1;
        }
        g_desiredNumLines.store(desired);

        // LED2 shows preset activity; deterministic blink counters.
        static int led2Counter = 0;
        static bool led2State = false;
        static int prevPreset = -1;

        int curPreset = preset.load();
        if (curPreset != prevPreset) {
            prevPreset = curPreset;
            led2Counter = 0;
            if (curPreset == 3) {
                led2State = true; // steady on
            } else {
                led2State = false; // start off for blink presets
            }
        }

        switch (curPreset) {
            case 0:
                led2State = false;
                break;
            case 1:
                if (++led2Counter >= 30) { // ~300ms @ ~10ms loop
                    led2State = !led2State;
                    led2Counter = 0;
                }
                break;
            case 2:
                if (++led2Counter >= 10) { // ~100ms @ ~10ms loop
                    led2State = !led2State;
                    led2Counter = 0;
                }
                break;
            case 3:
                led2State = true; // steady on
                break;
        }

        led2.Set(led2State ? 0.1f : 0.0f);
        // Update after Set so change takes effect immediately

        // Work out CPU load every 250ms
        static uint32_t last_ms = 0;
        uint32_t now_ms = System::GetNow();
        if ((now_ms - last_ms) >= 250) {
            float load = cpu_meter.GetAvgCpuLoad() * 100.0f;
            cpu_led_indicator.UpdateFromLoad(load, now_ms);
            last_ms = now_ms;
        }

        // advance indicator state machine
        cpu_led_indicator.Tick(System::GetNow());

        System::Delay(8);
    }
}
