param(
    [ValidateSet('Install','Remove')][string]$Mode='Install',
    [string]$SourceExe,
    [string]$UserSid,
    [switch]$ValidateOnly
)
$ErrorActionPreference='Stop'
if (-not $UserSid) { $UserSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value }
if ($UserSid -notmatch '^S-1-5-(\d+-)*\d+$') { throw 'A Windows user SID is required.' }
$targetSid=[Security.Principal.SecurityIdentifier]::new($UserSid)
$installDirectory=Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'CellBattery'
$cacheRoot=Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'CellBattery'
$cacheDirectory=Join-Path $cacheRoot ('History-'+$UserSid)
$helperPath=Join-Path $installDirectory 'CellHistory.exe'
$taskName='CellBatteryHistory-'+$UserSid
if (-not $SourceExe) { $SourceExe=Join-Path $PSScriptRoot '..\CellBattery.exe' }
$SourceExe=[IO.Path]::GetFullPath($SourceExe)
if ($Mode -eq 'Install') {
    if (-not (Test-Path -LiteralPath $SourceExe -PathType Leaf)) { throw 'Build CellBattery.exe before setting up hourly history.' }
    if ((Get-Item -LiteralPath $SourceExe).VersionInfo.ProductName -ne 'Cell') { throw 'The source executable is not Cell.' }
}
if ($ValidateOnly) {
    [pscustomobject]@{Mode=$Mode;Source=$SourceExe;ProtectedHelper=$helperPath;ProtectedCache=$cacheDirectory;TaskName=$taskName;Interval='PT1H';WakeToRun=$false;ResidentService=$false} | ConvertTo-Json
    return
}
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $powerShell=Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$PSCommandPath+'" -Mode '+$Mode+' -SourceExe "'+$SourceExe+'" -UserSid "'+$UserSid+'"'
    # This is the only elevation prompt. Hourly runs execute the protected native helper.
    $setup=Start-Process -FilePath $powerShell -ArgumentList $arguments -Verb RunAs -WindowStyle Hidden -PassThru
    $setup.WaitForExit()
    if ($setup.ExitCode -ne 0) { throw ('Hourly setup failed with exit code '+$setup.ExitCode+'.') }
    return
}
function Protect-CellDirectory([string]$Path,[bool]$AllowUserRead) {
    if (Test-Path -LiteralPath $Path) {
        if ((Get-Item -LiteralPath $Path -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw ('Refusing a redirected setup directory: '+$Path) }
    } else { New-Item -ItemType Directory -Path $Path | Out-Null }
    $acl=[Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true,$false)
    $administrators=[Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $system=[Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $acl.SetOwner($administrators)
    $inheritance=[Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit'
    $propagation=[Security.AccessControl.PropagationFlags]::None
    foreach ($account in @($administrators,$system)) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($account,[Security.AccessControl.FileSystemRights]::FullControl,$inheritance,$propagation,[Security.AccessControl.AccessControlType]::Allow))
    }
    if ($AllowUserRead) {
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($targetSid,[Security.AccessControl.FileSystemRights]::ReadAndExecute,$inheritance,$propagation,[Security.AccessControl.AccessControlType]::Allow))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}
$service=New-Object -ComObject Schedule.Service
$service.Connect()
$folder=$service.GetFolder('\')
if ($Mode -eq 'Remove') {
    $existing=@($folder.GetTasks(1) | Where-Object Name -eq $taskName)
    if ($existing.Count) { $folder.DeleteTask($taskName,0) }
    $marker=Join-Path $cacheDirectory 'hourly.enabled'
    if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker }
    return
}
Protect-CellDirectory $installDirectory $true
Protect-CellDirectory $cacheRoot $true
Protect-CellDirectory $cacheDirectory $true
if ((Test-Path -LiteralPath $helperPath) -and ((Get-Item -LiteralPath $helperPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'The helper destination is redirected.' }
Copy-Item -LiteralPath $SourceExe -Destination $helperPath -Force
$definition=$service.NewTask(0)
$definition.RegistrationInfo.Description='Refresh Cell battery app-energy history once an hour using Windows existing SRUM records. No resident service.'
$definition.Principal.UserId=$UserSid
$definition.Principal.LogonType=3 # InteractiveToken: no stored password; only while signed in.
$definition.Principal.RunLevel=1 # Highest: approved once at setup, not at each refresh.
$definition.Settings.Enabled=$true
$definition.Settings.Hidden=$true
$definition.Settings.AllowDemandStart=$true
$definition.Settings.DisallowStartIfOnBatteries=$false
$definition.Settings.StopIfGoingOnBatteries=$false
$definition.Settings.StartWhenAvailable=$true
$definition.Settings.WakeToRun=$false
$definition.Settings.MultipleInstances=2 # IgnoreNew
$definition.Settings.ExecutionTimeLimit='PT2M'
$definition.Settings.Priority=7
$trigger=$definition.Triggers.Create(1) # TimeTrigger, repeated indefinitely.
$trigger.StartBoundary=([DateTime]::Now.Date.AddHours([DateTime]::Now.Hour+1)).ToString('yyyy-MM-ddTHH:mm:ss')
$trigger.Enabled=$true
$trigger.Repetition.Interval='PT1H'
$trigger.Repetition.StopAtDurationEnd=$false
$action=$definition.Actions.Create(0)
$action.Path=$helperPath
$action.Arguments='--refresh-to "'+$cacheDirectory+'"'
$action.WorkingDirectory=$installDirectory
# Normal-user access permits reading/running this fixed task, not editing its elevated action.
$security='D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;'+$UserSid+')'
$registered=$folder.RegisterTaskDefinition($taskName,$definition,6,$UserSid,$null,3,$security)
if (-not $registered.Enabled -or $registered.Definition.Principal.RunLevel -ne 1 -or $registered.Definition.Triggers.Item(1).Repetition.Interval -ne 'PT1H') { throw 'Task registration verification failed.' }
[IO.File]::WriteAllText((Join-Path $cacheDirectory 'hourly.enabled'),'Cell hourly history is enabled. Managed by source/Setup-HourlyCache.ps1.')
$before=[DateTime]::UtcNow
$registered.Run($null) | Out-Null
$deadline=[DateTime]::UtcNow.AddSeconds(95)
$succeeded=$false
do {
    Start-Sleep -Seconds 2
    $cache=Get-Item -LiteralPath (Join-Path $cacheDirectory 'energy.bin') -ErrorAction SilentlyContinue
    if ($cache -and $cache.LastWriteTimeUtc -ge $before) { $succeeded=$true;break }
} while ([DateTime]::UtcNow -lt $deadline)
$result=[ordered]@{Enabled=$registered.Enabled;Interval='PT1H';SilentRefreshVerified=$succeeded;WakeToRun=$registered.Definition.Settings.WakeToRun;RunLevel=$registered.Definition.Principal.RunLevel;LastTaskResult=$registered.LastTaskResult;HelperVersion=(Get-Item -LiteralPath $helperPath).VersionInfo.FileVersion}
[IO.File]::WriteAllText((Join-Path $cacheDirectory 'setup-result.json'),($result | ConvertTo-Json))
if (-not $succeeded) { throw 'The task is registered, but its first cache export did not complete. Inspect refresh.status and setup-result.json in the protected cache directory.' }
