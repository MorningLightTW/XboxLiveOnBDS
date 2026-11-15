<#
Build XboxLiveOnBDS and copy it into a BDS (LeviLamina) server's mods folder.

    .\scripts\deploy.ps1                       # server folder from XBOX_LIVE_BDS_DIR or scripts\server-dir.txt
    .\scripts\deploy.ps1 -Server D:\my-bds     # another server
    .\scripts\deploy.ps1 -NoBuild              # copy the last build only

The server must be stopped: Windows locks a loaded DLL.
scripts\server-dir.txt (one line: the server folder) is git-ignored, so your own path stays out of the repo.
#>
param(
    [string]$Server = $(
        $pathFile = Join-Path $PSScriptRoot "server-dir.txt"
        if ($env:XBOX_LIVE_BDS_DIR) { $env:XBOX_LIVE_BDS_DIR }
        elseif (Test-Path $pathFile) { (Get-Content $pathFile -TotalCount 1).Trim() }
        else { "" }
    ),
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$project = Split-Path -Parent $PSScriptRoot
$modName = "xbox-live-on-bds"

if (-not $Server) {
    throw "Which server? Pass -Server <folder>, set XBOX_LIVE_BDS_DIR, or put the folder in scripts\server-dir.txt."
}
if (-not (Test-Path (Join-Path $Server "bedrock_server_mod.exe"))) {
    throw "No LeviLamina server in '$Server' (bedrock_server_mod.exe not found)."
}

if (-not $NoBuild) {
    Push-Location $project
    try {
        xmake -y
        if ($LASTEXITCODE -ne 0) { throw "Build failed." }
    } finally {
        Pop-Location
    }
}

$source = Join-Path $project "bin\$modName"
$target = Join-Path $Server "mods\$modName"
if (-not (Test-Path $source)) { throw "Nothing built yet: $source" }

New-Item -ItemType Directory -Force $target | Out-Null
# Copy the build only; the mod's data folder (auth.json: the signed-in account) stays as it is.
Get-ChildItem $source -File | Copy-Item -Destination $target -Force
Copy-Item (Join-Path $source "lang") -Destination $target -Recurse -Force
Write-Host "Deployed to $target"
Write-Host "Start the server with: $(Join-Path $Server 'bedrock_server_mod.exe')"
