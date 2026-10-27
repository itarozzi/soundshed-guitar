# ── Soundshed Guitar Nano ───────────────────────────────────────────────────────
# The same engine and profile as Soundshed Guitar with a native JUCE UI for small displays,
# and no WebView anywhere (docs/plans/native-ui.md). A product of its own: its own name,
# plugin ids, bundle id and installer, so it installs alongside Soundshed Guitar. What it
# shares is everything under the profile folder (juce/source/ProfileFolder.h).
#
# Included from juce/CMakeLists.txt after the Soundshed Guitar target is set up; reuses its
# CommonSourceFiles, FORMATS, COMPANY_NAME and the helpers defined there.

option(GUITARFX_BUILD_NANO "Build Soundshed Guitar Nano, the WebView-free product" ON)

if(NOT GUITARFX_BUILD_NANO)
    return()
endif()

set(NANO_TARGET "SoundshedGuitarNano")
set(NANO_PRODUCT_NAME "Soundshed Guitar Nano")
set(NANO_BUNDLE_ID "com.soundshed.guitar.nano")
set(GUITARFX_NANO_LV2_URI "urn:soundshed:guitar-nano" CACHE STRING "Stable LV2 URI for Soundshed Guitar Nano")

juce_add_plugin("${NANO_TARGET}"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../core/ui/images/icon.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/../core/ui/images/icon_sm.png"
    COMPANY_NAME "${COMPANY_NAME}"
    BUNDLE_ID "${NANO_BUNDLE_ID}"
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Shed
    # Unique per product: a host must never mistake one product for the other.
    PLUGIN_CODE SGNA
    LV2URI "${GUITARFX_NANO_LV2_URI}"
    FORMATS "${FORMATS}"
    VST3_AUTO_MANIFEST ${GUITARFX_VST3_AUTO_MANIFEST}
    NEEDS_WEB_BROWSER FALSE
    NEEDS_WEBVIEW2 FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Soundshed Guitar Nano needs microphone access to process live audio input."
    # iOS and iPadOS (ignored elsewhere). Unlike Soundshed Guitar, Nano lays itself out for
    # portrait too (a tab bar and slider rows), as it does on Android, so every orientation
    # and iPad multitasking are allowed. Status bar and home indicator are kept clear by
    # NanoEditor::systemInsets.
    TARGETED_DEVICE_FAMILY "1,2"
    IPHONE_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    IPAD_SCREEN_ORIENTATIONS UIInterfaceOrientationPortrait UIInterfaceOrientationPortraitUpsideDown UIInterfaceOrientationLandscapeLeft UIInterfaceOrientationLandscapeRight
    BACKGROUND_AUDIO_ENABLED TRUE
    PRODUCT_NAME "${NANO_PRODUCT_NAME}")

add_library(NanoSharedCode INTERFACE)

if(NOT SSG_MOBILE)
    clap_juce_extensions_plugin(TARGET "${NANO_TARGET}"
        CLAP_ID "${NANO_BUNDLE_ID}"
        CLAP_FEATURES audio-effect)
endif()

set(SOUNDSHED_SHARED_CODE_TARGET NanoSharedCode)
include(SharedCodeDefaults)
set(SOUNDSHED_SHARED_CODE_TARGET SharedCode)

