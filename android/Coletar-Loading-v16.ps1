param(
    [ValidateRange(1, 8)][int]$ExpectedCores = 2,
    [string]$Adb = "",
    [string]$DeviceSerial = "",
    [string]$OutputDirectory = "",
    [switch]$SkipNativeStacks
)

# Windows PowerShell 5.1 / PowerShell 7. Does not build, install, stop or restart
# the app. Keep the FAILED ATTEMPT open on the phone while this runs on the PC.
$ErrorActionPreference = "Stop"
if (-not $Adb) {
    $sdkAdb = Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools\adb.exe"
    if (Test-Path -LiteralPath $sdkAdb) { $Adb = $sdkAdb }
    else { $Adb = "adb" }
}
if (-not (Get-Command $Adb -ErrorAction SilentlyContinue)) {
    throw "ADB nao encontrado. Informe -Adb com o caminho de adb.exe."
}
$snapshotScript = Join-Path $PSScriptRoot "coletar-loading.sh"
if (-not (Test-Path -LiteralPath $snapshotScript)) {
    throw "Extraia o ZIP inteiro: falta coletar-loading.sh junto deste script."
}
$deviceArgs = @()
if ($DeviceSerial) { $deviceArgs = @("-s", $DeviceSerial) }
$script:lastAdbExit = 0
function Invoke-Device {
    param([string[]]$Arguments)
    # Native stderr is evidence (including Permission denied), not a reason to
    # discard the rest of a partial capture in Windows PowerShell 5.1.
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = "Continue"
        $result = @(& $Adb @deviceArgs @Arguments 2>&1 | ForEach-Object { "$_" })
        $script:lastAdbExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    return $result
}
function Save-DeviceText {
    param([string]$Name, [string[]]$Arguments)
    $result = @(Invoke-Device -Arguments $Arguments)
    (@("adb exit=$script:lastAdbExit") + $result) | Out-File -LiteralPath (Join-Path $OutputDirectory $Name) -Encoding utf8
}
$deviceState = @(Invoke-Device -Arguments @("get-state"))
if ($script:lastAdbExit -ne 0 -or $deviceState -notcontains "device") {
    throw "Celular nao acessivel: $($deviceState -join ' '). Autorize a depuracao USB; se houver varios aparelhos, use -DeviceSerial."
}
if (-not $OutputDirectory) {
    $name = "DefJam-Loading-{0}-{1}" -f (Get-Date -Format "yyyyMMdd-HHmmss"), ([Guid]::NewGuid().ToString("N").Substring(0, 6))
    $OutputDirectory = Join-Path (Join-Path $env:USERPROFILE "Downloads") $name
}
if (Test-Path -LiteralPath $OutputDirectory) {
    throw "Escolha uma pasta nova para nao misturar tentativas: $OutputDirectory"
}
$null = New-Item -ItemType Directory -Path $OutputDirectory
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$phoneLog = "/storage/emulated/0/DefJamFFNY/defjam-native.log"
$initialLog = Join-Path $OutputDirectory "defjam-native-inicio.log"
$finalLog = Join-Path $OutputDirectory "defjam-native-fim.log"
$summary = New-Object 'System.Collections.Generic.List[string]'
$summary.Add("Coleta local: $((Get-Date).ToString('o'))")
$summary.Add("Origem EXCLUSIVA do log: adb pull $phoneLog")
$summary.Add("Nucleos escolhidos informados pelo usuario: $ExpectedCores")
$summary.Add("Nenhum ajuste aplicado. Nenhum app reiniciado, fechado ou instalado.")
$summary.Add("Estado de atividade vale no instante da coleta; nao reconstrui a pausa anterior.")
$summary.Add("PSR = ultimo CPU; afinidade = Cpus_allowed_list. Wchan nao identifica sozinho dono/objeto guest.")
$summary.Add("Stacks podem conter somente offsets se o APK nao tiver simbolos; permission denied nao e thread parada.")
$summary.Add("Amostras por thread sao sequenciais, com uptime; nao assumir que o intervalo real e exatamente sleep 2.")
$summary.Add("MemAvailable, pressao e atividade de swap precisam ser avaliados em conjunto; temperatura/frequencia isoladas nao provam throttling.")
$remoteScript = "/data/local/tmp/defjam-loading-$([Guid]::NewGuid().ToString('N')).sh"
$pushed = $false
$captureError = $null
try {
    # FIRST data operation: preserve the phone's failed attempt before doing
    # expensive diagnostics. No Copy-Item from C:\Games is used anywhere.
    $pullText = @(Invoke-Device -Arguments @("pull", $phoneLog, $initialLog))
    $pullText | Out-File -LiteralPath (Join-Path $OutputDirectory "adb-pull-inicio.txt") -Encoding utf8
    if ($script:lastAdbExit -ne 0 -or -not (Test-Path -LiteralPath $initialLog)) {
        throw "Nao consegui obter o log do celular. Veja adb-pull-inicio.txt; nao substitui por log do PC."
    }
    $headerCores = $null
    $n = 0
    foreach ($line in [IO.File]::ReadLines($initialLog)) {
        if ($line -match 'guest CPU request\s+([1-8])\b') { $headerCores = [int]$Matches[1]; break }
        $n++
        if ($n -ge 100) { break }
    }
    if ($null -eq $headerCores) {
        $summary.Add("ATENCAO: cabecalho de nucleos nao localizado nas primeiras 100 linhas; tentativa nao confirmada.")
    } elseif ($headerCores -ne $ExpectedCores) {
        $summary.Add("DIVERGENCIA: selecionado/informado=$ExpectedCores, nativo recebeu=$headerCores. Investigar LauncherActivity -> DefJamActivity -> src/main.c ANTES de atribuir a multicore.")
    } else { $summary.Add("Cabecalho nativo confirma request=$headerCores; isso nao prova a mascara efetiva aplicada.") }

    Save-DeviceText "log-metadata.txt" @("shell", "stat", $phoneLog)
    Save-DeviceText "device-time.txt" @("shell", "date")
    Save-DeviceText "partition-tamanho-logico.txt" @("shell", "ls", "-l", "/storage/emulated/0/DefJamFFNY/save/Partition*.img")
    Save-DeviceText "partition-espaco-real.txt" @("shell", "du", "-k", "/storage/emulated/0/DefJamFFNY/save/Partition*.img")
    Save-DeviceText "package.txt" @("shell", "dumpsys", "package", "org.defjamffny.android")
    Save-DeviceText "launcher-preferences.txt" @("shell", "run-as", "org.defjamffny.android", "cat", "shared_prefs/launcher.xml")
    $pidText = @(Invoke-Device -Arguments @("shell", "pidof", "org.defjamffny.android:game"))
    $pidText | Out-File -LiteralPath (Join-Path $OutputDirectory "game-pid.txt") -Encoding utf8
    $gamePid = ($pidText -join " ").Trim()
    if ($script:lastAdbExit -ne 0 -or $gamePid -notmatch '^\d+$') {
        $summary.Add("SEM PID :game unico. Log pode ser de uma tentativa encerrada; nao substitui pelo PID do launcher.")
        Save-DeviceText "processes.txt" @("shell", "ps", "-A")
    } else {
        $summary.Add("PID Linux do processo org.defjamffny.android:game = $gamePid")
        $pushText = @(Invoke-Device -Arguments @("push", $snapshotScript, $remoteScript))
        $pushText | Out-File -LiteralPath (Join-Path $OutputDirectory "adb-push-diagnostico.txt") -Encoding utf8
        if ($script:lastAdbExit -ne 0) { throw "Nao consegui enviar o coletor temporario. Log inicial preservado." }
        $pushed = $true
        $stacks = "1"
        if ($SkipNativeStacks) { $stacks = "0" }
        Write-Host "Log do celular salvo. Capturando threads de :game; mantenha a tentativa aberta."
        Save-DeviceText "threads-lifecycle.txt" @("shell", "sh", $remoteScript, $gamePid, $stacks)
        Save-DeviceText "logcat-game.txt" @("logcat", "-b", "all", "-d", "-v", "threadtime", "-t", "1200", "--pid=$gamePid")
    }
    $pullText = @(Invoke-Device -Arguments @("pull", $phoneLog, $finalLog))
    $pullText | Out-File -LiteralPath (Join-Path $OutputDirectory "adb-pull-fim.txt") -Encoding utf8
    if ($script:lastAdbExit -ne 0) { $summary.Add("Coleta final falhou; log inicial foi preservado.") }
    foreach ($logFile in @($initialLog, $finalLog)) {
        if (Test-Path -LiteralPath $logFile) {
            $hash = Get-FileHash -LiteralPath $logFile -Algorithm SHA256
            $summary.Add("SHA256 $([IO.Path]::GetFileName($logFile)): $($hash.Hash)")
        }
    }
} catch {
    $captureError = $_.Exception.Message
    $summary.Add("COLETA PARCIAL: $captureError")
} finally {
    if ($pushed) { $null = Invoke-Device -Arguments @("shell", "rm", "-f", $remoteScript) }
    $summary | Out-File -LiteralPath (Join-Path $OutputDirectory "RESUMO.txt") -Encoding utf8
    $archive = "$OutputDirectory.zip"
    Compress-Archive -Path (Join-Path $OutputDirectory "*") -DestinationPath $archive
    Write-Host "Arquivo para enviar: $archive"
}
if ($captureError) { throw $captureError }
