<#
.SYNOPSIS
    Packs the VocalBridge driver into the CAB that Microsoft attestation signing expects,
    and optionally signs it with your EV code-signing certificate.

.DESCRIPTION
    Attestation signing is how a driver gets a Microsoft signature that loads on any
    Windows 10/11 PC with Secure Boot ON and no test mode. You submit an EV-signed CAB
    containing the INF + SYS (+ PDB) through Partner Center; Microsoft returns the same
    files plus a Microsoft-signed catalog.

    Build the driver UNSIGNED first (no test signature on the .sys):
        msbuild driver\VocalBridgeDriver.sln /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off

.PARAMETER DriverDir
    Folder containing VocalBridge.inf and VocalBridge.sys (VocalBridge.pdb optional).

.PARAMETER OutDir
    Where VocalBridge.cab is written.

.PARAMETER CertThumbprint
    Optional: SHA-1 thumbprint of your EV certificate (e.g. on a USB token / in the
    certificate store). If given, the CAB is signed with signtool.

.EXAMPLE
    .\make-attestation-cab.ps1 -DriverDir ..\..\driver\Source\Main\x64\Release -CertThumbprint 0123ABCD...
#>
param(
    [Parameter(Mandatory = $true)] [string] $DriverDir,
    [string] $OutDir = (Join-Path $PSScriptRoot "out"),
    [string] $CertThumbprint = "",
    [string] $TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

$DriverDir = (Resolve-Path $DriverDir).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$files = @("VocalBridge.inf", "VocalBridge.sys")
foreach ($f in $files) {
    if (-not (Test-Path (Join-Path $DriverDir $f))) { throw "$f not found in $DriverDir" }
}
if (Test-Path (Join-Path $DriverDir "VocalBridge.pdb")) { $files += "VocalBridge.pdb" }

# A test-signed .sys can't go to Microsoft; it must be built with /p:SignMode=Off.
$sig = Get-AuthenticodeSignature (Join-Path $DriverDir "VocalBridge.sys")
if ($sig.Status -ne "NotSigned") {
    Write-Warning "VocalBridge.sys already carries a signature ($($sig.SignerCertificate.Subject)). Rebuild with /p:SignMode=Off for attestation."
}

$ddf = Join-Path $OutDir "VocalBridge.ddf"
$lines = @(
    ".OPTION EXPLICIT",
    ".Set CabinetFileCountThreshold=0",
    ".Set FolderFileCountThreshold=0",
    ".Set FolderSizeThreshold=0",
    ".Set MaxCabinetSize=0",
    ".Set MaxDiskFileCount=0",
    ".Set MaxDiskSize=0",
    ".Set CompressionType=MSZIP",
    ".Set Cabinet=on",
    ".Set Compress=on",
    ".Set CabinetNameTemplate=VocalBridge.cab",
    ".Set DiskDirectoryTemplate=`"$OutDir`"",
    ".Set DestinationDir=VocalBridge"
)
foreach ($f in $files) { $lines += "`"$(Join-Path $DriverDir $f)`"" }
$lines | Set-Content -Encoding ASCII $ddf

Push-Location $OutDir
try {
    makecab /f $ddf | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "makecab failed" }
} finally {
    Pop-Location
    Remove-Item -ErrorAction SilentlyContinue (Join-Path $OutDir "setup.inf"), (Join-Path $OutDir "setup.rpt")
}

$cab = Join-Path $OutDir "VocalBridge.cab"
Write-Host "Created $cab"

if ($CertThumbprint) {
    signtool sign /fd sha256 /tr $TimestampUrl /td sha256 /sha1 $CertThumbprint $cab
    if ($LASTEXITCODE -ne 0) { throw "signtool failed" }
    Write-Host "Signed $cab with certificate $CertThumbprint"
} else {
    Write-Host "Next: sign it with your EV certificate, e.g."
    Write-Host "  signtool sign /fd sha256 /tr $TimestampUrl /td sha256 /sha1 <EV-thumbprint> `"$cab`""
}
