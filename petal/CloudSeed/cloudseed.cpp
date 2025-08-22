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
::daisy::Parameter dry, earlyOut, lateOut, time, diffusion, tapDecay;

bool bypassing;
bool bypassed;
bool pendingBypass;
int c;
Led led1, led2;

// deadband threshold: ignore changes smaller than one step (0.002 ~= 1/500)
constexpr float PARAM_EPS = 0.002f;
// update analog controls every N audio blocks to reduce audio-thread work
constexpr int CONTROL_UPDATE_BLOCKS = 4;

// For fade outs of controls when bypass pressed
constexpr int EARLY_BYPASS_BLOCKS = 500 / CONTROL_UPDATE_BLOCKS;  // 500ms when 48000hz Fs and 48 block size
constexpr int LATE_BYPASS_BLOCKS = 12000 / CONTROL_UPDATE_BLOCKS;


daisy::CpuLoadMeter cpu_meter;

CloudSeed::ReverbController* reverb = 0;

// Cpu LED indicator (extracted class)
CpuLedIndicator cpu_led_indicator;


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
        case 0: reverb->initFactorySmallRoom(); break;
        case 1: reverb->initGpt5AiryWideChamber(); break;
        case 2: reverb->initFactoryMediumSpace(); break;
        case 3: reverb->initFactoryNoiseInTheHallway(); break;
        case 4: reverb->initFactoryDullEchos(); break;
        case 5: reverb->initFactoryHyperplane(); break;
        case 6: reverb->initFactoryChorus(); break;
        case 7: reverb->initFactoryRubiKaFields(); break;
        case 8: reverb->initGpt5NearInfinitePad(); break;
        default: break;
    }
}

void cyclePreset()
{
    c += 1;
    if (c > 8) {
        c = 0;
    }

    reverb->ClearBuffers();
    applyPreset(c);
}


// This runs at a fixed rate, to prepare audio samples
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    // Notify CpuLoadMeter of block start
    cpu_meter.OnBlockStart();

    // Process digital controls every block (fast)
    hw.ProcessDigitalControls();
    led1.Update();
    led2.Update();

    static int earlyBypassCountdown = EARLY_BYPASS_BLOCKS;
    static int lateBypassCountdown = LATE_BYPASS_BLOCKS;

    static int control_block_ctr = 0;
    static float dryValue = 0.0f, earlyValue = 0.0f, lateValue = 0.0f,
                 timeValue = 0.0f, diffusionValue = 0.0f, tapDecayValue = 0.0f;
    static float prevEarlyOut, prevLateOut, prevTime, prevDiffusion, prevTapDecay;
    static int prevNumLines;

    // Downsample analog control reads to reduce ADC work and avoid calling SetParameter unnecessarily.
    if (--control_block_ctr <= 0 && !bypassed)
    {
        control_block_ctr = CONTROL_UPDATE_BLOCKS;

        hw.ProcessAnalogControls();
        dryValue       = dry.Process();

        if (!bypassed) {
            earlyValue     = earlyOut.Process();
            lateValue      = lateOut.Process();
            timeValue      = time.Process();
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
                    timeValue *= lateReduxFactor;
                }
                else {
                    lateValue = tapDecayValue = timeValue = 0;
                    bypassing = false;
                    bypassed = true;
                    reverb->ClearBuffers();
                }
            }
        }

        if (!bypassed) {
            if (fabsf(prevEarlyOut - earlyValue) > PARAM_EPS) {
                reverb->SetParameter(::Parameter::EarlyOut, earlyValue);
                prevEarlyOut = earlyValue;
            }
            if (fabsf(prevLateOut - lateValue) > PARAM_EPS) {
                reverb->SetParameter(::Parameter::MainOut, lateValue);
                prevLateOut = lateValue;
            }
            if (fabsf(prevTime - timeValue) > PARAM_EPS) {
                reverb->SetParameter(::Parameter::LineDecay, timeValue);
                prevTime = timeValue;
            }
            if (fabsf(prevDiffusion - diffusionValue) > PARAM_EPS) {
                reverb->SetParameter(::Parameter::LateDiffusionFeedback, diffusionValue);
                prevDiffusion = diffusionValue;
            }
            if (fabsf(prevTapDecay - tapDecayValue) > PARAM_EPS) {
                reverb->SetParameter(::Parameter::TapDecay, tapDecayValue);
                prevTapDecay = tapDecayValue;
            }
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
            led1.Set(true);
            earlyBypassCountdown = EARLY_BYPASS_BLOCKS;
            lateBypassCountdown = LATE_BYPASS_BLOCKS;
        }
        else {
            bypassing = true;
            led1.Set(false);
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


    // Notify CpuLoadMeter of block end
    cpu_meter.OnBlockEnd();
}

int main(void)
{
    float samplerate;

    hw.Init();
    samplerate = hw.AudioSampleRate();
    // Initialize CPU load meter for the audio configuration
    cpu_meter.Init(samplerate, hw.AudioBlockSize());
    // Initialize CPU LED indicator
    cpu_led_indicator.Init(hw);
    c = 0;

    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();
    
    reverb = new CloudSeed::ReverbController(samplerate);
    reverb->ClearBuffers();

    bypassed = true;

    dry.Init(hw.knob[Terrarium::KNOB_1], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    earlyOut.Init(hw.knob[Terrarium::KNOB_2], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    lateOut.Init(hw.knob[Terrarium::KNOB_3], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    diffusion.Init(hw.knob[Terrarium::KNOB_4], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 
    tapDecay.Init(hw.knob[Terrarium::KNOB_5], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 
    time.Init(hw.knob[Terrarium::KNOB_6], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 


    led1.Init(hw.seed.GetPin(Terrarium::LED_1), false);
    led1.Update();
    led2.Init(hw.seed.GetPin(Terrarium::LED_2), false);
    led2.Update();

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
