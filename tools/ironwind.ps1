[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('Status', 'Check', 'Build', 'Deploy', 'DevFinish', 'Finish', 'Verify', 'Housekeeping', 'SessionStart', 'AnalyzeLog', 'Snapshot', 'Help')]
    [string]$Task = 'Status',
    [string]$Label,
    [string]$LogPath,
    [switch]$SkipBuild,
    [switch]$SkipInstall,
    [switch]$SkipArchive,
    [switch]$SkipSymbols,
    [switch]$ReplaceConfig,
    [switch]$ReplaceArchive,
    [switch]$OpenReport
)

$ErrorActionPreference = 'Stop'

$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $toolsDir
$moddevDir = Split-Path -Parent $projectDir
$gameDir = Split-Path -Parent $moddevDir
$buildDir = Join-Path $projectDir 'build'
$packageDir = Join-Path $projectDir 'package\IronwindCombat'
$installDir = Join-Path $gameDir 'mods\IronwindCombat'
$runtimeDir = Join-Path $projectDir 'runtime'
$versionFile = Join-Path $projectDir 'VERSION'
$sourceFile = Join-Path $projectDir 'WASDCombatPlugin.cpp'
$buildDll = Join-Path $buildDir 'WASDCombatPlugin.dll'
$buildPdb = Join-Path $buildDir 'WASDCombatPlugin.pdb'
$buildStamp = Join-Path $buildDir 'BUILD-STAMP.json'
$packageDll = Join-Path $packageDir 'WASDCombatPlugin.dll'
$installDll = Join-Path $installDir 'WASDCombatPlugin.dll'
$modsConfig = Join-Path $gameDir 'data\mods.cfg'
$defaultLog = Join-Path $gameDir 'RE_Kenshi_log.txt'
$sessionStateDir = Join-Path $projectDir 'reports\session-state'
$sessionStatePath = Join-Path $sessionStateDir 'current.json'

function Get-IwHash([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
}

function Assert-IwFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing required file: $Path" }
}

function Get-IwVersion {
    Assert-IwFile $versionFile
    $value = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    if ($value -notmatch '^[0-9]+$') { throw "VERSION must contain digits only: '$value'" }
    return $value
}

function Get-IwBuildState {
    $version = Get-IwVersion
    $state = [ordered]@{
        exists = Test-Path -LiteralPath $buildDll -PathType Leaf
        pdbExists = Test-Path -LiteralPath $buildPdb -PathType Leaf
        stampExists = Test-Path -LiteralPath $buildStamp -PathType Leaf
        fresh = $false
        reason = 'build output missing'
        dllSha256 = Get-IwHash $buildDll
        pdbSha256 = Get-IwHash $buildPdb
    }
    if (-not $state.exists) { return [pscustomobject]$state }
    if (-not $state.stampExists) { $state.reason = 'BUILD-STAMP.json missing'; return [pscustomobject]$state }
    try { $stamp = Get-Content -LiteralPath $buildStamp -Raw | ConvertFrom-Json }
    catch { $state.reason = 'BUILD-STAMP.json is invalid'; return [pscustomobject]$state }
    if ($stamp.ironwindVersion -ne $version) { $state.reason = 'build version differs from VERSION'; return [pscustomobject]$state }
    if ($stamp.inputs.sourceCppSha256 -ne (Get-IwHash $sourceFile)) { $state.reason = 'source changed after build'; return [pscustomobject]$state }
    if ($stamp.inputs.versionSha256 -ne (Get-IwHash $versionFile)) { $state.reason = 'VERSION changed after build'; return [pscustomobject]$state }
    if ($stamp.outputs.dllSha256 -ne $state.dllSha256) { $state.reason = 'DLL hash differs from build stamp'; return [pscustomobject]$state }
    if (-not $state.pdbExists) { $state.reason = 'matching PDB missing'; return [pscustomobject]$state }
    if ($stamp.outputs.pdbSha256 -ne $state.pdbSha256) { $state.reason = 'PDB hash differs from build stamp'; return [pscustomobject]$state }
    $state.fresh = $true
    $state.reason = 'fresh'
    return [pscustomobject]$state
}

