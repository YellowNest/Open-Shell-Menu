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
  $r['Stage']='EXPLORER_PROVEN'
  $r['Terminal']=$false
  Publish $r
  if (-not $r.NativeWindows11X64 -or -not $r.ExplorerRunning) { exit 3 }
  $installers=@('Z:\OpenShellSetup_4_4_202.exe','\\host.lan\Shared\OpenShellSetup_4_4_202.exe')
  $installer=$installers | Where-Object { Test-Path $_ } | Select-Object -First 1
  if (-not $installer) {
    $r['OpenShellIntegration']='NOT_RUN_INSTALLER_UNAVAILABLE'
    Publish $r
    exit 0
  }
  $localInstaller='C:\\OEM\\OpenShellSetup_4_4_202.exe'
  $r['Stage']='COPYING_INSTALLER'
  Publish $r
  Copy-Item -LiteralPath $installer -Destination $localInstaller -Force -ErrorAction Stop
  $hash=(Get-FileHash -LiteralPath $localInstaller -Algorithm SHA256).Hash.ToLowerInvariant()
  $r['InstallerSHA256']=$hash
  if($hash -ne '10f976b90127f6d7a402a9934949de50eeffa71f6687133f9f88b5b23692882f') { throw 'Installer SHA256 mismatch inside Windows guest' }
  $r['Stage']='INSTALLER_STARTING'
  Publish $r
  $p=Start-Process -FilePath $localInstaller -ArgumentList '/qn REBOOT=ReallySuppress /l*v "C:\\OEM\\openshell-msi.log"' -PassThru -ErrorAction Stop
  $r['InstallerPid']=$p.Id
  $installerEnded=$false
  for ($i=0; $i -lt 60; $i++) {
    if($p.WaitForExit(5000)) { $installerEnded=$true; break }
    $r['Stage']='INSTALLER_RUNNING'
    $r['InstallerPoll']=$i+1
    $r['MSIExecProcessCount']=@(Get-Process msiexec -ErrorAction SilentlyContinue).Count
    Publish $r
  }
  if(-not $installerEnded) {
    $r['Stage']='INSTALL_TIMEOUT'
    $r['OpenShellIntegration']='INSTALL_TIMEOUT'
    $r['Terminal']=$true
    Publish $r
    exit 7
  }
  $r['Stage']='INSTALLER_FINISHED'
  $r['InstallerExit']=$p.ExitCode
  Publish $r
  if ($p.ExitCode -notin @(0,3010)) {
    $r['OpenShellIntegration']='INSTALL_FAILED'
    Publish $r
    exit 4
  }
  $r['Stage']='CHECKING_INSTALLED_EXE'
  Publish $r
  $exe=Join-Path $env:ProgramFiles 'Open-Shell\StartMenu.exe'
  if (-not (Test-Path $exe)) {
    $r['OpenShellIntegration']='STARTMENU_NOT_INSTALLED'
    Publish $r
    exit 5
  }
  $r['Stage']='STARTING_OPEN_SHELL'
  Publish $r
  Start-Process -FilePath $exe
  Start-Sleep -Seconds 15
  $r['StartMenuRunning'] = (@(Get-Process StartMenu -ErrorAction SilentlyContinue).Count -gt 0)
  $r['ExplorerAliveAfterStart'] = (@(Get-Process explorer -ErrorAction SilentlyContinue).Count -gt 0)
  $r['Stage']='STOPPING_OPEN_SHELL'
  Publish $r
  $exitProcess=Start-Process -FilePath $exe -ArgumentList '-exit' -PassThru
  if(-not $exitProcess.WaitForExit(30000)) { throw 'StartMenu -exit exceeded 30 seconds' }
  Start-Sleep -Seconds 8
  $r['ExplorerAliveAfterStop'] = (@(Get-Process explorer -ErrorAction SilentlyContinue).Count -gt 0)
  $r['OpenShellIntegration']='START_STOP_EXECUTED_NOT_XAML_VISUALLY_VERIFIED'
  $r['Stage']='TEST_COMPLETE'
  $r['Terminal']=$true
  Publish $r
} catch {
  $r['Stage']='TEST_ERROR'
  $r['Terminal']=$true
  $r['OpenShellIntegration']='TEST_ERROR'
  $r['Failure']=$_.Exception.ToString()
  Publish $r
  exit 6
}
