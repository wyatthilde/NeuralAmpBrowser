#include <algorithm> // std::clamp, std::min
#include <cmath> // pow
#include <filesystem>
#include <iostream>
#include <utility>

#ifndef NO_IGRAPHICS
#include "Colors.h"
#endif
#include "../NeuralAmpModelerCore/NAM/activations.h"
#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
// clang-format off
// These includes need to happen in this order or else the latter won't know
// a bunch of stuff.
#include "NeuralAmpModeler.h"
#include "IPlug_include_in_plug_src.h"
// clang-format on
#include "architecture.hpp"

#ifndef NO_IGRAPHICS
#include "NeuralAmpModelerControls.h"
#endif

#ifdef NO_IGRAPHICS
#include "NeuralAmpModelerDialogs.h"
#endif

using namespace iplug;
#ifndef NO_IGRAPHICS
using namespace igraphics;
#endif

const double kDCBlockerFrequency = 5.0;

#ifndef NO_IGRAPHICS
// Styles
const IVColorSpec colorSpec{
  DEFAULT_BGCOLOR, // Background
  PluginColors::NAM_THEMECOLOR, // Foreground
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.3f), // Pressed
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.4f), // Frame
  PluginColors::MOUSEOVER, // Highlight
  DEFAULT_SHCOLOR, // Shadow
  PluginColors::NAM_THEMECOLOR, // Extra 1
  COLOR_RED, // Extra 2 --> color for clipping in meters
  PluginColors::NAM_THEMECOLOR.WithContrast(0.1f), // Extra 3
};

const IVStyle style =
  IVStyle{true, // Show label
          true, // Show value
          colorSpec,
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Middle, PluginColors::NAM_THEMEFONTCOLOR}, // Knob label text5
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Bottom, PluginColors::NAM_THEMEFONTCOLOR}, // Knob value text
          DEFAULT_HIDE_CURSOR,
          DEFAULT_DRAW_FRAME,
          false,
          DEFAULT_EMBOSS,
          0.2f,
          2.f,
          DEFAULT_SHADOW_OFFSET,
          DEFAULT_WIDGET_FRAC,
          DEFAULT_WIDGET_ANGLE};
const IVStyle titleStyle =
  DEFAULT_STYLE.WithValueText(IText(30, COLOR_WHITE, "Michroma-Regular")).WithDrawFrame(false).WithShadowOffset(2.f);
const IVStyle radioButtonStyle =
  style
    .WithColor(EVColor::kON, PluginColors::NAM_THEMECOLOR) // Pressed buttons and their labels
    .WithColor(EVColor::kOFF, PluginColors::NAM_THEMECOLOR.WithOpacity(0.1f)) // Unpressed buttons
    .WithColor(EVColor::kX1, PluginColors::NAM_THEMECOLOR.WithOpacity(0.6f)); // Unpressed buttons' labels

EMsgBoxResult _ShowMessageBox(iplug::igraphics::IGraphics* pGraphics, const char* str, const char* caption,
                              EMsgBoxType type)
{
#ifdef OS_MAC
  // macOS is backwards?
  return pGraphics->ShowMessageBox(caption, str, type);
#else
  return pGraphics->ShowMessageBox(str, caption, type);
#endif
}
#endif // NO_IGRAPHICS

const std::string kCalibrateInputParamName = "CalibrateInput";
const bool kDefaultCalibrateInput = false;
const std::string kInputCalibrationLevelParamName = "InputCalibrationLevel";
const double kDefaultInputCalibrationLevel = 12.0;


