#!/bin/bash
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

platform="$(uname -s)"

if [ "$platform" = "Darwin" ]; then
    build_dir="build-macos"
    parallelism="$(sysctl -n hw.ncpu)"

    echo "==> Configuring CMake for macOS..."
    cmake -B "$build_dir" -S .

    echo "==> Building macOS binary..."
    cmake --build "$build_dir" --clean-first -j"$parallelism"

    echo "==> Running local validation..."
    ctest --test-dir "$build_dir" --output-on-failure

    echo "==> Installing to ~/.claude/model-gateway/bin/..."
    mkdir -p "$HOME/.claude/model-gateway/bin"
    install -m 755 "$build_dir/model-gateway" "$HOME/.claude/model-gateway/bin/model-gateway"

    echo "==> Done! Binary installed to ~/.claude/model-gateway/bin/model-gateway"
    echo "==> Run in foreground: ~/.claude/model-gateway/bin/model-gateway"
    echo "==> Verbose startup: ~/.claude/model-gateway/bin/model-gateway --show"
    echo "==> Run in background: nohup ~/.claude/model-gateway/bin/model-gateway >/tmp/model-gateway.log 2>&1 &"
    echo "==> Install launchd agent: bash darwin/install-launchd.sh install"
    exit 0
fi

echo "==> Configuring CMake for Windows/MinGW..."
cmake -B build -S . -G "MinGW Makefiles"

echo "==> Building Windows binary..."
cmake --build build -j$(nproc)

echo "==> Running local validation..."
ctest --test-dir build --output-on-failure

echo "==> Installing to ~/.claude/model-gateway/"
mkdir -p "$HOME/.claude/model-gateway/bin"

# Copy binary and DLLs (capture ldd output to find exact DLLs)
# This ensures the same build-linked DLLs are used
cp build/model-gateway.exe "$HOME/.claude/model-gateway/bin/"
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    ldd build/model-gateway.exe 2>/dev/null | grep "$dll" | awk '{print $3}' | while read -r p; do
        [ -n "$p" ] && cp "$p" "$HOME/.claude/model-gateway/bin/"
    done
done
# Fallback: also copy DLLs that are next to the binary in the build dir
cp build/*.dll "$HOME/.claude/model-gateway/bin/" 2>/dev/null || true

echo "==> Done! Binary installed to ~/.claude/model-gateway/bin/model-gateway.exe"
echo "==> Run in foreground: ~/.claude/model-gateway/bin/model-gateway.exe"
echo "==> Use Task Scheduler, NSSM, or another external supervisor for background restarts"
