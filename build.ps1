param(
    [string]$CompilerPath = ""
)

$ErrorActionPreference = "Stop"

if (-not $CompilerPath) {
    $cmd = Get-Command "x86_64-w64-mingw32-clang++.exe" -ErrorAction SilentlyContinue
    if ($cmd) {
        $CompilerPath = $cmd.Source
    } else {
        $defaultPath = "C:\Users\DiegoPorro\AppData\Local\Microsoft\WinGet\Packages\MartinStorsjo.LLVM-MinGW.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\llvm-mingw-20261020-ucrt-x86_64\bin\x86_64-w64-mingw32-clang++.exe"
        if (Test-Path $defaultPath) {
            $CompilerPath = $defaultPath
        }
    }
}

if (-not $CompilerPath -or -not (Test-Path $CompilerPath)) {
    throw "Compiler not found. Add x86_64-w64-mingw32-clang++.exe to PATH or pass -CompilerPath."
}

$RootDir = $PSScriptRoot
$SrcDir  = Join-Path $RootDir "src"
$DistDir = Join-Path $RootDir "dist"
New-Item -ItemType Directory -Force -Path $DistDir | Out-Null

$OutDll = Join-Path $DistDir "dinput8.dll"

$DllArgs = @(
    "-shared",
    "-O2",
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-static",
    "-static-libgcc",
    "-static-libstdc++",
    (Join-Path $SrcDir "patch_main.cpp"),
    (Join-Path $SrcDir "dinput8_proxy.cpp"),
    (Join-Path $SrcDir "dinput8.def"),
    "-luser32",
    "-o", $OutDll
)

Write-Host "Building $OutDll ..."
& $CompilerPath @DllArgs
if ($LASTEXITCODE -ne 0) {
    throw "DLL build failed with exit code $LASTEXITCODE"
}

Copy-Item -Force (Join-Path $RootDir "config\FC4_AspectFix.ini") (Join-Path $DistDir "FC4_AspectFix.ini")

Write-Host "Build succeeded:"
Write-Host "  $OutDll ($((Get-Item $OutDll).Length) bytes)"
Write-Host "  $(Join-Path $DistDir 'FC4_AspectFix.ini')"
