// Shape detection and weight packing for the fused WaveNet (FusedWaveNet.h).
//
// The detector is strict: it accepts a config only when every option it names is one the fused
// pass computes exactly as the NAM core does, and it never accepts a file the NAM core would
// refuse. Anything else returns nullptr and stays on the NAM core.

#include "dsp/nam/FusedWaveNet.h"

#include <cstddef>
#include <string>
#include <utility>

namespace guitarfx::fusedwavenet
{
namespace
{
// Past this the receptive field is over 20 s at 48 kHz: no shipped model is near it, and it
// keeps every ring size well inside an int at any time scale.
constexpr long long kMaxLookback = 1 << 20;
constexpr int kMaxChannels = 1024;

bool HasNonNull(const nlohmann::json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && !it->is_null();
}

// Tanh, or LeakyReLU with a slope in [0, 1). The app always runs the NAM core with fast tanh,
// so Tanh means fast_tanh here.
bool ParseActivation(const nlohmann::json& json, Activation& activation, float& slope)
{
    std::string type;

    if (json.is_string())
    {
        type = json.get<std::string>();
    }
    else if (json.is_object())
    {
        type = json.at("type").get<std::string>();
    }
    else
    {
        return false;
    }

    if (type == "Tanh")
    {
        activation = Activation::Tanh;
        return json.is_string() || json.size() == 1;
    }

    if (type == "LeakyReLU")
    {
        activation = Activation::LeakyRelu;
        slope = 0.01f;

        if (json.is_object())
        {
            if (json.size() > 2 || (json.size() == 2 && !json.contains("negative_slope")))
            {
                return false;
            }

            slope = json.value("negative_slope", 0.01f);
        }

        return slope >= 0.0f && slope < 1.0f;
    }

    return false;
}

// Absent, false, or an object that says it is inactive. The NAM core refuses a null or true.
bool FilmOff(const nlohmann::json& layer, const char* key)
{
    const auto it = layer.find(key);

    if (it == layer.end())
    {
        return true;
    }

    if (it->is_boolean())
    {
        return !it->get<bool>();
    }

    return it->is_object() && !it->value("active", true);
}

bool AllNone(const nlohmann::json& gating)
{
    if (gating.is_string())
    {
        return gating.get<std::string>() == "none";
    }

    if (!gating.is_array())
    {
        return false;
    }

    for (const auto& mode : gating)
    {
        if (!mode.is_string() || mode.get<std::string>() != "none")
        {
            return false;
        }
    }

    return true;
}

// One layer array's shape, or false. Weights are read afterwards, in the NAM core's order.
bool ParseLayerArray(const nlohmann::json& json, const int previousChannels, const int previousHeadSize,
                     LayerArray& array)
{
    if (!json.is_object() || HasNonNull(json, "slimmable"))
    {
        return false;
    }

    array.inputSize = json.at("input_size").get<int>();
    array.channels = json.at("channels").get<int>();

    if (array.channels < 1 || array.channels > kMaxChannels || array.inputSize != previousChannels ||
        json.value("bottleneck", array.channels) != array.channels || json.at("condition_size").get<int>() != 1)
    {
        return false;
    }

    // Each array after the first takes the previous one's head output as its head input.
    if (previousHeadSize >= 0 && previousHeadSize != array.channels)
    {
        return false;
    }

    if (json.value("groups_input", 1) != 1 || json.value("groups_input_mixin", 1) != 1)
    {
        return false;
    }

    if (json.contains("layer1x1"))
    {
        const auto& layer1x1 = json.at("layer1x1");

        if (!layer1x1.at("active").get<bool>() || layer1x1.at("groups").get<int>() != 1)
        {
            return false;
        }
    }

    if (json.contains("head1x1"))
    {
        const auto& head1x1 = json.at("head1x1");
        (void)head1x1.at("out_channels").get<int>();
        (void)head1x1.at("groups").get<int>();

        if (head1x1.at("active").get<bool>())
        {
            return false;
        }
    }

    if (json.contains("gating_mode"))
    {
        if (!AllNone(json.at("gating_mode")))
        {
            return false;
        }
    }
    else if (json.contains("gated") && json.at("gated").get<bool>())
    {
        return false;
    }

    for (const char* film : {"conv_pre_film", "conv_post_film", "input_mixin_pre_film", "input_mixin_post_film",
                             "activation_pre_film", "activation_post_film", "layer1x1_post_film", "head1x1_post_film"})
    {
        if (!FilmOff(json, film))
        {
            return false;
        }
    }

    if (HasNonNull(json, "head"))
    {
        const auto& head = json.at("head");

        if (!head.is_object())
        {
            return false;
        }

        array.headSize = head.at("out_channels").get<int>();
        array.headKernelSize = head.at("kernel_size").get<int>();
        array.hasHeadBias = head.at("bias").get<bool>();
    }
    else
    {
        array.headSize = json.at("head_size").get<int>();
        array.headKernelSize = 1;
        array.hasHeadBias = json.at("head_bias").get<bool>();
    }

    if (array.headSize < 1 || array.headSize > kMaxChannels || array.headKernelSize < 1 ||
        array.headKernelSize > kMaxLookback)
    {
        return false;
    }

    const auto& dilations = json.at("dilations");

    if (!dilations.is_array())
    {
        return false;
    }

    const std::size_t layerCount = dilations.size();
    std::vector<int> kernelSizes;
    const bool hasKernelSize = json.contains("kernel_size");
    const bool hasKernelSizes = json.contains("kernel_sizes");

    if (hasKernelSize == hasKernelSizes)
    {
        return false; // the NAM core wants exactly one of them
    }

    if (hasKernelSizes)
    {
        kernelSizes = json.at("kernel_sizes").get<std::vector<int>>();
    }
    else
    {
        kernelSizes.assign(layerCount, json.at("kernel_size").get<int>());
    }

    const auto& activations = json.at("activation");

    if (kernelSizes.size() != layerCount || (activations.is_array() && activations.size() != layerCount))
    {
        return false;
    }

    for (std::size_t i = 0; i < layerCount; ++i)
    {
        Layer layer;
        layer.kernelSize = kernelSizes[i];
        layer.dilation = dilations[i].get<int>();

        if (layer.kernelSize < 1 || layer.dilation < 1 ||
            static_cast<long long>(layer.kernelSize - 1) * layer.dilation > kMaxLookback ||
            !ParseActivation(activations.is_array() ? activations[i] : activations, layer.activation, layer.slope))
        {
            return false;
        }

        array.layers.push_back(std::move(layer));
    }

    return true;
}

class WeightReader
{
  public:
    explicit WeightReader(const std::vector<float>& weights) : mWeights(weights)
    {
    }

