#!/bin/sh
# Builds both variants of the GBFR Crash Logger from crashlogger.cpp.
#   out/dinput8.dll  - default (the game imports DINPUT8.dll)
#   out/version.dll  - alternative if dinput8.dll is taken by another mod
# Needs the mingw-w64 cross compiler (x86_64-w64-mingw32-g++), e.g. on Ubuntu/WSL:
#   sudo apt install g++-mingw-w64-x86-64
# Fully static so the DLL has no runtime dependencies beyond Windows itself.
set -e
CXX=x86_64-w64-mingw32-g++
FLAGS="-O2 -std=c++17 -shared -static -static-libgcc -static-libstdc++ -fno-exceptions -fno-rtti -Wall -Wno-unused-function -Wno-cast-function-type -s"
LIBS="-lpsapi -ldbghelp"
mkdir -p out
$CXX $FLAGS crashlogger.cpp version.def -o out/version.dll $LIBS
$CXX $FLAGS -DPROXY_DINPUT8 crashlogger.cpp dinput8.def -o out/dinput8.dll $LIBS
echo "built: out/dinput8.dll out/version.dll"