function Invoke-IwStatus {
    $version = Get-IwVersion
    $buildState = Get-IwBuildState
    $enabledMods = @()
    if (Test-Path -LiteralPath $modsConfig -PathType Leaf) {
        $enabledMods = @(Get-Content -LiteralPath $modsConfig | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    }
    [pscustomobject]@{
        Version = $version
        KenshiRunning = [bool](Get-Process -Name kenshi_x64 -ErrorAction SilentlyContinue)
        BuildFresh = $buildState.fresh
        BuildReason = $buildState.reason
        BuildDllSha256 = $buildState.dllSha256
        PackageDllSha256 = Get-IwHash $packageDll
        InstalledDllSha256 = Get-IwHash $installDll
        IronwindEnabled = $enabledMods -contains 'IronwindCombat.mod'
        LegacyPluginEnabled = $enabledMods -contains 'WASDCombatPlugin.mod'
        InstalledIniSha256 = Get-IwHash (Join-Path $installDir 'WASDCombatPlugin.ini')
    }
}

function Invoke-IwCheck {
    $version = Get-IwVersion
    $required = @(
        'AGENTS.md', 'ARCHITECTURE.md', 'CHANGELOG.md', 'CODE_STRUCTURE.md',
        'KNOWN_ISSUES.md', 'NOTES.md', 'NOTICE.md', 'PROGRESS.md', 'README.zh-CN.md',
        'TESTING.md', 'VERSION', 'WASDCombatPlugin.cpp', 'WASDCombatPlugin.ini',
        'build-local.ps1', 'deploy-local.ps1',
        'tools\ironwind.ps1', 'tools\Analyze-IronwindLog.ps1', 'tools\log-rules.json',
        'runtime\IronwindCombat.mod', 'runtime\RE_Kenshi.json',
        'runtime\Settings.zh-CN.tsv', 'runtime\materials\deferred\character.hlsl'
    )
    foreach ($name in $required) { Assert-IwFile (Join-Path $projectDir $name) }

    $publicReadme = Get-Content -LiteralPath (Join-Path $projectDir 'README.md') -Raw
    $noticeText = Get-Content -LiteralPath (Join-Path $projectDir 'NOTICE.md') -Raw
    $solutionText = Get-Content -LiteralPath (Join-Path $projectDir 'WASDCombatPlugin.sln') -Raw
    $projectText = Get-Content -LiteralPath (Join-Path $projectDir 'WASDCombatPlugin.vcxproj') -Raw
    if ($publicReadme -notmatch '(?m)^# 铁风执刃（Ironwind: Hands-On Combat）') {
        throw 'Public README must identify the project as Ironwind: Hands-On Combat.'
    }
    if ($noticeText -notmatch 'Direct Control for Kenshi v1\.5\.0' -or $noticeText -notmatch 'GNU GPL v3') {
        throw 'NOTICE.md must preserve the upstream base and GPL attribution.'
    }
    if ($solutionText -notmatch '= "IronwindCombat", "WASDCombatPlugin\.vcxproj"') {
        throw 'Visual Studio solution display name must remain IronwindCombat.'
    }
    if ($projectText -notmatch '<RootNamespace>IronwindCombat</RootNamespace>') {
        throw 'Visual Studio root namespace must remain IronwindCombat.'
    }

    $versionMatch = Select-String -LiteralPath $sourceFile -Pattern '^#define IRONWIND_VERSION "([0-9]+)"$'
    if (-not $versionMatch -or $versionMatch.Matches[0].Groups[1].Value -ne $version) {
        throw 'IRONWIND_VERSION in WASDCombatPlugin.cpp does not match VERSION.'
    }

    # Lifecycle ownership is a gameplay invariant, not merely a style rule.
    # These coupled fields previously drifted across load/V-exit/stagger/hit
    # paths and produced permanent queued attacks or reaction locks. Keep one
    # engine-free reset owner for each group and fail Check if ad-hoc teardown
    # starts spreading through the monolithic translation unit again.
    $sourceText = Get-Content -LiteralPath $sourceFile -Raw

    # v116 deliberately removed Ironwind's player-authored dodge subsystem.
    # Native Kenshi `isDodge` / HIT_MISSED handling remains valid, so guard the
    # obsolete Ironwind identifiers rather than banning the word "dodge".
    $removedDodgeSymbols = @(
        'KR_DODGE', 's_dodge', 's_freeDodge', 's_dcDodge', 'DcDodge',
        'manualDodge', 'manualRoll', 'getDodge_hook', 's_getDodgeOrig',
        'DC_DODGE', 'pendingDodge', 'freeDodgeLive', 'attackAfterDodge'
    )
    foreach ($symbol in $removedDodgeSymbols) {
        if ($sourceText -match [regex]::Escape($symbol)) {
            throw "Removed active-dodge symbol returned: $symbol"
        }
    }
    foreach ($symbol in @('s_camRotateToggle', 's_ctrlPrevPoll')) {
        if ($sourceText -match [regex]::Escape($symbol)) {
            throw "Removed legacy Ctrl camera-toggle symbol returned: $symbol"
        }
    }
    $defaultIniText = Get-Content -LiteralPath (Join-Path $projectDir 'WASDCombatPlugin.ini') -Raw
    foreach ($key in @('DodgeWindowMs', 'DodgeSkillBonus', 'DodgeCooldownMs', 'DodgeRecoveryMs')) {
        if ($defaultIniText -match "(?m)^\s*$key\s*=") {
            throw "Removed active-dodge setting returned: $key"
        }
    }
    if ($defaultIniText -match '(?m)^\s*Dodge\s*=') {
        throw 'Removed active-dodge key binding returned: Dodge'
    }
    if ($defaultIniText -notmatch '(?m)^\s*FirstPerson\s*=\s*F\s*$') {
        throw 'FirstPerson default must remain F (mouse wheel is the second supported switch).'
    }
    if ($defaultIniText -match '(?m)^\s*FirstPerson\s*=\s*P\s*$') {
        throw 'Removed P first-person binding returned.'
    }
    if ($defaultIniText -notmatch '(?m)^\s*Block\s*=\s*Q\s*$') {
        throw 'Keyboard Block default must remain Q so it cannot collide with FirstPerson=F.'
    }
    if ($defaultIniText -notmatch '(?m)^\s*KeyLayout\s*=\s*3\s*$') {
        throw 'KeyLayout must remain 3 for the v117 F/Q migration.'
    }
    foreach ($key in @('SpeedCycle', 'ManualAttack')) {
        if ($defaultIniText -match "(?m)^\s*$key\s*=") {
            throw "Removed legacy key binding returned: $key"
        }
    }
    foreach ($symbol in @('KR_SPEED', 'KR_MANUAL', 's_xPressed', 'handleSpeedPress')) {
        if ($sourceText -match [regex]::Escape($symbol)) {
            throw "Removed legacy input symbol returned: $symbol"
        }
    }
    foreach ($key in @('PerfectBlockWindowMs', 'PerfectBlockRewardMs')) {
        if ($defaultIniText -match "(?m)^\s*$key\s*=") {
            throw "Removed timed-perfect-block setting returned: $key"
        }
    }
    foreach ($symbol in @('s_perfect', 'PERFECT_FOLLOWUP_MS', 'perfect_block',
                           's_rewardGlow', 'dc_perfect_reward_glow', 'block_press_reward')) {
        if ($sourceText -match [regex]::Escape($symbol)) {
            throw "Removed timed-perfect-block symbol returned: $symbol"
        }
    }
    $optionsRowsMatch = [regex]::Match(
        $sourceText,
        '(?s)static DcOptRow s_dcOptRows\[\]\s*=\s*\{(.*?)\r?\n\};\s*static const int DC_OPT_ROW_COUNT'
    )
    if (-not $optionsRowsMatch.Success) {
        throw 'Cannot locate the Ironwind settings-page row table.'
    }
    $visibleOptions = $optionsRowsMatch.Groups[1].Value
    $advancedIniOnly = @(
        'WasdSpeedCap', 'EnemyPursuit', 'InventoryFaceCam',
        'AttackWindowMs', 'AttackRecoveryMs', 'ManualBlock', 'DirectionalCombat',
        'BlockHoldMs', 'BlockCooldownMs', 'AimHoverPx', 'InputRepeatGuardMs',
        'GlintLeadMs', 'GlintColourTiers', 'OtsDistance', 'OtsSide', 'OtsNearClip',
        'NeckLimit', 'LookSmooth', 'HideHead', 'HideHeadgear', 'HideHair',
        'HideInjuryOverlay', 'FloorRevealBelow', 'EyeUpAdjust', 'JogForward',
        'RunForward', 'SideLookForward', 'ActionClearForward', 'NearClip',
        'DownNearClip', 'BodySolidInside'
    )
    foreach ($key in $advancedIniOnly) {
        $pattern = '\{\s*"(?:Settings|FirstPerson)"\s*,\s*"' + [regex]::Escape($key) + '"'
        if ($visibleOptions -match $pattern) {
            throw "Advanced INI-only setting returned to the normal options page: $key"
        }
    }
    $visibleSettingCount = [regex]::Matches(
        $visibleOptions, '\{\s*"(?:Settings|FirstPerson)"\s*,'
    ).Count
    if ($visibleSettingCount -gt 13) {
        throw "Ironwind options page regrew beyond the v120 compact limit: $visibleSettingCount visible settings."
    }
    if ($sourceText -notmatch 'en\s*==\s*"focus_char"\s*&&\s*viewDown') {
        throw 'V-mode FirstPerson=F must suppress the colliding vanilla focus_char event.'
    }
    $stateOwners = @(
        'manualAttackClearCapturedTarget',
        'manualAttackClearAimInput',
        'manualAttackClearQueuedInput',
        'manualAttackClearActiveRuntime',
        'manualHitReactionClearRuntime',
        'manualInteractionClearRuntime'
    )
    foreach ($owner in $stateOwners) {
        if ($sourceText -notmatch "static void $owner\s*\(") {
            throw "Missing runtime-state owner: $owner"
        }
    }
    $singleOwnerAssignments = [ordered]@{
        'queued target reset' = '(?m)^\s+s_attackQueuedTarget\s*=\s*nullptr;'
        'captured target reset' = '(?m)^\s+s_attackAimCapturedTarget\s*=\s*nullptr;'
        'active attack reset' = '(?m)^\s+s_attackActive\s*=\s*false;'
    }
    foreach ($entry in $singleOwnerAssignments.GetEnumerator()) {
        $count = [regex]::Matches($sourceText, $entry.Value).Count
        if ($count -ne 1) {
            throw "Runtime-state ownership drift: $($entry.Key) has $count direct writers (expected 1)."
        }
    }

    Get-Content -LiteralPath (Join-Path $toolsDir 'log-rules.json') -Raw -Encoding UTF8 | ConvertFrom-Json | Out-Null
    foreach ($script in @('build-local.ps1', 'deploy-local.ps1', 'tools\ironwind.ps1', 'tools\Analyze-IronwindLog.ps1')) {
        [void][scriptblock]::Create((Get-Content -LiteralPath (Join-Path $projectDir $script) -Raw))
    }

    $tsvKeys = @{}
    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath (Join-Path $runtimeDir 'Settings.zh-CN.tsv') -Encoding UTF8) {
        $lineNumber++
        if ([string]::IsNullOrWhiteSpace($line) -or $line.TrimStart().StartsWith('#')) { continue }
        $columns = $line -split "`t"
        if ($columns.Count -ne 2 -or [string]::IsNullOrWhiteSpace($columns[0]) -or [string]::IsNullOrWhiteSpace($columns[1])) {
            throw "Invalid runtime Settings.zh-CN.tsv row $lineNumber."
        }
        if ($tsvKeys.ContainsKey($columns[0])) { throw "Duplicate TSV key: $($columns[0])" }
        $tsvKeys[$columns[0]] = $true
    }

    if (Test-Path -LiteralPath $modsConfig -PathType Leaf) {
        $mods = @(Get-Content -LiteralPath $modsConfig | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        if ($mods -contains 'WASDCombatPlugin.mod') { throw 'Legacy WASDCombatPlugin.mod is enabled.' }
        if ($mods -notcontains 'IronwindCombat.mod') { Write-Warning 'IronwindCombat.mod is not enabled.' }
    }

    if (Get-Command git -ErrorAction SilentlyContinue) {
        & git -C $projectDir diff --check
        if ($LASTEXITCODE -ne 0) { throw 'git diff --check failed.' }
    }

    $buildState = Get-IwBuildState
    if (-not $buildState.fresh) { Write-Warning "Build is not fresh: $($buildState.reason)" }
    Write-Output "Static checks passed for Ironwind v$version."
    return $buildState
}

function Invoke-IwVerify {
    $version = Get-IwVersion
    foreach ($path in @($buildDll, $packageDll, $installDll)) { Assert-IwFile $path }
    $hashes = @(@((Get-IwHash $buildDll), (Get-IwHash $packageDll), (Get-IwHash $installDll)) | Select-Object -Unique)
    if ($hashes.Count -ne 1) { throw 'Build, package and installed DLL hashes differ.' }

    $manifestPath = Join-Path $packageDir 'BUILD-MANIFEST.json'
    Assert-IwFile $manifestPath
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.ironwindVersion -ne $version) { throw 'Package manifest version mismatch.' }
    if ($manifest.hashes.dllSha256 -ne $hashes[0]) { throw 'Package manifest DLL hash mismatch.' }

    $mods = @(Get-Content -LiteralPath $modsConfig | ForEach-Object { $_.Trim() } | Where-Object { $_ })
    if (@($mods | Where-Object { $_ -eq 'IronwindCombat.mod' }).Count -ne 1) { throw 'IronwindCombat.mod must be enabled exactly once.' }
    if ($mods -contains 'WASDCombatPlugin.mod') { throw 'Legacy WASDCombatPlugin.mod is enabled.' }

    Write-Output "Verified Ironwind v$version. DLL SHA256: $($hashes[0])"
    return [pscustomobject]@{
        Version = $version
        DllSha256 = $hashes[0]
        PdbSha256 = Get-IwHash $buildPdb
        PackageManifest = $manifestPath
    }
}

