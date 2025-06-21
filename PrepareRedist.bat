@ECHO OFF
SET "TARGET_DIR=%1"


CD "%~dp0"

mkdir "%TARGET_DIR%\GD3D11\shaders"
Xcopy "D3D11Engine\Shaders\*" "%TARGET_DIR%\GD3D11\shaders" /y /s

Xcopy "blobs\data" "%TARGET_DIR%\GD3D11\data\" /y /s
Xcopy "blobs\Meshes" "%TARGET_DIR%\GD3D11\Meshes\" /y /s
Xcopy "blobs\Textures" "%TARGET_DIR%\GD3D11\Textures\" /y /s
Xcopy "blobs\Fonts" "%TARGET_DIR%\GD3D11\Fonts\" /y /s


Xcopy "blobs\libs\*" "%TARGET_DIR%\" /y /s

