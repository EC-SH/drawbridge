$ErrorActionPreference = 'Stop'

# default to latest stable release tag
$tag     = 'v1.2.0'
$zipDir  = 'drawbridge-1.2.0'
$url     = "https://github.com/EC-SH/drawbridge/archive/refs/tags/$tag.zip"

# check if we requested bleeding edge / unreleased
$reqBranch = $args[0]
if (!$reqBranch) {
    $reqBranch = $env:DRAWBRIDGE_BRANCH
    if (!$reqBranch) {
        $reqBranch = $env:POCKET_DIAL_BRANCH
    }
}

if ($reqBranch -eq 'main' -or $reqBranch -eq 'unreleased') {
    Write-Host "Using bleeding-edge (unreleased) main branch..."
    $url    = "https://github.com/EC-SH/drawbridge/archive/refs/heads/main.zip"
    $zipDir = "drawbridge-main"
}

$tmp = Join-Path $env:TEMP ([guid]::NewGuid())
New-Item -ItemType Directory $tmp | Out-Null
$zip = Join-Path $tmp 'pd.zip'

Write-Host "Downloading drawbridge..."
Invoke-WebRequest $url -OutFile $zip

Expand-Archive $zip -DestinationPath $tmp
Set-Location (Join-Path $tmp $zipDir)
.\quickstart.bat