function Write-IwReleaseReport($Verification) {
    $reportDir = Join-Path $projectDir 'reports\releases'
    New-Item -ItemType Directory -Path $reportDir -Force | Out-Null
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $path = Join-Path $reportDir "Ironwind-v$($Verification.Version)-$stamp.md"
    $content = @(
        "# 铁风 v$($Verification.Version) 自动发布事实",
        '',
        "- 生成时间：$((Get-Date).ToString('o'))",
        "- DLL SHA256：``$($Verification.DllSha256)``",
        "- PDB SHA256：``$($Verification.PdbSha256)``",
        "- 构建、暂存、安装 DLL：一致",
        "- 启用 Mod：仅 ``IronwindCombat.mod``",
        '',
        '> 这是自动化构建与部署证据，不代表游戏画面或完整回归已经验证。'
    )
    $content | Set-Content -LiteralPath $path -Encoding UTF8
    Write-Output "Release facts: $path"
}

function Start-IwSession {
    $path = if ([string]::IsNullOrWhiteSpace($LogPath)) { $defaultLog } else { $LogPath }
    $lineCount = 0
    $length = 0
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $lineCount = [System.IO.File]::ReadAllLines($path).Count
        $length = (Get-Item -LiteralPath $path).Length
    }
    New-Item -ItemType Directory -Path $sessionStateDir -Force | Out-Null
    $state = [ordered]@{
        schemaVersion = 1
        startedAt = (Get-Date).ToString('o')
        label = $Label
        version = Get-IwVersion
        logPath = $path
        startLine = $lineCount + 1
        startByteLength = $length
        installedDllSha256 = Get-IwHash $installDll
    }
    $state | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $sessionStatePath -Encoding UTF8
    Write-Output "Session marker saved: $sessionStatePath"
    Write-Output "Next log line: $($state.startLine); version: v$($state.version); label: $($state.label)"
}

