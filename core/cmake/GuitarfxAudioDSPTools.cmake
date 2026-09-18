include_guard(GLOBAL)

# The core uses one thing from the pinned AudioDSPTools checkout: dsp/ResamplingContainer, the
# NAM oversampler's resampler. It carries local fixes upstream does not have yet, so the core
# compiles against a copy of that directory generated into the build tree with the fixes applied,
# and the checkout itself is never on the include path. Leaving the checkout untouched means a
# pin bump updates it cleanly and a FETCHCONTENT_SOURCE_DIR clone is never edited.
#
# A fix stops the configure if the text it replaces is missing: the pin has moved, and the fix
# needs checking against the new code rather than silently falling away. Drop a fix once a pin
# bump brings it in from upstream.

function(_guitarfx_replace_once content_var label search replacement)
    string(FIND "${${content_var}}" "${search}" _first)
    string(FIND "${${content_var}}" "${search}" _last REVERSE)

    if(_first EQUAL -1 OR NOT _first EQUAL _last)
        message(FATAL_ERROR
            "AudioDSPTools fix '${label}' no longer matches the pinned source (expected exactly one "
            "'${search}'). Check whether upstream fixed it, then update or remove the fix in "
            "core/cmake/GuitarfxAudioDSPTools.cmake.")
    endif()

    string(REPLACE "${search}" "${replacement}" _content "${${content_var}}")
    set(${content_var} "${_content}" PARENT_SCOPE)
endfunction()

