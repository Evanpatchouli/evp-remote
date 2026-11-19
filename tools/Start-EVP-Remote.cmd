@echo off
setlocal
if not defined EVP_SIGNAL_URL set /p "EVP_SIGNAL_URL=Public signaling URL (wss://...): "
if not defined EVP_SIGNAL_URL exit /b 1
"%~dp0runtime\node.exe" "%~dp0desktop\main.mjs"
if errorlevel 1 pause