function Invoke-IwLogAnalysis {
    $parameters = @{}
    if (-not [string]::IsNullOrWhiteSpace($LogPath)) { $parameters.LogPath = $LogPath }
    if ($OpenReport) { $parameters.OpenReport = $true }
    if (Test-Path -LiteralPath $sessionStatePath -PathType Leaf) {
        $state = Get-Content -LiteralPath $sessionStatePath -Raw | ConvertFrom-Json
        $analysisLog = if ([string]::IsNullOrWhiteSpace($LogPath)) { $state.logPath } else { $LogPath }
        Assert-IwFile $analysisLog
        $currentLines = [System.IO.File]::ReadAllLines($analysisLog).Count
        if ($currentLines -ge $state.startLine) {
            $parameters.LogPath = $analysisLog
            $parameters.StartLine = [int]$state.startLine
        } else {
            Write-Warning 'Session marker is newer than the current log; falling back to the latest detected plugin session.'
        }
    }
    & (Join-Path $toolsDir 'Analyze-IronwindLog.ps1') @parameters
}

function New-IwSnapshot {
    if (Get-Process -Name kenshi_x64 -ErrorAction SilentlyContinue) { throw 'Exit Kenshi before creating a stable snapshot.' }
    $verification = Invoke-IwVerify
    $version = $verification.Version
    $safeLabel = if ([string]::IsNullOrWhiteSpace($Label)) { 'snapshot' } else { $Label -replace '[^A-Za-z0-9_-]+', '-' }
    $date = Get-Date -Format 'yyyyMMdd'
    $snapshotDir = Join-Path $projectDir "backup\stable-v$version-$safeLabel-$date"
    if (Test-Path -LiteralPath $snapshotDir) { $snapshotDir += '-' + (Get-Date -Format 'HHmmss') }
    New-Item -ItemType Directory -Path $snapshotDir -Force | Out-Null

    $archive = Join-Path $projectDir "package\IronwindCombat-actions-v$version.zip"
    foreach ($path in @($archive, $installDll, (Join-Path $installDir 'WASDCombatPlugin.ini'), (Join-Path $packageDir 'BUILD-MANIFEST.json'))) { Assert-IwFile $path }
    Copy-Item -LiteralPath $archive -Destination (Join-Path $snapshotDir "IronwindCombat-v$version-$safeLabel.zip")
    Copy-Item -LiteralPath $installDll -Destination (Join-Path $snapshotDir "WASDCombatPlugin-v$version.dll")
    Copy-Item -LiteralPath (Join-Path $installDir 'WASDCombatPlugin.ini') -Destination (Join-Path $snapshotDir "WASDCombatPlugin-v$version-user.ini")
    Copy-Item -LiteralPath (Join-Path $packageDir 'BUILD-MANIFEST.json') -Destination (Join-Path $snapshotDir "BUILD-MANIFEST-v$version.json")
    if (Test-Path -LiteralPath $buildPdb -PathType Leaf) { Copy-Item -LiteralPath $buildPdb -Destination (Join-Path $snapshotDir "WASDCombatPlugin-v$version.pdb") }
    if (Test-Path -LiteralPath $buildStamp -PathType Leaf) { Copy-Item -LiteralPath $buildStamp -Destination (Join-Path $snapshotDir "BUILD-STAMP-v$version.json") }
    if (Test-Path -LiteralPath $defaultLog -PathType Leaf) { Copy-Item -LiteralPath $defaultLog -Destination (Join-Path $snapshotDir "RE_Kenshi-v$version.log") }

    $sourceItems = @((Get-ChildItem -LiteralPath $projectDir -File | Select-Object -ExpandProperty FullName), $runtimeDir, $toolsDir)
    Compress-Archive -LiteralPath $sourceItems -DestinationPath (Join-Path $snapshotDir "IronwindCombat-v$version-source.zip") -CompressionLevel Optimal

    $readme = @(
        "# 铁风 v$version 快照：$safeLabel",
        '',
        "生成时间：$((Get-Date).ToString('o'))。",
        '',
        '包含可安装发布包、实际安装 DLL、个人 INI、构建清单、可用符号、日志及源码。',
        '恢复前必须退出 Kenshi；启动器只启用 IronwindCombat.mod。',
        '',
        '> 快照证明文件可恢复，不代表完整游戏回归通过。'
    )
    $readme | Set-Content -LiteralPath (Join-Path $snapshotDir 'README-RESTORE.md') -Encoding UTF8

    $sumLines = @()
    foreach ($file in Get-ChildItem -LiteralPath $snapshotDir -File | Sort-Object Name) {
        $sumLines += "$(Get-IwHash $file.FullName)  $($file.Name)"
    }
    $sumLines | Set-Content -LiteralPath (Join-Path $snapshotDir 'SHA256SUMS.txt') -Encoding ASCII
    Write-Output "Stable snapshot: $snapshotDir"
}

