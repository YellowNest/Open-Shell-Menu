$ErrorActionPreference='Stop'
$state=[ordered]@{Stage='BOOT';Terminal=$false;Started=(Get-Date).ToString('o');Results=@()}
function Publish([string]$stage,[bool]$done=$false){
  $state['Stage']=$stage
  $state['Terminal']=$done
  $state['Updated']=(Get-Date).ToString('o')
  $json=$state|ConvertTo-Json -Depth 7
  Set-Content -LiteralPath 'C:\OEM\issue2564-win11-proof.json' -Value $json -Encoding UTF8
  foreach($dest in @('Z:\issue2564-win11-proof.json','\\host.lan\Shared\issue2564-win11-proof.json')){
    try { Set-Content -LiteralPath $dest -Value $json -Encoding UTF8 -ErrorAction Stop; break } catch {}
  }
}
function Check([bool]$ok,[string]$what){if(-not $ok){throw $what}}
try{
  Publish 'WAITING_FOR_EXPLORER'
  $os=Get-CimInstance Win32_OperatingSystem
  $state['OS']=$os.Caption
  $state['Build']=$os.BuildNumber
  $state['OSArchitecture']=[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
  $state['CurrentSessionId']=[Diagnostics.Process]::GetCurrentProcess().SessionId
  Check ($os.Caption -match 'Windows 11' -and $state['OSArchitecture'] -eq 'X64') 'Windows 11 x64 guest not verified'
  $explorer=@()
  for($i=0;$i -lt 120;$i++){
    $explorer=@(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object {$_.SessionId -eq $state['CurrentSessionId']})
    if($explorer.Count -gt 0){break}
    Start-Sleep -Seconds 5
  }
  Check ($explorer.Count -gt 0) 'Explorer not running in the interactive desktop session'
  $state['ExplorerPidBefore']=$explorer[0].Id
  $state['Windows11X64Verified']=$true
  Publish 'DESKTOP_READY'
  $share=@('Z:\','\\host.lan\Shared\')|Where-Object {Test-Path ($_+'issue2564-link-test.exe')}|Select-Object -First 1
  Check ([bool]$share) 'Compiled test executable not visible in shared directory'
  $probe='C:\OEM\issue2564-link-test.exe'
  Copy-Item ($share+'issue2564-link-test.exe') $probe -Force
  $expected=(Get-Content ($share+'probe.sha256') -TotalCount 1).Trim().ToLowerInvariant()
  $actual=(Get-FileHash $probe -Algorithm SHA256).Hash.ToLowerInvariant()
  $state['ProbeSHA256']=$actual
  Check ($expected -match '^[0-9a-f]{64}$' -and $expected -eq $actual) 'Compiled Windows probe hash mismatch'
  Publish 'RUNNING_SHELL_LINK_TEST'
  $out='C:\OEM\issue2564-link-output.txt'
  $err='C:\OEM\issue2564-link-errors.txt'
  $process=Start-Process -FilePath $probe -ArgumentList @() -Wait -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
  $state['ProbeExitCode']=$process.ExitCode
  $state['Results']=@(Get-Content -LiteralPath $out -ErrorAction SilentlyContinue)
  $state['Stderr']=@(Get-Content -LiteralPath $err -ErrorAction SilentlyContinue)
  Check ($process.ExitCode -eq 0) ('Shell link probe failed: '+($state['Results'] -join ' | '))
  Check (($state['Results'] -contains 'ISSUE2564_LINK_TARGET_RESULT=PASS')) 'Windows Shell link target assertion absent'
  $after=@(Get-Process explorer -ErrorAction SilentlyContinue | Where-Object {$_.SessionId -eq $state['CurrentSessionId']})
  $state['ExplorerPidAfter']=if($after.Count -gt 0){$after[0].Id}else{0}
  Check ($after.Count -gt 0 -and $state['ExplorerPidBefore'] -eq $state['ExplorerPidAfter']) 'Explorer process restarted or exited during probe'
  Publish 'TEST_COMPLETE' $true
} catch {
  $state['Failure']=$_.Exception.ToString()
  Publish 'TEST_FAILED' $true
  exit 1
}
