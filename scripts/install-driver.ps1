<#
.SYNOPSIS
    Installs the VocalBridge Virtual Mic driver.

.DESCRIPTION
    1. If the package is Microsoft attestation-signed: installs directly (Secure Boot on is fine).
    2. If it is a test-signed build: needs Windows test mode, which Secure Boot blocks. With
       Secure Boot on it stops and points you to the no-driver option (a signed virtual
       cable such as VB-CABLE, used automatically by VocalBridge's "Auto" mode).
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

# --- signature / Secure Boot ------------------------------------------------------
$catalog = Get-ChildItem -Path $PackageDir -Filter *.cat | Select-Object -First 1
$catSigner = if ($catalog) { (Get-AuthenticodeSignature $catalog.FullName).SignerCertificate.Subject } else { "" }
$microsoftSigned = $catSigner -match "Microsoft Windows Hardware Compatibility Publisher"

$secureBoot = $false
try { $secureBoot = [bool](Confirm-SecureBootUEFI) } catch { }   # throws on legacy BIOS

$testSigningOn = (bcdedit /enum "{current}" | Select-String -Pattern "testsigning\s+Yes") -ne $null
$cert = Get-ChildItem -Path $PackageDir, $PSScriptRoot -Filter *.cer -ErrorAction SilentlyContinue | Select-Object -First 1

if ($microsoftSigned) {
    Write-Host "Driver is Microsoft attestation-signed: no test mode needed, Secure Boot can stay on." -ForegroundColor Green
}
elseif (-not $testSigningOn) {
    Write-Host ""
    Write-Host "This is a TEST-SIGNED driver build. Windows only loads it in test mode." -ForegroundColor Yellow
    if ($secureBoot) {
        Write-Host "Secure Boot is ON, so test mode cannot be enabled (and you don't need to):" -ForegroundColor Yellow
        Write-Host "  * Use VocalBridge without this driver: install a Microsoft-signed virtual cable such as"
        Write-Host "    VB-CABLE (https://vb-audio.com/Cable/). VocalBridge's 'Auto' virtual-mic mode picks it up"
        Write-Host "    automatically - it works with Secure Boot and anti-cheat."
        Write-Host "  * Or install an attestation-signed build of this driver (see README > Driver signing)."
        Read-Host "Press Enter to exit"
        exit 1
    }
    Write-Host "  * Some anti-cheat systems refuse to run while test mode is on."
    Write-Host "  * Alternative with no driver: a signed virtual cable such as VB-CABLE (VocalBridge 'Auto' mode)."
    $answer = Read-Host "Enable test mode now? A reboot is required afterwards. [y/N]"
    if ($answer -match '^[Yy]') {
        bcdedit /set testsigning on | Out-Null
        Write-Host "Test mode enabled. Reboot, then run this script again." -ForegroundColor Green
        Read-Host "Press Enter to exit"
        exit 0
    }
    Write-Host "Continuing without test mode - the device will show error 52 until it is enabled." -ForegroundColor Yellow
}

if ($cert -and -not $microsoftSigned) {
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
