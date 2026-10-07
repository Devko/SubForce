# Builds and runs the renderer in WSL: build.sh [out dir]
set -e
cd /mnt/d/DEV/SubForce/showreel
OUT=${1:-out}
mkdir -p "$OUT"
g++ -std=c++17 -O2 -Wall -Wextra -I../plugin render.cpp -ldl -o "$OUT/render"
time "$OUT/render" "$OUT"
