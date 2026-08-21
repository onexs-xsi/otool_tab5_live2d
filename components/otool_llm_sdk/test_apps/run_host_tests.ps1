# run_host_tests.ps1 — WP7: 一键编译并运行 otool_llm_sdk 全部宿主测试。
#
# 用法（项目根目录或组件目录均可）：
#   powershell -File components/otool_llm_sdk/test_apps/run_host_tests.ps1
#
# 依赖：TinyCC（默认 %TEMP%\tcc\tcc\tcc.exe，可用 -TccPath 覆盖）。
# 输出：两个测试程序的结果与"X checks, Y failures"摘要；任一失败返回非 0。

param(
    [string]$TccPath = (Join-Path $env:TEMP "tcc\tcc\tcc.exe")
)

$ErrorActionPreference = "Stop"

$Comp = Resolve-Path (Join-Path $PSScriptRoot "..")          # components/otool_llm_sdk
$Shim = Join-Path $PSScriptRoot "parser_and_adapters\host_shim"
$CJson = Join-Path $PSScriptRoot "parser_and_adapters\third_party\cjson"

if (-not (Test-Path $TccPath)) {
    Write-Error "tcc not found at $TccPath (set -TccPath)"
}

$Common = @(
    "-I", (Join-Path $Comp "include"),
    "-I", $Shim,
    "-I", (Join-Path $Comp "private_include"),
    "-I", $CJson
)
$Core = @(
    (Join-Path $Comp "src\transports\sse_parser.c"),
    (Join-Path $Comp "src\protocols\responses_sse.c"),
    (Join-Path $Comp "src\protocols\chat_completions_sse.c"),
    (Join-Path $Comp "src\protocols\protocol_resolve.c"),
    (Join-Path $Comp "src\providers\provider_ark.c"),
    (Join-Path $Comp "src\providers\provider_openai.c"),
    (Join-Path $Comp "src\providers\provider_custom.c"),
    (Join-Path $Comp "src\providers\provider_table.c"),
    (Join-Path $Comp "src\agent\tool_registry.c"),
    (Join-Path $Comp "src\agent\tool_schema.c"),
    (Join-Path $CJson "cJSON.c")
)

$Failed = $false

function Run-One {
    param([string]$Name, [string[]]$Sources, [string]$Main)
    $Exe = Join-Path $PSScriptRoot "parser_and_adapters\$Name.exe"
    Write-Host "== building $Name =="
    & $TccPath @Common -o $Exe $Main @Sources
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: $Name compile" -ForegroundColor Red
        $script:Failed = $true
        return
    }
    Write-Host "== running $Name =="
    $Out = & $Exe
    $Out | Select-Object -Last 1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL: $Name run" -ForegroundColor Red
        $script:Failed = $true
    }
}

Run-One "host_tests" $Core (Join-Path $PSScriptRoot "parser_and_adapters\host_tests.c")

Run-One "agent_host_tests" @(
    (Join-Path $Comp "src\agent\agent.c"),
    (Join-Path $Comp "src\agent\tool_registry.c"),
    (Join-Path $Comp "src\agent\tool_schema.c"),
    (Join-Path $CJson "cJSON.c")
) (Join-Path $PSScriptRoot "agent_host_tests.c")

if ($Failed) {
    Write-Host "HOST TESTS: FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "HOST TESTS: ALL PASS" -ForegroundColor Green
exit 0
