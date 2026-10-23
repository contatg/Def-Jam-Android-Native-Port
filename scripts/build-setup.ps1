<# Build only the game-free Windows launcher/setup; never analyze or build a dump. #>
param([switch]$Development)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $PSScriptRoot 'env.ps1')
Push-Location $repo
try {
    cmake -S setup -B build/setup -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE) { throw 'Setup configure failed' }
    cmake --build build/setup --target DefJamLauncher
    if ($LASTEXITCODE) { throw 'Launcher build failed' }
    $packageArgs = @('scripts/package-setup.py', '--launcher', 'build/setup/DefJamLauncher.exe')
    if ($Development) { $packageArgs += '--development' }
    python @packageArgs
    if ($LASTEXITCODE) { throw 'Setup payload packaging failed' }
    $payload = Join-Path $repo 'build/setup-payload.zip'
    cmake -S setup -B build/setup -G Ninja -DCMAKE_BUILD_TYPE=Release "-DSETUP_PAYLOAD=$payload"
    if ($LASTEXITCODE) { throw 'Setup payload configure failed' }
    cmake --build build/setup --target DefJamSetup
    if ($LASTEXITCODE) { throw 'Setup build failed' }
    $assetArgs = @('scripts/check-setup-assets.py', '--payload', 'build/setup-payload.zip',
                  '--installer', 'build/setup/DefJamSetup.exe', '--output-dir', 'build/setup-assets')
    if ($Development) { $assetArgs += '--development' }
    python @assetArgs
    if ($LASTEXITCODE) { throw 'Setup asset validation failed' }
} finally { Pop-Location }
