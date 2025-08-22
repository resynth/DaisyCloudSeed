// Modified version of DaisyCloudSeed by Keith Bloemer.
// Intended for the Terrarrium guitar pedal hardware.
// Code has been modified for mono processing (originally stereo) to 
// allow up to 5 delay lines on the Daisy Seed.

#include "daisy_petal.h"
#include "daisysp.h"
#include "terrarium.h"

#include <stdio.h>
#include <stdint.h>
#include "util/CpuLoadMeter.h"

#include "../../CloudSeed/Default.h"
#include "../../CloudSeed/ReverbController.h"
#include "../../CloudSeed/FastSin.h"
#include "../../CloudSeed/AudioLib/ValueTables.h"
#include "../../CloudSeed/AudioLib/MathDefs.h"

using namespace daisy;
using namespace daisysp;
using namespace terrarium;  // This is important for mapping the correct controls to the Daisy Seed on Terrarium PCB

DaisyPetal hw;
::daisy::Parameter earlyOut, mainOut, time, diffusion, tapDecay;

bool bypass;
int c;
Led led1, led2;

float prevEarlyOut, prevMainOut, prevTime, prevDiffusion, prevTapDecay;
int prevNumLines;

// libDaisy CPU load meter (only keep what's needed)
daisy::CpuLoadMeter cpu_meter;

CloudSeed::ReverbController* reverb = 0;

// LED flash state for CPU load indication
static bool cpu_overload_latched = false; // becomes true when load >= 95%
static int cpu_flash_target = 0; // 0..4 flashes
static int cpu_flash_index = 0; // current flash number
static uint32_t cpu_flash_phase_start = 0; // ms timestamp for phase timing
enum CpuFlashState { CFP_IDLE = 0, CFP_ON, CFP_OFF };
static CpuFlashState cpu_flash_state = CFP_IDLE;
// timing (ms)
static const uint32_t CPU_FLASH_ON_MS  = 120;
static const uint32_t CPU_FLASH_OFF_MS = 120;
static const uint32_t CPU_FLASH_GAP_MS = 600; // gap between sequences


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


void cyclePreset()
{
    c += 1;
    if ( c > 7 ) {
        c = 0;
    }

    reverb->ClearBuffers();
        
    if ( c == 0 ) {
            reverb->initFactoryChorus();
    } else if ( c == 1 ) {
            reverb->initFactoryDullEchos();
    } else if ( c == 2 ) {
            reverb->initFactoryHyperplane();
    } else if ( c == 3 ) {
            reverb->initFactoryMediumSpace();
    } else if ( c == 4 ) {
            reverb->initFactoryNoiseInTheHallway();
    } else if ( c == 5 ) {
            reverb->initFactoryRubiKaFields();
    } else if ( c == 6 ) {
            reverb->initFactorySmallRoom();
    } else if ( c == 7 ) {
            reverb->initFactory90sAreBack();
    //} else if ( c == 8 ) {
    //        reverb->initFactoryThroughTheLookingGlass(); // Only preset that sounds scratchy (using 4-5 delay lines, mono) causes buffer underruns
    //                                                       //   TODO Try slight modifications to this preset to allow to work
    }

}


