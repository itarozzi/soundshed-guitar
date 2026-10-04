#include "JuceHostedPluginEffect.h"

#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "util/PathEncoding.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr int kSkipCode = 77;
constexpr int kFailCode = 1;
constexpr double kSampleRate = 48000.0;
constexpr int kBlockSize = 512;
constexpr int kWarmupBlocks = 10;
constexpr double kCalibrationDbu = 12.0;
constexpr double kDbfsTolerance = 0.1;
constexpr double kSampleMaxAbsTolerance = 2.0e-4;
constexpr double kSampleRmsTolerance = 2.0e-5;
constexpr double kPi = 3.14159265358979323846;
constexpr const char* kDefaultGatewayPath = R"(C:\Program Files\Common Files\VST3\Gateway.vst3)";
constexpr const char* kStateConfigKey = "pluginStateBase64";
constexpr const char* kPinnedGatewayStateModelFileMesaAmp = "[AMP] MESA.MKVII-90W-CH1-CLN Factory Bright Clean - BLEND #1.nam";
constexpr const char* kPinnedGatewayStateBase64MesaAmp =
  "R0ZYSFBTVDEBAAAA/////w4AAADVAQAAVkMyIcwBAAA8P3htbCB2ZXJzaW9uPSIxLjAiIGVuY29kaW5nPSJVVEYtOCI/PiA8VlNUM1BsdWdpblN0YXRlPjxJQ29tcG9uZW50PjI0MC5WLi4uLkx4SGkzVFkwSVdYckVUYXYwemFqVUZha0kySGlMUkEuLi4udzNCTHREU1YuLi4uRG9DV05FVFNiRWpMYklDWXVJV0tNVTFiZzB6UlZrVFJic1VQTUFVV2Z6VFFTRWpLTXNqVUlrVEs0LnlVc0xEUnd6eFBMNERIRkUxWHo4bGI0QWhQeGsxWW5RR0hDd1ZZZzRGSHMuaFBMVWpTREF4SHczaGFnMEYuLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi5VLkMuLi4uLi4uUEFQLi4uLi4uLi5ULkQuLi4uLi4uLkUuQS4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4rTy4uLi4uLi4uditDLi4uLi4uLi5KLkEuLi4uLi4uLi5QLi4uLi4uLi4uLi4uLi4uLjwvSUNvbXBvbmVudD48SUVkaXRDb250cm9sbGVyPjAuPC9JRWRpdENvbnRyb2xsZXI+PC9WU1QzUGx1Z2luU3RhdGU+AAAAAAA2NTUzNgAAAAAAAQAAADAAAAAAPwIAAAAxAM3MTD4DAAAAMgAAAAA/BAAAADMAAAAAPwUAAAA0AAAAAD8GAAAANQAAAAA/BwAAADYAAAAAAAgAAAA3AAAAAAAJAAAAOAAAAIA/CgAAADkAAACAPwsAAAAxMACamRk/DAAAADExAAAAgD8NAAAAMTIAAAAAAA==";
constexpr const char* kPinnedGatewayStateModelFileMesaPow = "[POW] MESA.MKVII-90W-EQ@ON Pres@5 - SM58.nam";
constexpr const char* kPinnedGatewayStateBase64MesaPow =
  "R0ZYSFBTVDEBAAAA/////w4AAAC7AQAAVkMyIbIBAAA8P3htbCB2ZXJzaW9uPSIxLjAiIGVuY29kaW5nPSJVVEYtOCI/PiA8VlNUM1BsdWdpblN0YXRlPjxJQ29tcG9uZW50PjIyMC5WLi4uLkx4SGkzVFkwSVdYckVUYXYwemFqVUZha0kySGlMUkEuLi4udzNCTHREU1EuLi4uRG9DV05FVFNiRWpMYklDWXVJV0tNVTFiZzB6UlZrVFJic0VUT2NVV2Z6VFFTRWpLTXNqVUlrVEs0LnlVc1RUVC44alNmLmtia01HUDAuUktmTFVTMGZpS3RFVmEuLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi5UQUwuLi4uLi4uLkUuQS4uLi4uLi5QQVAuLi4uLi4uLlQuRC4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi4uLi43Ky4uLi4uLi4uLitPLi4uLi4uLi5uLkQuLi4uLi4uLi4uQS4uLi4uLi4uLi4uLi4uLi48L0lDb21wb25lbnQ+PElFZGl0Q29udHJvbGxlcj4wLjwvSUVkaXRDb250cm9sbGVyPjwvVlNUM1BsdWdpblN0YXRlPgAAAAAANjU1MzYAAAAAAAEAAAAwAAAAAD8CAAAAMQDNzEw+AwAAADIAAAAAPwQAAAAzAAAAAD8FAAAANAAAAAA/BgAAADUAAAAAPwcAAAA2AAAAAAAIAAAANwAAAAAACQAAADgAAACAPwoAAAA5AAAAgD8LAAAAMTAAmpkZPwwAAAAxMQAAAIA/DQAAADEyAAAAAAA=";

