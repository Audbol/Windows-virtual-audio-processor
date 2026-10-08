<#
.SYNOPSIS
    Installs the VocalBridge Virtual Mic driver.

.DESCRIPTION
    1. Checks that Windows test-signing mode is on (needed for a test-signed
       driver build; not needed once the driver is properly signed).
    2. Trusts the driver package's test certificate (if present).
    3. Creates the ROOT\VocalBridge device and installs VocalBridge.inf.

    Run from the release folder:  right-click > "Run with PowerShell",
    or: powershell -ExecutionPolicy Bypass -File .\install-driver.ps1
#>
param(
    [string]$PackageDir = (Join-Path $PSScriptRoot "driver")
)

$ErrorActionPreference = "Stop"

# --- self-elevate -------------------------------------------------------------
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell.exe -Verb RunAs -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"", "-PackageDir", "`"$PackageDir`"")
    exit
}

$inf = Join-Path $PackageDir "VocalBridge.inf"
$vbsetup = Join-Path $PSScriptRoot "vbsetup.exe"
if (-not (Test-Path $inf))     { throw "VocalBridge.inf not found in $PackageDir" }
if (-not (Test-Path $vbsetup)) { throw "vbsetup.exe not found next to this script" }

# --- test signing ---------------------------------------------------------------
$cert = Get-ChildItem -Path $PackageDir, $PSScriptRoot -Filter *.cer -ErrorAction SilentlyContinue | Select-Object -First 1
$testSigningOn = (bcdedit /enum "{current}" | Select-String -Pattern "testsigning\s+Yes") -ne $null

if ($cert -and -not $testSigningOn) {
    Write-Host ""
    Write-Host "This is a TEST-SIGNED driver build. Windows only loads it with test signing enabled." -ForegroundColor Yellow
    Write-Host "  * Secure Boot must be off for test signing to take effect."
    Write-Host "  * Some anti-cheat systems refuse to run while test signing is on."
    Write-Host "    (Alternative: use the app's 'Output device' mode with a signed virtual cable.)"
    $answer = Read-Host "Enable test signing now? A reboot is required afterwards. [y/N]"
    if ($answer -match '^[Yy]') {
        bcdedit /set testsigning on | Out-Null
        Write-Host "Test signing enabled. Reboot, then run this script again." -ForegroundColor Green
        Read-Host "Press Enter to exit"
        exit 0
    }
    Write-Host "Continuing without test signing - the device will show error 52 until it is enabled." -ForegroundColor Yellow
}

if ($cert) {
    Write-Host "Trusting test certificate $($cert.Name)..."
    Import-Certificate -FilePath $cert.FullName -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    Import-Certificate -FilePath $cert.FullName -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
}

# --- install ----------------------------------------------------------------------
& $vbsetup install $inf
$code = $LASTEXITCODE
& $vbsetup status

if ($code -eq 0) {
    Write-Host ""
    Write-Host "Done. 'Microphone (VocalBridge Virtual Mic)' should now be listed under Sound > Recording." -ForegroundColor Green
} else {
    Write-Host "Installation failed (exit code $code)." -ForegroundColor Red
}
Read-Host "Press Enter to exit"
exit $code
