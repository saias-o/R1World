@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0game\Play.ps1" %*
if errorlevel 1 (
    echo.
    echo Le jeu n'a pas pu demarrer. Voir le message ci-dessus.
    pause
)
