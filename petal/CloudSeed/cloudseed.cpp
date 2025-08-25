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
int preset;
Led led1, led2;

// deadband threshold: ignore pot changes smaller than one step (0.002 ~= 1/500, so each pot has 500 steps)
constexpr float PARAM_EPS = 0.002f;
// update analog controls every N audio blocks to reduce audio-thread work
constexpr int CONTROL_UPDATE_BLOCKS = 4;

// For fade outs of controls when bypass pressed
constexpr int EARLY_BYPASS_BLOCKS = 500 / CONTROL_UPDATE_BLOCKS;  // 500ms when 48000hz Fs and 48 block size
constexpr int LATE_BYPASS_BLOCKS = 12000 / CONTROL_UPDATE_BLOCKS;


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
            valueRanges[(int)p] = (1.0f - v) * 2.0f;
        else
            valueRanges[(int)p] = v * 2.0f;
    }

    updateParms = true;
}

void cyclePreset()
{
    preset += 1;
    if (preset > 3) {
        preset = 0;
    }

    //reverb->ClearBuffers();
    applyPreset(preset);
}


// This runs at a fixed rate, to prepare audio samples
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    cpu_meter.OnBlockStart();

    // Process digital controls every block (fast)
    hw.ProcessDigitalControls();
    led1.Update();
    led2.Update();

    static int earlyBypassCountdown = EARLY_BYPASS_BLOCKS;
    static int lateBypassCountdown = LATE_BYPASS_BLOCKS;

    static int control_block_ctr = 0;
    static float dryValue, earlyValue, lateValue, lineDecayValue, diffusionValue, tapDecayValue;
    static float prevEarlyOut, prevLateOut, prevLineDecay, prevDiffusion, prevTapDecay;
    static int prevNumLines = -1;

    static float led1Level = 0.9f;
    static float led2Level = 0.1f;
    static unsigned int led2Timer = 10;
    static bool led2State = false;

    // Downsample analog control reads to reduce ADC work and avoid calling SetParameter unnecessarily.
    if ((--control_block_ctr <= 0 && !bypassed) || updateParms)
    {
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
            float v = presetValues[(int)::Parameter::EarlyOut];
            float scaled = (earlyValue <= 0.5f)
                                ? (2.0f * v * earlyValue)
                                : (2.0f * (1.0f - v) * earlyValue + (2.0f * v - 1.0f));
            reverb->SetParameter(::Parameter::EarlyOut, scaled);
            prevEarlyOut = earlyValue;
        }
        if (fabsf(prevLateOut - lateValue) > PARAM_EPS || updateParms) {
            float v = presetValues[(int)::Parameter::MainOut];
            float scaled = (lateValue <= 0.5f)
                                ? (2.0f * v * lateValue)
                                : (2.0f * (1.0f - v) * lateValue + (2.0f * v - 1.0f));
            reverb->SetParameter(::Parameter::MainOut, scaled);
            prevLateOut = lateValue;
        }
        if (fabsf(prevLineDecay - lineDecayValue) > PARAM_EPS || updateParms) {
            float v = presetValues[(int)::Parameter::LineDecay];
            float scaled = (lineDecayValue <= 0.5f)
                                ? (2.0f * v * lineDecayValue)
                                : (2.0f * (1.0f - v) * lineDecayValue + (2.0f * v - 1.0f));
            reverb->SetParameter(::Parameter::LineDecay, scaled);
            prevLineDecay = lineDecayValue;
        }
        if (fabsf(prevDiffusion - diffusionValue) > PARAM_EPS || updateParms) {
            float v = presetValues[(int)::Parameter::LateDiffusionFeedback];
            float scaled = (diffusionValue <= 0.5f)
                                ? (2.0f * v * diffusionValue)
                                : (2.0f * (1.0f - v) * diffusionValue + (2.0f * v - 1.0f));
            reverb->SetParameter(::Parameter::LateDiffusionFeedback, scaled);
            prevDiffusion = diffusionValue;
        }
        if (fabsf(prevTapDecay - tapDecayValue) > PARAM_EPS || updateParms) {
            float v = presetValues[(int)::Parameter::TapDecay];
            float scaled = (tapDecayValue <= 0.5f)
                                ? (2.0f * v * tapDecayValue)
                                : (2.0f * (1.0f - v) * tapDecayValue + (2.0f * v - 1.0f));
            reverb->SetParameter(::Parameter::TapDecay, scaled);
            prevTapDecay = tapDecayValue;
        }    
    }


    // Delay Line Switches
    //     - The .Pressed() function below counts an 'ON' switch as pressed.
    //     - Total number of switches on sets how many delay lines are activated (1 - 5)
    int switches[4] = {Terrarium::SWITCH_1, Terrarium::SWITCH_2, Terrarium::SWITCH_3, Terrarium::SWITCH_4}; // Can this be moved elsewhere?
    
    int numDelayLines = 1;
    for(int i=0; i<4; i++) {
        if (hw.switches[switches[i]].Pressed()) {
            numDelayLines += 1;
        }
    }
    if (prevNumLines != numDelayLines) {
        //reverb->ClearBuffers();  //TODO is this needed?
        reverb->SetParameter(::Parameter::LineCount, numDelayLines);
        prevNumLines = numDelayLines;
    }


    float reverbIn[48];
    float reverbOut[48];


    // ToDo: Do footswitches need de-bouncing?
    
    // Cycle available models
    if (hw.switches[Terrarium::FOOTSWITCH_2].RisingEdge()) {  
        cyclePreset();
    }
    
    // (De-)Activate bypass and toggle LED when left footswitch is pressed
    if (hw.switches[Terrarium::FOOTSWITCH_1].RisingEdge()) {
        if (bypassing || bypassed) {
            bypassing = bypassed = false;
            led1.Set(led1Level);
            earlyBypassCountdown = EARLY_BYPASS_BLOCKS;
            lateBypassCountdown = LATE_BYPASS_BLOCKS;
            // Re-apply current preset when turning effect back on
            applyPreset(preset);
            // Force parameter refresh on next control tick
            prevEarlyOut = prevLateOut = prevLineDecay = prevDiffusion = prevTapDecay;
        }
        else {
            bypassing = true;
            led1.Set(0);
        }
    }

    
    if (bypassing) {
        for (size_t i = 0; i < size; i++) {
            reverbIn[i] = 0;
        }
    }    
    else if (!bypassed) {
        for (size_t i = 0; i < size; i++) {
            reverbIn[i] = in[0][i];
        }
    }
    
    if (!bypassed) {
        reverb->Process(reverbIn, reverbOut, 48);
        for (size_t i = 0; i < size; i++) {  
            out[0][i] = (in[0][i] * dryValue) + reverbOut[i];
        }
    }
    else {
        for (size_t i = 0; i < size; i++) {  
            out[0][i] = in[0][i];
        }
    }


    // LED 2 shows which preset selected.
    switch (preset) {
        case 0:
            led2State = false;
            break;
        case 1:
            if (--led2Timer <= 0) {
                led2State = !led2State;
                led2Timer = rand() % 400 + 150;
            }
            break;
        case 2:
            if (--led2Timer <= 0) {
                led2State = !led2State;
                led2Timer = rand() % 100 + 50;
            }
            break;
        case 3:
            led2State = true;
            break;
    }
    if (led2State) {
        led2.Set(led2Level);
    }
    else {
        led2.Set(0);
    }


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

    while(1) {
        // Work out CPU load
        static uint32_t last_ms = 0;
        uint32_t now_ms = System::GetNow();
        if ((now_ms - last_ms) >= 250) {
            float load = cpu_meter.GetAvgCpuLoad() * 100.0f;
            cpu_led_indicator.UpdateFromLoad(load, now_ms);
            last_ms = now_ms;
        }

        // advance indicator state machine
        cpu_led_indicator.Tick(System::GetNow());

        System::Delay(10);
    }
}
