# Keeps an archive.org account's S3 keys where `python -m rexnative dlc` (and the internetarchive
# tool) read them: %USERPROFILE%\.config\internetarchive\ia.ini, section [s3]. For an account that
# signs in with Google, which has no password for `ia configure`: this opens
# https://archive.org/account/s3.php in the browser (signed in there, the page shows the account's
# access key and secret key), then asks for the two keys here. They are typed into this window
# only, never shown or logged; the rest of an existing ia.ini (cookies, settings) is kept.
#
#   powershell -ExecutionPolicy Bypass -File scripts\ia-keys.ps1
$ErrorActionPreference = 'Stop'
$dir = Join-Path $env:USERPROFILE '.config\internetarchive'
$ini = Join-Path $dir 'ia.ini'

Write-Host 'Opening https://archive.org/account/s3.php in your browser: sign in there (Google is fine),'
Write-Host 'then copy the two keys the page shows into this window (right-click pastes).'
Start-Process 'https://archive.org/account/s3.php'
$access = (Read-Host 'Access key').Trim()
$secure = Read-Host 'Secret key' -AsSecureString
$bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try { $secret = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr).Trim() }
finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr) }
if (-not $access -or -not $secret) { throw 'Both keys are needed.' }

# The file without its old [s3] section, then the new one.
$kept = @()
if (Test-Path $ini) {
    $skip = $false
    foreach ($line in Get-Content $ini) {
        if ($line -match '^\s*\[(.+)\]\s*$') { $skip = ($Matches[1].Trim() -eq 's3') }
        if (-not $skip) { $kept += $line }
    }
}
New-Item -ItemType Directory -Force $dir | Out-Null
Set-Content -Path $ini -Value ($kept + @('[s3]', "access = $access", "secret = $secret")) -Encoding ascii
Remove-Variable secret, access

# The internetarchive tool checks them with archive.org when it is installed.
if (Get-Command ia -ErrorAction SilentlyContinue) {
    & ia configure --check
    if ($LASTEXITCODE -eq 0) { Write-Host "Keys saved in $ini and accepted by archive.org." -ForegroundColor Green }
    else { Write-Host "Keys saved in $ini, but archive.org did not accept them: run this again." -ForegroundColor Yellow }
} else {
    Write-Host "Keys saved in $ini."
}