NeuralAmpModeler::NeuralAmpModeler(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  _InitToneStack();
  nam::activations::Activation::enable_fast_tanh();
  GetParam(kInputLevel)->InitGain("Input", 0.0, -20.0, 20.0, 0.1);
  GetParam(kToneBass)->InitDouble("Bass", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneMid)->InitDouble("Middle", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneTreble)->InitDouble("Treble", 5.0, 0.0, 10.0, 0.1);
  GetParam(kOutputLevel)->InitGain("Output", 0.0, -40.0, 40.0, 0.1);
  GetParam(kNoiseGateThreshold)->InitGain("Threshold", -80.0, -100.0, 0.0, 0.1);
  GetParam(kNoiseGateActive)->InitBool("NoiseGateActive", true);
  GetParam(kEQActive)->InitBool("ToneStack", true);
  GetParam(kOutputMode)->InitEnum("OutputMode", 1, {"Raw", "Normalized", "Calibrated"}); // TODO DRY w/ control
  GetParam(kIRToggle)->InitBool("IRToggle", true);
  GetParam(kCalibrateInput)->InitBool(kCalibrateInputParamName.c_str(), kDefaultCalibrateInput);
  GetParam(kInputCalibrationLevel)
    ->InitDouble(kInputCalibrationLevelParamName.c_str(), kDefaultInputCalibrationLevel, -60.0, 60.0, 0.1, "dBu");
  GetParam(kSlim)->InitDouble("Slim", 0.0, 0.0, 1.0, 0.01);
  // Routing, not tone. Defaults to Mono so that sessions saved by earlier
  // versions load with byte-identical behaviour.
  GetParam(kChannelMode)->InitEnum("ChannelMode", 0, {"Mono", "Stereo"});

  mNoiseGateTrigger.AddListener(&mNoiseGateGain);

#ifdef NO_IGRAPHICS
  // WebView editor: load HTML UI
  mEditorInitFunc = [&]() {
#ifdef OS_WIN
    LoadFile("index.html", nullptr);
#else
    LoadFile("index.html", GetBundleID());
#endif
    EnableScroll(false);
  };
#else
  mMakeGraphicsFunc = [&]() {

#ifdef OS_IOS
    auto scaleFactor = GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT) * 0.85f;
#else
    auto scaleFactor = 1.0f;
#endif

    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, scaleFactor);
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachTextEntryControl();
    pGraphics->EnableMouseOver(true);
    pGraphics->EnableTooltips(true);
    pGraphics->EnableMultiTouch(true);

    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->LoadFont("Michroma-Regular", MICHROMA_FN);

    const auto gearSVG = pGraphics->LoadSVG(GEAR_FN);
    const auto fileSVG = pGraphics->LoadSVG(FILE_FN);
    const auto globeSVG = pGraphics->LoadSVG(GLOBE_ICON_FN);
    const auto crossSVG = pGraphics->LoadSVG(CLOSE_BUTTON_FN);
    const auto rightArrowSVG = pGraphics->LoadSVG(RIGHT_ARROW_FN);
    const auto leftArrowSVG = pGraphics->LoadSVG(LEFT_ARROW_FN);
    const auto modelIconSVG = pGraphics->LoadSVG(MODEL_ICON_FN);
    const auto irIconOnSVG = pGraphics->LoadSVG(IR_ICON_ON_FN);
    const auto irIconOffSVG = pGraphics->LoadSVG(IR_ICON_OFF_FN);
    const auto slimIconSVG = pGraphics->LoadSVG(SLIMMABLE_ICON_FN);

    const auto backgroundBitmap = pGraphics->LoadBitmap(BACKGROUND_FN);
    const auto fileBackgroundBitmap = pGraphics->LoadBitmap(FILEBACKGROUND_FN);
    const auto inputLevelBackgroundBitmap = pGraphics->LoadBitmap(INPUTLEVELBACKGROUND_FN);
    const auto linesBitmap = pGraphics->LoadBitmap(LINES_FN);
    const auto knobBackgroundBitmap = pGraphics->LoadBitmap(KNOBBACKGROUND_FN);
    const auto switchHandleBitmap = pGraphics->LoadBitmap(SLIDESWITCHHANDLE_FN);
    const auto meterBackgroundBitmap = pGraphics->LoadBitmap(METERBACKGROUND_FN);

    const auto b = pGraphics->GetBounds();
    const auto mainArea = b.GetPadded(-20);
    const auto contentArea = mainArea.GetPadded(-10);
    const auto titleHeight = 50.0f;
    const auto titleArea = contentArea.GetFromTop(titleHeight);

    // Areas for knobs
    const auto knobsPad = 20.0f;
    const auto knobsExtraSpaceBelowTitle = 25.0f;
    const auto singleKnobPad = -2.0f;
    const auto knobsArea = contentArea.GetFromTop(NAM_KNOB_HEIGHT)
                             .GetReducedFromLeft(knobsPad)
                             .GetReducedFromRight(knobsPad)
                             .GetVShifted(titleHeight + knobsExtraSpaceBelowTitle);
    const auto inputKnobArea = knobsArea.GetGridCell(0, kInputLevel, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto noiseGateArea = knobsArea.GetGridCell(0, kNoiseGateThreshold, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto bassKnobArea = knobsArea.GetGridCell(0, kToneBass, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto midKnobArea = knobsArea.GetGridCell(0, kToneMid, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto trebleKnobArea = knobsArea.GetGridCell(0, kToneTreble, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto outputKnobArea = knobsArea.GetGridCell(0, kOutputLevel, 1, numKnobs).GetPadded(-singleKnobPad);

    const auto ngToggleArea =
      noiseGateArea.GetVShifted(noiseGateArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto eqToggleArea = midKnobArea.GetVShifted(midKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto stereoToggleArea =
      outputKnobArea.GetVShifted(outputKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);

    // Areas for model and IR
    const auto fileWidth = 200.0f;
    const auto fileHeight = 30.0f;
    const auto irYOffset = 38.0f;
    const auto modelArea =
      contentArea.GetFromBottom((2.0f * fileHeight)).GetFromTop(fileHeight).GetMidHPadded(fileWidth).GetVShifted(-1);
    const auto slimIconArea =
      IRECT(modelArea.R + 6.f, modelArea.MH() - 14.f, modelArea.R + 6.f + 2.f * 28.f, modelArea.MH() + 14.f);
    const auto modelIconArea = modelArea.GetFromLeft(30).GetTranslated(-40, 10);
    const auto irArea = modelArea.GetVShifted(irYOffset);
    const auto irSwitchArea = irArea.GetFromLeft(30.0f).GetHShifted(-40.0f).GetScaledAboutCentre(0.6f);

    // Areas for meters
    const auto inputMeterArea = contentArea.GetFromLeft(50).GetHShifted(-20).GetMidVPadded(100).GetVShifted(-25);
    const auto outputMeterArea = contentArea.GetFromRight(50).GetHShifted(20).GetMidVPadded(100).GetVShifted(-25);

    // Misc Areas
    const auto settingsButtonArea = CornerButtonArea(b);

    // Model loader button
    auto loadModelCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        // Sets mNAMPath and mStagedNAM
        const std::string msg = _StageModel(fileName);
        // TODO error messages like the IR loader.
        if (msg.size())
        {
          std::stringstream ss;
          ss << "Failed to load NAM model. Message:\n\n" << msg;
          _ShowMessageBox(GetUI(), ss.str().c_str(), "Failed to load model!", kMB_OK);
        }
        std::cout << "Loaded: " << fileName.Get() << std::endl;
      }
    };

    // IR loader button
    auto loadIRCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        mIRPath = fileName;
        const dsp::wav::LoadReturnCode retCode = _StageIR(fileName);
        if (retCode != dsp::wav::LoadReturnCode::SUCCESS)
        {
          std::stringstream message;
          message << "Failed to load IR file " << fileName.Get() << ":\n";
          message << dsp::wav::GetMsgForLoadReturnCode(retCode);

          _ShowMessageBox(GetUI(), message.str().c_str(), "Failed to load IR!", kMB_OK);
        }
      }
    };

    pGraphics->AttachBackground(BACKGROUND_FN);
    pGraphics->AttachControl(new IBitmapControl(b, linesBitmap));
    pGraphics->AttachControl(new IVLabelControl(titleArea, "NEURAL AMP MODELER", titleStyle));
    pGraphics->AttachControl(new ISVGControl(modelIconArea, modelIconSVG));

#ifdef NAM_PICK_DIRECTORY
    const std::string defaultNamFileString = "Select model directory...";
    const std::string defaultIRString = "Select IR directory...";
#else
    const std::string defaultNamFileString = "Select model...";
    const std::string defaultIRString = "Select IR...";
#endif
    // Getting started page listing additional resources
    const char* const getUrl = "https://www.neuralampmodeler.com/users#comp-marb84o5";
    pGraphics->AttachControl(
      new NAMFileBrowserControl(modelArea, kMsgTagClearModel, defaultNamFileString.c_str(), "nam",
                                loadModelCompletionHandler, style, fileSVG, crossSVG, leftArrowSVG, rightArrowSVG,
                                fileBackgroundBitmap, globeSVG, "Get NAM Models", getUrl),
      kCtrlTagModelFileBrowser);

    auto hideSlimOverlay = [](IControl* pCaller) {
      IGraphics* ui = pCaller->GetUI();
      if (auto* backdrop = ui->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        backdrop->Hide(true);
      if (auto* knob = ui->GetControlWithTag(kCtrlTagSlimKnob))
        knob->Hide(true);
      ui->SetAllControlsDirty();
    };
    auto showSlimOverlay = [](IControl* pCaller) {
      IGraphics* ui = pCaller->GetUI();
      if (auto* backdrop = ui->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        backdrop->Hide(false);
      if (auto* knob = ui->GetControlWithTag(kCtrlTagSlimKnob))
        knob->Hide(false);
      ui->SetAllControlsDirty();
    };

    pGraphics
      ->AttachControl(
        new NAMSquareButtonControl(slimIconArea, DefaultClickActionFunc, slimIconSVG), kCtrlTagSlimmableIcon)
      ->SetAnimationEndActionFunction(showSlimOverlay)
      ->Hide(true);

    pGraphics->AttachControl(new ISVGSwitchControl(irSwitchArea, {irIconOffSVG, irIconOnSVG}, kIRToggle));
    pGraphics->AttachControl(
      new NAMFileBrowserControl(irArea, kMsgTagClearIR, defaultIRString.c_str(), "wav", loadIRCompletionHandler, style,
                                fileSVG, crossSVG, leftArrowSVG, rightArrowSVG, fileBackgroundBitmap, globeSVG,
                                "Get IRs", getUrl),
      kCtrlTagIRFileBrowser);
    pGraphics->AttachControl(
      new NAMSwitchControl(ngToggleArea, kNoiseGateActive, "Noise Gate", style, switchHandleBitmap));
    pGraphics->AttachControl(new NAMSwitchControl(eqToggleArea, kEQActive, "EQ", style, switchHandleBitmap));
    pGraphics->AttachControl(
      new NAMSwitchControl(stereoToggleArea, kChannelMode, "Stereo", style, switchHandleBitmap));

    // The knobs
    pGraphics->AttachControl(new NAMKnobControl(inputKnobArea, kInputLevel, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(noiseGateArea, kNoiseGateThreshold, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(
      new NAMKnobControl(bassKnobArea, kToneBass, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(midKnobArea, kToneMid, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(trebleKnobArea, kToneTreble, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(new NAMKnobControl(outputKnobArea, kOutputLevel, "", style, knobBackgroundBitmap));

    // The meters
    pGraphics->AttachControl(new NAMMeterControl(inputMeterArea, meterBackgroundBitmap, style), kCtrlTagInputMeter);
    pGraphics->AttachControl(new NAMMeterControl(outputMeterArea, meterBackgroundBitmap, style), kCtrlTagOutputMeter);

    // Settings/help/about box
    pGraphics->AttachControl(new NAMCircleButtonControl(
      settingsButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagSettingsBox)->As<NAMSettingsPageControl>()->HideAnimated(false);
      },
      gearSVG));

    pGraphics
      ->AttachControl(new NAMSettingsPageControl(b, backgroundBitmap, inputLevelBackgroundBitmap, switchHandleBitmap,
                                                 crossSVG, style, radioButtonStyle),
                      kCtrlTagSettingsBox)
      ->Hide(true);

    const auto slimKnobArea = b.GetCentredInside(100.f, NAM_KNOB_HEIGHT + 24.f);
    pGraphics->AttachControl(new NAMSlimOverlayBackdropControl(b, hideSlimOverlay), kCtrlTagSlimOverlayBackdrop)
      ->Hide(true);
    pGraphics
      ->AttachControl(new NAMKnobControl(slimKnobArea, kSlim, "Slim", style, knobBackgroundBitmap), kCtrlTagSlimKnob)
      ->Hide(true);

    pGraphics->ForAllControlsFunc([](IControl* pControl) {
      pControl->SetMouseEventsWhenDisabled(true);
      pControl->SetMouseOverWhenDisabled(true);
    });

    // pGraphics->GetControlWithTag(kCtrlTagOutNorm)->SetMouseEventsWhenDisabled(false);
    // pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetMouseEventsWhenDisabled(false);
  };
#endif // NO_IGRAPHICS
}

NeuralAmpModeler::~NeuralAmpModeler()
{
  _DeallocateIOPointers();
}

void NeuralAmpModeler::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  const size_t numChannelsExternalIn = (size_t)NInChansConnected();
  const size_t numChannelsExternalOut = (size_t)NOutChansConnected();
  const size_t numChannelsInternal = mNumChannelsInternal.load();
  const size_t numFrames = (size_t)nFrames;
  const double sampleRate = GetSampleRate();

  // Disable floating point denormals
  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();

  // Always size for the maximum: switching mode mid-session must not allocate.
  _PrepareBuffers(kMaxChannelsInternal, numFrames);
  // Collapsed to mono, or mapped 1:1, depending on kChannelMode.
  _ProcessInput(inputs, numFrames, numChannelsExternalIn, numChannelsInternal);
  _ApplyDSPStaging();
  const bool noiseGateActive = GetParam(kNoiseGateActive)->Value();
  const bool toneStackActive = GetParam(kEQActive)->Value();

  // Noise gate trigger
  sample** triggerOutput = mInputPointers;
  if (noiseGateActive)
  {
    const double time = 0.01;
    const double threshold = GetParam(kNoiseGateThreshold)->Value(); // GetParam...
    const double ratio = 0.1; // Quadratic...
    const double openTime = 0.005;
    const double holdTime = 0.01;
    const double closeTime = 0.05;
    const dsp::noise_gate::TriggerParams triggerParams(time, threshold, ratio, openTime, holdTime, closeTime);
    mNoiseGateTrigger.SetParams(triggerParams);
    mNoiseGateTrigger.SetSampleRate(sampleRate);
    triggerOutput = mNoiseGateTrigger.Process(mInputPointers, numChannelsInternal, numFrames);
  }

  if (mModel[0] != nullptr)
  {
    // One inference instance per channel. Each gets a 1-channel view into the
    // buffers, so &ptr[c] is a valid single-element channel array.
    for (size_t c = 0; c < numChannelsInternal; c++)
    {
      mModel[c]->process(&triggerOutput[c], &mOutputPointers[c], nFrames);
    }
  }
  else
  {
    _FallbackDSP(triggerOutput, mOutputPointers, numChannelsInternal, numFrames);
  }
  // Apply the noise gate after the NAM
  sample** gateGainOutput =
    noiseGateActive ? mNoiseGateGain.Process(mOutputPointers, numChannelsInternal, numFrames) : mOutputPointers;

  sample** toneStackOutPointers = (toneStackActive && mToneStack != nullptr)
                                    ? mToneStack->Process(gateGainOutput, numChannelsInternal, nFrames)
                                    : gateGainOutput;

  sample** irPointers = toneStackOutPointers;
  if (mIR[0] != nullptr && GetParam(kIRToggle)->Value())
  {
    // dsp::ImpulseResponse keeps a single mono history internally: it convolves
    // channel 0 and copies the result across the others. So each channel needs
    // its own instance, or the stereo image collapses silently.
    for (size_t c = 0; c < numChannelsInternal; c++)
    {
      sample** wet = mIR[c]->Process(&toneStackOutPointers[c], 1, numFrames);
      // Safe even when toneStackOutPointers[c] aliases mOutputArray[c]: the IR
      // has finished reading its input by the time Process() returns.
      for (size_t s = 0; s < numFrames; s++)
        mOutputArray[c][s] = wet[0][s];
    }
    irPointers = mOutputPointers;
  }

  // And the HPF for DC offset (Issue 271)
  const double highPassCutoffFreq = kDCBlockerFrequency;
  // const double lowPassCutoffFreq = 20000.0;
  const recursive_linear_filter::HighPassParams highPassParams(sampleRate, highPassCutoffFreq);
  // const recursive_linear_filter::LowPassParams lowPassParams(sampleRate, lowPassCutoffFreq);
  mHighPass.SetParams(highPassParams);
  // mLowPass.SetParams(lowPassParams);
  sample** hpfPointers = mHighPass.Process(irPointers, numChannelsInternal, numFrames);
  // sample** lpfPointers = mLowPass.Process(hpfPointers, numChannelsInternal, numFrames);

  // restore previous floating point state
  std::feupdateenv(&fe_state);

  // Let's get outta here
  // This is where we exit mono for whatever the output requires.
  _ProcessOutput(hpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // _ProcessOutput(lpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // * Output of input leveling (inputs -> mInputPointers),
  // * Output of output leveling (mOutputPointers -> outputs)
  _UpdateMeters(mInputPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
}

void NeuralAmpModeler::OnReset()
{
  const auto sampleRate = GetSampleRate();
  const int maxBlockSize = GetBlockSize();

  // Tail is because the HPF DC blocker has a decay.
  // 10 cycles should be enough to pass the VST3 tests checking tail behavior.
  // I'm ignoring the model & IR, but it's not the end of the world.
  const int tailCycles = 10;
  SetTailSize(tailCycles * (int)(sampleRate / kDCBlockerFrequency));
  mInputSender.Reset(sampleRate);
  mOutputSender.Reset(sampleRate);
  // If there is a model or IR loaded, they need to be checked for resampling.
  _ResetModelAndIR(sampleRate, GetBlockSize());
  mToneStack->Reset(sampleRate, maxBlockSize);
  _UpdateLatency();
}

void NeuralAmpModeler::OnIdle()
{
  mInputSender.TransmitData(*this);
  mOutputSender.TransmitData(*this);

  if (mNewModelLoadedInDSP)
  {
#ifdef NO_IGRAPHICS
    _UpdateControlsFromModel();
    mNewModelLoadedInDSP = false;
#else
    if (auto* pGraphics = GetUI())
    {
      _UpdateControlsFromModel();
      mNewModelLoadedInDSP = false;
    }
#endif
  }
  if (mModelCleared)
  {
#ifdef NO_IGRAPHICS
    EvaluateJavaScript("onModelCleared()");
    mModelCleared = false;
#else
    if (auto* pGraphics = GetUI())
    {
      // FIXME -- need to disable only the "normalized" model
      // pGraphics->GetControlWithTag(kCtrlTagOutputMode)->SetDisabled(false);
      static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->ClearModelInfo();
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimmableIcon))
        p->Hide(true);
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        p->Hide(true);
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimKnob))
        p->Hide(true);
      pGraphics->SetAllControlsDirty();
      mModelCleared = false;
    }
#endif
  }
}

bool NeuralAmpModeler::SerializeState(IByteChunk& chunk) const
{
  // If this isn't here when unserializing, then we know we're dealing with something before v0.8.0.
  WDL_String header("###NeuralAmpModeler###"); // Don't change this!
  chunk.PutStr(header.Get());
  // Plugin version, so we can load legacy serialized states in the future!
  WDL_String version(PLUG_VERSION_STR);
  chunk.PutStr(version.Get());
  // Model directory (don't serialize the model itself; we'll just load it again
  // when we unserialize)
  chunk.PutStr(mNAMPath.Get());
  chunk.PutStr(mIRPath.Get());
  return SerializeParams(chunk);
}

int NeuralAmpModeler::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // Look for the expected header. If it's there, then we'll know what to do.
  WDL_String header;
  int pos = startPos;
  pos = chunk.GetStr(header, pos);

  const char* kExpectedHeader = "###NeuralAmpModeler###";
  if (strcmp(header.Get(), kExpectedHeader) == 0)
  {
    return _UnserializeStateWithKnownVersion(chunk, pos);
  }
  else
  {
    return _UnserializeStateWithUnknownVersion(chunk, startPos);
  }
}

void NeuralAmpModeler::OnUIOpen()
{
  Plugin::OnUIOpen();

  if (mNAMPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
    // If it's not loaded yet, then mark as failed.
    // If it's yet to be loaded, then the completion handler will set us straight once it runs.
    if (mModel[0] == nullptr && !mStagedModelReady)
      SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed, 0, nullptr);
  }

  if (mIRPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
    if (mIR[0] == nullptr && !mStagedIRReady)
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed, 0, nullptr);
  }

  if (mModel[0] != nullptr)
  {
    _UpdateControlsFromModel();
  }
}

void NeuralAmpModeler::OnParamChange(int paramIdx)
{
  switch (paramIdx)
  {
    // Changes to the input gain
    case kCalibrateInput:
    case kInputCalibrationLevel:
    case kInputLevel: _SetInputGain(); break;
    // Changes to the output gain
    case kOutputLevel:
    case kOutputMode: _SetOutputGain(); break;
    // Tone stack:
    case kToneBass: mToneStack->SetParam("bass", GetParam(paramIdx)->Value()); break;
    case kToneMid: mToneStack->SetParam("middle", GetParam(paramIdx)->Value()); break;
    case kToneTreble: mToneStack->SetParam("treble", GetParam(paramIdx)->Value()); break;
    case kSlim: _ApplySlimParamToLoadedNAMs(); break;
    case kChannelMode: mNumChannelsInternal = GetParam(kChannelMode)->Int() == 0 ? 1 : 2; break;
    default: break;
  }
}

void NeuralAmpModeler::OnParamChangeUI(int paramIdx, EParamSource source)
{
#ifdef NO_IGRAPHICS
  // In WebView mode, the JS OnParamChange callback handles enable/disable
  // logic for dependent controls (gate threshold, EQ knobs, IR browser).
  // Parameter values are automatically sent to JS via SPVFD().
#else
  if (auto pGraphics = GetUI())
  {
    bool active = GetParam(paramIdx)->Bool();

    switch (paramIdx)
    {
      case kNoiseGateActive: pGraphics->GetControlWithParamIdx(kNoiseGateThreshold)->SetDisabled(!active); break;
      case kEQActive:
        pGraphics->ForControlInGroup("EQ_KNOBS", [active](IControl* pControl) { pControl->SetDisabled(!active); });
        break;
      case kIRToggle: pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser)->SetDisabled(!active); break;
      default: break;
    }
  }
#endif
}

bool NeuralAmpModeler::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgTagClearModel: mShouldRemoveModel = true; return true;
    case kMsgTagClearIR: mShouldRemoveIR = true; return true;
    case kMsgTagHighlightColor:
    {
      mHighLightColor.Set((const char*)pData);

#ifdef NO_IGRAPHICS
      // In WebView mode, update the CSS theme color via JS
      WDL_String js;
      js.SetFormatted(256, "document.documentElement.style.setProperty('--theme-color','%s')", mHighLightColor.Get());
      EvaluateJavaScript(js.Get());
#else
      if (GetUI())
      {
        GetUI()->ForStandardControlsFunc([&](IControl* pControl) {
          if (auto* pVectorBase = pControl->As<IVectorBase>())
          {
            IColor color = IColor::FromColorCodeStr(mHighLightColor.Get());

            pVectorBase->SetColor(kX1, color);
            pVectorBase->SetColor(kPR, color.WithOpacity(0.3f));
            pVectorBase->SetColor(kFR, color.WithOpacity(0.4f));
            pVectorBase->SetColor(kX3, color.WithContrast(0.1f));
          }
          pControl->GetUI()->SetAllControlsDirty();
        });
      }
#endif

      return true;
    }
#ifdef NO_IGRAPHICS
    case kMsgTagOpenModelDialog:
    {
      WDL_String path;
      if (NAM_ShowOpenFileDialog("Select NAM Model", "nam", path))
      {
        const std::string msg = _StageModel(path);
        if (msg.size())
        {
          std::cerr << "Failed to load NAM model: " << msg << std::endl;
          SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed, 0, nullptr);
        }
        else
        {
          std::cout << "Loaded model: " << path.Get() << std::endl;
        }
      }
      return true;
    }
    case kMsgTagOpenIRDialog:
    {
      WDL_String path;
      if (NAM_ShowOpenFileDialog("Select IR File", "wav", path))
      {
        mIRPath = path;
        const dsp::wav::LoadReturnCode retCode = _StageIR(path);
        if (retCode != dsp::wav::LoadReturnCode::SUCCESS)
        {
          std::cerr << "Failed to load IR: " << dsp::wav::GetMsgForLoadReturnCode(retCode) << std::endl;
        }
      }
      return true;
    }