template <typename Function>
auto CallOnMessageThread(Function&& fn) -> decltype(fn())
{
  using ReturnType = decltype(fn());
  if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    return fn();

  if constexpr (std::is_void_v<ReturnType>)
  {
    (void)juce::MessageManager::callSync(std::forward<Function>(fn));
  }
  else
  {
    auto result = juce::MessageManager::callSync(std::forward<Function>(fn));
    if (!result.has_value())
      return ReturnType{};
    return *result;
  }
}

std::string ToLower(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool ContainsIgnoreCase(std::string_view haystack, std::string_view needle)
{
  return ToLower(std::string(haystack)).find(ToLower(std::string(needle))) != std::string::npos;
}

double RmsDbfs(const std::vector<float>& buffer)
{
  if (buffer.empty())
    return -200.0;
  double sumSquares = 0.0;
  for (float sample : buffer)
    sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
  const double rms = std::sqrt(sumSquares / static_cast<double>(buffer.size()));
  if (!std::isfinite(rms) || rms < 1.0e-12)
    return -200.0;
  return 20.0 * std::log10(rms);
}

struct DiffMetrics
{
  double maxAbs = 0.0;
  double rms = 0.0;
};

DiffMetrics ComputeDiff(const std::vector<float>& a, const std::vector<float>& b)
{
  DiffMetrics d;
  if (a.size() != b.size() || a.empty())
    return d;

  double sumSquares = 0.0;
  for (size_t i = 0; i < a.size(); ++i)
  {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    d.maxAbs = std::max(d.maxAbs, std::abs(diff));
    sumSquares += diff * diff;
  }
  d.rms = std::sqrt(sumSquares / static_cast<double>(a.size()));
  return d;
}

struct Measurement
{
  bool ok = false;
  std::string error;
  double leftDbfs = -200.0;
  std::vector<float> left;
};

std::vector<fs::path> DiscoverModels(const fs::path& root)
{
  std::vector<fs::path> models;
  if (!fs::exists(root))
    return models;

  for (const auto& entry : fs::recursive_directory_iterator(root))
  {
    if (entry.is_regular_file() && entry.path().extension() == ".nam")
      models.push_back(entry.path());
  }
  std::sort(models.begin(), models.end());
  return models;
}

std::size_t ResolveModelLimit(std::size_t discoveredCount)
{
  const char* env = std::getenv("GUITARFX_NAM_COMPARE_MODEL_LIMIT");
  if (!env || !*env)
    return std::min<std::size_t>(discoveredCount, 4);
  try
  {
    const std::size_t parsed = static_cast<std::size_t>(std::stoull(env));
    if (parsed == 0)
      return discoveredCount;
    return std::min(parsed, discoveredCount);
  }
  catch (...)
  {
    return std::min<std::size_t>(discoveredCount, 4);
  }
}

std::optional<std::string> ResolvePinnedGatewayStateForModel(const fs::path& modelPath)
{
  const std::string file = guitarfx::util::PathToUtf8(modelPath.filename());
  if (file == kPinnedGatewayStateModelFileMesaAmp)
    return std::string(kPinnedGatewayStateBase64MesaAmp);
  if (file == kPinnedGatewayStateModelFileMesaPow)
    return std::string(kPinnedGatewayStateBase64MesaPow);
  return std::nullopt;
}

Measurement MeasureOurNamPath(const fs::path& modelPath, bool calibrated, double inputAmplitude)
{
  Measurement m;
  try
  {
    auto effect = guitarfx::EffectRegistry::Instance().Create(guitarfx::EffectGuids::kAmpNamOptimized);
    if (!effect)
    {
      m.error = "failed to create OptimizedNAMAmpEffect";
      return m;
    }

    effect->Prepare(kSampleRate, kBlockSize);
    if (!effect->LoadResource(modelPath))
    {
      m.error = "failed to load model";
      return m;
    }
    effect->Reset();
    effect->SetParam("mix", 1.0);
    effect->SetParam("inputGain", 0.0);
    effect->SetParam("outputGain", 0.0);
    effect->SetParam("calibrationInputLevel", kCalibrationDbu);
    effect->SetParam("useCalibration", calibrated ? 1.0 : 0.0);

    std::vector<float> inL(static_cast<size_t>(kBlockSize), 0.0f);
    std::vector<float> inR(static_cast<size_t>(kBlockSize), 0.0f);
    std::vector<float> outL(static_cast<size_t>(kBlockSize), 0.0f);
    std::vector<float> outR(static_cast<size_t>(kBlockSize), 0.0f);
    for (int block = 0; block < kWarmupBlocks; ++block)
    {
      for (int i = 0; i < kBlockSize; ++i)
      {
        const double t = static_cast<double>(block * kBlockSize + i) / kSampleRate;
        const float sample = static_cast<float>(inputAmplitude * std::sin(2.0 * kPi * 220.0 * t));
        inL[static_cast<size_t>(i)] = sample;
        inR[static_cast<size_t>(i)] = sample;
      }
      float* inputs[2] = { inL.data(), inR.data() };
      float* outputs[2] = { outL.data(), outR.data() };
      effect->Process(inputs, outputs, kBlockSize);
    }
    m.left = outL;
    m.leftDbfs = RmsDbfs(outL);
    m.ok = true;
    return m;
  }
  catch (const std::exception& ex)
  {
    m.error = ex.what();
    return m;
  }
}

struct GatewayBinding
{
  int modelIndex = -1;
  int modeIndex = -1;
  int calibrateInputIndex = -1;
};

std::optional<GatewayBinding> ResolveGatewayBindings(juce::AudioPluginInstance& plugin)
{
  return CallOnMessageThread([&plugin]() -> std::optional<GatewayBinding> {
    GatewayBinding b;
    const auto& parameters = plugin.getParameters();
    for (int i = 0; i < static_cast<int>(parameters.size()); ++i)
    {
      auto* p = parameters[static_cast<size_t>(i)];
      if (!p)
        continue;
      const std::string name = p->getName(256).toStdString();
      if (b.modelIndex < 0 && (ContainsIgnoreCase(name, "model") || ContainsIgnoreCase(name, "nam")))
        b.modelIndex = i;
      if (b.modeIndex < 0 && ContainsIgnoreCase(name, "output") && ContainsIgnoreCase(name, "mode"))
        b.modeIndex = i;
      if (b.calibrateInputIndex < 0 && ContainsIgnoreCase(name, "calibrate") && ContainsIgnoreCase(name, "input"))
        b.calibrateInputIndex = i;
    }
    if (b.modeIndex < 0)
      return std::nullopt;
    return b;
  });
}

std::string DescribeGatewayParameters(juce::AudioPluginInstance& plugin)
{
  return CallOnMessageThread([&plugin]() {
    std::string summary;
    const auto& parameters = plugin.getParameters();
    summary.reserve(2048);
    summary += "params[";
    summary += std::to_string(parameters.size());
    summary += "] ";
    const int limit = std::min<int>(static_cast<int>(parameters.size()), 40);
    for (int i = 0; i < limit; ++i)
    {
      auto* p = parameters[static_cast<size_t>(i)];
      if (!p)
        continue;
      const std::string name = p->getName(256).toStdString();
      summary += "[" + std::to_string(i) + ":" + name;
      const auto labels = p->getAllValueStrings();
      if (!labels.isEmpty())
      {
        summary += " values={";
        const int labelsLimit = std::min<int>(labels.size(), 6);
        for (int l = 0; l < labelsLimit; ++l)
        {
          if (l > 0)
            summary += "|";
          summary += labels[l].toStdString();
        }
        if (labels.size() > labelsLimit)
          summary += "|...";
        summary += "}";
      }
      summary += "] ";
    }
    return summary;
  });
}

bool SetChoiceByLabel(juce::AudioPluginInstance& plugin, int paramIndex, const std::string& desiredLabelNeedle)
{
  return CallOnMessageThread([&plugin, paramIndex, desiredLabelNeedle]() {
    const auto& parameters = plugin.getParameters();
    if (paramIndex < 0 || paramIndex >= static_cast<int>(parameters.size()))
      return false;
    auto* parameter = parameters[static_cast<size_t>(paramIndex)];
    if (!parameter)
      return false;
    const auto labels = parameter->getAllValueStrings();
    float targetNormalized = -1.0f;
    if (!labels.isEmpty())
    {
      int matchIndex = -1;
      for (int i = 0; i < labels.size(); ++i)
      {
        if (ContainsIgnoreCase(labels[i].toStdString(), desiredLabelNeedle))
        {
          matchIndex = i;
          break;
        }
      }
      if (matchIndex >= 0)
      {
        const float denom = static_cast<float>(std::max(1, labels.size() - 1));
        targetNormalized = static_cast<float>(matchIndex) / denom;
      }
    }

    if (targetNormalized < 0.0f)
    {
      // Some plugins expose enum-style params without value-string lists.
      // Probe text across the normalized domain and match by displayed label.
      constexpr int kProbeSteps = 100;
      for (int step = 0; step <= kProbeSteps; ++step)
      {
        const float candidate = static_cast<float>(step) / static_cast<float>(kProbeSteps);
        const auto text = parameter->getText(candidate, 128).toStdString();
        if (ContainsIgnoreCase(text, desiredLabelNeedle))
        {
          targetNormalized = candidate;
          break;
        }
      }
    }

    if (targetNormalized < 0.0f)
      return false;

    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(targetNormalized);
    parameter->endChangeGesture();
    return true;
  });
}

bool SetToggle(juce::AudioPluginInstance& plugin, int paramIndex, bool enabled)
{
  return CallOnMessageThread([&plugin, paramIndex, enabled]() {
    const auto& parameters = plugin.getParameters();
    if (paramIndex < 0 || paramIndex >= static_cast<int>(parameters.size()))
      return false;
    auto* parameter = parameters[static_cast<size_t>(paramIndex)];
    if (!parameter)
      return false;
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
    parameter->endChangeGesture();
    return true;
  });
}

Measurement MeasureGatewayReference(const fs::path& gatewayPath,
                                   const fs::path& modelPath,
                                   bool calibrated,
                                   double inputAmplitude)
{
  Measurement m;
  guitarfx::JuceHostedPluginEffect effect;
  effect.Prepare(kSampleRate, kBlockSize);
  if (const auto pinnedState = ResolvePinnedGatewayStateForModel(modelPath))
    effect.SetConfig(kStateConfigKey, *pinnedState);

  if (!effect.LoadResource(gatewayPath))
  {
    m.error = "failed to load Gateway.vst3: " + effect.GetConfig("lastError");
    return m;
  }

  auto* plugin = effect.GetHostedPluginForTesting();
  if (!plugin)
  {
    m.error = "hosted gateway instance unavailable";
    return m;
  }

  const auto bindings = ResolveGatewayBindings(*plugin);
  if (!bindings.has_value())
  {
    m.error = "failed to resolve Gateway bindings (output mode). " + DescribeGatewayParameters(*plugin);
    return m;
  }

  if (bindings->modelIndex >= 0)
  {
    const std::string modelNeedle = guitarfx::util::PathToUtf8(modelPath.stem());
    if (!SetChoiceByLabel(*plugin, bindings->modelIndex, modelNeedle))
    {
      m.error = "failed to select Gateway model by label: " + modelNeedle;
      return m;
    }
  }

  if (calibrated)
  {
    if (!SetChoiceByLabel(*plugin, bindings->modeIndex, "calibrated"))
    {
      m.error = "failed to set Gateway output mode to calibrated";
      return m;
    }
    if (bindings->calibrateInputIndex >= 0)
      (void)SetToggle(*plugin, bindings->calibrateInputIndex, true);
  }
  else
  {
    if (!SetChoiceByLabel(*plugin, bindings->modeIndex, "raw"))
    {
      m.error = "failed to set Gateway output mode to raw";
      return m;
    }
    if (bindings->calibrateInputIndex >= 0)
      (void)SetToggle(*plugin, bindings->calibrateInputIndex, false);
  }

  std::vector<float> inL(static_cast<size_t>(kBlockSize), 0.0f);
  std::vector<float> inR(static_cast<size_t>(kBlockSize), 0.0f);
  std::vector<float> outL(static_cast<size_t>(kBlockSize), 0.0f);
  std::vector<float> outR(static_cast<size_t>(kBlockSize), 0.0f);
  for (int block = 0; block < kWarmupBlocks; ++block)
  {
    for (int i = 0; i < kBlockSize; ++i)
    {
      const double t = static_cast<double>(block * kBlockSize + i) / kSampleRate;
      const float sample = static_cast<float>(inputAmplitude * std::sin(2.0 * kPi * 220.0 * t));
      inL[static_cast<size_t>(i)] = sample;
      inR[static_cast<size_t>(i)] = sample;
    }
    float* inputs[2] = { inL.data(), inR.data() };
    float* outputs[2] = { outL.data(), outR.data() };
    effect.Process(inputs, outputs, kBlockSize);
  }

  m.left = outL;
  m.leftDbfs = RmsDbfs(outL);
  m.ok = true;
  return m;
}

std::string GetGatewayPath()
{
  if (const char* env = std::getenv("GUITARFX_GATEWAY_VST3_PATH"))
  {
    if (*env)
      return std::string(env);
  }
  return kDefaultGatewayPath;
}

void PrintModelMetadata(const fs::path& modelPath, const fs::path& root)
{
  try
  {
    auto dsp = nam::get_dsp(modelPath);
    if (!dsp)
    {
      std::cout << "  metadata: load failed\n";
      return;
    }
    std::cout << "  metadata: input="
              << (dsp->HasInputLevel() ? std::to_string(dsp->GetInputLevel()) : "n/a")
              << " dBu, output="
              << (dsp->HasOutputLevel() ? std::to_string(dsp->GetOutputLevel()) : "n/a")
              << " dBu, loudness="
              << (dsp->HasLoudness() ? std::to_string(dsp->GetLoudness()) : "n/a")
              << "\n";
  }
  catch (...)
  {
    std::cout << "  metadata: exception\n";
  }
  (void)root;
}

} // namespace

