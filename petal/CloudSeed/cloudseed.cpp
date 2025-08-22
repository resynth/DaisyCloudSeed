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
            // Turn on onboard LED if CPU load exceeds %
            if (load > 50.0f) {
                hw.seed.SetLed(true);
            } else {
                hw.seed.SetLed(false);
            }
            last_ms = now_ms;
        }
        System::Delay(10);
    }
}
