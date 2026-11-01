@echo off
chcp 65001 >nul
cd /d "%~dp0"
if exist ".git\index.lock" (
  tasklist /fi "imagename eq git.exe" 2>nul | find /i "git.exe" >nul
  if errorlevel 1 (
    echo Borrando un archivo de bloqueo viejo de Git...
    del /f /q ".git\index.lock"
  ) else (
    echo Hay otro Git corriendo. Cerralo ^(GitHub Desktop, VS Code, otra ventana de CMD^) y volve a intentar.
    pause
    exit /b 1
  )
)
echo ==============================================
echo   Quake 2 web - crear repositorio y subir
echo ==============================================
echo.
where git >nul 2>nul || (echo Falta Git: https://git-scm.com & pause & exit /b 1)
where gh >nul 2>nul || (echo Falta GitHub CLI: https://cli.github.com & pause & exit /b 1)
if not exist "index.html" (echo Poné este .bat DENTRO de la carpeta que tiene index.html, baseq2 y Qwasm2-main. & pause & exit /b 1)
if not exist "baseq2\pak0.part0" (echo No encuentro baseq2\pak0.part0. Esta no es la carpeta correcta. & pause & exit /b 1)

gh auth status >nul 2>nul || gh auth login
for /f %%u in ('gh api user -q .login') do set GHU=%%u
echo Cuenta de GitHub: %GHU%

REM identidad de Git (solo si falta)
set GMAIL=
for /f "delims=" %%e in ('git config --global user.email 2^>nul') do set GMAIL=%%e
if not defined GMAIL (
  set /p GMAIL=Tu CORREO de GitHub: 
  git config --global user.email "%GMAIL%"
  git config --global user.name "%GHU%"
)

if not exist ".git" (git init & git branch -M main)
git add .
git commit -m "quake2 web"
git remote remove origin >nul 2>nul

echo.
echo Creando el repositorio quake-2-web...
gh repo create quake-2-web --public --source=. --remote=origin
if errorlevel 1 (
  echo.
  echo No se pudo crear el repositorio (puede que ya exista). Mirá el mensaje de arriba.
  pause
  exit /b 1
)

echo Activando GitHub Pages con Actions...
gh api -X POST repos/%GHU%/quake-2-web/pages -f build_type=workflow >nul 2>nul

echo.
echo Subiendo (unos 180 MB, puede tardar)...
git push -u origin main
if errorlevel 1 (
  echo.
  echo Fallo el push. Copiá el mensaje de arriba y pasámelo.
  pause
  exit /b 1
)

echo.
echo LISTO. La compilacion tarda unos 10-20 min en https://github.com/%GHU%/quake-2-web/actions
echo Cuando este en verde, el juego queda en https://%GHU%.github.io/quake-2-web/
pause
