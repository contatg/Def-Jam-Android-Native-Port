<#
.SYNOPSIS  Save what the game's window is showing right now as a PNG.
.DESCRIPTION
  The harness's captures are read from the back buffer before presentation. This grabs the
  window's client area off the screen instead, so it shows what the player sees: use it to
  check anything applied at presentation (the gamma ramp, scaling). The window must be
  visible and not covered. The PNG is the game's artwork: keep it under logs/, never commit it.
.EXAMPLE   .\scripts\window-shot.ps1 -Out logs\shots\window.png
#>
param([Parameter(Mandatory = $true)][string]$Out)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class GameWindow {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    public struct RECT { public int L, T, R, B; }
    public struct POINT { public int X, Y; }
}
"@
$p = Get-Process defjam_recomp -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Output "the game has no window"; exit 1 }
$r = New-Object GameWindow+RECT; [GameWindow]::GetClientRect($p.MainWindowHandle, [ref]$r) | Out-Null
$pt = New-Object GameWindow+POINT; [GameWindow]::ClientToScreen($p.MainWindowHandle, [ref]$pt) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $($bmp.Width)x$($bmp.Height) to $Out"
