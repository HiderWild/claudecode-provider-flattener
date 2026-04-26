@echo off
REM Model Gateway Launcher
REM Adds the DLL directory to PATH and starts the gateway

set "GATEWAY_DIR=%USERPROFILE%\.claude\model-gateway\bin"
set "PATH=%GATEWAY_DIR%;%PATH%"

echo Starting Model Gateway...
"%GATEWAY_DIR%\model-gateway.exe" %*
if %ERRORLEVEL% NEQ 0 (
    echo Gateway exited with error code %ERRORLEVEL%
    pause
)
