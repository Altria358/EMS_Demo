#!/bin/bash
set -e
cd "$(dirname "$0")"

CMAKE_ARGS=""
if [ "$1" = "--arm" ]; then
    CMAKE_ARGS="-DCMAKE_C_COMPILER=aarch64-linux-gcc -DCMAKE_CXX_COMPILER=aarch64-linux-g++"
    echo "🔨 交叉编译 ARM64..."
else
    echo "🔨 本机编译..."
fi

rm -rf build
mkdir -p build
cd build
cmake .. $CMAKE_ARGS
make

echo "✅ 编译完成: $(pwd)/ems_gateway"
