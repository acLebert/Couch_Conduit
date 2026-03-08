@echo off
echo.
echo   Couch Conduit Client
echo   ====================
echo.
set /p HOST_IP="Enter host IP address: "
echo.
echo Connecting to %HOST_IP%...
echo Press Ctrl+C to stop.
echo.
"%~dp0cc_client.exe" --host %HOST_IP% %*
pause
