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

    # Written through a temporary so the header's timestamp only moves when its content does.
    file(WRITE "${_to}/ResamplingContainer.h.new" "${_content}")
    file(COPY_FILE "${_to}/ResamplingContainer.h.new" "${_to}/ResamplingContainer.h" ONLY_IF_DIFFERENT)
    file(REMOVE "${_to}/ResamplingContainer.h.new")

    set(${out_include_dir} "${_root}" PARENT_SCOPE)
endfunction()
