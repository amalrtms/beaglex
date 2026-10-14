#!/usr/bin/env bash
# Compiles the C++ engine and its JS bindings to WebAssembly.
# Needs the Emscripten SDK on PATH (source emsdk_env.sh first).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p src/wasm
em++ -std=c++20 -O3 -flto -I../include \
  ../src/order_book.cpp ../src/types.cpp \
  ../src/sim/simulation.cpp ../src/sim/agents.cpp ../src/sim/population.cpp \
  ../src/evo/genome.cpp ../src/evo/evolved_trader.cpp \
  wasm/bindings.cpp \
  -lembind -sMODULARIZE -sEXPORT_ES6 -sEXPORT_NAME=createBeaglex \
  -sALLOW_MEMORY_GROWTH -sSINGLE_FILE -sENVIRONMENT=web \
  -o src/wasm/beaglex.mjs
echo "built src/wasm/beaglex.mjs"