# Generates the fixed copy of ${source_dir}/dsp/ResamplingContainer and sets out_include_dir to
# the include root that holds it.
function(guitarfx_prepare_audio_dsp_tools source_dir out_include_dir)
    set(_from "${source_dir}/dsp/ResamplingContainer")
    set(_root "${CMAKE_CURRENT_BINARY_DIR}/guitarfx_audio_dsp_tools")
    set(_to "${_root}/dsp/ResamplingContainer")

    if(NOT EXISTS "${_from}/ResamplingContainer.h")
        message(FATAL_ERROR "AudioDSPTools: ${_from}/ResamplingContainer.h not found.")
    endif()

    # Regenerate when the checkout changes, not only when a CMakeLists.txt does.
    file(GLOB_RECURSE _inputs "${_from}/*")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_inputs})

    # The headers ResamplingContainer.h includes, by paths relative to itself. file(COPY) skips
    # a file whose copy already has the same timestamp, so an unchanged configure rebuilds nothing.
    file(COPY "${_from}/Dependencies" DESTINATION "${_to}")

    file(READ "${_from}/ResamplingContainer.h" _content)

    # Reset() pre-rolls the upsampler with GetLatency() samples of silence taken from a scratch
    # buffer that holds one host block. At a fractional rate ratio the latency can be longer than
    # a small block -- 117 samples for 48 kHz -> 88.2 kHz with a linear-phase filter, 71 for
    # 48 kHz -> 44.1 kHz with minimum phase -- and the single push read uninitialised heap past the
    # buffer's end, which the NAM model turned into NaN. Push the silence in block-sized pieces.
    _guitarfx_replace_once(_content "preroll-in-chunks"
        "    mResampler1->PushBlock(mScratchExternalInputPointers.GetList(), mLatency);"
[=[
    // Soundshed fix (core/cmake/GuitarfxAudioDSPTools.cmake): the scratch buffer holds
    // mMaxBlockSize zeroed samples and the latency can be longer, so pre-roll in pieces.
    if (mMaxBlockSize <= 0)
      throw std::runtime_error("ResamplingContainer::Reset() needs a positive block size.");

    for (int remaining = mLatency; remaining > 0;)
    {
      const int chunk = std::min(remaining, mMaxBlockSize);
      mResampler1->PushBlock(mScratchExternalInputPointers.GetList(), static_cast<size_t>(chunk));
      remaining -= chunk;
    }
]=])

    # Each minimum-phase IIR stage resets itself when a sample goes non-finite or huge, and tests
    # for that with std::isfinite. Release builds use fast math, and -ffast-math (every non-MSVC
    # build: Android, macOS, Linux) folds std::isfinite to true while `NaN > 1e12` is false, so a
    # NaN from the model or the input got past the check and latched in the filter state: every
    # later sample came out NaN. Test with core/src/dsp/FiniteCheck.h instead; core/src is on the
    # include path of everything that includes this header, as both are public include
    # directories of SoundshedGuitarCore. The four guards read the same, so each search takes in
    # enough of the reset after it to match exactly once. Regression test:
    # TestNamResamplerRecoversFromNaN in core/tests/SampleRateConverterTests.cpp, which only a
    # clang Release build (core/build-clangcl) can fail.
    _guitarfx_replace_once(_content "finite-check-include"
        "#include \"Dependencies/LanczosResampler.h\""
[=[
#include "Dependencies/LanczosResampler.h"

// Soundshed fix (core/cmake/GuitarfxAudioDSPTools.cmake): std::isfinite folds to true under
// -ffast-math, so the filters' NaN guards use guitarfx::IsFinite.
#include "dsp/FiniteCheck.h"]=])

    _guitarfx_replace_once(_content "finite-check-upsampler-input-guard"
[=[
            if (!std::isfinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
            {
              for (size_t resetSection = 0; resetSection < numSections; resetSection++)
                mUpsamplingInputIIRState[]=]
[=[
            if (!guitarfx::IsFinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
            {
              for (size_t resetSection = 0; resetSection < numSections; resetSection++)
                mUpsamplingInputIIRState[]=])

    _guitarfx_replace_once(_content "finite-check-output-strict-guard"
[=[
      if (!std::isfinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
      {
        for (size_t resetSection = 0; resetSection < numSections; resetSection++)
          mMinPhaseOutputStrictIIRState[]=]
[=[
      if (!guitarfx::IsFinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
      {
        for (size_t resetSection = 0; resetSection < numSections; resetSection++)
          mMinPhaseOutputStrictIIRState[]=])

    _guitarfx_replace_once(_content "finite-check-anti-alias-guard"
[=[
          if (!std::isfinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
          {
            for (size_t resetSection = 0; resetSection < mMinimumPhaseSections.size(); resetSection++)]=]
[=[
          if (!guitarfx::IsFinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
          {
            for (size_t resetSection = 0; resetSection < mMinimumPhaseSections.size(); resetSection++)]=])

    _guitarfx_replace_once(_content "finite-check-fused-decimator-guard"
[=[
          if (!std::isfinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
          {
            for (size_t resetSection = 0; resetSection < numSections; resetSection++)
              mMinPhaseDownIIRState[]=]
[=[
          if (!guitarfx::IsFinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
          {
            for (size_t resetSection = 0; resetSection < numSections; resetSection++)
              mMinPhaseDownIIRState[]=])

    # A pin bump that adds a guard would otherwise bring std::isfinite back without a word.
    string(FIND "${_content}" "std::isfinite(" _unguarded)
    if(NOT _unguarded EQUAL -1)
        message(FATAL_ERROR
            "AudioDSPTools: ResamplingContainer.h has a std::isfinite the fast-math fix in "
            "core/cmake/GuitarfxAudioDSPTools.cmake does not cover; replace it with guitarfx::IsFinite.")
    endif()

    # An integer power-of-two ratio with minimum phase -- the default, e.g. a 48 kHz model at
    # 48 kHz with 2x -- takes the realtime IIR half-band path, whose all-pass branches had no
    # guard at all: one NaN block from the model or the input latched NaN in their state in every
    # build, Debug included. With the host above the model's rate the output guard zeroed each
    # sample instead, so the amp went silent for good. The guards work like the biquad stages' --
    # a non-finite or huge sample restarts that stage's filters from silence -- with one test per
    # sample on each stage's low-rate side: the interpolator tests its input, the only way a NaN
    # can get into its stable all-pass state, and the decimator tests its output, which a NaN in
    # either branch reaches. Regression test: the half-band cases in
    # TestNamResamplerRecoversFromNaN, which every build fails without these.
    _guitarfx_replace_once(_content "half-band-interpolator-guard"
[=[
        const float x = static_cast<float>(input[chan][n]);
        const float even = ProcessIIRHalfBandInterpBranchA(stage, chan, x);]=]
[=[
        const double sample = static_cast<double>(input[chan][n]);
        float x = static_cast<float>(sample);

        // Soundshed fix (core/cmake/GuitarfxAudioDSPTools.cmake): the all-pass branches had no
        // guard, so a non-finite input stayed in their state for good.
        if (!guitarfx::IsFinite(sample) || std::abs(sample) > 1.0e12)
        {
          for (int resetSection = 0; resetSection < kIIRHalfBandSections; resetSection++)
          {
            mIIRInterpAState[IIRStateIndex(stage, chan, resetSection)] = {};
            mIIRInterpBState[IIRStateIndex(stage, chan, resetSection)] = {};
          }
          x = 0.0f;
        }

        const float even = ProcessIIRHalfBandInterpBranchA(stage, chan, x);]=])

    _guitarfx_replace_once(_content "half-band-decimator-guard"
[=[
        T y = static_cast<T>(0.5f * (even + odd));
]=]
[=[
        T y = static_cast<T>(0.5f * (even + odd));

        // Soundshed fix (core/cmake/GuitarfxAudioDSPTools.cmake): the all-pass branches had no
        // guard, so a non-finite sample from the model stayed in their state for good.
        if (!guitarfx::IsFinite(static_cast<double>(y)) || std::abs(static_cast<double>(y)) > 1.0e12)
        {
          for (int resetSection = 0; resetSection < kIIRHalfBandSections; resetSection++)
          {
            mIIRPolyDecimEvenState[IIRStateIndex(stage, chan, resetSection)] = {};
            mIIRPolyDecimOddState[IIRStateIndex(stage, chan, resetSection)] = {};
          }
          y = T(0.0);
        }
]=])

    # The guards sit in the only two functions that run the branches (each of the four branch
    # functions is defined once and called once), so a pin bump that runs one from anywhere else
    # would bring an unguarded path back without a word.
    string(REGEX MATCHALL "ProcessIIRHalfBand[A-Za-z]+\\(" _branch_uses "${_content}")
    list(LENGTH _branch_uses _branch_use_count)
    if(NOT _branch_use_count EQUAL 8)
        message(FATAL_ERROR
            "AudioDSPTools: ResamplingContainer.h runs its IIR half-band all-pass branches from a "
            "place the non-finite guards in core/cmake/GuitarfxAudioDSPTools.cmake do not cover "
            "(expected 8 uses of ProcessIIRHalfBand*(, found ${_branch_use_count}); guard it too.")
    endif()

    # ClearBuffers(), which a Reset() with unchanged settings calls instead of rebuilding the
    # filters, left out the fractional path's minimum-phase anti-alias filter state, so the reset
    # played out the tail of the audio before it. Regression test:
    # core/tests/NamResamplerResetTests.cpp.
    _guitarfx_replace_once(_content "clear-minimum-phase-anti-alias-state"
        "    std::fill(mMinPhaseOutputStrictIIRState.begin(), mMinPhaseOutputStrictIIRState.end(), BiquadState {});"
[=[
    std::fill(mMinPhaseOutputStrictIIRState.begin(), mMinPhaseOutputStrictIIRState.end(), BiquadState {});
    // Soundshed fix (core/cmake/GuitarfxAudioDSPTools.cmake): this filter's state was not cleared.
    std::fill(mMinimumPhaseState.begin(), mMinimumPhaseState.end(), BiquadState {});]=])

    # Written through a temporary so the header's timestamp only moves when its content does.
    file(WRITE "${_to}/ResamplingContainer.h.new" "${_content}")
    file(COPY_FILE "${_to}/ResamplingContainer.h.new" "${_to}/ResamplingContainer.h" ONLY_IF_DIFFERENT)
    file(REMOVE "${_to}/ResamplingContainer.h.new")

    set(${out_include_dir} "${_root}" PARENT_SCOPE)
endfunction()
