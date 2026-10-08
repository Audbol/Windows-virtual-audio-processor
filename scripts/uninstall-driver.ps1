<#
.SYNOPSIS
    Removes the VocalBridge Virtual Mic device and its driver package.
#>
$ErrorActionPreference = "Continue"

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    exit
}

$vbsetup = Join-Path $PSScriptRoot "vbsetup.exe"
& $vbsetup uninstall

# Remove the package from the driver store so a reinstall starts clean.
Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like "*\vocalbridge.inf" } | ForEach-Object {
    Write-Host "Removing driver package $($_.Driver)..."
    pnputil /delete-driver $_.Driver /uninstall /force | Out-Null
}

Write-Host ""
Write-Host "VocalBridge driver removed. (Test signing, if you enabled it, is left on:" -ForegroundColor Green
Write-Host "  run 'bcdedit /set testsigning off' as admin and reboot to turn it off.)"
Read-Host "Press Enter to exit"
