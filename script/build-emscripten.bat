@echo off

echo [1/3] Configuring CMake for Emscripten...
cmake --preset="Emscripten Config" -DUSE_FFMPEG=OFF
echo[

echo [2/3] Building Debug version...
cmake --build --preset="Emscripten Debug Build"
echo[

echo [3/3] Building Release version...
cmake --build --preset="Emscripten Release Build"
echo[