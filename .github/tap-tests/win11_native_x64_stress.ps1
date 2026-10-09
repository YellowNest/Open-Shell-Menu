# Real Windows 11 x64 Explorer / XAML TAP stress test. Isolated test branch.
$ErrorActionPreference='Stop'
$report='C:\OEM\win11-native-x64-proof.json'
$started=Get-Date
$state=[ordered]@{Stage='BOOT';Terminal=$false;Cycles=@();Started=$started.ToString('o')}
$cycles=[System.Collections.Generic.List[object]]::new()
function Publish([string]$stage,[bool]$terminal=$false) {
  $state['Stage']=$stage;$state['Terminal']=$terminal;$state['Updated']=(Get-Date).ToString('o')
  $state['Cycles']=@($cycles.ToArray())
  $json=$state|ConvertTo-Json -Depth 10
  Set-Content $report $json -Encoding UTF8
  foreach($remote in @('Z:\win11-native-x64-proof.json','\\host.lan\Shared\win11-native-x64-proof.json')) {
    try { Set-Content $remote $json -Encoding UTF8 -ErrorAction Stop;break } catch {}
  }
}
function Check([bool]$ok,[string]$reason) {if(-not $ok){throw $reason}}
function ProcessList([string]$name) {return @(Get-Process -Name $name -ErrorAction SilentlyContinue)}
function WaitProcess([string]$path,[string]$arg,[int]$timeout,[string]$stage) {
  $p=Start-Process -FilePath $path -ArgumentList $arg -PassThru -ErrorAction Stop
  $state['CommandPid']=$p.Id
  $state['CommandName']=$stage
  Publish $stage
  for($i=0;$i -lt $timeout;$i+=3) {
    if($p.WaitForExit(3000)) {
      $code=$p.ExitCode;$p.Dispose()
      $state['LastCommandCode']=$code
      return $code
    }
    $state['CommandSeconds']=$i+3
    Publish $stage
  }
  throw ('Timed out: '+$stage)
}
function ExplorerModules() {
  $names=@()
  foreach($p in (ProcessList 'explorer')) {
    try {$names+=@($p.Modules|Where-Object {$_.ModuleName -match 'StartMenu|OpenShell'}|ForEach-Object ModuleName)}
    catch {$state['ModuleReadError']=$_.Exception.Message}
  }
  return @($names|Sort-Object -Unique)
}
function UIAStart() {
  try {
    Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes -ErrorAction Stop
    $h=[ShellProbe]::FindWindow('Shell_TrayWnd',$null)
    $bar=[System.Windows.Automation.AutomationElement]::FromHandle($h)
    $cond=New-Object System.Windows.Automation.PropertyCondition(
      [System.Windows.Automation.AutomationElement]::AutomationIdProperty,'StartButton')
    $list=$bar.FindAll([System.Windows.Automation.TreeScope]::Descendants,$cond)
    $items=@()
    foreach($x in $list) {
      $r=$x.Current.BoundingRectangle
      $items+=@{Name=$x.Current.Name;Offscreen=$x.Current.IsOffscreen;Enabled=$x.Current.IsEnabled;
        X=$r.X;Y=$r.Y;Width=$r.Width;Height=$r.Height}
    }
    return @{Count=$list.Count;Items=$items}
  } catch {return @{Error=$_.Exception.Message}}
}
function Screenshot([string]$label) {
  try {
    Add-Type -AssemblyName System.Drawing,System.Windows.Forms
    $rect=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bmp=New-Object System.Drawing.Bitmap($rect.Width,$rect.Height)
    $gfx=[System.Drawing.Graphics]::FromImage($bmp)
    try {
      $gfx.CopyFromScreen($rect.Left,$rect.Top,0,0,$bmp.Size)
      $file='C:\OEM\'+$label+'.png'
      $bmp.Save($file,[System.Drawing.Imaging.ImageFormat]::Png)
      foreach($remote in @('Z:\','\\host.lan\Shared\')) {
        try {Copy-Item $file ($remote+$label+'.png') -Force -ErrorAction Stop;break}catch{}
      }
    }finally{$gfx.Dispose();$bmp.Dispose()}
  } catch {$state['ScreenshotError']=$_.Exception.Message}
}
try {
  Publish 'BOOTSTRAP'
  $os=Get-CimInstance Win32_OperatingSystem
  $arch=[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
  $procArch=[Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
  $state['OS']=$os.Caption;$state['Build']=$os.BuildNumber
  $state['OSArchitecture']=$arch;$state['ProcessArchitecture']=$procArch
  $state['CurrentSessionId']=[Diagnostics.Process]::GetCurrentProcess().SessionId
  Check ($os.Caption -match 'Windows 11' -and $arch -eq 'X64' -and $procArch -eq 'X64') 'Not native Win11 x64'
  for($i=0;$i -lt 90 -and (ProcessList 'explorer').Count -eq 0;$i++){Start-Sleep -Seconds 5}
  $explorer=ProcessList 'explorer'
  Check ($explorer.Count -gt 0) 'Explorer not running'
  Check (@($explorer|Where-Object {$_.SessionId -eq $state['CurrentSessionId']}).Count -gt 0) 'Explorer not in interactive session'
  $state['ExplorerRunning']=$true
  $state['ExplorerProcesses']=@($explorer|ForEach-Object {@{Id=$_.Id;SessionId=$_.SessionId}})
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)] public struct TAP_RECT { public int Left,Top,Right,Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct TAP_POINT { public int X,Y; }
public static class ShellProbe {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")]public static extern IntPtr GetShellWindow();
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr hwnd,out TAP_RECT rect);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr hwnd);
 [DllImport("user32.dll")]public static extern IntPtr WindowFromPoint(TAP_POINT point);
 [DllImport("user32.dll")]public static extern IntPtr GetAncestor(IntPtr hwnd,uint flags);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint pid);
}
'@
  $bar=[ShellProbe]::FindWindow('Shell_TrayWnd',$null)
  Check ($bar -ne [IntPtr]::Zero) 'Taskbar unavailable'
  $state['TaskbarDetected']=$true;$state['NativeWindows11X64']=$true
  # Explorer can start while first-sign-in/OOBE still covers the entire desktop.
  # Only proceed once the taskbar is physically visible and WWAHost is no longer
  # the fullscreen hit-test owner. Observe stability across two samples.
  $stable=0
  for($gate=0;$gate -lt 65;$gate++){
    $rect=New-Object TAP_RECT
    $rectOK=[ShellProbe]::GetWindowRect($bar,[ref]$rect)
    $visible=[ShellProbe]::IsWindowVisible($bar)
    $owner='UNKNOWN'
    $foregroundHandle=[IntPtr]::Zero
    if($rectOK -and $rect.Right -gt $rect.Left -and $rect.Bottom -gt $rect.Top){
      $pt=New-Object TAP_POINT
      $pt.X=[int](($rect.Left+$rect.Right)/2)
      $pt.Y=[int](($rect.Top+$rect.Bottom)/2)
      $hit=[ShellProbe]::WindowFromPoint($pt)
      $foregroundHandle=[ShellProbe]::GetAncestor($hit,2)
      $hitPid=[uint32]0
      if($foregroundHandle -ne [IntPtr]::Zero){
        $discard=[ShellProbe]::GetWindowThreadProcessId($foregroundHandle,[ref]$hitPid)
        try{$owner=(Get-Process -Id $hitPid -ErrorAction Stop).ProcessName}catch{}
      }
    }
    $state['DesktopGatePoll']=$gate+1
    $state['DesktopGateTaskbarVisible']=$visible
    $state['DesktopGateHitRootProcess']=$owner
    $state['DesktopGateHitRootHWND']=[string]$foregroundHandle
    $state['DesktopGateTaskbarRect']=@{Top=$rect.Top;Bottom=$rect.Bottom;Left=$rect.Left;Right=$rect.Right}
    $state['DesktopGateStableSamples']=$stable
    Publish 'WAITING_FOR_REAL_EXPLORER_DESKTOP'
    $oobe= $owner -in @('WWAHost','LogonUI','CloudExperienceHostBroker','FirstLogonAnim')
    if($visible -and $rectOK -and -not $oobe -and $owner -ne 'UNKNOWN'){
      $stable++
      if($stable -ge 3){break}
    }else{$stable=0}
    Start-Sleep -Seconds 5
  }
  $state['DesktopGateStableSamples']=$stable
  Check ($stable -ge 3) ('First-login/OOBE overlay never cleared; last taskbar hit owner='+$state['DesktopGateHitRootProcess'])
  Publish 'INTERACTIVE_DESKTOP_VERIFIED'
  $state['BaselineStartUIA']=UIAStart
  Screenshot 'tap-baseline'
  Publish 'NATIVE_EXPLORER_BASELINE'
  $sources=@('Z:\OpenShellSetup_4_4_202.exe','\\host.lan\Shared\OpenShellSetup_4_4_202.exe')
  $source=$sources|Where-Object {Test-Path $_}|Select-Object -First 1
  Check ([bool]$source) 'Hardened installer unavailable'
  $local='C:\OEM\OpenShellSetup_4_4_202.exe'
  Copy-Item $source $local -Force -ErrorAction Stop
  $hash=(Get-FileHash $local -Algorithm SHA256).Hash.ToLowerInvariant()
  $state['InstallerSHA256']=$hash
  Check ($hash -eq '10f976b90127f6d7a402a9934949de50eeffa71f6687133f9f88b5b23692882f') 'Wrong hardened installer hash'
  # Set logging before the installer or Explorer can load any Open-Shell DLL.
  $reg='HKCU:\Software\OpenShell\StartMenu\Settings'
  New-Item -Path $reg -Force|Out-Null
  New-ItemProperty -Path $reg -Name LogStartup -PropertyType DWord -Value 1 -Force|Out-Null
  New-ItemProperty -Path $reg -Name EnableStartButton -PropertyType DWord -Value 0 -Force|Out-Null
  Publish 'INSTALLING'
  # Wix LaunchStartMenu normally starts Open-Shell after MSI; avoid a competing
  # uncontrolled instance and any startup-log initialization before our cycles.
  $code=WaitProcess $local '/qn NOSTART=1 REBOOT=ReallySuppress /l*v "C:\OEM\openshell-msi.log"' 300 'INSTALL_RUNNING'
  $state['InstallerExit']=$code
  Check ($code -in @(0,3010)) ('MSI install failed '+$code)
  $exe=Join-Path $env:ProgramFiles 'Open-Shell\StartMenu.exe'
  Check (Test-Path $exe) 'Installed StartMenu.exe missing'
  $helper=Join-Path $env:WINDIR 'System32\StartMenuHelper64.dll'
  Check (Test-Path $helper) 'TAP helper DLL missing from System32'
  $state['HelperSHA256']=(Get-FileHash $helper -Algorithm SHA256).Hash
  Publish 'INSTALLED'
  $reg='HKCU:\Software\OpenShell\StartMenu\Settings'
  New-Item -Path $reg -Force|Out-Null
  New-ItemProperty -Path $reg -Name LogStartup -PropertyType DWord -Value 1 -Force|Out-Null
  for($i=1;$i -le 4;$i++) {
    $enabled=if($i -eq 1){0}else{1}
    if((ProcessList 'StartMenu').Count -gt 0) {
      $null=WaitProcess $exe '-exit' 20 'PREFLIGHT_EXIT'
      Start-Sleep -Seconds 5
      Check ((ProcessList 'StartMenu').Count -eq 0) 'Preflight StartMenu did not exit'
    }
    New-ItemProperty -Path $reg -Name EnableStartButton -PropertyType DWord -Value $enabled -Force|Out-Null
    New-ItemProperty -Path $reg -Name AllTaskbars -PropertyType DWord -Value 0 -Force|Out-Null
    $case=[ordered]@{Iteration=$i;EnableStartButton=$enabled;Started=(Get-Date).ToString('o')}
    $cycles.Add($case)
    $case['ExplorerPidsBefore']=@((ProcessList 'explorer')|ForEach-Object Id)
    Publish ('CYCLE_'+$i+'_START')
    $p=Start-Process -FilePath $exe -PassThru
    $case['LaunchedPID']=$p.Id
    Start-Sleep -Seconds 10
    $case['MenuPIDs']=@((ProcessList 'StartMenu')|ForEach-Object Id)
    $case['ExplorerAlive']=((ProcessList 'explorer').Count -gt 0)
    $case['Modules']=@(ExplorerModules)
    $case['HelperLoadedInExplorer']=($case['Modules'] -contains 'StartMenuHelper64.dll')
    $case['NativeStartUIA']=UIAStart
    Screenshot ('tap-cycle-'+$i+'-active')
    Publish ('CYCLE_'+$i+'_RUNNING')
    Check ($case['MenuPIDs'].Count -gt 0) ('StartMenu.exe not running cycle '+$i)
    Check $case['ExplorerAlive'] ('Explorer not running cycle '+$i)
    if($enabled -eq 1) {
      Check $case['HelperLoadedInExplorer'] ('TAP helper not loaded in Explorer cycle '+$i)
    }
    $null=WaitProcess $exe '-exit' 20 'SHUTDOWN_COMMAND'
    for($w=0;$w -lt 15 -and (ProcessList 'StartMenu').Count -gt 0;$w++){Start-Sleep -Seconds 2}
    $case['MenuStopped']=((ProcessList 'StartMenu').Count -eq 0)
    $case['ExplorerAfterStop']=((ProcessList 'explorer').Count -gt 0)
    $case['ExplorerPidsAfterStop']=@((ProcessList 'explorer')|ForEach-Object Id)
    $case['ExplorerPidContinuity']=(@($case['ExplorerPidsBefore']|Where-Object {$case['ExplorerPidsAfterStop'] -contains $_}).Count -gt 0)
    $case['NativeStartUIAAfterStop']=UIAStart
    Screenshot ('tap-cycle-'+$i+'-stopped')
    Publish ('CYCLE_'+$i+'_STOPPED')
    Check $case['MenuStopped'] ('StartMenu.exe did not exit cycle '+$i)
    Check $case['ExplorerAfterStop'] ('Explorer died on shutdown cycle '+$i)
    Check $case['ExplorerPidContinuity'] ('Explorer process restarted during cycle '+$i)
    $case['Passed']=$true
    Publish ('CYCLE_'+$i+'_PASSED')
  }
  Publish 'FOUR_LIFECYCLES_COMPLETE'
  # Compare the rendered Windows 11 taskbar in native x64 screenshots.
  # The disabled cycle must remain unchanged; each enabled cycle must replace
  # the native Start glyph and restore the same baseline when stopped.
  Publish 'VERIFYING_VISUAL_START_BUTTON_RESTORATION'
  Add-Type -ReferencedAssemblies 'System.Drawing' -TypeDefinition @'
