@ECHO OFF
setlocal enableextensions enabledelayedexpansion

PUSHD %~dp0

SET "VERSION=17.8-dev9"

CALL :SUB_BUILD "Gothic2-GD3D11-%VERSION%_avx2" "Release_AVX"
CD /D %~dp0
CALL :SUB_BUILD "Gothic1-GD3D11-%VERSION%_avx" "Release_G1_AVX"
CD /D %~dp0
CALL :SUB_BUILD "Gothic2-GD3D11-%VERSION%" "Release"
CD /D %~dp0
CALL :SUB_BUILD "Gothic1-GD3D11-%VERSION%" "Release_G1"
CD /D %~dp0
CALL :SUB_BUILD "Gothic2-GD3D11-%VERSION%_SpacerNET" "Spacer_NET"

EXIT /B

:SUB_BUILD

CALL PrepareRedist.bat %1
COPY "%2\ddraw.dll" %1
PUSHD %1
"C:\Program Files\7-Zip\7z.exe" a -tzip "..\%1.zip" "*.*" -r
POPD

EXIT /B

