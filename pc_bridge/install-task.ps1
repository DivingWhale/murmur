param(
    [Parameter(Mandatory)][string]$Config,
    [string]$Python = (Get-Command python.exe).Source
)
$ErrorActionPreference = 'Stop'
$configPath = (Resolve-Path -LiteralPath $Config).Path
$repo = Split-Path -Parent $PSScriptRoot
$pythonw = Join-Path (Split-Path -Parent $Python) 'pythonw.exe'
$action = New-ScheduledTaskAction -Execute $pythonw -WorkingDirectory $repo -Argument ('-m pc_bridge --daemon --config "{0}"' -f $configPath)
$user = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -RestartCount 5 -RestartInterval (New-TimeSpan -Minutes 2) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName 'Murmur Obsidian Sync' -User $user -Action $action -Trigger $trigger -Settings $settings
