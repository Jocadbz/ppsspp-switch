#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build-switch"
DIST_DIR="$ROOT_DIR/dist/switch/PPSSPP"

APP_NAME="PPSSPP"
AUTHOR="m4xw / PPSSPP Team"
VERSION="1.12.3"

ELF="$BUILD_DIR/PPSSPPSDL.elf"
NACP="$BUILD_DIR/PPSSPP.nacp"
NRO="$BUILD_DIR/PPSSPP.nro"
GENERATED_ICON="$BUILD_DIR/PPSSPP-icon.jpg"

: "${DEVKITPRO:=/opt/devkitpro}"
export DEVKITPRO
export DEVKITA64="${DEVKITA64:-$DEVKITPRO/devkitA64}"

NACPTOOL="$DEVKITPRO/tools/bin/nacptool"
ELF2NRO="$DEVKITPRO/tools/bin/elf2nro"

die() {
    echo "Error: $*" >&2
    exit 1
}

command -v cmake >/dev/null || die "cmake is not installed."
command -v ninja >/dev/null || die "ninja is not installed."
[[ -x "$NACPTOOL" ]] || die "nacptool not found at $NACPTOOL"
[[ -x "$ELF2NRO" ]] || die "elf2nro not found at $ELF2NRO"
[[ -f "$DEVKITPRO/cmake/Switch.cmake" ]] || die "Switch.cmake not found."

find_icon() {
    local icon=""

    for candidate in \
        "$ROOT_DIR/icon.jpg" \
        "$ROOT_DIR/icon.jpeg" \
        "$ROOT_DIR/assets/icon.jpg" \
        "$ROOT_DIR/assets/icon.jpeg" \
        "$ROOT_DIR/icons/icon.jpg" \
        "$ROOT_DIR/icons/icon.jpeg"; do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    icon="$(
        find "$ROOT_DIR" \
            -path "$BUILD_DIR" -prune -o \
            -path "$ROOT_DIR/dist" -prune -o \
            -type f \( -iname '*ppsspp*.jpg' -o -iname '*ppsspp*.jpeg' \) \
            -print -quit
    )"

    if [[ -n "$icon" ]]; then
        printf '%s\n' "$icon"
        return 0
    fi

    local png=""
    png="$(
        find "$ROOT_DIR" \
            -path "$BUILD_DIR" -prune -o \
            -path "$ROOT_DIR/dist" -prune -o \
            -type f \( -iname '*ppsspp*.png' -o -iname 'icon.png' \) \
            -print -quit
    )"

    if [[ -n "$png" ]] && command -v magick >/dev/null; then
        echo "Converting icon: $png" >&2
        magick "$png" \
            -resize '256x256^' \
            -gravity center \
            -extent 256x256 \
            -strip \
            -quality 95 \
            "$GENERATED_ICON"

        printf '%s\n' "$GENERATED_ICON"
        return 0
    fi

    return 1
}

configure() {
    echo "Configuring Switch build..."

    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
        -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_STANDARD=11 \
        -DCMAKE_C_STANDARD_REQUIRED=ON \
        -DCMAKE_C_EXTENSIONS=ON \
        -DUSE_LIBNX=ON \
        -DARM64=ON \
        -DUSING_EGL=ON \
        -DUSING_GLES2=ON \
        -DUSE_SYSTEM_ZSTD=ON \
        -DZSTD_LIBRARY="$DEVKITPRO/portlibs/switch/lib/libzstd.a" \
        -DZSTD_INCLUDE_DIR="$DEVKITPRO/portlibs/switch/include" \
        -DUSE_DISCORD=OFF \
        -DUSE_MINIUPNPC=OFF \
        -DUSE_FFMPEG=OFF \
        -DHEADLESS=OFF \
        -DUNITTEST=OFF
}

case "${1:-build}" in
    clean)
        echo "Removing build and distribution directories..."
        rm -rf "$BUILD_DIR" "$ROOT_DIR/dist"
        exit 0
        ;;

    rebuild)
        rm -rf "$BUILD_DIR"
        configure
        ;;

    configure)
        configure
        exit 0
        ;;

    build)
        if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
            configure
        fi
        ;;

    *)
        echo "Usage: $0 [build|rebuild|configure|clean]" >&2
        exit 2
        ;;
esac

echo "Building PPSSPP..."
cmake --build "$BUILD_DIR" --parallel "$(nproc)"

[[ -f "$ELF" ]] || die "Build completed, but $ELF was not created."

echo "Creating NACP metadata..."
"$NACPTOOL" \
    --create \
    "$APP_NAME" \
    "$AUTHOR" \
    "$VERSION" \
    "$NACP"

ELF2NRO_ARGS=(
    "$ELF"
    "$NRO"
    "--nacp=$NACP"
)

if ICON="$(find_icon)"; then
    echo "Embedding icon: $ICON"
    ELF2NRO_ARGS+=("--icon=$ICON")
else
    echo "Warning: no JPEG icon found." >&2
    echo "Place a 256x256 icon at assets/icon.jpg and rerun the script." >&2
fi

echo "Creating NRO..."
"$ELF2NRO" "${ELF2NRO_ARGS[@]}"

echo "Preparing distribution folder..."
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

cp "$NRO" "$DIST_DIR/PPSSPP.nro"

if [[ -d "$BUILD_DIR/assets" ]]; then
    cp -a "$BUILD_DIR/assets" "$DIST_DIR/"
elif [[ -d "$ROOT_DIR/assets" ]]; then
    cp -a "$ROOT_DIR/assets" "$DIST_DIR/"
else
    echo "Warning: no assets directory was found." >&2
fi

echo
echo "Build complete:"
echo "  NRO:  $NRO"
echo "  Dist: $DIST_DIR"
echo
echo "Copy this directory to the SD card:"
echo "  dist/switch/PPSSPP -> sd:/switch/PPSSPP"