file(GLOB_RECURSE NanoSourceFiles CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/source/nativeui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/nativeui/*.h")

target_sources(NanoSharedCode INTERFACE ${CommonSourceFiles} ${NanoSourceFiles})
target_include_directories(NanoSharedCode INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/source)

target_compile_definitions(NanoSharedCode
    INTERFACE
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1
    # No WebView of any kind: that is the point of this product.
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_PLUGINHOST_VST3=${GUITARFX_PLUGINHOST_ENABLED}
    JUCE_PLUGINHOST_AU=${GUITARFX_PLUGINHOST_ENABLED}
    JUCE_PLUGINHOST_LV2=${GUITARFX_PLUGINHOST_ENABLED}
    JUCE_ASIO=${GUITARFX_JUCE_ASIO_ENABLED}
    JUCE_ASIO_USE_EXTERNAL_SDK=${GUITARFX_JUCE_ASIO_USE_EXTERNAL_SDK}
    CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    VERSION="${CURRENT_VERSION}"
    PRODUCT_NAME_WITHOUT_VERSION="${NANO_PRODUCT_NAME}"
    SOUNDSHED_NATIVE_UI=1
    SOUNDSHED_MOBILE=${SSG_MOBILE_DEFINE}
)

if(GUITARFX_JUCE_ASIO_USE_EXTERNAL_SDK)
    target_include_directories(NanoSharedCode INTERFACE ${_guitarfx_asio_external_include_dirs})
endif()

target_link_libraries(NanoSharedCode
    INTERFACE
    SoundshedGuitarCore
    SoundshedUiClient
    nlohmann_json::nlohmann_json
    juce_audio_utils
    juce_audio_processors
    juce_dsp
    juce_gui_basics
    juce_gui_extra
    juce_opengl
    juce::juce_recommended_config_flags
    guitarfx_lto_flags
    juce::juce_recommended_warning_flags)

target_link_libraries("${NANO_TARGET}" PRIVATE NanoSharedCode)

# An Xcode scheme for the standalone app, as XcodePrettify.cmake makes for Soundshed Guitar's
# targets: xcodebuild honours a -destination (a device to run on, or to register for signing)
# only together with a scheme.
if(TARGET ${NANO_TARGET}_Standalone)
    set_target_properties(${NANO_TARGET}_Standalone PROPERTIES XCODE_GENERATE_SCHEME ON)
endif()

if(GUITARFX_IPP_AVAILABLE)
    guitarfx_enable_ipp_for_target(NanoSharedCode)
endif()

# ── Runtime data next to the built artefacts ────────────────────────────────────
# The engine reads factory presets, layouts, composites, metronome clicks and demo audio
# from resources/ui/, and the native UI reads the icons, the effect presentation table and
# the fonts from there too: static cuts of the web UI's Inter (core/nativeui/fonts, shared with
# Nano Q, made by tools/gen-nano-fonts.mjs), since
# JUCE cannot choose a variable font's weight. None of the web UI (TypeScript, CSS, HTML)
# ships with Nano.
set(_NANO_UI_DATA_DIRS presets assets metronome demo data images)

function(soundshed_copy_nano_resources FORMAT_TARGET)
    if(NOT TARGET ${FORMAT_TARGET})
        return()
    endif()

    if(IOS)
        # Flat iOS bundle: ui/ beside the executable (see soundshed_copy_ui_resources).
        set(_dst_base "$<TARGET_BUNDLE_DIR:${FORMAT_TARGET}>/ui")
    elseif(APPLE)
        set(_dst_base "$<TARGET_BUNDLE_DIR:${FORMAT_TARGET}>/Contents/Resources/ui")
    else()
        set(_dst_base "$<TARGET_FILE_DIR:${FORMAT_TARGET}>/resources/ui")
    endif()

    set(_copy_commands)
    foreach(_dir IN LISTS _NANO_UI_DATA_DIRS)
        list(APPEND _copy_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${_UI_SRC_DIR}/${_dir}" "${_dst_base}/${_dir}")
    endforeach()

    add_custom_command(TARGET ${FORMAT_TARGET} POST_BUILD
        COMMENT "Copying runtime data for ${FORMAT_TARGET}"
        ${_copy_commands}
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/../core/nativeui/fonts" "${_dst_base}/fonts")
endfunction()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    soundshed_copy_nano_resources(${NANO_TARGET}_Standalone)
    soundshed_copy_nano_resources(${NANO_TARGET}_VST3)
    soundshed_copy_nano_resources(${NANO_TARGET}_AU)
    soundshed_copy_nano_resources(${NANO_TARGET}_CLAP)
    soundshed_copy_nano_resources(${NANO_TARGET}_LV2)
endif()
