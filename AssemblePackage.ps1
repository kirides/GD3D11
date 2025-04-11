# Define paths
$solutionPath = "Direct3D7Wrapper.sln"
$outputFolder = "Z:/HumiliationRitual/"
$binFolder = "$outputFolder/GD3D11/Bin/"
$msBuildPath = "C:\Program Files\Microsoft Visual Studio\2022\Community\Msbuild\Current\Bin\amd64\MSBuild.exe"
$current_date = Get-Date -Format "dd-MM-yyyy"
$configurations = @{
    "Release_G1_SSE2" = "g1.dll"
    "Release_G1_AVX" = "g1_avx.dll"
    "Release_G1_AVX2" = "g1_avx2.dll"
    "Release_G1A_SSE2" = "g1a.dll"
    "Release_G1A_AVX" = "g1a_avx.dll"
    "Release_G1A_AVX2" = "g1a_avx2.dll"
    "Release_G2A_SSE2" = "g2a.dll"
    "Release_G2A_AVX" = "g2a_avx.dll"
    "Release_G2A_AVX2" = "g2a_avx2.dll"
    "SpacerNET_G1_SSE2" = "spacernet_g1.dll"
    "SpacerNET_G1_AVX" = "spacernet_g1_avx.dll"
    "SpacerNET_G1_AVX2" = "spacernet_g1_avx2.dll"
    "SpacerNET_G2A_SSE2" = "spacernet_g2a.dll"
    "SpacerNET_G2A_AVX" = "spacernet_g2a_avx.dll"
    "SpacerNET_G2A_AVX2" = "spacernet_g2a_avx2.dll"
}

if (-Not (Test-Path -Path $binFolder)) {
    New-Item -ItemType Directory -Path $binFolder
}

robocopy "$PSScriptRoot\blobs\Meshes\" "$outputFolder\GD3D11\Meshes\" /E /MT
robocopy "$PSScriptRoot\blobs\Textures\" "$outputFolder\GD3D11\Textures\" /E /MT
robocopy "$PSScriptRoot\blobs\Fonts\" "$outputFolder\GD3D11\Fonts\" /E /MT
robocopy "$PSScriptRoot\blobs\libs\" "$outputFolder\" /E /MT
robocopy "$PSScriptRoot\D3D11Engine\Shaders\" "$outputFolder\GD3D11\Shaders\" /E /MT
robocopy "$PSScriptRoot\D3D11Engine\CSFFT\" "$outputFolder\GD3D11\Shaders\CSFFT" *.hlsl /E /MT

# Compile main ddraw.dll component
& $msBuildPath $solutionPath /p:Configuration="Launcher"
$outputFile = "$PSScriptRoot\Launcher\ddraw.dll"
Copy-Item -Path $outputFile -Destination $outputFolder -Recurse -Force

# Compile all the bullshit dlls
foreach ($config in $configurations.Keys) {
    & $msBuildPath $solutionPath /p:Configuration=$config
    $outputFile = "$PSScriptRoot\$config\$($configurations[$config])"
    Copy-Item -Path $outputFile -Destination $binFolder -Recurse -Force
}


# pack this bitch and go
& "C:\Program Files\7-Zip\7z.exe" -mx=9 -t7z a "GD3D11-$current_date.7z" "$outputFolder\*"
#& "C:\Program Files\7-Zip-Zstandard\7z.exe" -mx=9 -t7z a "GD3D11-$current_date.7z" "$outputFolder\*"