function Show-IwHelp {
    @'
铁风开发工具：tools\ironwind.ps1

  Status                         查看版本、进程、构建新鲜度和三处 DLL 哈希
  Check                          执行静态、配置、脚本和加载顺序检查
  Build                          编译 Release DLL、匹配 PDB 和 BUILD-STAMP
  Deploy [-SkipBuild]            部署；可用 -SkipArchive / -SkipSymbols 控制归档
  DevFinish                      Check → Build → 安装 → Verify，不新增 ZIP/永久符号副本
  Finish                         Check → Build → Deploy → Verify → 发布事实报告
  Verify                         核对构建、暂存、安装、清单和 mods.cfg
  Housekeeping                   只读统计 build/package/backup/reports 占用，不删除文件
  SessionStart [-Label 名称]     记录下一次实机日志的起点，不清空日志
  AnalyzeLog [-OpenReport]       分析已标记会话；无标记时分析最新插件会话
  Snapshot [-Label 名称]         留存发布包、DLL/PDB、配置、日志、源码和哈希

常用：
  .\tools\ironwind.ps1 DevFinish
  .\tools\ironwind.ps1 Finish   # 确定正式发布时
  .\tools\ironwind.ps1 SessionStart -Label v85-melee
  .\tools\ironwind.ps1 AnalyzeLog -OpenReport
'@ | Write-Output
}