#endif
    default: return false;
  }
}

// Private methods ============================================================

void NeuralAmpModeler::_AllocateIOPointers(const size_t nChans)
{
  if (mInputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mInputPointers without freeing");
  mInputPointers = new sample*[nChans];
  if (mInputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mOutputPointers without freeing");
  mOutputPointers = new sample*[nChans];
  if (mOutputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_ApplyDSPStaging()
{
  // Remove marked modules
  if (mShouldRemoveModel)
  {
    for (auto& model : mModel)
      model = nullptr;
    mNAMPath.Set("");
    mShouldRemoveModel = false;
    mModelCleared = true;
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
  }
  if (mShouldRemoveIR)
  {
    for (auto& ir : mIR)
      ir = nullptr;
    mIRPath.Set("");
    mShouldRemoveIR = false;
  }
  // Move things from staged to live
  if (mStagedModelReady)
  {
    // Swap every channel within one block, so L and R can never disagree about
    // which model they're running.
    for (size_t c = 0; c < kMaxChannelsInternal; c++)
      mModel[c] = std::move(mStagedModel[c]);
    mStagedModelReady = false;
    mNewModelLoadedInDSP = true;
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
  }
  if (mStagedIRReady)
  {
    for (size_t c = 0; c < kMaxChannelsInternal; c++)
      mIR[c] = std::move(mStagedIR[c]);
    mStagedIRReady = false;
  }
}

void NeuralAmpModeler::_DeallocateIOPointers()
{
  if (mInputPointers != nullptr)
  {
    delete[] mInputPointers;
    mInputPointers = nullptr;
  }
  if (mInputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
  {
    delete[] mOutputPointers;
    mOutputPointers = nullptr;
  }
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels,
                                    const size_t numFrames)
{
  for (auto c = 0; c < numChannels; c++)
    for (auto s = 0; s < numFrames; s++)
      mOutputArray[c][s] = mInputArray[c][s];
}

void NeuralAmpModeler::_ResetModelAndIR(const double sampleRate, const int maxBlockSize)
{
  // Model -- reset every channel together so their histories stay aligned.
  if (mStagedModelReady)
  {
    for (auto& model : mStagedModel)
      if (model != nullptr)
        model->Reset(sampleRate, maxBlockSize);
  }
  else
  {
    for (auto& model : mModel)
      if (model != nullptr)
        model->Reset(sampleRate, maxBlockSize);
  }

  // IR
  dsp::ImpulseResponse* irSource = mStagedIRReady ? mStagedIR[0].get() : mIR[0].get();
  if (irSource != nullptr && irSource->GetSampleRate() != sampleRate)
  {
    const auto irData = irSource->GetData();
    for (size_t c = 0; c < kMaxChannelsInternal; c++)
      mStagedIR[c] = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    mStagedIRReady = true;
  }
}

void NeuralAmpModeler::_SetInputGain()
{
  iplug::sample inputGainDB = GetParam(kInputLevel)->Value();
  // Input calibration
  if ((_RefModel() != nullptr) && (_RefModel()->HasInputLevel()) && GetParam(kCalibrateInput)->Bool())
  {
    inputGainDB += GetParam(kInputCalibrationLevel)->Value() - _RefModel()->GetInputLevel();
  }
  mInputGain = DBToAmp(inputGainDB);
}

void NeuralAmpModeler::_SetOutputGain()
{
  double gainDB = GetParam(kOutputLevel)->Value();
  if (_RefModel() != nullptr)
  {
    const int outputMode = GetParam(kOutputMode)->Int();
    switch (outputMode)
    {
      case 1: // Normalized
        if (_RefModel()->HasLoudness())
        {
          const double loudness = _RefModel()->GetLoudness();
          const double targetLoudness = -18.0;
          gainDB += (targetLoudness - loudness);
        }
        break;
      case 2: // Calibrated
        if (_RefModel()->HasOutputLevel())
        {
          const double inputLevel = GetParam(kInputCalibrationLevel)->Value();
          const double outputLevel = _RefModel()->GetOutputLevel();
          gainDB += (outputLevel - inputLevel);
        }
        break;
      case 0: // Raw
      default: break;
    }
  }
  mOutputGain = DBToAmp(gainDB);
}

void NeuralAmpModeler::_ApplySlimParamToLoadedNAMs()
{
  const double v = GetParam(kSlim)->Value();
  auto apply = [v](ResamplingNAM* p) {
    if (p == nullptr)
      return;
    if (nam::SlimmableModel* s = p->GetSlimmableModel())
      s->SetSlimmableSize(v);
  };
  for (auto& model : mModel)
    apply(model.get());
  for (auto& model : mStagedModel)
    apply(model.get());
}

std::string NeuralAmpModeler::_StageModel(const WDL_String& modelPath)
{
  WDL_String previousNAMPath = mNAMPath;
  try
  {
    auto dspPath = std::filesystem::u8path(modelPath.Get());

    std::array<std::unique_ptr<ResamplingNAM>, kMaxChannelsInternal> temp;
    for (size_t c = 0; c < kMaxChannelsInternal; c++)
    {
      // Parse once per channel. nam::DSP carries inference state and isn't
      // copyable, so channels can't share one instance. This runs off the audio
      // thread, so the extra parse only costs load time.
      std::unique_ptr<nam::DSP> model = nam::get_dsp(dspPath);

      // Check that the model has 1 input and 1 output channel
      if (model->NumInputChannels() != 1)
      {
        throw std::runtime_error("Model must have 1 input channel, but has "
                                 + std::to_string(model->NumInputChannels()));
      }
      if (model->NumOutputChannels() != 1)
      {
        throw std::runtime_error("Model must have 1 output channel, but has "
                                 + std::to_string(model->NumOutputChannels()));
      }

      temp[c] = std::make_unique<ResamplingNAM>(std::move(model), GetSampleRate());
      temp[c]->Reset(GetSampleRate(), GetBlockSize());
      if (nam::SlimmableModel* slimmable = temp[c]->GetSlimmableModel())
      {
        slimmable->SetSlimmableSize(GetParam(kSlim)->Value());
      }
    }
    // Publish all channels, then flag. The audio thread only reads the array
    // once the flag is set, so it can't pick up a partial stage.
    for (size_t c = 0; c < kMaxChannelsInternal; c++)
      mStagedModel[c] = std::move(temp[c]);
    mStagedModelReady = true;
    mNAMPath = modelPath;
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
  }
  catch (std::runtime_error& e)
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed, 0, nullptr);

    mStagedModelReady = false;
    for (auto& model : mStagedModel)
      model = nullptr;
    mNAMPath = previousNAMPath;
    std::cerr << "Failed to read DSP module" << std::endl;
    std::cerr << e.what() << std::endl;
    return e.what();
  }
  return "";
}

dsp::wav::LoadReturnCode NeuralAmpModeler::_StageIR(const WDL_String& irPath)
{
  // FIXME it'd be better for the path to be "staged" as well. Just in case the
  // path and the model got caught on opposite sides of the fence...
  WDL_String previousIRPath = mIRPath;
  const double sampleRate = GetSampleRate();
  dsp::wav::LoadReturnCode wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
  try
  {
    auto first = std::make_unique<dsp::ImpulseResponse>(irPath.Get(), sampleRate);
    wavState = first->GetWavState();
    if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
    {
      // Reuse the parsed data for the remaining channels rather than re-reading
      // the WAV. Each channel still needs its own instance because the
      // convolver's history buffer is per-instance.
      const auto irData = first->GetData();
      std::array<std::unique_ptr<dsp::ImpulseResponse>, kMaxChannelsInternal> temp;
      temp[0] = std::move(first);
      for (size_t c = 1; c < kMaxChannelsInternal; c++)
        temp[c] = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
      for (size_t c = 0; c < kMaxChannelsInternal; c++)
        mStagedIR[c] = std::move(temp[c]);
      mStagedIRReady = true;
    }
  }
  catch (std::runtime_error& e)
  {
    wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
    std::cerr << "Caught unhandled exception while attempting to load IR:" << std::endl;
    std::cerr << e.what() << std::endl;
  }

  if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
  {
    mIRPath = irPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
  }
  else
  {
    mStagedIRReady = false;
    for (auto& ir : mStagedIR)
      ir = nullptr;
    mIRPath = previousIRPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed, 0, nullptr);
  }

  return wavState;
}

size_t NeuralAmpModeler::_GetBufferNumChannels() const
{
  // Assumes input=output (no mono->stereo effects)
  return mInputArray.size();
}

size_t NeuralAmpModeler::_GetBufferNumFrames() const
{
  if (_GetBufferNumChannels() == 0)
    return 0;
  return mInputArray[0].size();
}

void NeuralAmpModeler::_InitToneStack()
{
  // If you want to customize the tone stack, then put it here!
  mToneStack = std::make_unique<dsp::tone_stack::BasicNamToneStack>();
}
void NeuralAmpModeler::_PrepareBuffers(const size_t numChannels, const size_t numFrames)
{
  const bool updateChannels = numChannels != _GetBufferNumChannels();
  const bool updateFrames = updateChannels || (_GetBufferNumFrames() != numFrames);
  //  if (!updateChannels && !updateFrames)  // Could we do this?
  //    return;

  if (updateChannels)
  {
    _PrepareIOPointers(numChannels);
    mInputArray.resize(numChannels);
    mOutputArray.resize(numChannels);
  }
  if (updateFrames)
  {
    for (auto c = 0; c < mInputArray.size(); c++)
    {
      mInputArray[c].resize(numFrames);
      std::fill(mInputArray[c].begin(), mInputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mOutputArray.size(); c++)
    {
      mOutputArray[c].resize(numFrames);
      std::fill(mOutputArray[c].begin(), mOutputArray[c].end(), 0.0);
    }
  }
  // Would these ever get changed by something?
  for (auto c = 0; c < mInputArray.size(); c++)
    mInputPointers[c] = mInputArray[c].data();
  for (auto c = 0; c < mOutputArray.size(); c++)
    mOutputPointers[c] = mOutputArray[c].data();
}

void NeuralAmpModeler::_PrepareIOPointers(const size_t numChannels)
{
  _DeallocateIOPointers();
  _AllocateIOPointers(numChannels);
}

void NeuralAmpModeler::_ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn,
                                     const size_t nChansOut)
{
  if (nChansOut < 1 || nChansOut > kMaxChannelsInternal)
  {
    std::stringstream ss;
    ss << "Expected 1 or " << kMaxChannelsInternal << " internal channels, but " << nChansOut << " were requested!";
    throw std::runtime_error(ss.str());
  }
  if (nChansIn < 1)
  {
    throw std::runtime_error("No input channels connected!");
  }

  const double gain = mInputGain;
  // Assume _PrepareBuffers() was already called

  if (nChansOut == 1)
  {
    // Mono: collapse whatever the host hands us into the single chain.
    // On the standalone, assume the user plugged into one input and expects it
    // carried straight through, so don't divide -- we're just "catching
    // anything out there." In a DAW the source is probably stereo, so average
    // to avoid doubling the loudness.
    double monoGain = gain;
#ifndef APP_API
    monoGain /= (double)nChansIn;
#endif
    for (size_t c = 0; c < nChansIn; c++)
      for (size_t s = 0; s < nFrames; s++)
        if (c == 0)
          mInputArray[0][s] = monoGain * inputs[c][s];
        else
          mInputArray[0][s] += monoGain * inputs[c][s];
  }
  else
  {
    // Stereo: map 1:1 and deliberately do NOT divide. Each channel has its own
    // chain, so dividing would drive the models 6 dB softer than Mono mode --
    // and since a NAM capture is nonlinear, that changes the tone, not just
    // the level.
    for (size_t c = 0; c < nChansOut; c++)
    {
      // If the host only gave us one input, feed it to both chains.
      const size_t cin = (c < nChansIn) ? c : 0;
      for (size_t s = 0; s < nFrames; s++)
        mInputArray[c][s] = gain * inputs[cin][s];
    }
  }
}

void NeuralAmpModeler::_ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames,
                                      const size_t nChansIn, const size_t nChansOut)
{
  const double gain = mOutputGain;
  // Assume _PrepareBuffers() was already called
  if (nChansIn < 1 || nChansIn > kMaxChannelsInternal)
    throw std::runtime_error("Unexpected number of internal channels to write out.");

  if (nChansOut < nChansIn)
  {
    // Host wants fewer channels than we processed (stereo internal into a mono
    // bus). Average so we don't come out hot.
    const double foldGain = gain / (double)nChansIn;
    for (size_t cout = 0; cout < nChansOut; cout++)
      for (size_t s = 0; s < nFrames; s++)
      {
        double acc = 0.0;
        for (size_t cin = 0; cin < nChansIn; cin++)
          acc += inputs[cin][s];
        acc *= foldGain;
#ifdef APP_API // Ensure valid output to interface
        outputs[cout][s] = std::clamp(acc, -1.0, 1.0);
#else
        outputs[cout][s] = acc;
#endif
      }
    return;
  }

  // Map 1:1 where we can. A mono internal stream broadcasts to every output;
  // in stereo, channel 0 fills any extra outputs the host asked for.
  for (size_t cout = 0; cout < nChansOut; cout++)
  {
    const size_t cin = (cout < nChansIn) ? cout : 0;
    for (size_t s = 0; s < nFrames; s++)
#ifdef APP_API // Ensure valid output to interface
      outputs[cout][s] = std::clamp(gain * inputs[cin][s], -1.0, 1.0);
#else // In a DAW, other things may come next and should be able to handle large
      // values.
      outputs[cout][s] = gain * inputs[cin][s];
#endif
  }
}

void NeuralAmpModeler::_UpdateControlsFromModel()
{
  if (_RefModel() == nullptr)
  {
    return;
  }
#ifdef NO_IGRAPHICS
  // Send model info to WebView
  const double sampleRate = _RefModel()->GetEncapsulatedSampleRate();
  const bool hasInputLevel = _RefModel()->HasInputLevel();
  const double inputLevel = hasInputLevel ? _RefModel()->GetInputLevel() : 0.0;
  const bool hasOutputLevel = _RefModel()->HasOutputLevel();
  const double outputLevel = hasOutputLevel ? _RefModel()->GetOutputLevel() : 0.0;
  const bool hasLoudness = _RefModel()->HasLoudness();
  const bool isSlimmable = _RefModel()->GetSlimmableModel() != nullptr;

  WDL_String js;
  js.SetFormatted(512,
    "onModelLoaded({sampleRate:%.1f,hasInputLevel:%s,inputLevel:%.2f,hasOutputLevel:%s,outputLevel:%.2f,hasLoudness:%s,isSlimmable:%s})",
    sampleRate,
    hasInputLevel ? "true" : "false", inputLevel,
    hasOutputLevel ? "true" : "false", outputLevel,
    hasLoudness ? "true" : "false",
    isSlimmable ? "true" : "false");
  EvaluateJavaScript(js.Get());
#else
  if (auto* pGraphics = GetUI())
  {
    ModelInfo modelInfo;
    modelInfo.sampleRate.known = true;
    modelInfo.sampleRate.value = _RefModel()->GetEncapsulatedSampleRate();
    modelInfo.inputCalibrationLevel.known = _RefModel()->HasInputLevel();
    modelInfo.inputCalibrationLevel.value = _RefModel()->HasInputLevel() ? _RefModel()->GetInputLevel() : 0.0;
    modelInfo.outputCalibrationLevel.known = _RefModel()->HasOutputLevel();
    modelInfo.outputCalibrationLevel.value = _RefModel()->HasOutputLevel() ? _RefModel()->GetOutputLevel() : 0.0;

    static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->SetModelInfo(modelInfo);

    const bool disableInputCalibrationControls = !_RefModel()->HasInputLevel();
    pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetDisabled(disableInputCalibrationControls);
    pGraphics->GetControlWithTag(kCtrlTagInputCalibrationLevel)->SetDisabled(disableInputCalibrationControls);
    {
      auto* c = static_cast<OutputModeControl*>(pGraphics->GetControlWithTag(kCtrlTagOutputMode));
      c->SetNormalizedDisable(!_RefModel()->HasLoudness());
      c->SetCalibratedDisable(!_RefModel()->HasOutputLevel());
    }

    if (auto* pSlimIcon = pGraphics->GetControlWithTag(kCtrlTagSlimmableIcon))
    {
      const bool show = _RefModel()->GetSlimmableModel() != nullptr;
      pSlimIcon->Hide(!show);
    }
  }
#endif
}

void NeuralAmpModeler::_UpdateLatency()
{
  int latency = 0;
  if (_RefModel())
  {
    // All channels run the same model, so their resampler latencies match.
    // Revisit this when channels can hold different models.
    latency += _RefModel()->GetLatency();
  }
  // Other things that add latency here...

  // Feels weird to have to do this.
  if (GetLatency() != latency)
  {
    SetLatency(latency);
  }
}

void NeuralAmpModeler::_UpdateMeters(sample** inputPointer, sample** outputPointer, const size_t nFrames,
                                     const size_t nChansIn, const size_t nChansOut)
{
  const int nChans = (int)std::min(nChansIn, (size_t)2);
  mInputSender.ProcessBlock(inputPointer, (int)nFrames, kCtrlTagInputMeter, nChans);
  mOutputSender.ProcessBlock(outputPointer, (int)nFrames, kCtrlTagOutputMeter, nChans);
}

// HACK
#include "Unserialization.cpp"
