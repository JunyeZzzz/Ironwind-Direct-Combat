[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$SkipInstall,
    [switch]$SkipArchive,
    [switch]$SkipSymbols,
    [switch]$ReplaceConfig,
    [switch]$ReplaceArchive,
    [switch]$AllowStaleBuild,
    [string]$BuildDirectory,
    [string]$StageDirectory
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$moddevDir = Split-Path -Parent $projectDir
$gameDir = Split-Path -Parent $moddevDir
$stageDir = if ([string]::IsNullOrWhiteSpace($StageDirectory)) {
    Join-Path $projectDir 'package\IronwindCombat'
} elseif ([System.IO.Path]::IsPathRooted($StageDirectory)) {
    [System.IO.Path]::GetFullPath($StageDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectDir $StageDirectory))
}
$runtimeDir = Join-Path $projectDir 'runtime'
$installDir = Join-Path $gameDir 'mods\IronwindCombat'
$backupDir = Join-Path $projectDir 'backup'
$buildDir = if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    Join-Path $projectDir 'build'
} elseif ([System.IO.Path]::IsPathRooted($BuildDirectory)) {
    [System.IO.Path]::GetFullPath($BuildDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectDir $BuildDirectory))
}
$buildDll = Join-Path $buildDir 'WASDCombatPlugin.dll'
$buildPdb = Join-Path $buildDir 'WASDCombatPlugin.pdb'
$buildStamp = Join-Path $buildDir 'BUILD-STAMP.json'
$stageDll = Join-Path $stageDir 'WASDCombatPlugin.dll'
$installDll = Join-Path $installDir 'WASDCombatPlugin.dll'
$versionFile = Join-Path $projectDir 'VERSION'
$sourceFile = Join-Path $projectDir 'WASDCombatPlugin.cpp'
$modsConfig = Join-Path $gameDir 'data\mods.cfg'

function Get-Sha256([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Assert-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing required file: $Path"
    }
}

function Get-ArchiveDllHash([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $zip.Entries | Where-Object { $_.FullName -eq 'BUILD-MANIFEST.json' } | Select-Object -First 1
        if (-not $entry) { return $null }
        $reader = New-Object System.IO.StreamReader($entry.Open())
        try { $json = $reader.ReadToEnd() | ConvertFrom-Json }
        finally { $reader.Dispose() }
        return $json.hashes.dllSha256
    }
    finally { $zip.Dispose() }
}

Assert-File $versionFile
Assert-File $sourceFile
$version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
if ($version -notmatch '^\d+$') { throw "VERSION must contain digits only: '$version'" }

$versionMatch = Select-String -LiteralPath $sourceFile -Pattern '^#define IRONWIND_VERSION "([0-9]+)"$'
if (-not $versionMatch -or $versionMatch.Matches[0].Groups[1].Value -ne $version) {
    throw 'IRONWIND_VERSION in WASDCombatPlugin.cpp does not match VERSION.'
}
$baseMatch = Select-String -LiteralPath $sourceFile -Pattern '^#define DIRECT_CONTROL_BASE_VERSION "([^"]+)"$'
if (-not $baseMatch) { throw 'DIRECT_CONTROL_BASE_VERSION is missing from WASDCombatPlugin.cpp.' }
$baseVersion = $baseMatch.Matches[0].Groups[1].Value

if (-not $SkipInstall -and (Get-Process -Name 'kenshi_x64' -ErrorAction SilentlyContinue)) {
    throw 'Kenshi is running. Exit the game before replacing the loaded DLL.'
}

if (-not $SkipBuild) {
    & (Join-Path $projectDir 'build-local.ps1') -OutputDirectory $buildDir
    if ($LASTEXITCODE -ne 0) { throw "build-local.ps1 failed: $LASTEXITCODE" }
}
Assert-File $buildDll
if (-not $AllowStaleBuild) {
    Assert-File $buildPdb
    Assert-File $buildStamp
    $stamp = Get-Content -LiteralPath $buildStamp -Raw | ConvertFrom-Json
    if ($stamp.ironwindVersion -ne $version) { throw 'Build stamp version does not match VERSION; rebuild first.' }
    if ($stamp.inputs.sourceCppSha256 -ne (Get-Sha256 $sourceFile)) { throw 'WASDCombatPlugin.cpp changed after the DLL was built; rebuild first.' }
    if ($stamp.inputs.versionSha256 -ne (Get-Sha256 $versionFile)) { throw 'VERSION changed after the DLL was built; rebuild first.' }
    if ($stamp.outputs.dllSha256 -ne (Get-Sha256 $buildDll)) { throw 'Build DLL does not match BUILD-STAMP.json.' }
    if ($stamp.outputs.pdbSha256 -ne (Get-Sha256 $buildPdb)) { throw 'Build PDB does not match BUILD-STAMP.json.' }
}

