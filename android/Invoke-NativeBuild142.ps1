# Windows PowerShell5.1/PowerShell7:do not reinterpret child stderr as ErrorRecords.
# Drain both pipes asynchronously;keep live text and the actual process exit code.
param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string[]]$ArgumentList=@(),
    [Parameter(Mandatory=$true)][string]$WorkingDirectory,
    [Parameter(Mandatory=$true)][string]$LogFile,
    [switch]$Quiet
)
$ErrorActionPreference="Stop"
if(!(Test-Path -LiteralPath $Executable -PathType Leaf)){throw "Native executable not found: $Executable"}
$items=@($Executable)+@($ArgumentList)
$quoted=@(foreach($item in $items){
    # cmd expands percent variables even inside quotes. Build arguments never
    # need expansion here;reject such input rather than execute altered text.
    if($item -match '["\r\n%]'){throw "Unsupported quote/newline/percent in native build argument."}
    '"'+$item+'"'
})
$comspecPath=$env:ComSpec
if(!$comspecPath){$comspecPath=Join-Path $env:SystemRoot "System32\cmd.exe"}
$start=[System.Diagnostics.ProcessStartInfo]::new()
$start.FileName=$comspecPath
# /s removes exactly this outer quote pair;inner executable/argument quotes remain.
$start.Arguments='/d /s /v:off /c "'+($quoted -join ' ')+'"'
$start.WorkingDirectory=$WorkingDirectory
$start.UseShellExecute=$false
$start.CreateNoWindow=$true
$start.RedirectStandardOutput=$true
$start.RedirectStandardError=$true
$start.StandardOutputEncoding=[System.Text.Encoding]::UTF8
$start.StandardErrorEncoding=[System.Text.Encoding]::UTF8
$process=[System.Diagnostics.Process]::new()
$process.StartInfo=$start
$writer=$null
$started=$false
$exitCode=$null
try{
    [void][System.IO.Directory]::CreateDirectory((Split-Path -Parent $LogFile))
    $utf8=[System.Text.UTF8Encoding]::new($false)
    $writer=[System.IO.StreamWriter]::new($LogFile,$false,$utf8)
    $writer.AutoFlush=$true
    if(!$Quiet){Write-Host "Native build log: $LogFile"}
    $started=$process.Start()
    if(!$started){throw "Could not start the native build process."}
    $outRead=$process.StandardOutput.ReadLineAsync()
    $errRead=$process.StandardError.ReadLineAsync()
    while($null -ne $outRead -or $null -ne $errRead){
        $progress=$false
        if($null -ne $outRead -and $outRead.IsCompleted){
            $line=$outRead.GetAwaiter().GetResult()
            if($null -eq $line){$outRead=$null}
            else{$writer.WriteLine($line);Write-Output ([string]$line);$outRead=$process.StandardOutput.ReadLineAsync()}
            $progress=$true
        }
        if($null -ne $errRead -and $errRead.IsCompleted){
            $line=$errRead.GetAwaiter().GetResult()
            if($null -eq $line){$errRead=$null}
            else{$writer.WriteLine($line);Write-Output ([string]$line);$errRead=$process.StandardError.ReadLineAsync()}
            $progress=$true
        }
        if(!$progress){[System.Threading.Thread]::Sleep(20)}
    }
    $process.WaitForExit()
    $exitCode=$process.ExitCode
    $writer.WriteLine("[BUILD142] exit_code="+[string]$exitCode)
}finally{
    if($null -ne $writer){$writer.Dispose()}
    if($started -and !$process.HasExited){$process.Kill()}
    $process.Dispose()
}
if($null -eq $exitCode -or $exitCode -ne 0){throw "Native build failed (exit=$exitCode). Complete log: $LogFile"}
if(!$Quiet){Write-Host "Native build finished with exit0. Log: $LogFile"}
