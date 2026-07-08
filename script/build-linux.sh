echo "[1/3] Configuring CMake..."
cmake --preset="Linux Config"
echo

echo "[2/3] Building Debug version..."
cmake --build --preset="Linux Debug Build"
echo

echo "[3/3] Building Release version..."
cmake --build --preset="Linux Release Build"
echo
