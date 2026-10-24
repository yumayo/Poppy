#!/bin/sh
set -eu

# The entry point is installed in the image; run it from the repository root.
source_dir=$(pwd -P)
build_jobs=${BUILD_JOBS:-2}
case "$build_jobs" in
    ''|*[!0-9]*|0)
        printf '%s\n' 'BUILD_JOBS must be a positive integer.' >&2
        exit 2
        ;;
esac

# Run the portable core tests as native Linux executables before cross-compiling.
cmake -S "$source_dir" -B /build/linux -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=ON
cmake --build /build/linux --parallel "$build_jobs"
ctest --test-dir /build/linux --output-on-failure

# Windows executables cannot be tested natively in this Linux container.
cmake -S "$source_dir" -B /build/windows -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$source_dir/cmake/mingw-w64-x86_64.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF
cmake --build /build/windows --parallel "$build_jobs"

mkdir -p /output
build_output=$(mktemp /output/Poppy.XXXXXX.exe)
x86_64-w64-mingw32-strip --strip-all \
    -o "$build_output" /build/windows/Poppy.exe
# Windows may still hold the previous executable open. Keep the completed
# artifact available without touching that running process.
if mv -f "$build_output" /output/Poppy.exe; then
    build_output=/output/Poppy.exe
fi
rm -rf /output/ai
cp -R "$source_dir/integration/ai" /output/
printf 'Windows x64 build: %s (host: out/docker/)\n' "$build_output"
