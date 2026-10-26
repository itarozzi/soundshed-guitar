#!/usr/bin/env bash
# ios/build.sh — build Soundshed Guitar or Soundshed Guitar Nano for iOS and iPadOS, and
# optionally install and launch it in the Simulator. See docs/ios-build.md.
#
# Usage: ios/build.sh [guitar|nano] [sim|device] [debug|release] [run] [reconfigure] [legacy]
#
#   guitar | nano    Which app: the WebView app (default) or Nano, the native UI.
#   sim | device     The Simulator (default) or an iPhone/iPad. A device build needs a
#                    development team: SSG_IOS_DEVELOPMENT_TEAM=<team id>.
#   debug | release  Build configuration (default debug). Judge nothing about audio from
#                    a debug build: the DSP is unoptimised.
#   run              After building for the Simulator, boot one if none is running
#                    (SSG_IOS_SIMULATOR is its UDID; an iPad by default), install the app
#                    and launch it.
#   reconfigure      Re-run CMake's configure step before building.
#   legacy           Build for iOS 12 (the iPad mini 2, iPad Air 1, iPhone 5s/6) in a tree of
#                    its own, ios/build-legacy: std::filesystem is built into the app, and the
#                    WebView app's UI is not expected to look right on iOS 12's WebKit.
#
# Arguments may appear in any order. The build tree is ios/build (Xcode generator), one
# tree for both apps and both SDKs; open ios/build/SoundshedGuitar.xcodeproj to work in Xcode.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

PRODUCT=guitar
PLATFORM=sim
CONFIG=Debug
RUN=false
RECONFIGURE=false
LEGACY=false

for arg in "$@"; do
    case "$(printf '%s' "$arg" | tr '[:upper:]' '[:lower:]')" in
        guitar) PRODUCT=guitar ;;
        nano) PRODUCT=nano ;;
        sim|simulator) PLATFORM=sim ;;
        device|ios) PLATFORM=device ;;
        debug) CONFIG=Debug ;;
        release) CONFIG=Release ;;
        run) RUN=true ;;
        reconfigure) RECONFIGURE=true ;;
        legacy) LEGACY=true ;;
        -h|--help)
            sed -n '2,22p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg (try --help)" >&2
            exit 2
            ;;
    esac
done

if [[ "$LEGACY" == true ]]; then
    BUILD_DIR="${SCRIPT_DIR}/build-legacy"
fi

if [[ "$PRODUCT" == nano ]]; then
    TARGET=SoundshedGuitarNano_Standalone
    APP_NAME="Soundshed Guitar Nano"
    BUNDLE_ID=com.soundshed.guitar.nano
else
    TARGET=SoundshedGuitar_Standalone
    APP_NAME="Soundshed Guitar"
    BUNDLE_ID=com.soundshed.guitar
fi

if [[ "$RUN" == true && "$PLATFORM" != sim ]]; then
    echo "'run' installs into the Simulator only. For a device, install from Xcode or with" >&2
    echo "  xcrun devicectl device install app --device <id> <path to .app>" >&2
    exit 2
fi

# ── Configure ────────────────────────────────────────────────────────────────
# A device build signs with the team's development identity; the Simulator only needs an
# ad-hoc signature, so it builds with no team at all.
if [[ "$RECONFIGURE" == true || ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    configure_args=(
        -S "${REPO_ROOT}/juce" -B "${BUILD_DIR}" -G Xcode
        -DCMAKE_SYSTEM_NAME=iOS
        -DGUITARFX_BUILD_NANO=ON
    )
    if [[ "$LEGACY" == true ]]; then
        configure_args+=(-DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DGUITARFX_IOS_WEBVIEW_DEPLOYMENT_TARGET=12.0)
    fi
    if [[ -n "${SSG_IOS_DEVELOPMENT_TEAM:-}" ]]; then
        configure_args+=(-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="${SSG_IOS_DEVELOPMENT_TEAM}")
    fi
    cmake "${configure_args[@]}"
fi

# ── Build ────────────────────────────────────────────────────────────────────
if [[ "$PLATFORM" == sim ]]; then
    SDK=iphonesimulator
    # The Simulator runs the host's architecture: x86_64 on an Intel Mac, arm64 otherwise.
    ARCH="$(uname -m)"
    xcode_args=(-sdk "$SDK" ARCHS="$ARCH" ONLY_ACTIVE_ARCH=NO CODE_SIGN_IDENTITY=-)
else
    SDK=iphoneos
    if [[ -z "${SSG_IOS_DEVELOPMENT_TEAM:-}" ]] && ! grep -q "CMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM" "${BUILD_DIR}/CMakeCache.txt"; then
        echo "A device build needs a development team: SSG_IOS_DEVELOPMENT_TEAM=<team id> ios/build.sh device reconfigure" >&2
        exit 2
    fi
    # Lets Xcode create the development certificate and profile, and register the device.
    xcode_args=(-sdk "$SDK" ARCHS=arm64 -allowProvisioningUpdates -allowProvisioningDeviceRegistration)
    if [[ -n "${SSG_IOS_DEVELOPMENT_TEAM:-}" ]]; then
        xcode_args+=(DEVELOPMENT_TEAM="${SSG_IOS_DEVELOPMENT_TEAM}")
    fi
fi

cmake --build "${BUILD_DIR}" --config "$CONFIG" --target "$TARGET" -- "${xcode_args[@]}" -quiet

# JUCE puts the app under <target>_artefacts/<config>/Standalone, the same folder for both
# SDKs: a Simulator build and a device build replace each other's .app there.
APP_PATH="${BUILD_DIR}/${TARGET%_Standalone}_artefacts/${CONFIG}/Standalone/${APP_NAME}.app"

if [[ ! -d "$APP_PATH" ]]; then
    echo "Build finished but the .app was not found under ${BUILD_DIR}" >&2
    exit 1
fi

echo "Built: ${APP_PATH}"

[[ "$RUN" == true ]] || exit 0

# ── Install and launch in the Simulator ──────────────────────────────────────
booted="$(xcrun simctl list devices booted | grep -Eo '\(([0-9A-F-]{36})\)' | head -n 1 | tr -d '()')" || true

if [[ -z "$booted" ]]; then
    wanted="${SSG_IOS_SIMULATOR:-}"
    if [[ -z "$wanted" ]]; then
        # The first iPad the installed runtime lists, which is its largest current model.
        wanted="$(xcrun simctl list devices available | grep -E '^\s+iPad' | head -n 1 | grep -Eo '\(([0-9A-F-]{36})\)' | tr -d '()')" || true
    fi
    if [[ -z "$wanted" ]]; then
        echo "No Simulator to boot. Install a runtime with: xcodebuild -downloadPlatform iOS" >&2
        exit 1
    fi
    echo "Booting Simulator ${wanted}"
    xcrun simctl boot "$wanted"
    booted="$(xcrun simctl list devices booted | grep -Eo '\(([0-9A-F-]{36})\)' | head -n 1 | tr -d '()')"
fi

open -a Simulator
xcrun simctl install "$booted" "$APP_PATH"
xcrun simctl launch "$booted" "$BUNDLE_ID"
