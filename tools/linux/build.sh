#!/usr/bin/env bash
# Native Linux application; no PS5 stubs, no Proton.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$root"
cc=${CC:-clang-19}
cxx=${CXX:-clang++-19}
jobs=${JOBS:-4}
[[ -f assets/default.xex ]] || { echo 'Missing assets/default.xex (your extracted Ninja Gaiden II executable)' >&2; exit 2; }
[[ -f third_party/rexglue-sdk/CMakeLists.txt ]] || { echo 'Run tools/setup_sdk.sh first' >&2; exit 2; }
cmake -S third_party/rexglue-sdk -B out/sdk-host -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS='-stdlib=libstdc++ -march=x86-64-v3' \
  -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_GPU_EMULATION=OFF
cmake --build out/sdk-host --target rexglue --parallel "$jobs"
third_party/rexglue-sdk/out/linux-amd64/rexglue codegen ng2_manifest.toml
python3 local/diag/patch_missed_regs.py
python3 local/diag/patch_scanguard.py
python3 ps5/tools/ps5_patch_generated.py generated
python3 tools/izuna/gen_hook_thunks.py
cmake -S . -B out/linux-desktop -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
  -DCMAKE_C_FLAGS=-march=x86-64-v3 -DCMAKE_CXX_FLAGS='-stdlib=libstdc++ -march=x86-64-v3' \
  -DREXSDK_DIR="$root/third_party/rexglue-sdk" \
  -DIZUNA_LINUX_DESKTOP=ON -DREXGLUE_ENABLE_TRACY=OFF
cmake --build out/linux-desktop --target ng2 --parallel "$jobs"
