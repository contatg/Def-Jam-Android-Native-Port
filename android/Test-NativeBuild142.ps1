# Runs on the actual Windows host before Gradle:no SDK/game/device needed.
param([Parameter(Mandatory=$true)][string]$LogDirectory)
$ErrorActionPreference="Stop"
$pythonCommand=Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1
$pythonExe=$pythonCommand.Source
$runner=Join-Path $PSScriptRoot "Invoke-NativeBuild142.ps1"
$okLog=Join-Path $LogDirectory "runner142-exit0.log"
$failLog=Join-Path $LogDirectory "runner142-exit7.log"
$ok="import sys; assert sys.argv[1] == 'space value142'; sys.stdout.write('stdout142\n'); sys.stderr.write('Note: deprecated API142\n'); sys.exit(0)"
& $runner -Executable $pythonExe -ArgumentList @("-c",$ok,"space value142") -WorkingDirectory $PSScriptRoot -LogFile $okLog -Quiet | Out-Null
$content=Get-Content -LiteralPath $okLog -Raw -Encoding UTF8
if($content -notmatch 'stdout142' -or $content -notmatch 'Note: deprecated API142' -or $content -notmatch 'exit_code=0'){
    throw "Native runner142 success/stderr/argument-quoting test failed. See $okLog"
}
$fail="import sys; sys.stderr.write('exit7-test142\n'); sys.exit(7)"
$failedAsExpected=$false
try{
    & $runner -Executable $pythonExe -ArgumentList @("-c",$fail) -WorkingDirectory $PSScriptRoot -LogFile $failLog -Quiet | Out-Null
}catch{
    if($_.Exception.Message -notmatch 'exit=7'){throw}
    $failedAsExpected=$true
}
if(!$failedAsExpected){throw "Native runner142 failed to reject a real nonzero exit."}
if((Get-Content -LiteralPath $failLog -Raw -Encoding UTF8) -notmatch 'exit_code=7'){
    throw "Native runner142 did not retain the real failure log. See $failLog"
}
Write-Host "PASS runner142:stderr note does not abort;real exit7 fails;both streams and spaced arguments preserved."