// This runs at a fixed rate, to prepare audio samples
static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    // Notify CpuLoadMeter of block start
    cpu_meter.OnBlockStart();

    //hw.ProcessAllControls();
    hw.ProcessAnalogControls();
    hw.ProcessDigitalControls();
    led1.Update();
    led2.Update();

    float earlyout_value = earlyOut.Process();
    float mainout_value = mainOut.Process();
    float time_value = time.Process();
    float diffusion_value = diffusion.Process();
    float tap_decay_value = tapDecay.Process();


    if ((prevEarlyOut < earlyout_value) || ( prevEarlyOut> earlyout_value)) {
        reverb->SetParameter(::Parameter::EarlyOut, earlyout_value);
        prevEarlyOut = earlyout_value;
    }

    if ((prevMainOut < mainout_value) || ( prevMainOut > mainout_value)) {
        reverb->SetParameter(::Parameter::MainOut, mainout_value);
        prevMainOut = mainout_value;
    }

    if ((prevTime < time_value) || ( prevTime > time_value)) {
        reverb->SetParameter(::Parameter::LineDecay, time_value);
        prevTime = time_value;
    }
    if ((prevDiffusion < diffusion_value) || ( prevDiffusion > diffusion_value)) {
        reverb->SetParameter(::Parameter::LateDiffusionFeedback, diffusion_value);
        prevDiffusion = diffusion_value;
    }

    if ((prevTapDecay < tap_decay_value) || ( prevTapDecay > tap_decay_value)) {
        reverb->SetParameter(::Parameter::TapDecay, tap_decay_value);
        prevTapDecay = tap_decay_value;
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

    for (size_t i = 0; i < size; i++) {
        reverbIn[i] = in[0][i];
    }

    // ToDo: Do footswitches need de-bouncing?
    // (De-)Activate bypass and toggle LED when left footswitch is pressed
    if(hw.switches[Terrarium::FOOTSWITCH_1].RisingEdge()) {
        bypass = !bypass;
        led1.Set(bypass ? 0.0f : 1.0f);
    }

    // Cycle available models
    if(hw.switches[Terrarium::FOOTSWITCH_2].RisingEdge()) {  
        cyclePreset();
    }

    if(!bypass) {
        reverb->Process(reverbIn, reverbOut, 48);
        for (size_t i = 0; i < size; i++) {  
            // External dry passthrough + wet only from reverb
            out[0][i] = in[0][i] + reverbOut[i];
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
    c = 0;

    AudioLib::ValueTables::Init();
    CloudSeed::FastSin::Init();
    
    reverb = new CloudSeed::ReverbController(samplerate);
    reverb->ClearBuffers();
    //reverb->initFactoryChorus();

    //hw.SetAudioBlockSize(4);

    bypass = true;

    earlyOut.Init(hw.knob[Terrarium::KNOB_2], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    mainOut.Init(hw.knob[Terrarium::KNOB_3], 0.0f, 1.0f, ::daisy::Parameter::LINEAR);
    diffusion.Init(hw.knob[Terrarium::KNOB_4], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 
    tapDecay.Init(hw.knob[Terrarium::KNOB_5], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 
    time.Init(hw.knob[Terrarium::KNOB_6], 0.0f, 1.0f, ::daisy::Parameter::LINEAR); 

    prevEarlyOut = 0.0;
    prevMainOut = 0.0;
    prevTime = 0.0;
    prevDiffusion = 0.0;
    prevTapDecay = 0.0;
    prevNumLines = 5.0; // Set to max number of delay lines initially

    led1.Init(hw.seed.GetPin(Terrarium::LED_1), false);
    led1.Update();
    led2.Init(hw.seed.GetPin(Terrarium::LED_2), false);
    led2.Update();

    hw.StartAdc();
    hw.StartAudio(AudioCallback);

    while(1) {
        static uint32_t last_ms = 0;
        uint32_t now_ms = System::GetNow();
        if ((now_ms - last_ms) >= 250) {
            float load = cpu_meter.GetAvgCpuLoad() * 100.0f;
            // Latch on if load >= 95%
            if (load >= 95.0f) {
                cpu_overload_latched = true;
                hw.seed.SetLed(true);
            } else if (!cpu_overload_latched) {
                // determine flash count: 1 for >=20, 2 for >=40, 3 for >=60, 4 for >=80
                int target = 0;
                if (load >= 80.0f) target = 4;
                else if (load >= 60.0f) target = 3;
                else if (load >= 40.0f) target = 2;
                else if (load >= 20.0f) target = 1;
                else target = 0;
                cpu_flash_target = target;
                // reset sequence if target changed
                if (cpu_flash_index != 0 && cpu_flash_target != target) {
                    cpu_flash_index = 0;
                    cpu_flash_state = CFP_IDLE;
                }
            }
            last_ms = now_ms;
        }

        // Advance LED flash state machine (non-blocking)
        if (!cpu_overload_latched) {
            uint32_t t = System::GetNow();
            switch (cpu_flash_state) {
                case CFP_IDLE:
                    if (cpu_flash_target > 0) {
                        // only start a new sequence after the configured gap
                        if (cpu_flash_phase_start == 0 || (t - cpu_flash_phase_start) >= CPU_FLASH_GAP_MS) {
                            // start first ON phase
                            cpu_flash_state = CFP_ON;
                            cpu_flash_phase_start = t;
                            hw.seed.SetLed(true);
                            cpu_flash_index = 1;
                        } else {
                            // keep LED off during gap
                            hw.seed.SetLed(false);
                        }
                    } else {
                        hw.seed.SetLed(false);
                    }
                    break;
                case CFP_ON:
                    if ((t - cpu_flash_phase_start) >= CPU_FLASH_ON_MS) {
                        // move to OFF between flashes
                        cpu_flash_state = CFP_OFF;
                        cpu_flash_phase_start = t;
                        hw.seed.SetLed(false);
                    }
                    break;
                case CFP_OFF:
                    if ((t - cpu_flash_phase_start) >= CPU_FLASH_OFF_MS) {
                        if (cpu_flash_index < cpu_flash_target) {
                            // start next ON
                            cpu_flash_index++;
                            cpu_flash_state = CFP_ON;
                            cpu_flash_phase_start = t;
                            hw.seed.SetLed(true);
                        } else {
                            // finished sequence; gap then restart
                            cpu_flash_state = CFP_IDLE;
                            cpu_flash_phase_start = t; // used for gap, but we simply wait until next sample update
                            hw.seed.SetLed(false);
                            cpu_flash_index = 0;
                        }
                    }
                    break;
            }
        }
        System::Delay(10);
    }
}