$actualStage = [System.IO.Path]::GetFullPath($stageDir)
$defaultStage = [System.IO.Path]::GetFullPath((Join-Path $projectDir 'package\IronwindCombat'))
if ([string]::IsNullOrWhiteSpace($StageDirectory)) {
    if ($actualStage -ne $defaultStage) { throw "Refusing to reset unexpected default stage path: $actualStage" }
} else {
    $testStageRoot = [System.IO.Path]::GetFullPath((Join-Path $projectDir 'package\_test'))
    if (-not $SkipInstall) { throw 'Custom StageDirectory is test-only and requires -SkipInstall.' }
    if (-not $actualStage.StartsWith($testStageRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Custom StageDirectory must be below package\_test: $actualStage"
    }
}
if (Test-Path -LiteralPath $stageDir) { Remove-Item -LiteralPath $stageDir -Recurse -Force }
New-Item -ItemType Directory -Path $stageDir -Force | Out-Null

$sourceFiles = @(
    'AGENTS.md',
    'ARCHITECTURE.md',
    'CHANGELOG.md',
    'CODE_STRUCTURE.md',
    'KNOWN_ISSUES.md',
    'NOTES.md',
    'PROGRESS.md',
    'README.zh-CN.md',
    'NOTICE.md',
    'TESTING.md',
    'VERSION',
    'LICENSE',
    'WASDCombatPlugin.ini'
)
foreach ($name in $sourceFiles) {
    $sourcePath = Join-Path $projectDir $name
    Assert-File $sourcePath
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $stageDir $name) -Force
}
Copy-Item -LiteralPath (Join-Path $projectDir 'README.zh-CN.md') -Destination (Join-Path $stageDir 'README.md') -Force
Copy-Item -LiteralPath $buildDll -Destination $stageDll -Force
if (Test-Path -LiteralPath $buildStamp -PathType Leaf) {
    Copy-Item -LiteralPath $buildStamp -Destination (Join-Path $stageDir 'BUILD-STAMP.json') -Force
}

$runtimeFiles = @(
    'IronwindCombat.mod',
    'RE_Kenshi.json',
    'Settings.zh-CN.tsv',
    'materials\deferred\character.hlsl'
)
foreach ($name in $runtimeFiles) {
    $runtimePath = Join-Path $runtimeDir $name
    Assert-File $runtimePath
    $targetPath = Join-Path $stageDir $name
    $targetParent = Split-Path -Parent $targetPath
    New-Item -ItemType Directory -Path $targetParent -Force | Out-Null
    Copy-Item -LiteralPath $runtimePath -Destination $targetPath -Force
}

$tsvPath = Join-Path $stageDir 'Settings.zh-CN.tsv'
$seenTsvKeys = @{}
$tsvLineNumber = 0
foreach ($line in Get-Content -LiteralPath $tsvPath -Encoding UTF8) {
    $tsvLineNumber++
    if ([string]::IsNullOrWhiteSpace($line) -or $line.TrimStart().StartsWith('#')) { continue }
    $columns = $line -split "`t"
    if ($columns.Count -ne 2 -or [string]::IsNullOrWhiteSpace($columns[0]) -or [string]::IsNullOrWhiteSpace($columns[1])) {
        throw "Invalid Settings.zh-CN.tsv row $tsvLineNumber; expected two non-empty tab-separated columns."
    }
    if ($seenTsvKeys.ContainsKey($columns[0])) { throw "Duplicate Settings.zh-CN.tsv key: $($columns[0])" }
    $seenTsvKeys[$columns[0]] = $true
}

