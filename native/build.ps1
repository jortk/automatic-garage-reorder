$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$Repo = if ($env:SWOFTY_REPO) { $env:SWOFTY_REPO } else { Resolve-Path "..\.." }
$Sdk  = if ($env:SDK_INCLUDE) { $env:SDK_INCLUDE } else { Join-Path $Repo "packages\sdk\include" }
$Game = if ($env:GAME_INCLUDE) { $env:GAME_INCLUDE } else { Join-Path $Repo "native\include" }

New-Item -ItemType Directory -Force -Path "$PSScriptRoot\build\zig-cache" | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = "$PSScriptRoot\build\zig-cache"
$env:ZIG_LOCAL_CACHE_DIR  = "$PSScriptRoot\build\zig-cache"

$ZigCmd = Get-Command zig -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $ZigCmd) {
    $SearchGlobs = @(
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links\zig.exe",
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\zig.zig*\*\zig.exe",
        "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\zig.zig*\zig.exe",
        "$env:USERPROFILE\scoop\shims\zig.exe",
        "$env:USERPROFILE\Downloads\zig*\zig.exe",
        "$env:USERPROFILE\Downloads\zig*\*\zig.exe",
        "C:\zig*\zig.exe"
    )
    foreach ($g in $SearchGlobs) {
        $found = Get-Item $g -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
        if ($found) { $ZigCmd = $found; break }
    }
}
if (-not $ZigCmd) {
    throw "Zig compiler ('zig') not found on PATH. Install it with: winget install -e --id zig.zig"
}

$ZigDir = Split-Path -Parent $ZigCmd
if (Test-Path "$ZigDir\lib") {
    $env:ZIG_LIB_DIR = "$ZigDir\lib"
}

Write-Host "Running unit tests..."
& $ZigCmd c++ -target x86_64-windows-gnu -std=c++23 -O2 `
    -o "$PSScriptRoot\test_runner.exe" test\garage_order_logic_test.cpp
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& "$PSScriptRoot\test_runner.exe"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Remove-Item -Force -ErrorAction SilentlyContinue "$PSScriptRoot\test_runner.exe", "$PSScriptRoot\test_runner.pdb"

$SafetyHookDir = Join-Path $Repo "native\third_party\safetyhook"
$SafetyHookObj = Join-Path $Repo "native\build\safetyhook.o"
$ZydisObj      = Join-Path $Repo "native\build\Zydis.o"

Write-Host "Compiling pitlane_garage_order.dll..."
& $ZigCmd c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -Wno-nullability-completeness `
    -I"$Game" -I"$Sdk" -I"$SafetyHookDir" -D__USE_MINGW_ANSI_STDIO=1 -shared -s `
    -o pitlane_garage_order.dll src\pitlane_garage_order.cpp "$SafetyHookObj" "$ZydisObj"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Remove-Item -Force -ErrorAction SilentlyContinue *.lib, *.pdb
Write-Host "Built $PSScriptRoot\pitlane_garage_order.dll"

$AddonRoot = Resolve-Path "$PSScriptRoot\.."
$Manifest = Get-Content -Raw "$AddonRoot\swofty.json" | ConvertFrom-Json
$OutAddon = Join-Path $AddonRoot "$($Manifest.id).swoftyaddon"
$OutZip   = Join-Path $AddonRoot "$($Manifest.id).zip"
if (Test-Path $OutAddon) { Remove-Item -Force $OutAddon }
if (Test-Path $OutZip)   { Remove-Item -Force $OutZip }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$PackageEntries = @("LICENSE", "README.md", "swofty.json", "icons/pitlane.png", "native/pitlane_garage_order.dll")

foreach ($OutFile in @($OutAddon, $OutZip)) {
    $Zip = [System.IO.Compression.ZipFile]::Open($OutFile, [System.IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($Rel in $PackageEntries) {
            $Full = Join-Path $AddonRoot ($Rel -replace '/', '\')
            if (Test-Path $Full) {
                [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Zip, $Full, $Rel, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
            }
        }
    } finally {
        $Zip.Dispose()
    }
    Write-Host "Packaged $OutFile"
}

# Deploy to Swofty installed addons directory, game folder, and launcher repo addons
$DeployTargets = @(
    "$env:LOCALAPPDATA\Swofty\addons\$($Manifest.id)",
    "C:\Program Files (x86)\Steam\steamapps\common\F1 25\swofty\addons\$($Manifest.id)",
    (Join-Path $Repo "addons\$($Manifest.id)"),
    (Join-Path $Repo "addons\pitlane-garage-order")
)

foreach ($Target in $DeployTargets) {
    if (Test-Path (Split-Path -Parent $Target)) {
        try {
            New-Item -ItemType Directory -Force -Path (Join-Path $Target "native") | Out-Null
            New-Item -ItemType Directory -Force -Path (Join-Path $Target "icons") | Out-Null
            Copy-Item -Force "$PSScriptRoot\pitlane_garage_order.dll" (Join-Path $Target "native\pitlane_garage_order.dll") -ErrorAction Stop
            Copy-Item -Force "$AddonRoot\swofty.json" (Join-Path $Target "swofty.json") -ErrorAction Stop
            Copy-Item -Force "$AddonRoot\README.md" (Join-Path $Target "README.md") -ErrorAction SilentlyContinue
            Copy-Item -Force "$AddonRoot\LICENSE" (Join-Path $Target "LICENSE") -ErrorAction SilentlyContinue
            if (Test-Path "$AddonRoot\icons\pitlane.png") {
                Copy-Item -Force "$AddonRoot\icons\pitlane.png" (Join-Path $Target "icons\pitlane.png") -ErrorAction SilentlyContinue
            }
            Copy-Item -Force $OutAddon (Join-Path $Target (Split-Path -Leaf $OutAddon)) -ErrorAction Stop
            Write-Host "Deployed to $Target"
        } catch {
            Write-Warning "Could not copy to $Target (file in use by running game?)"
        }
    }
}
