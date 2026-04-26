#!/bin/bash
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

echo "==> Configuring CMake..."
cmake -B build -S . -G "MinGW Makefiles"

echo "==> Building..."
cmake --build build -j$(nproc)

echo "==> Installing to ~/.claude/model-gateway/"
mkdir -p ~/.claude/model-gateway/bin

# Copy binary and DLLs (capture ldd output to find exact DLLs)
# This ensures the same build-linked DLLs are used
cp build/model-gateway.exe ~/.claude/model-gateway/bin/
for dll in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    ldd build/model-gateway.exe 2>/dev/null | grep "$dll" | awk '{print $3}' | while read -r p; do
        [ -n "$p" ] && cp "$p" ~/.claude/model-gateway/bin/
    done
done
# Fallback: also copy DLLs that are next to the binary in the build dir
cp build/*.dll ~/.claude/model-gateway/bin/ 2>/dev/null || true

echo "==> Done! Binary installed to ~/.claude/model-gateway/bin/model-gateway.exe"
echo "==> Run: ~/.claude/model-gateway/bin/model-gateway.exe"
