@echo off
echo.
echo   Couch Conduit Host
echo   ==================
echo.
echo Starting host... (waiting for client connection on port 47100)
echo Press Ctrl+C to stop.
echo.
"%~dp0cc_host.exe" %*
pause