    // False once the weights run out; every later read then gives zero.
    bool Take(float& value)
    {
        if (mPosition >= mWeights.size())
        {
            mExhausted = true;
            value = 0.0f;
            return false;
        }

        value = mWeights[mPosition++];
        return true;
    }

    // count values into out[index(n)], n = 0..count-1, in file order.
    template <typename Index> void TakeInto(std::vector<float>& out, std::size_t count, Index index)
    {
        for (std::size_t n = 0; n < count; ++n)
        {
            Take(out[index(n)]);
        }
    }

    [[nodiscard]] bool ExactlyConsumed() const
    {
        return !mExhausted && mPosition == mWeights.size();
    }

  private:
    const std::vector<float>& mWeights;
    std::size_t mPosition = 0;
    bool mExhausted = false;
};

// The NAM core's weight order, transposed to input-major.
void ReadWeights(WeightReader& reader, LayerArray& array)
{
    const auto c = static_cast<std::size_t>(array.channels);
    const auto in = static_cast<std::size_t>(array.inputSize);
    const auto h = static_cast<std::size_t>(array.headSize);

    // Rechannel: out x in, no bias.
    array.rechannel.assign(in * c, 0.0f);
    reader.TakeInto(array.rechannel, c * in, [&](std::size_t n) { return (n % in) * c + n / in; });

    for (auto& layer : array.layers)
    {
        const auto k = static_cast<std::size_t>(layer.kernelSize);
        // Dilated conv: out x in x tap, then bias.
        layer.conv.assign(k * c * c, 0.0f);
        reader.TakeInto(layer.conv, c * c * k, [&](std::size_t n) {
            const std::size_t o = n / (c * k);
            const std::size_t i = (n / k) % c;
            return ((n % k) * c + i) * c + o;
        });
        layer.convBias.assign(c, 0.0f);
        reader.TakeInto(layer.convBias, c, [](std::size_t n) { return n; });
        // Input mixin: out x 1, no bias.
        layer.mixin.assign(c, 0.0f);
        reader.TakeInto(layer.mixin, c, [](std::size_t n) { return n; });
        // layer1x1: out x in, then bias.
        layer.layer1x1.assign(c * c, 0.0f);
        reader.TakeInto(layer.layer1x1, c * c, [&](std::size_t n) { return (n % c) * c + n / c; });
        layer.layer1x1Bias.assign(c, 0.0f);
        reader.TakeInto(layer.layer1x1Bias, c, [](std::size_t n) { return n; });
    }

    // Head rechannel: out x in x tap, then bias when it has one.
    const auto hk = static_cast<std::size_t>(array.headKernelSize);
    array.head.assign(hk * c * h, 0.0f);
    reader.TakeInto(array.head, h * c * hk, [&](std::size_t n) {
        const std::size_t o = n / (c * hk);
        const std::size_t i = (n / hk) % c;
        return ((n % hk) * c + i) * h + o;
    });
    array.headBias.assign(h, 0.0f);

    if (array.hasHeadBias)
    {
        reader.TakeInto(array.headBias, h, [](std::size_t n) { return n; });
    }
}

std::size_t CountBytes(const Weights& weights)
{
    std::size_t floats = 0;

    for (const auto& array : weights.arrays)
    {
        floats += array.rechannel.size() + array.head.size() + array.headBias.size();

        for (const auto& layer : array.layers)
        {
            floats += layer.conv.size() + layer.convBias.size() + layer.mixin.size() + layer.layer1x1.size() +
                      layer.layer1x1Bias.size();
        }
    }

    return floats * sizeof(float) + sizeof(Weights);
}

std::shared_ptr<const Weights> PackWaveNetOrThrow(const nlohmann::json& config, const std::vector<float>& weights)
{
    if (!config.is_object() || HasNonNull(config, "head") || HasNonNull(config, "condition_dsp") ||
        config.value("in_channels", 1) != 1)
    {
        return nullptr;
    }

    const auto& layers = config.at("layers");

    if (!layers.is_array() || layers.empty())
    {
        return nullptr;
    }

    auto packed = std::make_shared<Weights>();
    int previousChannels = 1;
    int previousHeadSize = -1;

    for (const auto& json : layers)
    {
        LayerArray array;

        if (!ParseLayerArray(json, previousChannels, previousHeadSize, array))
        {
            return nullptr;
        }

        previousChannels = array.channels;
        previousHeadSize = array.headSize;
        packed->arrays.push_back(std::move(array));
    }

    if (packed->arrays.back().headSize != 1)
    {
        return nullptr;
    }

    WeightReader reader(weights);

    for (auto& array : packed->arrays)
    {
        ReadWeights(reader, array);
    }

    reader.Take(packed->headScale);

    if (!reader.ExactlyConsumed())
    {
        return nullptr; // the NAM core throws on a weight count mismatch
    }

    packed->bytes = CountBytes(*packed);
    return packed;
}

// As nam::get_dsp reads them, for nam::create_dsp to apply.
Levels ReadLevels(const nlohmann::json& metadata)
{
    Levels levels;

    if (metadata.is_null())
    {
        return levels;
    }

    auto extract = [&metadata](const char* key) -> std::optional<double> {
        const auto it = metadata.find(key);

        if (it != metadata.end() && !it->is_null())
        {
            return it->get<double>();
        }

        return std::nullopt;
    };

    levels.loudness = extract("loudness");
    levels.inputLevel = extract("input_level_dbu");
    levels.outputLevel = extract("output_level_dbu");
    return levels;
}

void ApplyLevels(::nam::DSP& model, const Levels& levels)
{
    if (levels.loudness)
    {
        model.SetLoudness(*levels.loudness);
    }

    if (levels.inputLevel)
    {
        model.SetInputLevel(*levels.inputLevel);
    }

    if (levels.outputLevel)
    {
        model.SetOutputLevel(*levels.outputLevel);
    }
}

bool PackContainer(const ::nam::dspData& data, PackedModel& packed)
{
    const auto& submodels = data.config.at("submodels");

    if (!submodels.is_array() || submodels.empty())
    {
        return false;
    }

    for (const auto& entry : submodels)
    {
        const auto& model = entry.at("model");
        ::nam::verify_config_version(model.at("version").get<std::string>());

        if (model.at("architecture").get<std::string>() != "WaveNet")
        {
            return false;
        }

        const auto weightsIt = model.find("weights");

        if (weightsIt == model.end())
        {
            return false;
        }

        PackedModel::Submodel submodel;
        submodel.maxValue = entry.at("max_value").get<double>();
        submodel.weights = PackWaveNetOrThrow(model.at("config"), weightsIt->get<std::vector<float>>());

        if (!submodel.weights)
        {
            return false;
        }

        if (model.contains("sample_rate"))
        {
            submodel.expectedSampleRate = model.at("sample_rate").get<double>();
        }

        submodel.levels = ReadLevels(model.value("metadata", nlohmann::json()));
        packed.submodels.push_back(std::move(submodel));
    }

    // What nam::container::ContainerModel refuses.
    for (std::size_t i = 1; i < packed.submodels.size(); ++i)
    {
        if (packed.submodels[i].maxValue <= packed.submodels[i - 1].maxValue)
        {
            return false;
        }
    }

    if (packed.submodels.back().maxValue < 1.0)
    {
        return false;
    }

    for (const auto& submodel : packed.submodels)
    {
        const double rate = submodel.expectedSampleRate;

        if (rate != packed.expectedSampleRate && rate != NAM_UNKNOWN_EXPECTED_SAMPLE_RATE &&
            packed.expectedSampleRate != NAM_UNKNOWN_EXPECTED_SAMPLE_RATE)
        {
            return false;
        }
    }

    return true;
}
} // namespace

std::shared_ptr<const Weights> PackWaveNet(const nlohmann::json& config, const std::vector<float>& weights)
{
    try
    {
        return PackWaveNetOrThrow(config, weights);
    }
    catch (...)
    {
        return nullptr;
    }
}

std::shared_ptr<const PackedModel> Pack(const ::nam::dspData& data)
{
    if (!IsAvailable())
    {
        return nullptr;
    }

    try
    {
        ::nam::verify_config_version(data.version);
        auto packed = std::make_shared<PackedModel>();
        packed->expectedSampleRate = data.expected_sample_rate;
        packed->levels = ReadLevels(data.metadata);

        if (data.architecture == "WaveNet")
        {
            PackedModel::Submodel submodel;
            submodel.expectedSampleRate = data.expected_sample_rate;
            submodel.weights = PackWaveNetOrThrow(data.config, data.weights);
            submodel.levels = packed->levels;

            if (!submodel.weights)
            {
                return nullptr;
            }

            packed->submodels.push_back(std::move(submodel));
        }
        else if (data.architecture == "SlimmableContainer")
        {
            packed->isContainer = true;

            if (!PackContainer(data, *packed))
            {
                return nullptr;
            }
        }
        else
        {
            return nullptr;
        }

        packed->bytes = sizeof(PackedModel);

        for (const auto& submodel : packed->submodels)
        {
            packed->bytes += submodel.weights->bytes;
        }

        return packed;
    }
    catch (...)
    {
        return nullptr;
    }
}

std::unique_ptr<::nam::DSP> Build(const PackedModel& packed)
{
    std::unique_ptr<::nam::DSP> model;

    if (!packed.isContainer)
    {
        model = std::make_unique<FusedWaveNet>(packed.submodels.front().weights, packed.expectedSampleRate);
    }
    else
    {
        // Each submodel as nam::get_dsp builds it from its own JSON, with its own levels.
        std::vector<FusedContainer::Submodel> submodels;
        submodels.reserve(packed.submodels.size());

        for (const auto& submodel : packed.submodels)
        {
            auto fused = std::make_unique<FusedWaveNet>(submodel.weights, submodel.expectedSampleRate);
            ApplyLevels(*fused, submodel.levels);
            submodels.push_back({submodel.maxValue, std::move(fused)});
        }

        model = std::make_unique<FusedContainer>(std::move(submodels), packed.expectedSampleRate);
    }

    // As nam::create_dsp leaves a model: levels applied, and prewarmed. Only a container's
    // active submodel is prewarmed: switching size resets the next one anyway.
    ApplyLevels(*model, packed.levels);
    model->prewarm();
    return model;
}
} // namespace guitarfx::fusedwavenet