using System;
using System.Drawing;
public static class TAPTaskbarVisualDiff {
  public static int CountChanged(string aPath, string bPath, int threshold) {
    using (Bitmap a = new Bitmap(aPath))
    using (Bitmap b = new Bitmap(bPath)) {
      if (a.Width != b.Width || a.Height != b.Height)
        throw new InvalidOperationException("Screenshot dimensions changed");
      int count = 0;
      int x0 = a.Width / 5, x1 = a.Width * 4 / 5;
      int y0 = Math.Max(0, a.Height - 58);
      for (int y = y0; y < a.Height; ++y) {
        for (int x = x0; x < x1; ++x) {
          Color ca = a.GetPixel(x, y), cb = b.GetPixel(x, y);
          if (Math.Abs(ca.R-cb.R) > threshold ||
              Math.Abs(ca.G-cb.G) > threshold ||
              Math.Abs(ca.B-cb.B) > threshold) ++count;
        }
      }
      return count;
    }
  }
}
'@
  $baseline='C:\OEM\tap-baseline.png'
  Check (Test-Path -LiteralPath $baseline) 'Taskbar baseline screenshot missing'
  foreach($case in $cycles) {
    $number=$case['Iteration']
    $active='C:\OEM\tap-cycle-'+$number+'-active.png'
    $stopped='C:\OEM\tap-cycle-'+$number+'-stopped.png'
    Check ((Test-Path -LiteralPath $active) -and (Test-Path -LiteralPath $stopped)) ('Missing cycle screenshots '+$number)
    $activeDiff=[TAPTaskbarVisualDiff]::CountChanged($active,$stopped,25)
    $restoredDiff=[TAPTaskbarVisualDiff]::CountChanged($baseline,$stopped,25)
    $case['TaskbarActiveVsStoppedChangedPixels']=$activeDiff
    $case['TaskbarStoppedVsBaselineChangedPixels']=$restoredDiff
    Publish ('VISUAL_CYCLE_'+$number+'_MEASURED')
    if($case['EnableStartButton'] -eq 1) {
      Check ($activeDiff -ge 350) ('Enabled Start button was not visibly replaced: cycle '+$number)
    } else {
      Check ($activeDiff -le 60) ('Disabled cycle unexpectedly changed Start button: cycle '+$number)
    }
    Check ($restoredDiff -le 60) ('Taskbar Start glyph did not visually restore after cycle '+$number)
  }
  Publish 'VISUAL_START_BUTTON_RESTORATION_PASSED'
  $log=Join-Path $env:LOCALAPPDATA 'OpenShell\StartupLog.txt'
  $state['StartupLogPresent']=(Test-Path -LiteralPath $log)
  Publish 'STARTUP_LOG_LOCATED'
  Check $state['StartupLogPresent'] 'Startup log not found; cannot prove XAML connected'
  $lines=@(Get-Content -LiteralPath $log -Encoding Unicode -ErrorAction Stop)
  $state['XamlConnectedLogLines']=@($lines|Where-Object {$_ -match 'Win11StartButton: connected using'})
  $state['XamlFailureLogLines']=@($lines|Where-Object {$_ -match 'Win11StartButton: connection failed|Win11StartButtonTap: deactivate failed|Win11StartButtonTap: synchronous restore failed|Win11StartButtonTap: visual tree unadvise failed'})
  $state['XamlConnectedCount']=$state['XamlConnectedLogLines'].Count
  $state['XamlFailureCount']=$state['XamlFailureLogLines'].Count
  Publish 'XAML_LOG_READ'
  Check ($state['XamlConnectedCount'] -ge 3) 'Expected three successful XAML diagnostics connections'
  Check ($state['XamlFailureCount'] -eq 0) 'XAML TAP startup, restore or Unadvise reported failure'
  Publish 'XAML_CONNECTION_AND_LOGS_VERIFIED'
  # Event log queries are a separate bounded subprocess. Never leave the whole
  # Windows VM run waiting indefinitely inside Get-WinEvent.
  $audit=Start-Job -ScriptBlock {
    param($since)
    $ErrorActionPreference='Stop'
    @(Get-WinEvent -FilterHashtable @{
      LogName='Application'
      StartTime=$since
      Id=1000,1001,1002
    } -ErrorAction SilentlyContinue |
      Where-Object {$_.Message -match 'explorer.exe|StartMenu.exe|StartMenuDLL|StartMenuHelper'} |
      ForEach-Object {@{Id=$_.Id;Time=$_.TimeCreated.ToString('o');Message=$_.Message}})
  } -ArgumentList $started
  $state['CrashAuditStarted']=(Get-Date).ToString('o')
  Publish 'CRASH_AUDIT_RUNNING'
  if (-not (Wait-Job -Job $audit -Timeout 30)) {
    $state['CrashAudit']='TIMED_OUT_30_SECONDS'
    Publish 'CRASH_AUDIT_TIMED_OUT'
    Stop-Job -Job $audit -ErrorAction SilentlyContinue
    Remove-Job -Job $audit -Force -ErrorAction SilentlyContinue
    throw 'Windows Application event-log audit timed out after 30 seconds'
  }
  $auditErrors=@()
  $events=@(Receive-Job -Job $audit -ErrorVariable auditErrors -ErrorAction SilentlyContinue)
  $auditState=$audit.State.ToString()
  Remove-Job -Job $audit -Force -ErrorAction SilentlyContinue
  $state['CrashAudit']=$auditState
  $state['CrashAuditErrors']=@($auditErrors|ForEach-Object {$_.ToString()})
  $state['CrashOrHangEvents']=$events
  Publish 'CRASH_AUDIT_COMPLETED'
  Check ($auditState -eq 'Completed' -and $auditErrors.Count -eq 0) 'Windows crash-audit job failed'
  Check ($events.Count -eq 0) 'Explorer or Open-Shell application crash/hang detected'
  $state['StartMenuRunning']=$true
  $state['ExplorerAliveAfterStart']=$true
  $state['ExplorerAliveAfterStop']=$true
  $state['OpenShellIntegration']='FOUR_X64_LIFECYCLE_CYCLES_AND_XAML_CONNECTION_VERIFIED'
  # Publish terminal result BEFORE copying large diagnostics over the guest share.
  # If diagnostic-copy blocks, the CI host still has a genuine terminal report.
  Publish 'TEST_COMPLETE' $true
  foreach($remote in @('Z:\','\\host.lan\Shared\')) {
    try {
      Copy-Item -LiteralPath $log -Destination ($remote+'openshell-startup.txt') -Force -ErrorAction Stop
      break
    } catch {}
  }
} catch {
  $state['Failure']=$_.Exception.ToString()
  $state['OpenShellIntegration']='FAILED_WITH_DETAIL'
  $state['ExplorerPIDsAtFailure']=@((ProcessList 'explorer')|ForEach-Object Id)
  $state['StartMenuPIDsAtFailure']=@((ProcessList 'StartMenu')|ForEach-Object Id)
  foreach($item in @(
    @{Local='C:\OEM\openshell-msi.log';Remote='openshell-msi.log'},
    @{Local=(Join-Path $env:LOCALAPPDATA 'OpenShell\StartupLog.txt');Remote='openshell-startup.txt'}
  )) {
    if(Test-Path $item.Local){
      try {
        $state[($item.Remote+'Tail')]=@(Get-Content -Path $item.Local -Tail 35 -ErrorAction Stop)
        foreach($root in @('Z:\','\\host.lan\Shared\')){
          try{Copy-Item -LiteralPath $item.Local -Destination ($root+$item.Remote) -Force -ErrorAction Stop;break}catch{}
        }
      } catch{$state['DiagnosticCopyError']=$_.Exception.Message}
    }
  }
  Publish 'TEST_FAILED' $true
  exit 1
}
