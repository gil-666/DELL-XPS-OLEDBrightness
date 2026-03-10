#!/bin/bash
#
# build.sh — Build OLEDBrightness kext and CLI tool
#
# Usage:
#   ./Scripts/build.sh           # Build everything
#   ./Scripts/build.sh kext      # Build only the kext
#   ./Scripts/build.sh cli       # Build only the CLI tool
#   ./Scripts/build.sh clean     # Clean build artifacts
#

set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/Build"
KEXT_SRC="$PROJECT_DIR/OLEDBrightnessKext"
TOOLS_SRC="$PROJECT_DIR/UserspaceTools"
KEXT_BUNDLE="$BUILD_DIR/OLEDBrightness.kext"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info()  { echo -e "${CYAN}[INFO]${NC} $*"; }
ok()    { echo -e "${GREEN}[OK]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
err()   { echo -e "${RED}[ERROR]${NC} $*"; }

build_cli() {
    info "Building CLI tool..."
    mkdir -p "$BUILD_DIR"

    clang -Wall -Wextra -O2 -std=c11 \
        -framework IOKit \
        -framework CoreFoundation \
        -o "$BUILD_DIR/oled_brightness" \
        "$TOOLS_SRC/oled_brightness.c"

    ok "Built: $BUILD_DIR/oled_brightness"
}

build_kext() {
    info "Building OLEDBrightness kext..."
    mkdir -p "$BUILD_DIR"

    KERNEL_HEADERS=""

    # Try KDK first
    KDK_PATH=""
    if [ -d "/Library/Developer/KDKs" ]; then
        KDK_PATH=$(ls -d /Library/Developer/KDKs/KDK_*.kdk 2>/dev/null | sort -V | tail -1)
    fi
    if [ -n "$KDK_PATH" ]; then
        KERNEL_HEADERS="$KDK_PATH/System/Library/Frameworks/Kernel.framework/Headers"
        info "Using KDK: $KDK_PATH"
    fi

    # Try SDK matching running OS
    if [ -z "$KERNEL_HEADERS" ]; then
        for sdk_ver in 14 14.5 15 15.2 15.4 15.5; do
            SDK_CHECK="/Library/Developer/CommandLineTools/SDKs/MacOSX${sdk_ver}.sdk"
            if [ -d "$SDK_CHECK/System/Library/Frameworks/Kernel.framework/Headers" ]; then
                KERNEL_HEADERS="$SDK_CHECK/System/Library/Frameworks/Kernel.framework/Headers"
                info "Using macOS $sdk_ver SDK"
                break
            fi
        done
    fi

    # Fallback to default SDK
    if [ -z "$KERNEL_HEADERS" ]; then
        SDK_PATH=$(xcrun --show-sdk-path 2>/dev/null || echo "")
        if [ -n "$SDK_PATH" ] && [ -d "$SDK_PATH/System/Library/Frameworks/Kernel.framework" ]; then
            KERNEL_HEADERS="$SDK_PATH/System/Library/Frameworks/Kernel.framework/Headers"
            warn "Using default SDK (may cause vtable mismatch): $SDK_PATH"
        else
            KERNEL_HEADERS="/System/Library/Frameworks/Kernel.framework/Headers"
            warn "No suitable SDK found — using system headers"
        fi
    fi

    mkdir -p "$KEXT_BUNDLE/Contents/MacOS"

    KEXT_CFLAGS="-Wall -Wextra -O2 -std=c++14 \
        -target x86_64-apple-macos14 \
        -mkernel \
        -nostdinc \
        -fno-builtin \
        -fno-exceptions \
        -fno-rtti \
        -DKERNEL \
        -DKERNEL_PRIVATE \
        -DDRIVER_PRIVATE \
        -DAPPLE \
        -DNeXT \
        -I$KERNEL_HEADERS"

    OBJECTS=""
    for src in "$KEXT_SRC"/*.cpp; do
        obj="$BUILD_DIR/$(basename "$src" .cpp).o"
        info "  Compiling $(basename "$src")..."
        clang++ $KEXT_CFLAGS -c -o "$obj" "$src" 2>&1 || {
            err "Compilation failed"
            return 1
        }
        OBJECTS="$OBJECTS $obj"
    done

    info "Linking kext..."
    clang++ -target x86_64-apple-macos14 -mkernel -nostdlib \
        -Xlinker -kext \
        -Xlinker -x \
        -o "$KEXT_BUNDLE/Contents/MacOS/OLEDBrightness" \
        $OBJECTS 2>&1 || {
        err "Linking failed"
        return 1
    }

    cp "$KEXT_SRC/Info.plist" "$KEXT_BUNDLE/Contents/Info.plist"
    ok "Built: $KEXT_BUNDLE"
}

clean() {
    info "Cleaning build artifacts..."
    rm -rf "$BUILD_DIR"
    ok "Clean"
}

case "${1:-all}" in
    cli)        build_cli ;;
    kext)       build_kext ;;
    all)        build_cli; build_kext ;;
    clean)      clean ;;
    *)          err "Unknown command: $1"; echo "Usage: $0 [cli|kext|all|clean]"; exit 1 ;;
esac
