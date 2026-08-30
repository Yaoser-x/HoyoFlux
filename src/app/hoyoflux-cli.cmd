@echo off
setlocal

rem HoyoFlux.exe is deliberately a Windows-subsystem application so a
rem double-click stays silent. PowerShell does not wait for GUI-subsystem
rem applications, so this console wrapper provides synchronous CLI semantics.
start "" /wait "%~dp0hoyoflux.exe" %*
set "exit_code=%ERRORLEVEL%"

endlocal & exit /b %exit_code%
