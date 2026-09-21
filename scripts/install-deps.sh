#!/usr/bin/env bash
# Install C++/GTK3 build dependencies (needs your sudo password).
set -euo pipefail
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config libgtk-3-dev libcurl4-openssl-dev
echo "Deps installed. Build with: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"
