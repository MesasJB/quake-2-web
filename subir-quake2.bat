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
echo   Quake 2 web - subir cambios a GitHub
echo ==============================================
echo.
where git >nul 2>nul
if errorlevel 1 (
  echo No tenes Git instalado. Bajalo de https://git-scm.com y volve a ejecutar este archivo.
  pause
  exit /b 1
)
if not exist "index.html" (
  echo Este .bat tiene que estar DENTRO de la carpeta que tiene index.html, baseq2 y Qwasm2-main.
  pause
  exit /b 1
)

if exist ".git" goto :actualizar

REM ---------- Primera vez: configurar el repositorio ----------
set /p GHUSER=Tu USUARIO de GitHub (el de tu perfil): 
set /p GHMAIL=Tu CORREO de GitHub: 
set /p GHREPO=Nombre del repositorio (ej: quake-2-web): 
git config --global user.name "%GHUSER%"
git config --global user.email "%GHMAIL%"
git init
git branch -M main
git remote add origin https://github.com/%GHUSER%/%GHREPO%.git

:actualizar
echo.
echo Subiendo cambios (si hay archivos grandes puede tardar)...
git add .
git commit -m "actualizacion"
git push -u origin main
if errorlevel 1 (
  echo.
  echo Fallo el push. Copia el mensaje de arriba (tapando cualquier token) y pasamelo.
  pause
  exit /b 1
)

echo.
echo LISTO. Ahora en github.com abri la pestana Actions y espera el tilde verde.
echo Despues abri el juego con Ctrl+F5.
pause
