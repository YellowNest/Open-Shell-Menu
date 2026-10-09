$ErrorActionPreference = 'Continue'
$report = 'C:\OEM\win11-native-x64-proof.json'
function Publish($obj) {
  $j = $obj | ConvertTo-Json -Depth 8
  Set-Content -LiteralPath $report -Value $j -Encoding UTF8
  foreach ($dest in @('Z:\win11-native-x64-proof.json','\\host.lan\Shared\win11-native-x64-proof.json')) {
    try { Set-Content -LiteralPath $dest -Value $j -Encoding UTF8 -ErrorAction Stop; break } catch {}
  }
}
try {
  $os = Get-CimInstance Win32_OperatingSystem
  $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
  $arch = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
  $processArch = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
  $explorer = @()
  for ($i=0; $i -lt 100; $i++) {
    $explorer = @(Get-Process explorer -ErrorAction SilentlyContinue)
    if ($explorer.Count -gt 0) { break }
    Start-Sleep -Seconds 10
  }
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ShellProbe {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)]
  public static extern IntPtr FindWindow(string className, string title);
  [DllImport("user32.dll")]
  public static extern IntPtr GetShellWindow();
}
'@
  $taskbar = [ShellProbe]::FindWindow('Shell_TrayWnd',$null)
  $shell = [ShellProbe]::GetShellWindow()
  $r = [ordered]@{
    Observed=(Get-Date).ToString('o')
    OS=$os.Caption
    Build=$os.BuildNumber
    OSArchitecture=$arch
    ProcessArchitecture=$processArch
    CurrentSessionId=([Diagnostics.Process]::GetCurrentProcess().SessionId)
    Processor=$cpu.Name
    ExplorerProcesses=@($explorer | ForEach-Object { [ordered]@{Id=$_.Id;SessionId=$_.SessionId} })
    TaskbarHwnd=[string]$taskbar
    ShellHwnd=[string]$shell
    NativeWindows11X64=($os.Caption -match 'Windows 11' -and $arch -eq 'X64' -and $processArch -eq 'X64')
    ExplorerRunning=($explorer.Count -gt 0)
    TaskbarDetected=($taskbar -ne [IntPtr]::Zero)
  }
  Publish $r
  if (-not $r.NativeWindows11X64 -or -not $r.ExplorerRunning) { exit 3 }
  $installers=@('Z:\OpenShellSetup_4_4_202.exe','\\host.lan\Shared\OpenShellSetup_4_4_202.exe')
  $installer=$installers | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $installer) {
    $r['OpenShellIntegration']='NOT_RUN_INSTALLER_UNAVAILABLE'
    Publish $r
    exit 0
  }
  $p=Start-Process -FilePath $installer -ArgumentList '/qn REBOOT=ReallySuppress' -Wait -PassThru
  $r['InstallerExit']=$p.ExitCode
  if ($p.ExitCode -notin @(0,3010)) {
    $r['OpenShellIntegration']='INSTALL_FAILED'
    Publish $r
    exit 4
  }
  $exe=Join-Path $env:ProgramFiles 'Open-Shell\StartMenu.exe'
  if (-not (Test-Path $exe)) {
    $r['OpenShellIntegration']='STARTMENU_NOT_INSTALLED'
    Publish $r
    exit 5
  }
  Start-Process -FilePath $exe
  Start-Sleep -Seconds 15
  $r['StartMenuRunning'] = (@(Get-Process StartMenu -ErrorAction SilentlyContinue).Count -gt 0)
  $r['ExplorerAliveAfterStart'] = (@(Get-Process explorer -ErrorAction SilentlyContinue).Count -gt 0)
  Start-Process -FilePath $exe -ArgumentList '-exit' -Wait
  Start-Sleep -Seconds 8
  $r['ExplorerAliveAfterStop'] = (@(Get-Process explorer -ErrorAction SilentlyContinue).Count -gt 0)
  $r['OpenShellIntegration']='START_STOP_EXECUTED_NOT_XAML_VISUALLY_VERIFIED'
  Publish $r
} catch {
  $e=[ordered]@{Stage='UNHANDLED_TEST_ERROR';Error=$_.Exception.ToString()}
  Publish $e
  exit 6
}