function Invoke-IwHousekeeping {
    $targets = @(
        @{ Name = 'Build'; Path = (Join-Path $projectDir 'build') },
        @{ Name = 'Release archives'; Path = (Join-Path $projectDir 'package'); Filter = '*.zip' },
        @{ Name = 'Permanent symbols'; Path = (Join-Path $projectDir 'package\symbols') },
        @{ Name = 'Rollback backups'; Path = (Join-Path $projectDir 'backup') },
        @{ Name = 'Reports'; Path = (Join-Path $projectDir 'reports') }
    )
    foreach ($target in $targets) {
        $files = @()
        if (Test-Path -LiteralPath $target.Path) {
            if ($target.ContainsKey('Filter')) {
                $files = @(Get-ChildItem -LiteralPath $target.Path -File -Recurse -Filter $target.Filter -ErrorAction SilentlyContinue)
            } else {
                $files = @(Get-ChildItem -LiteralPath $target.Path -File -Recurse -ErrorAction SilentlyContinue)
            }
        }
        $bytes = ($files | Measure-Object -Property Length -Sum).Sum
        if ($null -eq $bytes) { $bytes = 0 }
        [pscustomobject]@{
            Category = $target.Name
            Files = $files.Count
            MiB = [math]::Round($bytes / 1MB, 2)
            Path = $target.Path
        }
    }
    Write-Host 'No files were deleted. Use Snapshot for explicit long-term retention; DevFinish avoids new release archives.'
}

