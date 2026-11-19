@echo off
chcp 65001 >nul
setlocal
if not defined EVP_SIGNAL_URL set /p "EVP_SIGNAL_URL=请输入信令服务器地址（wss://.../）："
if not defined EVP_SIGNAL_URL exit /b 1
"%~dp0runtime\node.exe" "%~dp0desktop\main.mjs"
if errorlevel 1 pause
