#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
ANDROID_DIR="${ROOT_DIR}/platforms/android"
# MELEE_XR=1: Meta Quest mixed-reality build. The game is shown on a virtual
# screen over passthrough through OpenXR (extern/aurora/lib/xr); it builds in
# its own directory and packages the Khronos OpenXR loader and the XR manifest
# overlay (platforms/android/app/src/xr).
MELEE_XR="${MELEE_XR:-0}"
XR_CMAKE_ARGS=()
XR_GRADLE_ARGS=()
APK_NAME=Melee-Android-arm64.apk
DEFAULT_BUILD_DIR="${ROOT_DIR}/build/android-arm64"
if [[ "${MELEE_XR}" == 1 ]]; then
    # The XR build renders into OpenXR's swapchain images on Dawn's own device,
    # which needs Dawn built from the melee-xr fork (single-device hooks; see
    # docs/quest-xr.md). MELEE_DAWN_SOURCE: a checkout of that branch.
    # Without one, the pinned commit of github.com/rhythmerc/dawn (branch
    # melee-xr) is cloned into build/dawn-src with its dependencies.
    MELEE_DAWN_REPO="${MELEE_DAWN_REPO:-https://github.com/rhythmerc/dawn.git}"
    MELEE_DAWN_REF="${MELEE_DAWN_REF:-b93f0f2c85245166de3a2277c69f49bbb0e34f0b}"
    if [[ -z "${MELEE_DAWN_SOURCE:-}" ]]; then
        if [[ -d "${HOME}/projects/dawn/.git" ]]; then
            MELEE_DAWN_SOURCE="${HOME}/projects/dawn"
        else
            MELEE_DAWN_SOURCE="${ROOT_DIR}/build/dawn-src"
            if [[ ! -d "${MELEE_DAWN_SOURCE}/.git" ]]; then
                echo "=== Fetching Dawn (${MELEE_DAWN_REPO} @ ${MELEE_DAWN_REF}) ==="
                git init -q "${MELEE_DAWN_SOURCE}"
                git -C "${MELEE_DAWN_SOURCE}" fetch -q --depth 1 "${MELEE_DAWN_REPO}" "${MELEE_DAWN_REF}"
                git -C "${MELEE_DAWN_SOURCE}" checkout -q FETCH_HEAD
                (cd "${MELEE_DAWN_SOURCE}" && python3 tools/fetch_dawn_dependencies.py)
            fi
        fi
    fi
    if [[ ! -f "${MELEE_DAWN_SOURCE}/include/dawn/native/VulkanBackend.h" ]] ||
        ! grep -q SetExternalVulkanHooks "${MELEE_DAWN_SOURCE}/include/dawn/native/VulkanBackend.h"; then
        echo "error: MELEE_DAWN_SOURCE (${MELEE_DAWN_SOURCE}) is not a melee-xr Dawn checkout" >&2
        exit 1
    fi
    XR_CMAKE_ARGS=(-DAURORA_ENABLE_OPENXR=ON -DAURORA_DAWN_PROVIDER=vendor
        "-DFETCHCONTENT_SOURCE_DIR_DAWN=${MELEE_DAWN_SOURCE}" -DDAWN_SUPPORTS_CXX_MODULES=OFF
        -DDAWN_BUILD_PROTOBUF=OFF -DTINT_BUILD_IR_BINARY=OFF)
    XR_GRADLE_ARGS=(-Pmelee.xr=true)
    APK_NAME=Melee-Quest-XR-arm64.apk
    DEFAULT_BUILD_DIR="${ROOT_DIR}/build/android-arm64-xr"
fi
BUILD_DIR="${BUILD_DIR:-${DEFAULT_BUILD_DIR}}"

# Honour a preconfigured SDK/NDK (CI sets these); fall back to the local layout.
export ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-${HOME}/Android}}"
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
    ANDROID_NDK_HOME="$(find "${ANDROID_HOME}/ndk" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -1)"
fi
if [[ ! -d "${ANDROID_NDK_HOME}" ]]; then
    echo "error: no Android NDK found; set ANDROID_NDK_HOME" >&2
    exit 1
fi
# The XR build compiles Dawn from source, which needs C++20 aggregate CTAD:
# NDK r28 or newer (r26's Clang 17 rejects Tint). Prefer the newest installed.
if [[ "${MELEE_XR}" == 1 ]]; then
    ndk_major() { sed -n 's/^Pkg.Revision *= *\([0-9]*\).*/\1/p' "$1/source.properties" 2>/dev/null; }
    if (( $(ndk_major "${ANDROID_NDK_HOME}") < 28 )); then
        NEWEST_NDK="$(find "${ANDROID_HOME}/ndk" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -1)"
        if [[ -n "${NEWEST_NDK}" ]] && (( $(ndk_major "${NEWEST_NDK}") >= 28 )); then
            echo "MELEE_XR: using ${NEWEST_NDK} (Dawn needs NDK r28+, ${ANDROID_NDK_HOME} is older)"
            # The game is compiled by GCC, which rejects r28+ headers'
            # Clang-only availability attributes: keep it on the older NDK.
            export GCC_NDK_HOME="${GCC_NDK_HOME:-${ANDROID_NDK_HOME}}"
            ANDROID_NDK_HOME="${NEWEST_NDK}"
        else
            echo "error: MELEE_XR=1 needs NDK r28 or newer (Dawn from source); install one with sdkmanager" >&2
            exit 1
        fi
    fi
fi
export ANDROID_NDK_HOME
if [[ -d "${HOME}/Android/jdk17" && -z "${JAVA_HOME:-}" ]]; then
    export JAVA_HOME="${HOME}/Android/jdk17"
fi
[[ -n "${JAVA_HOME:-}" ]] && export PATH="${JAVA_HOME}/bin:${PATH}"

