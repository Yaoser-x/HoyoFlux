[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$PackageDirectory,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'

function Assert-Release([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-ToolSucceeded([int]$ExitCode, [string]$Output, [string]$Name) {
    Assert-Release ($ExitCode -eq 0) "$Name failed with exit code ${ExitCode}: $Output"
    Assert-Release (-not [string]::IsNullOrWhiteSpace($Output)) "$Name returned no output"
}

function Test-ResourceText([string]$Text) {
    Assert-Release (-not [string]::IsNullOrWhiteSpace($Text)) 'resource tool returned no output'
    Assert-Release ($Text -match '(?m)^\s*Type: ICON \(ID 3\)') 'EXE does not contain ICON resources'
    Assert-Release ($Text -match '(?m)^\s*Type: GROUP_ICON \(ID 14\)') 'EXE does not contain a GROUP_ICON resource'
    $iconBlock = [regex]::Match($Text, '(?s)Type: ICON \(ID 3\).*?(?=\s*Type: GROUP_ICON \(ID 14\))').Value
    Assert-Release (([regex]::Matches($iconBlock, '(?m)^\s*Name: \(ID')).Count -eq 7) 'EXE must contain seven ICON images'
}

function Get-EmbeddedManifestText([string]$Text) {
    $marker = 'Type: MANIFEST (ID 24)'
    $resourceStart = $Text.IndexOf($marker, [StringComparison]::Ordinal)
    Assert-Release ($resourceStart -ge 0) 'EXE does not contain a manifest resource'
    $manifestBlock = $Text.Substring($resourceStart)
    $dataStart = $manifestBlock.IndexOf('Data (', [StringComparison]::Ordinal)
    Assert-Release ($dataStart -ge 0) 'manifest resource data is missing'
    $dataEndMatch = [regex]::Match($manifestBlock.Substring($dataStart), '(?m)^\s*\)\s*$')
    Assert-Release $dataEndMatch.Success 'manifest resource data is truncated'
    $dataText = $manifestBlock.Substring($dataStart, $dataEndMatch.Index)
    $hex = [Text.StringBuilder]::new()
    foreach ($line in ($dataText -split "`r?`n")) {
        $lineMatch = [regex]::Match($line, '^\s*[0-9A-Fa-f]{4}:\s*([0-9A-Fa-f ]+?)\s+\|')
        if ($lineMatch.Success) {
            [void]$hex.Append(($lineMatch.Groups[1].Value -replace '\s', ''))
        }
    }
    Assert-Release ($hex.Length -gt 0 -and $hex.Length % 2 -eq 0) 'manifest resource hex data is invalid'
    $bytes = [byte[]]::new($hex.Length / 2)
    for ($index = 0; $index -lt $hex.Length; $index += 2) {
        $bytes[$index / 2] = [Convert]::ToByte($hex.ToString($index, 2), 16)
    }
    return [Text.Encoding]::UTF8.GetString($bytes)
}

function Get-IcoSizes([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    Assert-Release ($bytes.Length -ge 6) 'ICO is truncated'
    $count = [BitConverter]::ToUInt16($bytes, 4)
    Assert-Release ($bytes.Length -ge 6 + 16 * $count) 'ICO directory is truncated'
    $sizes = @()
    for ($i = 0; $i -lt $count; ++$i) {
        $width = $bytes[6 + 16 * $i]
        $height = $bytes[7 + 16 * $i]
        if ($width -eq 0) { $width = 256 }
        if ($height -eq 0) { $height = 256 }
        Assert-Release ($width -eq $height) 'ICO contains a non-square application icon'
        $sizes += [int]$width
    }
    return @($sizes | Sort-Object -Unique)
}

function Get-PeVersionString([string]$Path, [string]$Key) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    $needle = [Text.Encoding]::Unicode.GetBytes($Key + [char]0)
    for ($offset = 0; $offset -le $bytes.Length - $needle.Length; ++$offset) {
        $matched = $true
        for ($index = 0; $index -lt $needle.Length; ++$index) {
            if ($bytes[$offset + $index] -ne $needle[$index]) { $matched = $false; break }
        }
        if (-not $matched) { continue }
        $start = $offset + $needle.Length
        # VERSIONINFO values are word-aligned; FileVersion may have one
        # padding UTF-16 NUL after its key while ProductVersion does not.
        while ($start + 1 -lt $bytes.Length -and
               [BitConverter]::ToUInt16($bytes, $start) -eq 0) { $start += 2 }
        $characters = [Collections.Generic.List[char]]::new()
        for ($cursor = $start; $cursor + 1 -lt $bytes.Length; $cursor += 2) {
            $code = [BitConverter]::ToUInt16($bytes, $cursor)
            if ($code -eq 0) { break }
            if ($code -lt 0x20 -or $code -gt 0x7e) { $characters.Clear(); break }
            $characters.Add([char]$code)
        }
        if ($characters.Count -gt 0) { return -join $characters }
    }
    return ''
}

function Test-Imports([string]$Text) {
    Assert-Release (-not [string]::IsNullOrWhiteSpace($Text)) 'import tool returned no output'
    $imports = @([regex]::Matches($Text, '(?im)^\s*DLL Name:\s*([^\r\n]+)$') | ForEach-Object {
        $_.Groups[1].Value.Trim().ToLowerInvariant()
    })
    Assert-Release ($imports.Count -gt 0) 'EXE import table is empty or could not be parsed'
    $allowed = @('kernel32.dll', 'user32.dll', 'shell32.dll', 'ole32.dll',
        'advapi32.dll', 'comctl32.dll', 'bcrypt.dll')
    foreach ($import in $imports) {
        if ($allowed -contains $import -or $import.StartsWith('api-ms-win-') -or
            $import.StartsWith('ext-ms-win-')) { continue }
        throw "portable EXE imports a non-system dependency '$import'"
    }
}

function Test-DocumentationLinks([string]$Package) {
    $documents = @(Get-ChildItem -LiteralPath $Package -File -Recurse -Filter *.md)
    foreach ($document in $documents) {
        $content = Get-Content -LiteralPath $document.FullName -Raw
        $matches = [regex]::Matches(
            $content,
            '\]\(([^)]+)\)|<img\s+[^>]*\bsrc="([^"]+)"',
            [Text.RegularExpressions.RegexOptions]::IgnoreCase
        )
        foreach ($match in $matches) {
            $reference = if ($match.Groups[1].Success) {
                $match.Groups[1].Value
            } else {
                $match.Groups[2].Value
            }
            if ($reference -match '^(?:https?:|mailto:|#)') { continue }
            $reference = ($reference -split '#', 2)[0]
            if ([string]::IsNullOrWhiteSpace($reference)) { continue }
            $target = Join-Path $document.DirectoryName ($reference -replace '/', '\\')
            Assert-Release (Test-Path -LiteralPath $target) (
                "package documentation link is missing: $($document.Name) -> $reference"
            )
        }
    }
}

function Invoke-ReleaseAuditSelfTest {
    $valid = @"
Type: ICON (ID 3) [
Name: (ID 1) [
Name: (ID 2) [
Name: (ID 3) [
Name: (ID 4) [
Name: (ID 5) [
Name: (ID 6) [
Name: (ID 7) [
Type: GROUP_ICON (ID 14) [
"@
    Test-ResourceText $valid
    foreach ($invalid in @('', $valid -replace 'GROUP_ICON', 'GROUP_MISSING')) {
        $failed = $false
        try { Test-ResourceText $invalid } catch { $failed = $true }
        Assert-Release $failed 'resource audit self-test accepted invalid output'
    }
    $failed = $false
    try { Test-Imports "`nDLL Name: custom-runtime.dll" } catch { $failed = $true }
    Assert-Release $failed 'import audit self-test accepted a non-system DLL'
    $failed = $false
    try { Test-Imports '' } catch { $failed = $true }
    Assert-Release $failed 'import audit self-test accepted empty tool output'
    $failed = $false
    try { Assert-ToolSucceeded 1 'synthetic failure' 'synthetic tool' } catch { $failed = $true }
    Assert-Release $failed 'release audit self-test accepted a failed tool invocation'
    'release audit self-test passed'
}

if ($SelfTest) {
    Invoke-ReleaseAuditSelfTest
    exit 0
}

$exePath = (Resolve-Path -LiteralPath $Exe).Path
Assert-Release ([IO.Path]::GetFileName($exePath) -eq 'hoyoflux.exe') 'expected hoyoflux.exe'
$root = Split-Path -Parent $PSScriptRoot
$readobj = (Get-Command llvm-readobj.exe -ErrorAction Stop).Source
$objdump = (Get-Command llvm-objdump.exe -ErrorAction Stop).Source

$resources = @(& $readobj --coff-resources $exePath 2>&1)
$resourceText = $resources -join "`n"
Assert-ToolSucceeded $LASTEXITCODE $resourceText 'llvm-readobj resource inspection'
Test-ResourceText $resourceText

$headers = @(& $readobj --file-headers $exePath 2>&1)
$headerText = $headers -join "`n"
Assert-ToolSucceeded $LASTEXITCODE $headerText 'llvm-readobj header inspection'
Assert-Release ($headerText -match 'IMAGE_SUBSYSTEM_WINDOWS_GUI') 'EXE is not linked as a Windows GUI program'
Assert-Release (($resourceText -split 'Type: MANIFEST \(ID 24\)').Count -eq 2) 'EXE must contain exactly one manifest resource'
$embeddedManifest = Get-EmbeddedManifestText $resourceText
Assert-Release ($embeddedManifest -match 'level="asInvoker"') 'EXE manifest must remain asInvoker'

$imports = @(& $objdump -p $exePath 2>&1)
$importText = $imports -join "`n"
Assert-ToolSucceeded $LASTEXITCODE $importText 'llvm-objdump import inspection'
Test-Imports $importText

$icoSizes = Get-IcoSizes (Join-Path $root 'assets/hoyoflux.ico')
$expectedSizes = @(16, 24, 32, 48, 64, 128, 256)
Assert-Release (@(Compare-Object $expectedSizes $icoSizes).Count -eq 0) 'ICO must contain exactly 16, 24, 32, 48, 64, 128, and 256 px images'

$versionHeader = Get-Content (Join-Path $root 'build/release/generated/version.hpp') -Raw
$configuredVersion = [regex]::Match($versionHeader, '(?m)^#define HOYOFLUX_VERSION_STRING "([^"]*)"\s*$').Groups[1].Value
Assert-Release (-not [string]::IsNullOrWhiteSpace($configuredVersion)) 'generated version header is missing HOYOFLUX_VERSION_STRING'
$fileVersion = Get-PeVersionString $exePath 'FileVersion'
$productVersion = Get-PeVersionString $exePath 'ProductVersion'
Assert-Release ($fileVersion -eq $configuredVersion) 'PE FileVersion differs from CMake version source'
Assert-Release ($productVersion -eq $configuredVersion) 'PE ProductVersion differs from CMake version source'
$manifestVersion = [regex]::Match($embeddedManifest, '<assemblyIdentity version="([0-9.]+)"').Groups[1].Value
Assert-Release ($manifestVersion -eq "$configuredVersion.0") 'application manifest identity differs from CMake version source'

if ($PackageDirectory) {
    $package = [IO.Path]::GetFullPath($PackageDirectory)
    if (Test-Path -LiteralPath $package) { Remove-Item -LiteralPath $package -Recurse -Force }
    New-Item -ItemType Directory -Path $package | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $package 'assets') | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $package 'docs') | Out-Null
    Copy-Item -LiteralPath $exePath -Destination (Join-Path $package 'hoyoflux.exe')
    Copy-Item -LiteralPath (Join-Path $root 'README.md'), (Join-Path $root 'CHANGELOG.md'),
        (Join-Path $root 'LICENSE'),
        (Join-Path $root 'THIRD_PARTY_NOTICES.md'), (Join-Path $root 'config.example.toml') -Destination $package
    Copy-Item -LiteralPath (Join-Path $root 'docs/compatibility-matrix.md'),
        (Join-Path $root 'docs/persistent-state-experiment.md') -Destination (Join-Path $package 'docs')
    Copy-Item -LiteralPath (Join-Path $root 'assets/hoyoflux-logo.svg'), (Join-Path $root 'assets/hoyoflux-logo.png'),
        (Join-Path $root 'assets/hoyoflux.ico') -Destination (Join-Path $package 'assets')
    $files = @(Get-ChildItem -LiteralPath $package -File -Recurse)
    Assert-Release (($files | Where-Object Extension -eq '.exe').Count -eq 1) 'package must contain exactly one EXE'
    Assert-Release (($files | Where-Object Extension -eq '.dll').Count -eq 0) 'package must not contain runtime DLLs'
    Assert-Release (-not (Test-Path (Join-Path $package 'config.toml'))) 'package must not contain a personal config.toml'
    Assert-Release (-not (Test-Path (Join-Path $package 'data'))) 'package must not contain runtime data'
    Test-DocumentationLinks $package
    $hash = (Get-FileHash (Join-Path $package 'hoyoflux.exe') -Algorithm SHA256).Hash
    "$hash  hoyoflux.exe" | Set-Content -LiteralPath (Join-Path $package 'SHA256SUMS.txt') -Encoding ascii
}

'release artifact audit passed'
