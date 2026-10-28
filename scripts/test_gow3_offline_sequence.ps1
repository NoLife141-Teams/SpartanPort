param([string]$Toolchain = 'C:\Users\jerem\SynologyDrive\code\SpartanPort\out\toolchain\msys64\clang64')
# Standalone CPU tests only. No probe, process inspection, input, game data or save access.
$ErrorActionPreference='Stop'
$taskRepo=Split-Path -Parent $PSScriptRoot
$taskBase=Join-Path $taskRepo 'out'
$taskOutput=Join-Path $taskBase ('offline-sequence-'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
foreach($taskPath in @($taskRepo,$taskBase)) {
    if((Get-Item -LiteralPath $taskPath).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Reparse point refused' }
}
if(Test-Path -LiteralPath $taskOutput) { throw 'Existing output preserved' }
$taskCompiler=Join-Path $Toolchain 'bin\clang++.exe'
if(-not(Test-Path -LiteralPath $taskCompiler -PathType Leaf)) { throw 'Existing Clang missing; nothing installed' }
New-Item -ItemType Directory -Path $taskOutput | Out-Null
$taskTests=@('gow3_offline_sequence','gow3_reprojection_contract','gow3_depth_samples','gow3_clip_pipeline','gow3_vertex_contract')
$taskResults=@()
$taskOldPath=$env:PATH
Push-Location -LiteralPath $taskRepo
try {
    $env:PATH="$Toolchain\bin;$taskOldPath"
    foreach($taskTest in $taskTests) {
        $taskSource="tests\test_$taskTest.cpp"
        $taskExe=Join-Path $taskOutput "$taskTest.exe"
        $taskBuildLog=Join-Path $taskOutput "$taskTest.build.log"
        $taskLog=Join-Path $taskOutput "$taskTest.log"
        & $taskCompiler -std=c++20 -O1 -g -UNDEBUG -Wall -Wextra -Werror -I. $taskSource -o $taskExe > $taskBuildLog 2>&1
        $taskBuildExit=$LASTEXITCODE
        $taskRunExit=$null
        if($taskBuildExit -eq 0) {
            & $taskExe > $taskLog 2>&1
            $taskRunExit=$LASTEXITCODE
            Get-Content -LiteralPath $taskLog
        } else { Get-Content -LiteralPath $taskBuildLog }
        $taskResults += [ordered]@{test=$taskTest;sourceSha256=(Get-FileHash -LiteralPath $taskSource -Algorithm SHA256).Hash;buildExit=$taskBuildExit;testExit=$taskRunExit;log=$taskLog}
    }
    $taskPassed=@($taskResults | Where-Object {$_.buildExit -ne 0 -or $_.testExit -ne 0}).Count -eq 0
    [ordered]@{utc=[DateTime]::UtcNow.ToString('o');cpuOnly=$true;compilerJobs=1;gameLaunched=$false;savesRead=$false;processInspection=$false;compiler=$taskCompiler;allPassed=$taskPassed;tests=$taskResults} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $taskOutput 'result.json') -Encoding utf8
    Write-Output "RESULTS: $taskOutput"
    if(-not $taskPassed) { throw 'One or more offline tests failed; see result.json' }
} finally {
    $env:PATH=$taskOldPath
    Pop-Location
}