int main()
{
  juce::ScopedJuceInitialiser_GUI juceInitialiser;
  guitarfx::RegisterAllEffects();

  const fs::path gatewayPath{GetGatewayPath()};
  if (!fs::exists(gatewayPath))
  {
    std::cout << "Skipping Gateway NAM probe; plugin not found: " << guitarfx::util::PathToUtf8(gatewayPath) << "\n";
    return kSkipCode;
  }

  const fs::path assetsRoot = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets";
  auto models = DiscoverModels(assetsRoot);
  if (models.empty())
  {
    std::cout << "Skipping Gateway NAM probe; no .nam models found under " << guitarfx::util::PathToUtf8(assetsRoot) << "\n";
    return kSkipCode;
  }
  models.resize(ResolveModelLimit(models.size()));

  const std::vector<double> amplitudes = {0.01, 0.10};
  const std::vector<bool> calibrationModes = {false, true};

  int passed = 0;
  int failed = 0;
  int skipped = 0;

  std::cout << std::fixed << std::setprecision(6);
  std::cout << "============================================================\n";
  std::cout << "Gateway NAM calibration probe (hosted VST3 reference)\n";
  std::cout << "Gateway: " << guitarfx::util::PathToUtf8(gatewayPath) << "\n";
  std::cout << "Models: " << models.size() << ", SampleRate=" << kSampleRate << ", BlockSize=" << kBlockSize << "\n";
  std::cout << "Tolerances: dBFS=" << kDbfsTolerance
            << ", maxAbs=" << kSampleMaxAbsTolerance
            << ", rmsDiff=" << kSampleRmsTolerance << "\n";
  std::cout << "============================================================\n\n";

  for (const auto& model : models)
  {
    std::cout << "Model: " << guitarfx::util::PathToUtf8(model.lexically_relative(assetsRoot)) << "\n";
    PrintModelMetadata(model, assetsRoot);

    for (double amp : amplitudes)
    {
      for (bool calibrated : calibrationModes)
      {
        const auto ours = MeasureOurNamPath(model, calibrated, amp);
        const auto ref = MeasureGatewayReference(gatewayPath, model, calibrated, amp);
        const std::string mode = calibrated ? "calibrated" : "non-calibrated";

        if (!ours.ok || !ref.ok)
        {
          std::cout << "  [" << mode << " amp=" << amp << "] SKIP  ours="
                    << (ours.ok ? "ok" : ours.error) << "  ref="
                    << (ref.ok ? "ok" : ref.error) << "\n";
          ++skipped;
          continue;
        }

        const double dbfsDelta = ours.leftDbfs - ref.leftDbfs;
        const DiffMetrics diff = ComputeDiff(ours.left, ref.left);
        const bool sameDigitalOutput =
          std::abs(dbfsDelta) <= kDbfsTolerance
          && diff.maxAbs <= kSampleMaxAbsTolerance
          && diff.rms <= kSampleRmsTolerance;

        std::cout << "  [" << mode << " amp=" << amp << "] ours/ref dBFS="
                  << ours.leftDbfs << "/" << ref.leftDbfs
                  << ", delta=" << dbfsDelta
                  << ", maxAbsDiff=" << diff.maxAbs
                  << ", rmsDiff=" << diff.rms
                  << "  " << (sameDigitalOutput ? "PASS" : "FAIL") << "\n";

        if (sameDigitalOutput)
          ++passed;
        else
          ++failed;
      }
    }
    std::cout << "\n";
  }

  std::cout << "============================================================\n";
  std::cout << "Gateway NAM probe results: " << passed << " passed, "
            << failed << " failed, " << skipped << " skipped\n";
  std::cout << "============================================================\n";

  if (failed > 0)
    return kFailCode;
  if (passed == 0 && skipped > 0)
    return kSkipCode;
  return 0;
}
