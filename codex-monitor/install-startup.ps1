$ErrorActionPreference='Stop'
$monitor=Join-Path $PSScriptRoot 'monitor.py'
$python=(Get-Command python).Source
$pythonw=Join-Path (Split-Path $python) 'pythonw.exe'
if(!(Test-Path -LiteralPath $pythonw)){throw 'pythonw.exe is required for the hidden monitor.'}
$startup=[Environment]::GetFolderPath('Startup')
$shell=New-Object -ComObject WScript.Shell
$shortcut=$shell.CreateShortcut((Join-Path $startup 'RoundClock Codex Monitor.lnk'))
$shortcut.TargetPath=$pythonw
$shortcut.Arguments='"'+$monitor+'"'
$shortcut.WorkingDirectory=$PSScriptRoot
$shortcut.WindowStyle=7
$shortcut.Save()
Start-Process -FilePath $pythonw -ArgumentList ('"'+$monitor+'"') -WorkingDirectory $PSScriptRoot -WindowStyle Hidden
Write-Output 'RoundClock Codex Monitor installed in this user Startup folder and launched hidden.'
