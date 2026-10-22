[CmdletBinding()]
param(
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$moddevDir = Split-Path -Parent $projectDir
$toolsDir = Join-Path $moddevDir 'toolchain'
$depsDir = Join-Path $moddevDir 'KenshiLib_Examples_deps'
$vc64Dir = Join-Path $toolsDir 'vc64\Program Files(64)\Microsoft Visual Studio 10.0\VC'
$vc86Dir = Join-Path $toolsDir 'vc86\Program Files\Microsoft Visual Studio 10.0\VC'
$sdkDir = Join-Path $toolsDir 'sdkbuild\Program Files\Microsoft SDKs\Windows\v7.1'
$boostDir = Join-Path $depsDir 'boost_1_60_0'
$kenshiDir = Join-Path $depsDir 'KenshiLib'
$buildDir = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    Join-Path $projectDir 'build'
} elseif ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
    [System.IO.Path]::GetFullPath($OutputDirectory)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $projectDir $OutputDirectory))
}

$clExe = Join-Path $vc64Dir 'bin\amd64\cl.exe'
$linkExe = Join-Path $vc64Dir 'bin\amd64\link.exe'
$source = Join-Path $projectDir 'WASDCombatPlugin.cpp'
$object = Join-Path $buildDir 'WASDCombatPlugin.obj'
$binary = Join-Path $buildDir 'WASDCombatPlugin.dll'
$pdb = Join-Path $buildDir 'WASDCombatPlugin.pdb'
$compilerPdb = Join-Path $buildDir 'WASDCombatPlugin-compile.pdb'
$stampPath = Join-Path $buildDir 'BUILD-STAMP.json'
$versionFile = Join-Path $projectDir 'VERSION'

foreach ($path in @($clExe, $linkExe, $source, $versionFile,
    (Join-Path $vc86Dir 'include\vector'),
    (Join-Path $sdkDir 'Include\Windows.h'),
    (Join-Path $boostDir 'boost\version.hpp'),
    (Join-Path $kenshiDir 'Libraries\KenshiLib.lib'))) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing build dependency: $path" }
}

New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
$env:PATH = (Join-Path $vc64Dir 'bin\amd64') + ';' + (Join-Path $vc86Dir 'bin') + ';' + $env:PATH
$env:INCLUDE = @((Join-Path $vc86Dir 'include'), (Join-Path $sdkDir 'Include'),
    (Join-Path $kenshiDir 'Include'), (Join-Path $kenshiDir 'Include\kenshi'), $boostDir) -join ';'
$env:LIB = @((Join-Path $vc64Dir 'lib\amd64'), (Join-Path $vc86Dir 'lib\amd64'),
    (Join-Path $sdkDir 'Lib\x64'), (Join-Path $kenshiDir 'Libraries'),
    (Join-Path $boostDir 'stage\lib')) -join ';'

Push-Location $projectDir
try {
    & $clExe /nologo /c /EHsc /MD /O2 /Gy /GL /Zi /DNDEBUG /D_CONSOLE /DUNICODE /D_UNICODE "/Fo$object" "/Fd$compilerPdb" $source
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $LASTEXITCODE" }
    & $linkExe /nologo /DLL /LTCG /DEBUG /OPT:REF /OPT:ICF /MACHINE:X64 "/OUT:$binary" "/PDB:$pdb" $object `
        KenshiLib.lib OgreMain_x64.lib MyGUIEngine_x64.lib user32.lib
    if ($LASTEXITCODE -ne 0) { throw "Link failed: $LASTEXITCODE" }
    foreach ($path in @($binary, $pdb)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Expected build output was not generated: $path"
        }
    }

    $version = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    $stamp = [ordered]@{
        schemaVersion = 1
        generatedAt = (Get-Date).ToString('o')
        ironwindVersion = $version
        architecture = 'x64'
        configuration = 'ReleaseWithSymbols'
        inputs = [ordered]@{
            sourceCppSha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
            versionSha256 = (Get-FileHash -LiteralPath $versionFile -Algorithm SHA256).Hash
            compilerSha256 = (Get-FileHash -LiteralPath $clExe -Algorithm SHA256).Hash
            linkerSha256 = (Get-FileHash -LiteralPath $linkExe -Algorithm SHA256).Hash
        }
        outputs = [ordered]@{
            dllSha256 = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash
            pdbSha256 = (Get-FileHash -LiteralPath $pdb -Algorithm SHA256).Hash
        }
    }
    $stamp | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $stampPath -Encoding UTF8
    Write-Output "Built: $binary"
    Write-Output "Symbols: $pdb"
    Write-Output "Build stamp: $stampPath"
}
finally { Pop-Location }
