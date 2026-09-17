@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "PACKAGE=%CD%\DO_WKLEJENIA_DO_SYSTEM"
set "PACKAGE_ZIP=%CD%\DO_WKLEJENIA_DO_SYSTEM.zip"
set "DEPLOY_TEMP=%CD%\_release_deploy_temp"
set "MSBUILD="

echo [1/6] Szukanie MSBuild...
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe`) do if not defined MSBUILD set "MSBUILD=%%I"
)

if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe"
if not defined MSBUILD if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe" set "MSBUILD=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe"

if not defined MSBUILD (
    echo BLAD: Nie znaleziono Visual Studio 2022 z obsluga C++.
    goto :fail
)

echo [2/6] Przygotowanie bezpiecznego katalogu posredniego...
if exist "%DEPLOY_TEMP%" rmdir /s /q "%DEPLOY_TEMP%"
mkdir "%DEPLOY_TEMP%\GD3D11\shaders" || goto :fail
set "G2_SYSTEM_PATH=%DEPLOY_TEMP%"

echo [3/6] Budowanie Release Win32 dla Gothic 2...
set "RESTORE_SWITCH="
if not exist "%CD%\packages\directxmath.2026.6.12.1\build\native\directxmath.targets" set "RESTORE_SWITCH=/restore"
if not exist "%CD%\packages\directxmesh_desktop_2019.2023.4.28.1\build\native\directxmesh_desktop_2019.targets" set "RESTORE_SWITCH=/restore"
if not exist "%CD%\packages\directxtk_desktop_2019.2023.4.28.1\build\native\directxtk_desktop_2019.targets" set "RESTORE_SWITCH=/restore"
if not exist "%CD%\packages\Microsoft.XAudio2.Redist.1.2.13\build\native\Microsoft.XAudio2.Redist.targets" set "RESTORE_SWITCH=/restore"

"%MSBUILD%" Direct3D7Wrapper.sln %RESTORE_SWITCH% /m /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143 /p:RestorePackagesConfig=true /v:minimal
if errorlevel 1 goto :fail

if not exist "%CD%\Release\ddraw.dll" (
    echo BLAD: Kompilacja nie utworzyla Release\ddraw.dll.
    goto :fail
)

echo [4/6] Skladanie czystej paczki dla graczy...
if exist "%PACKAGE%" rmdir /s /q "%PACKAGE%"
mkdir "%PACKAGE%\GD3D11\shaders" || goto :fail

copy /y "%CD%\Release\ddraw.dll" "%PACKAGE%\ddraw.dll" >nul || goto :fail
xcopy "%CD%\D3D11Engine\Shaders\*" "%PACKAGE%\GD3D11\shaders\" /e /i /q /y >nul || goto :fail
xcopy "%CD%\blobs\libs\*" "%PACKAGE%\" /e /i /q /y >nul || goto :fail

for %%D in (Fonts Meshes Textures) do (
    xcopy "%CD%\blobs\%%D\*" "%PACKAGE%\GD3D11\%%D\" /e /i /q /y >nul || goto :fail
)

if exist "%CD%\blobs\data" xcopy "%CD%\blobs\data\*" "%PACKAGE%\GD3D11\data\" /e /i /q /y >nul || goto :fail

rem Symbole debugowania nie sa potrzebne graczom.
for /r "%PACKAGE%" %%F in (*.pdb *.ilk *.exp *.lib) do del /q "%%F"

(
    echo Zawartosc tego folderu skopiuj bezposrednio do katalogu System gry.
    echo.
    echo Przed kopiowaniem zamknij gre.
    echo Potwierdz polaczenie folderu GD3D11 i zastapienie istniejacych plikow.
    echo.
    echo Paczka zawiera wersje Release dla Gothic 2, wymagane biblioteki, zasoby i shadery.
    echo Wymagany jest Microsoft Visual C++ Redistributable 2015-2022 ^(x86^).
) > "%PACKAGE%\INSTRUKCJA.txt"

echo [5/6] Tworzenie ZIP...
if exist "%PACKAGE_ZIP%" del /q "%PACKAGE_ZIP%"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -LiteralPath '%PACKAGE%' -DestinationPath '%PACKAGE_ZIP%' -CompressionLevel Optimal"
if errorlevel 1 goto :fail

echo [6/6] Kontrola paczki...
if not exist "%PACKAGE%\ddraw.dll" goto :fail
if not exist "%PACKAGE%\GD3D11\shaders\VS_ExInstancedObj.hlsl" goto :fail
if not exist "%PACKAGE_ZIP%" goto :fail

for /r "%PACKAGE%" %%F in (*.pdb *.ilk *.exp *.lib) do (
    echo BLAD: W paczce pozostal plik developerski: %%F
    goto :fail
)

if exist "%DEPLOY_TEMP%" rmdir /s /q "%DEPLOY_TEMP%"

echo.
echo GOTOWE:
echo   %PACKAGE%
echo   %PACKAGE_ZIP%
exit /b 0

:fail
echo.
echo TWORZENIE RELEASE NIE POWIODLO SIE.
echo Sprawdz komunikaty powyzej. Poprzednia poprawna paczka nie jest uznawana za nowy release.
exit /b 1
