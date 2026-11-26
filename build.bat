@echo off
cd /d "%~dp0"

if not defined MILLSAPH_VERSION set "MILLSAPH_VERSION=0.1.0"
if not defined MILLSAPH_THIRD_PARTY_DIR set "MILLSAPH_THIRD_PARTY_DIR=%CD%\third_party"
if not defined MILLSAPH_OFX_VERSION set "MILLSAPH_OFX_VERSION=OFX_Release_1.5.1"

rem build\win: CMake's work in cmake\, the plugins in release\, the programs in bin\
cmake -S . -B build\win\cmake || exit /b 1
cmake --build build\win\cmake --config Release || exit /b 1
