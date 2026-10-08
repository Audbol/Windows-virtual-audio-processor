<#
.SYNOPSIS
    Submits an EV-signed VocalBridge.cab for Microsoft attestation signing and downloads
    the Microsoft-signed driver package. Automates what you would otherwise click through
    in Partner Center (Hardware > Submit new hardware).

.DESCRIPTION
    Uses Microsoft's Surface Dev Center Manager (SDCM, https://github.com/microsoft/SDCM,
    MIT) which wraps the Partner Center Hardware Submission API. Flow adapted from its
    Scripts\Attestation.ps1.

    Credentials (an Entra ID app registered in your Partner Center account, see
    "Associate an Azure AD application with your Partner Center account") are read from:
        SDCM_CREDS_TENANTID, SDCM_CREDS_CLIENTID, SDCM_CREDS_KEY

.PARAMETER Sdcm
    Path to sdcm.exe.

.PARAMETER CabPath
    The EV-signed CAB from make-attestation-cab.ps1.

.PARAMETER Signatures
    Partner Center OS signature codes to request. Check the "Requested signatures"
    list in Partner Center for the current codes; the default targets Windows 11 x64.

.PARAMETER OutDir
    Where the signed package (VocalBridge.inf/.sys/.cat signed by Microsoft) is extracted.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Sdcm,
    [Parameter(Mandatory = $true)] [string] $CabPath,
    [string]   $ProductName = "VocalBridge Virtual Mic",
    [string[]] $Signatures = @("WINDOWS_v100_X64_CO_FULL"),
    [string]   $OutDir = (Join-Path $PSScriptRoot "signed")
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

foreach ($v in "SDCM_CREDS_TENANTID", "SDCM_CREDS_CLIENTID", "SDCM_CREDS_KEY") {
    if (-not [Environment]::GetEnvironmentVariable($v)) { throw "Environment variable $v is not set" }
}
$env:SDCM_CREDS_URL = "https://manage.devcenter.microsoft.com"
$env:SDCM_CREDS_URLPREFIX = "v2.0/my"

$CabPath = (Resolve-Path $CabPath).Path
if ((Get-AuthenticodeSignature $CabPath).Status -ne "Valid") {
    throw "$CabPath is not signed (or the signature is not trusted). Sign it with your EV certificate first."
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("vb-attest-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null

function Invoke-Sdcm([string[]] $SdcmArgs) {
    $output = & $Sdcm -creds ClientCredentials @SdcmArgs 2>&1 | Out-String
    Write-Host $output
    if ($LASTEXITCODE -ne 0) { throw "sdcm $($SdcmArgs -join ' ') failed with exit code $LASTEXITCODE" }
    return $output
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"

# --- product -------------------------------------------------------------------------
$product = @{
    createType    = "product"
    createProduct = @{
        productName          = "$ProductName $stamp"
        testHarness          = "Attestation"
        announcementDate     = (Get-Date).ToString("s")
        deviceMetadataIds    = $null
        firmwareVersion      = "0"
        deviceType           = "external"
        isTestSign           = $false
        isFlightSign         = $false
        marketingNames       = $null
        selectedProductTypes = @{ "windows_v100_RS4" = "Unclassified" }
        requestedSignatures  = $Signatures
        additionalAttributes = $null
    }
}
$productJson = Join-Path $work "product.json"
$product | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII $productJson

Write-Host "> Creating product"
$out = Invoke-Sdcm @("-create", $productJson)
if ($out -notmatch "--- Product: (\d+)") { throw "Could not find the product ID in SDCM output" }
$productId = $Matches[1]

# --- submission ------------------------------------------------------------------------
$submission = @{ createType = "submission"; createSubmission = @{ name = "VocalBridge $stamp"; type = "initial" } }
$submissionJson = Join-Path $work "submission.json"
$submission | ConvertTo-Json -Depth 5 | Set-Content -Encoding ASCII $submissionJson

Write-Host "> Creating submission"
$out = Invoke-Sdcm @("-create", $submissionJson, "-productid", $productId)
if ($out -notmatch "---- Submission: (\d+)") { throw "Could not find the submission ID in SDCM output" }
$submissionId = $Matches[1]

Write-Host "  Partner Center: https://partner.microsoft.com/dashboard/hardware/driver/$productId"

Write-Host "> Uploading $CabPath"
Invoke-Sdcm @("-upload", $CabPath, "-productid", $productId, "-submissionid", $submissionId) | Out-Null

Write-Host "> Committing"
Invoke-Sdcm @("-commit", "-productid", $productId, "-submissionid", $submissionId) | Out-Null

Write-Host "> Waiting for Microsoft to sign (usually 5-30 minutes)"
Invoke-Sdcm @("-wait", "-productid", $productId, "-submissionid", $submissionId) | Out-Null

$zip = Join-Path $work "signed.zip"
Write-Host "> Downloading signed package"
Invoke-Sdcm @("-download", $zip, "-productid", $productId, "-submissionid", $submissionId) | Out-Null

# --- extract the driver files ------------------------------------------------------------
$extract = Join-Path $work "signed"
Expand-Archive $zip -DestinationPath $extract -Force
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$inf = Get-ChildItem $extract -Recurse -Filter "VocalBridge.inf" | Select-Object -First 1
if (-not $inf) { throw "VocalBridge.inf not found in the signed package" }
Copy-Item (Join-Path $inf.DirectoryName "*") $OutDir -Recurse -Force

$cat = Get-ChildItem $OutDir -Filter *.cat | Select-Object -First 1
$signer = (Get-AuthenticodeSignature $cat.FullName).SignerCertificate.Subject
Write-Host ""
Write-Host "Signed package in $OutDir (catalog signer: $signer)" -ForegroundColor Green