if (Test-Path -LiteralPath $modsConfig -PathType Leaf) {
    $enabledMods = @(Get-Content -LiteralPath $modsConfig | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if ($enabledMods -contains 'WASDCombatPlugin.mod') {
        throw 'WASDCombatPlugin.mod is enabled alongside IronwindCombat; disable the original plugin first.'
    }
    if ($enabledMods -notcontains 'IronwindCombat.mod') {
        Write-Warning 'IronwindCombat.mod is not currently listed in data\mods.cfg.'
    }
}

$gitHead = $null
$gitDirty = $false
if (Get-Command git -ErrorAction SilentlyContinue) {
    $gitHead = (& git -C $projectDir rev-parse HEAD 2>$null)
    if ($LASTEXITCODE -ne 0) { $gitHead = $null }
    $gitStatus = @(& git -C $projectDir status --porcelain 2>$null)
    $gitDirty = $gitStatus.Count -gt 0
}

$installedConfig = Join-Path $installDir 'WASDCombatPlugin.ini'
$manifest = [ordered]@{
    schemaVersion = 1
    project = [ordered]@{
        name = 'Ironwind: Hands-On Combat'
        owner = 'cjy233'
        notice = 'NOTICE.md'
    }
    ironwindVersion = $version
    directControlBaseVersion = $baseVersion
    target = [ordered]@{
        kenshi = '1.0.65'
        reKenshi = '0.3.5'
        architecture = 'x64'
    }
    generatedAt = (Get-Date).ToString('o')
    git = [ordered]@{
        head = if ($gitHead) { ($gitHead | Select-Object -First 1).Trim() } else { $null }
        dirty = $gitDirty
    }
    hashes = [ordered]@{
        sourceCppSha256 = Get-Sha256 $sourceFile
        dllSha256 = Get-Sha256 $stageDll
        pdbSha256 = Get-Sha256 $buildPdb
        buildStampSha256 = Get-Sha256 $buildStamp
        defaultIniSha256 = Get-Sha256 (Join-Path $stageDir 'WASDCombatPlugin.ini')
        installedIniBeforeDeploySha256 = Get-Sha256 $installedConfig
        settingsZhCnSha256 = Get-Sha256 $tsvPath
    }
    installPolicy = [ordered]@{
        skipped = [bool]$SkipInstall
        replaceExistingConfig = [bool]$ReplaceConfig
    }
    retentionPolicy = [ordered]@{
        archiveSkipped = [bool]$SkipArchive
        permanentSymbolsSkipped = [bool]$SkipSymbols
    }
}
$manifestPath = Join-Path $stageDir 'BUILD-MANIFEST.json'
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

if (-not $SkipSymbols -and (Test-Path -LiteralPath $buildPdb -PathType Leaf)) {
    $symbolDir = Join-Path $projectDir ("package\symbols\v{0}-{1}" -f $version, (Get-Sha256 $buildDll).Substring(0, 12))
    New-Item -ItemType Directory -Path $symbolDir -Force | Out-Null
    Copy-Item -LiteralPath $buildDll -Destination (Join-Path $symbolDir 'WASDCombatPlugin.dll') -Force
    Copy-Item -LiteralPath $buildPdb -Destination (Join-Path $symbolDir 'WASDCombatPlugin.pdb') -Force
    Copy-Item -LiteralPath $buildStamp -Destination (Join-Path $symbolDir 'BUILD-STAMP.json') -Force
}

$archivePath = Join-Path (Split-Path -Parent $stageDir) "IronwindCombat-actions-v$version.zip"
if (-not $SkipArchive -and (Test-Path -LiteralPath $archivePath -PathType Leaf) -and -not $ReplaceArchive) {
    $archiveDllHash = Get-ArchiveDllHash $archivePath
    if (-not $archiveDllHash) {
        throw "Existing archive has no readable DLL manifest; use -ReplaceArchive only after verifying it: $archivePath"
    }
    if ($archiveDllHash -ne (Get-Sha256 $stageDll)) {
        throw "Refusing to overwrite v$version archive with a different DLL. Increment VERSION or use -ReplaceArchive explicitly."
    }
}

$backupPath = $null
if (-not $SkipInstall) {
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
    if ((Test-Path -LiteralPath $installDll -PathType Leaf) -and ((Get-Sha256 $installDll) -ne (Get-Sha256 $stageDll))) {
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
        $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        $backupPath = Join-Path $backupDir "IronwindCombat-pre-deploy-v$version-$timestamp.dll"
        Copy-Item -LiteralPath $installDll -Destination $backupPath -Force
    }

    foreach ($item in Get-ChildItem -LiteralPath $stageDir -Force) {
        if ($item.Name -eq 'WASDCombatPlugin.ini' -and (Test-Path -LiteralPath $installedConfig) -and -not $ReplaceConfig) {
            continue
        }
        Copy-Item -LiteralPath $item.FullName -Destination $installDir -Recurse -Force
    }
}

if (-not $SkipArchive) {
    Compress-Archive -Path (Join-Path $stageDir '*') -DestinationPath $archivePath -CompressionLevel Optimal -Force
}

$buildHash = Get-Sha256 $buildDll
$stageHash = Get-Sha256 $stageDll
if ($buildHash -ne $stageHash) { throw 'Build and package DLL hashes differ.' }
if (-not $SkipInstall) {
    $installHash = Get-Sha256 $installDll
    if ($stageHash -ne $installHash) { throw 'Package and installed DLL hashes differ.' }
}

Write-Output "Ironwind: Hands-On Combat v$version deployment complete."
Write-Output "DLL SHA256: $stageHash"
if (Test-Path -LiteralPath $buildPdb -PathType Leaf) { Write-Output "PDB SHA256: $(Get-Sha256 $buildPdb)" }
Write-Output "Package: $stageDir"
if (-not $SkipArchive) { Write-Output "Archive: $archivePath" }
if ($SkipSymbols) { Write-Output 'Permanent symbol copy: skipped (build PDB remains available).' }
if (-not $SkipInstall) { Write-Output "Installed: $installDir" }
if ($backupPath) { Write-Output "Previous DLL backup: $backupPath" }
if (-not $SkipInstall -and (Test-Path -LiteralPath $installedConfig) -and -not $ReplaceConfig) {
    Write-Output 'Installed WASDCombatPlugin.ini was preserved.'
}
