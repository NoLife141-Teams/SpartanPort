param(
    [string]$Toolchain = 'C:\Users\jerem\SynologyDrive\code\SpartanPort\out\toolchain\msys64\clang64'
)
# CPU characterization only. Never launches a game, reads saves, sends input,
# changes a system setting, or closes a process. One compiler job.
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
$taskOutputBase = Join-Path $taskRepo 'out'
$taskStamp = [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
$taskOutput = Join-Path $taskOutputBase "control-contract-$taskStamp"
foreach ($taskPath in @($taskRepo,$taskOutputBase)) {
    if ((Get-Item -LiteralPath $taskPath).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing a reparse point: $taskPath"
    }
}
if (Test-Path -LiteralPath $taskOutput) { throw 'Existing results preserved' }
$taskCompiler = Join-Path $Toolchain 'bin\clang.exe'
if (-not (Test-Path -LiteralPath $taskCompiler -PathType Leaf)) { throw 'Existing Clang toolchain missing; nothing installed' }
New-Item -ItemType Directory -Path $taskOutput,(Join-Path $taskOutput 'data') | Out-Null
$taskPreviousPath = $env:PATH
$taskExit = 1
Push-Location -LiteralPath $taskRepo
try {
    $env:PATH = "$Toolchain\bin;$taskPreviousPath"
    & $taskCompiler -std=c11 -O1 -g -UNDEBUG -Wall -Wextra -Werror -pthread -I. -Isrc "-I$Toolchain\include" tests/test_pad_automation_contract.c src/compat_win.c "-L$Toolchain\lib" -lSDL3 -lbcrypt -o "$taskOutput\pad-automation-contract.exe" > "$taskOutput\build.log" 2>&1
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed; see $taskOutput\build.log" }
    & "$taskOutput\pad-automation-contract.exe" "$taskOutput\data" > "$taskOutput\contract.log" 2>&1
    $taskExit = $LASTEXITCODE
    Get-Content -LiteralPath "$taskOutput\contract.log"
    [ordered]@{
        exitCode=$taskExit; cpuOnly=$true; gameLaunched=$false
        inputSentToGame=$false; savesRead=$false; compilerJobs=1
        executableSha256=(Get-FileHash -LiteralPath "$taskOutput\pad-automation-contract.exe" -Algorithm SHA256).Hash
        completedUtc=[DateTime]::UtcNow.ToString('o')
    } | ConvertTo-Json | Set-Content -LiteralPath "$taskOutput\result.json" -Encoding utf8
    if ($taskExit -ne 0) { throw "Contract test failed; see $taskOutput\contract.log" }
    Write-Output "CPU characterization passed: $taskOutput"
    Write-Output 'Autonomous gameplay is NOT enabled or validated by this test.'
} finally {
    $env:PATH = $taskPreviousPath
    Pop-Location
}