switch ($Task) {
    'Status' { Invoke-IwStatus | Format-List }
    'Check' { Invoke-IwCheck | Format-List }
    'Build' { & (Join-Path $projectDir 'build-local.ps1') }
    'Deploy' {
        $arguments = @{}
        if ($SkipBuild) { $arguments.SkipBuild = $true }
        if ($SkipInstall) { $arguments.SkipInstall = $true }
        if ($SkipArchive) { $arguments.SkipArchive = $true }
        if ($SkipSymbols) { $arguments.SkipSymbols = $true }
        if ($ReplaceConfig) { $arguments.ReplaceConfig = $true }
        if ($ReplaceArchive) { $arguments.ReplaceArchive = $true }
        & (Join-Path $projectDir 'deploy-local.ps1') @arguments
    }
    'DevFinish' {
        Invoke-IwCheck | Out-Null
        $arguments = @{ SkipArchive = $true; SkipSymbols = $true }
        if ($SkipBuild) { $arguments.SkipBuild = $true }
        if ($SkipInstall) { $arguments.SkipInstall = $true }
        if ($ReplaceConfig) { $arguments.ReplaceConfig = $true }
        & (Join-Path $projectDir 'deploy-local.ps1') @arguments
        Invoke-IwVerify | Format-List
    }
    'Finish' {
        Invoke-IwCheck | Out-Null
        $arguments = @{}
        if ($SkipBuild) { $arguments.SkipBuild = $true }
        if ($SkipInstall) { $arguments.SkipInstall = $true }
        if ($SkipArchive) { $arguments.SkipArchive = $true }
        if ($SkipSymbols) { $arguments.SkipSymbols = $true }
        if ($ReplaceConfig) { $arguments.ReplaceConfig = $true }
        if ($ReplaceArchive) { $arguments.ReplaceArchive = $true }
        & (Join-Path $projectDir 'deploy-local.ps1') @arguments
        $verified = Invoke-IwVerify
        Write-IwReleaseReport $verified
    }
    'Verify' { Invoke-IwVerify | Format-List }
    'Housekeeping' { Invoke-IwHousekeeping | Format-Table -AutoSize }
    'SessionStart' { Start-IwSession }
    'AnalyzeLog' { Invoke-IwLogAnalysis }
    'Snapshot' { New-IwSnapshot }
    'Help' { Show-IwHelp }
}
