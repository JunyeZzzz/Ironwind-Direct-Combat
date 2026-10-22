[CmdletBinding()]
param(
    [string]$LogPath,
    [ValidateSet('Latest', 'All')]
    [string]$Session = 'Latest',
    [int]$StartLine = 0,
    [string]$OutputDirectory,
    [string]$RulesPath,
    [switch]$NoFiles,
    [switch]$OpenReport
)

$ErrorActionPreference = 'Stop'

$toolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $toolsDir
$moddevDir = Split-Path -Parent $projectDir
$gameDir = Split-Path -Parent $moddevDir
if ([string]::IsNullOrWhiteSpace($LogPath)) { $LogPath = Join-Path $gameDir 'RE_Kenshi_log.txt' }
if ([string]::IsNullOrWhiteSpace($RulesPath)) { $RulesPath = Join-Path $toolsDir 'log-rules.json' }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $projectDir 'reports\log-analysis' }

foreach ($path in @($LogPath, $RulesPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing analysis input: $path" }
}

function Get-Median([double[]]$Values) {
    if (-not $Values -or $Values.Count -eq 0) { return $null }
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count / 2)
    if (($sorted.Count % 2) -eq 1) { return [double]$sorted[$middle] }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

function Get-EventName([string]$Payload, [string]$StartPattern) {
    if ($Payload -match $StartPattern) { return 'session_start' }
    if ($Payload -match '^unloaded\s*$') { return 'unloaded' }
    if ($Payload -match '\bhook OK\s*$') { return 'hook_ok' }
    if ($Payload -match '^MODE:') { return 'MODE' }
    if ($Payload -match '^(?<event>[A-Za-z_][A-Za-z0-9_.:-]*)') {
        return $Matches.event.TrimEnd(':')
    }
    return 'unclassified'
}

function Get-GroupIds([string]$EventName, $Groups, $DynamicPrefixes) {
    $ids = @()
    foreach ($group in $Groups) {
        foreach ($pattern in $group.patterns) {
            if ($EventName -match $pattern) {
                $ids += $group.id
                break
            }
        }
    }
    if ($ids.Count -eq 0) {
        foreach ($prefix in $DynamicPrefixes) {
            if ($EventName -match $prefix.pattern) { $ids += $prefix.id; break }
        }
    }
    return @($ids)
}

function Test-NoiseEvent([string]$EventName, $NoisePatterns) {
    foreach ($pattern in $NoisePatterns) {
        if ($EventName -match $pattern) { return $true }
    }
    return $false
}

function Escape-MarkdownCell([object]$Value) {
    if ($null -eq $Value) { return '' }
    return ([string]$Value).Replace('|', '\|').Replace("`r", ' ').Replace("`n", ' ')
}

$rules = Get-Content -LiteralPath $RulesPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($rules.schemaVersion -ne 1) { throw "Unsupported log-rules schema: $($rules.schemaVersion)" }

$allLines = [System.IO.File]::ReadAllLines($LogPath)
if ($allLines.Count -eq 0) { throw "Log is empty: $LogPath" }

$sessionStarts = @()
for ($i = 0; $i -lt $allLines.Count; $i++) {
    if ($allLines[$i] -match $rules.sessionStartPattern) { $sessionStarts += $i }
}

$startIndex = 0
if ($StartLine -gt 0) {
    $startIndex = [Math]::Min($allLines.Count - 1, [Math]::Max(0, $StartLine - 1))
} elseif ($Session -eq 'Latest' -and $sessionStarts.Count -gt 0) {
    $startIndex = $sessionStarts[-1]
}
$endIndex = $allLines.Count - 1
if ($Session -eq 'Latest' -and $StartLine -le 0) {
    foreach ($candidate in $sessionStarts) {
        if ($candidate -gt $startIndex) { $endIndex = $candidate - 1; break }
    }
}
$sessionLines = @($allLines[$startIndex..$endIndex])

$version = 'unknown'
foreach ($line in $sessionLines) {
    if ($line -match $rules.sessionStartPattern) { $version = $Matches.version; break }
}

$events = New-Object System.Collections.ArrayList
for ($relative = 0; $relative -lt $sessionLines.Count; $relative++) {
    $raw = $sessionLines[$relative]
    $markerIndex = $raw.IndexOf($rules.pluginPattern)
    if ($markerIndex -lt 0) { continue }
    $payload = $raw.Substring($markerIndex + $rules.pluginPattern.Length).Trim()
    while ($payload.StartsWith('WASDCombatPlugin:')) {
        $payload = $payload.Substring('WASDCombatPlugin:'.Length).Trim()
    }
    $channel = 'Plugin'
    if ($payload -match '^\[(?<channel>[^\]]+)\]\s*(?<body>.*)$') {
        $channel = $Matches.channel
        $payload = $Matches.body
    }
    $time = $null
    if ($raw -match '^\s*(?<time>[0-9]+(?:\.[0-9]+)?)') { $time = [double]$Matches.time }
    $eventName = Get-EventName $payload $rules.sessionStartPattern
    $groups = Get-GroupIds $eventName $rules.groups $rules.dynamicPrefixes
    $noise = Test-NoiseEvent $eventName $rules.noisePatterns
    [void]$events.Add([pscustomobject]@{
        SessionIndex = $relative
        Line = $startIndex + $relative + 1
        Time = $time
        Channel = $channel
        Event = $eventName
        Groups = $groups
        Noise = $noise
        Payload = $payload
        Raw = $raw
    })
}

$eventInventory = @($events | Group-Object Event | ForEach-Object {
    $sample = $_.Group | Select-Object -First 1
    [pscustomobject]@{
        event = $_.Name
        count = $_.Count
        groups = @($sample.Groups)
        noise = [bool]$sample.Noise
        sample = $sample.Payload
    }
} | Sort-Object @{Expression='count';Descending=$true}, event)

$riskFindings = New-Object System.Collections.ArrayList
foreach ($rule in $rules.riskRules) {
    $ruleMatches = New-Object System.Collections.ArrayList
    for ($i = 0; $i -lt $sessionLines.Count; $i++) {
        if ($sessionLines[$i] -match $rule.pattern) {
            [void]$ruleMatches.Add([pscustomobject]@{
                line = $startIndex + $i + 1
                sessionIndex = $i
                text = $sessionLines[$i]
            })
        }
    }
    if ($ruleMatches.Count -gt 0) {
        [void]$riskFindings.Add([pscustomobject]@{
            id = $rule.id
            severity = $rule.severity
            description = $rule.description
            count = $ruleMatches.Count
            samples = @($ruleMatches | Select-Object -First 3)
        })
    }
}

$chain = $rules.attackChain
$attackRows = New-Object System.Collections.ArrayList
$active = $null
$pendingAnimation = 'unknown'
foreach ($event in $events) {
    if ($event.Event -eq 'attack_direction_native_random' -and $event.Payload -match '\banim=(?<animation>.+)$') {
        $pendingAnimation = $Matches.animation.Trim()
    }
    if ($event.Event -eq $chain.startEvent -and $event.Payload -match $chain.startSuccessPattern) {
        if ($null -ne $active) {
            $active.status = 'superseded_without_finish'
            [void]$attackRows.Add([pscustomobject]$active)
        }
        $active = [ordered]@{
            startLine = $event.Line
            startTime = $event.Time
            finishLine = $null
            finishTime = $null
            durationMs = $null
            impact = $false
            verdicts = 0
            confirmedHits = 0
            markers = 0
            animation = $pendingAnimation
            interruptions = @()
            status = 'active'
        }
        $pendingAnimation = 'unknown'
        continue
    }
    if ($null -eq $active) { continue }
    if ($event.Event -eq $chain.impactEvent) { $active.impact = $true }
    if ($event.Event -eq $chain.verdictEvent) { $active.verdicts++ }
    if ($event.Event -eq $chain.confirmedHitEvent) { $active.confirmedHits++ }
    if ($event.Event -eq $chain.markerEvent) { $active.markers++ }
    if (@('dc_menu_suspend_begin', 'attack_guard_handoff', 'hit_on_me', 'stagger_start') -contains $event.Event) {
        $active.interruptions = @($active.interruptions) + $event.Event
    }
    if ($chain.finishEvents -contains $event.Event) {
        $active.finishLine = $event.Line
        $active.finishTime = $event.Time
        if ($null -ne $active.startTime -and $null -ne $event.Time) {
            $active.durationMs = [Math]::Round(($event.Time - $active.startTime) * 1000.0, 1)
        }
        $active.status = $event.Event
        [void]$attackRows.Add([pscustomobject]$active)
        $active = $null
    }
}
if ($null -ne $active) {
    $active.status = 'session_ended_without_finish'
    [void]$attackRows.Add([pscustomobject]$active)
}

$completedDurations = @($attackRows | Where-Object { $null -ne $_.durationMs -and $_.durationMs -ge 0 } | ForEach-Object { [double]$_.durationMs })
$durationMedian = Get-Median $completedDurations
$durationMad = $null
if ($null -ne $durationMedian -and $completedDurations.Count -gt 0) {
    $deviations = @($completedDurations | ForEach-Object { [Math]::Abs($_ - $durationMedian) })
    $durationMad = Get-Median $deviations
}
$durationOutliers = New-Object System.Collections.ArrayList
$durationGroups = @($attackRows | Where-Object { $null -ne $_.durationMs } | Group-Object { "$($_.animation)|impact=$($_.impact)|$($_.status)" })
foreach ($durationGroup in $durationGroups) {
    $groupValues = @($durationGroup.Group | ForEach-Object { [double]$_.durationMs })
    $groupMedian = Get-Median $groupValues
    $groupDeviations = @($groupValues | ForEach-Object { [Math]::Abs($_ - $groupMedian) })
    $groupMad = Get-Median $groupDeviations
    foreach ($attack in $durationGroup.Group) {
        $reason = $null
        if ($attack.durationMs -lt 120) {
            $reason = 'absolute_shorter_than_120ms'
        } elseif ($durationGroup.Count -ge 5) {
            $absoluteDelta = [Math]::Abs($attack.durationMs - $groupMedian)
            if ($groupMad -gt 0) {
                $robustZ = $absoluteDelta / (1.4826 * $groupMad)
                if ($robustZ -gt 3.5 -and $absoluteDelta -gt [Math]::Max(150, $groupMedian * 0.20)) {
                    $reason = 'animation_group_mad_outlier'
                }
            } elseif ($attack.durationMs -lt ($groupMedian * 0.25) -or $attack.durationMs -gt ($groupMedian * 2.5)) {
                $reason = 'animation_group_ratio_outlier'
            }
        }
        if ($reason) {
            [void]$durationOutliers.Add([pscustomobject]@{
                startLine = $attack.startLine
                finishLine = $attack.finishLine
                durationMs = $attack.durationMs
                status = $attack.status
                impact = $attack.impact
                confirmedHits = $attack.confirmedHits
                markers = $attack.markers
                animation = $attack.animation
                interruptions = @($attack.interruptions)
                baselineGroup = $durationGroup.Name
                baselineCount = $durationGroup.Count
                baselineMedianMs = [Math]::Round($groupMedian, 1)
                reason = $reason
            })
        }
    }
}

function Count-Event([string]$Name) {
    return @($events | Where-Object { $_.Event -eq $Name }).Count
}

$nativeStarts = @($events | Where-Object { $_.Event -eq $chain.startEvent -and $_.Payload -match $chain.startSuccessPattern }).Count
$nativeStartFailures = @($events | Where-Object { $_.Event -eq $chain.startEvent -and $_.Payload -match 'started=0' }).Count
$nativeVerdicts = @($events | Where-Object { $_.Event -eq $chain.verdictEvent })
$blocked = @($nativeVerdicts | Where-Object { $_.Payload -match 'outcome=blocked' }).Count
$dodged = @($nativeVerdicts | Where-Object { $_.Payload -match 'outcome=dodged' }).Count
$hitCandidates = @($nativeVerdicts | Where-Object { $_.Payload -match 'outcome=hit_candidate' }).Count
$confirmedHits = Count-Event $chain.confirmedHitEvent
$markers = Count-Event $chain.markerEvent
$unknownEvents = @($eventInventory | Where-Object { -not $_.noise -and $_.groups.Count -eq 0 })
$errorCount = @($riskFindings | Where-Object { $_.severity -eq 'error' } | ForEach-Object { $_.count } | Measure-Object -Sum).Sum
$warningCount = @($riskFindings | Where-Object { $_.severity -eq 'warning' } | ForEach-Object { $_.count } | Measure-Object -Sum).Sum
if ($null -eq $errorCount) { $errorCount = 0 }
if ($null -eq $warningCount) { $warningCount = 0 }

$summary = [ordered]@{
    version = $version
    logPath = $LogPath
    firstLine = $startIndex + 1
    lastLine = $endIndex + 1
    rawLines = $sessionLines.Count
    pluginEvents = $events.Count
    discoveredEventTypes = $eventInventory.Count
    unknownEventTypes = $unknownEvents.Count
    releases = Count-Event $chain.releaseEvent
    nativeStarts = $nativeStarts
    nativeStartFailures = $nativeStartFailures
    impacts = Count-Event $chain.impactEvent
    completedAttackRows = @($attackRows | Where-Object { $_.status -notmatch 'without_finish|active' }).Count
    incompleteAttackRows = @($attackRows | Where-Object { $_.status -match 'without_finish|active' }).Count
    verdicts = $nativeVerdicts.Count
    hitCandidates = $hitCandidates
    blocks = $blocked
    manualDirectionalBlocks = Count-Event 'directional_guard_block'
    dodges = $dodged
    confirmedHits = $confirmedHits
    hitMarkers = $markers
    markerMismatch = $confirmedHits -ne $markers
    durationMedianMs = if ($null -eq $durationMedian) { $null } else { [Math]::Round($durationMedian, 1) }
    durationMadMs = if ($null -eq $durationMad) { $null } else { [Math]::Round($durationMad, 1) }
    durationOutliers = $durationOutliers.Count
    riskErrors = [int]$errorCount
    riskWarnings = [int]$warningCount
}

$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$baseName = "Ironwind-v$version-$timestamp"
$jsonPath = Join-Path $OutputDirectory "$baseName.json"
$markdownPath = Join-Path $OutputDirectory "$baseName.md"

$reportObject = [ordered]@{
    schemaVersion = 1
    generatedAt = (Get-Date).ToString('o')
    summary = $summary
    risks = @($riskFindings)
    attacks = @($attackRows)
    durationOutliers = @($durationOutliers)
    eventInventory = @($eventInventory)
    unknownEvents = @($unknownEvents)
}

$md = New-Object System.Collections.Generic.List[string]
$md.Add("# 铁风执刃日志分析：v$version")
$md.Add('')
$md.Add("- 日志：``$LogPath``")
$md.Add("- 范围：第 $($summary.firstLine)～$($summary.lastLine) 行，共 $($summary.rawLines) 行")
$md.Add("- 插件事件：$($summary.pluginEvents) 条，自动发现 $($summary.discoveredEventTypes) 种事件")
$md.Add('')
$md.Add('## 攻击链摘要')
$md.Add('')
$md.Add('| 指标 | 数量 |')
$md.Add('|---|---:|')
foreach ($row in @(
    @('释放提交', $summary.releases),
    @('原版攻击启动', $summary.nativeStarts),
    @('原版启动失败', $summary.nativeStartFailures),
    @('impact 完成', $summary.impacts),
    @('完成的攻击链', $summary.completedAttackRows),
    @('未闭合攻击链', $summary.incompleteAttackRows),
    @('命中候选', $summary.hitCandidates),
    @('敌人格挡', $summary.blocks),
    @('玩家手动方向格挡', $summary.manualDirectionalBlocks),
    @('敌人闪避', $summary.dodges),
    @('真实 `_getHit`', $summary.confirmedHits),
    @('红色命中标记', $summary.hitMarkers)
)) { $md.Add("| $($row[0]) | $($row[1]) |") }
$md.Add('')
if ($summary.markerMismatch) { $md.Add('> **异常：** 真实命中数量与红色命中标记数量不一致。') }
if ($null -ne $summary.durationMedianMs) {
    $md.Add("原版攻击链时长中位数：**$($summary.durationMedianMs) ms**；MAD：**$($summary.durationMadMs) ms**；动态离群：**$($summary.durationOutliers)** 条。")
}
$md.Add('')
$md.Add('## 风险规则')
$md.Add('')
if ($riskFindings.Count -eq 0) {
    $md.Add('没有命中当前规则库中的风险事件。')
} else {
    $md.Add('| 级别 | 规则 | 数量 | 含义 |')
    $md.Add('|---|---|---:|---|')
    foreach ($finding in $riskFindings) {
        $md.Add("| $(Escape-MarkdownCell $finding.severity) | $(Escape-MarkdownCell $finding.id) | $($finding.count) | $(Escape-MarkdownCell $finding.description) |")
    }
}
$md.Add('')
if ($durationOutliers.Count -gt 0) {
    $md.Add('## 动态时长离群')
    $md.Add('')
    $md.Add('| 起始行 | 结束行 | 时长 ms | 动作 / 原因 / 中断 | impact | 命中/标记 |')
    $md.Add('|---:|---:|---:|---|---:|---:|')
    foreach ($attack in $durationOutliers) {
        $interruptText = if ($attack.interruptions.Count -gt 0) { $attack.interruptions -join ',' } else { '-' }
        $md.Add("| $($attack.startLine) | $($attack.finishLine) | $($attack.durationMs) | $(Escape-MarkdownCell $attack.animation) / $(Escape-MarkdownCell $attack.reason) / $(Escape-MarkdownCell $interruptText) | $([int]$attack.impact) | $($attack.confirmedHits)/$($attack.markers) |")
    }
    $md.Add('')
}
$md.Add('## 自动发现的事件')
$md.Add('')
$md.Add('| 事件 | 数量 | 分类 | 噪声 | 示例 |')
$md.Add('|---|---:|---|---:|---|')
foreach ($item in ($eventInventory | Select-Object -First 100)) {
    $groupText = if ($item.groups.Count -gt 0) { $item.groups -join ',' } else { '未分类' }
    $sampleText = Escape-MarkdownCell $item.sample
    if ($sampleText.Length -gt 120) { $sampleText = $sampleText.Substring(0, 117) + '...' }
    $md.Add("| $(Escape-MarkdownCell $item.event) | $($item.count) | $(Escape-MarkdownCell $groupText) | $([int]$item.noise) | $sampleText |")
}
$md.Add('')
if ($unknownEvents.Count -gt 0) {
    $md.Add('## 尚未分类的新事件')
    $md.Add('')
    $md.Add('这些事件不是错误；它们用于提醒维护者规则库可能需要扩展。')
    $md.Add('')
    foreach ($item in ($unknownEvents | Select-Object -First 50)) { $md.Add("- ``$($item.event)`` × $($item.count)") }
    $md.Add('')
}
$contextPoints = New-Object System.Collections.ArrayList
foreach ($finding in $riskFindings) {
    foreach ($sample in $finding.samples) { [void]$contextPoints.Add([pscustomobject]@{label=$finding.id;index=$sample.sessionIndex}) }
}
foreach ($attack in $durationOutliers) { [void]$contextPoints.Add([pscustomobject]@{label='duration_outlier';index=($attack.startLine - $startIndex - 1)}) }
if ($contextPoints.Count -gt 0) {
    $md.Add('## 异常上下文')
    $md.Add('')
    foreach ($point in ($contextPoints | Select-Object -First 20)) {
        $md.Add("### $($point.label)，会话行 $($point.index + 1)")
        $md.Add('')
        $md.Add('```text')
        $from = [Math]::Max(0, $point.index - 3)
        $to = [Math]::Min($sessionLines.Count - 1, $point.index + 5)
        for ($i = $from; $i -le $to; $i++) { $md.Add($sessionLines[$i]) }
        $md.Add('```')
        $md.Add('')
    }
}
$md.Add('## 解释边界')
$md.Add('')
$md.Add('本报告自动统计代码路径、事件配对和时间离群。它不能判断动作是否自然、HUD 是否好看，也不能把“有日志”自动升级为“画面已验证”。')

if (-not $NoFiles) {
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    $reportObject | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
    $md | Set-Content -LiteralPath $markdownPath -Encoding UTF8
}

Write-Output "Ironwind log analysis: v$version"
Write-Output "Lines: $($summary.firstLine)-$($summary.lastLine); plugin events: $($summary.pluginEvents); event types: $($summary.discoveredEventTypes)"
Write-Output "Attacks: releases=$($summary.releases) native_starts=$($summary.nativeStarts) impacts=$($summary.impacts) completed=$($summary.completedAttackRows) incomplete=$($summary.incompleteAttackRows)"
Write-Output "Outcomes: candidates=$($summary.hitCandidates) enemy_blocks=$($summary.blocks) manual_blocks=$($summary.manualDirectionalBlocks) dodges=$($summary.dodges) hits=$($summary.confirmedHits) markers=$($summary.hitMarkers)"
Write-Output "Risks: errors=$($summary.riskErrors) warnings=$($summary.riskWarnings) duration_outliers=$($summary.durationOutliers) unknown_event_types=$($summary.unknownEventTypes)"
if (-not $NoFiles) {
    Write-Output "Markdown: $markdownPath"
    Write-Output "JSON: $jsonPath"
}
if ($OpenReport -and -not $NoFiles) { Invoke-Item -LiteralPath $markdownPath }

[pscustomobject]@{
    Summary = [pscustomobject]$summary
    MarkdownPath = if ($NoFiles) { $null } else { $markdownPath }
    JsonPath = if ($NoFiles) { $null } else { $jsonPath }
}