# The decomp needs GCC's scalar_storage_order, so melee_game is compiled by an
# aarch64 cross GCC (see tools/gcc_launcher.py) rather than the NDK's Clang.
# CI installs gcc-aarch64-linux-gnu; a local unpacked toolchain also works.
if [[ -z "${GCC_AARCH64_BIN:-}" ]] && ! command -v aarch64-linux-gnu-gcc >/dev/null; then
    GCC_AARCH64_BIN="${HOME}/toolchains/gcc-aarch64/usr/bin/aarch64-linux-gnu-gcc"
    [[ -x "${GCC_AARCH64_BIN}" ]] || {
        echo "error: no aarch64 GCC; install gcc-aarch64-linux-gnu or set GCC_AARCH64_BIN" >&2
        exit 1
    }
    export GCC_AARCH64_BIN
fi

STRIP_TOOL="$(find "${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt" -name llvm-strip -print -quit)"
echo "=== NDK ${ANDROID_NDK_HOME} ==="

echo "=== Building native library (arm64-v8a) ==="
cmake -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-26 \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" \
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
    "${XR_CMAKE_ARGS[@]}"
SDL_JAVA_SOURCE_DIR="$(sed -n 's/^AURORA_SDL3_JAVA_SOURCE_DIR:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt")"
SDL_VERSION_PATTERN='private static final int SDL_(MAJOR|MINOR|MICRO)_VERSION ='
SDL_NATIVE_VERSION="$(grep -E "${SDL_VERSION_PATTERN}" "${SDL_JAVA_SOURCE_DIR}/org/libsdl/app/SDLActivity.java")"
SDL_APP_VERSION="$(grep -E "${SDL_VERSION_PATTERN}" "${ANDROID_DIR}/app/src/main/java/org/libsdl/app/SDLActivity.java")"
if [[ "${SDL_NATIVE_VERSION}" != "${SDL_APP_VERSION}" ]]; then
    echo "error: Android SDL Java version does not match the native SDL source" >&2
    exit 1
fi
ninja -C "${BUILD_DIR}" melee

echo "=== Staging assets and native libraries ==="
mkdir -p "${ANDROID_DIR}/app/src/main/assets/resources"
cp -r "${ROOT_DIR}/resources/"* "${ANDROID_DIR}/app/src/main/assets/"
cp -r "${ROOT_DIR}/resources/"* "${ANDROID_DIR}/app/src/main/assets/resources/"
# No initial_pipeline_cache.db: the seed is recorded on desktop GPUs, and
# Android builds at boot only pipelines this device has built before
# (DeviceBuiltRowsOnly, extern/aurora/lib/gfx/pipeline_cache.cpp), so shipping
# it would only have it merged into pipeline_cache.db and dropped again.
rm -f "${ANDROID_DIR}/app/src/main/assets/initial_pipeline_cache.db" \
    "${ANDROID_DIR}/app/src/main/assets/resources/initial_pipeline_cache.db"

mkdir -p "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a"
"${STRIP_TOOL}" --strip-unneeded -o "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libmelee.so" "${BUILD_DIR}/libmelee.so"
# The OpenXR loader only ships in XR builds; drop one a previous XR build staged.
rm -f "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libopenxr_loader.so"
if [[ -f "${BUILD_DIR}/libopenxr_loader.so" ]]; then
    cp "${BUILD_DIR}/libopenxr_loader.so" "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/"
fi
if [[ -f "${BUILD_DIR}/_deps/png-build/libpng16.so" ]]; then
    "${STRIP_TOOL}" --strip-unneeded -o "${ANDROID_DIR}/app/src/main/jniLibs/arm64-v8a/libpng16.so" "${BUILD_DIR}/_deps/png-build/libpng16.so"
fi

echo "=== Preparing release signing key ==="
KEYSTORE="${ANDROID_DIR}/melee-release.keystore"
if [[ -n "${MELEE_KEYSTORE_BASE64:-}" ]]; then
    # CI path: the keystore lives in repository secrets, never in the tree.
    base64 -d <<< "${MELEE_KEYSTORE_BASE64}" > "${KEYSTORE}"
elif [[ -f "${ANDROID_DIR}/release-signing.env" ]]; then
    # Local path: passwords sit next to the (gitignored) keystore.
    set -a; source "${ANDROID_DIR}/release-signing.env"; set +a
fi
if [[ ! -f "${KEYSTORE}" ]]; then
    echo "error: no signing key; set MELEE_KEYSTORE_BASE64 or create ${KEYSTORE}" >&2
    exit 1
fi
export MELEE_KEYSTORE_PASSWORD MELEE_KEY_ALIAS MELEE_KEY_PASSWORD

echo "=== Building Melee Android APK ==="
cd "${ANDROID_DIR}"
./gradlew --no-daemon :app:assembleRelease "${XR_GRADLE_ARGS[@]}" || {
    echo "Gradle assembleRelease failed, retrying once after 5s..."
    sleep 5
    ./gradlew --no-daemon :app:assembleRelease --stacktrace "${XR_GRADLE_ARGS[@]}"
}

APK="${ROOT_DIR}/dist/${APK_NAME}"
mkdir -p "${ROOT_DIR}/dist"
cp "${ANDROID_DIR}/app/build/outputs/apk/release/app-release.apk" "${APK}"

# A release APK that silently came out unsigned would fail to install.
APKSIGNER="$(find "${ANDROID_HOME}/build-tools" -name apksigner -print -quit 2>/dev/null || true)"
if [[ -n "${APKSIGNER}" ]]; then
    "${APKSIGNER}" verify --print-certs "${APK}" | head -4
fi

echo "=== APK build complete ==="
ls -lh "${APK}"
