@echo off
cd /d "%~dp0"

if not defined MILLSAPH_THIRD_PARTY_DIR set "MILLSAPH_THIRD_PARTY_DIR=%CD%\third_party"
if not defined MILLSAPH_OFX_VERSION set "MILLSAPH_OFX_VERSION=OFX_Release_1.5.1"

cmake -S . -B build || exit /b 1
cmake --build build --config Release || exit /b 1
