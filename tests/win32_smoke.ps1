param(
    [Parameter(Mandatory = $true)]
    [string]$BinaryPath
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("model-gateway-test-" + [System.Guid]::NewGuid().ToString('N'))
$homeDir = Join-Path $tempRoot 'home'
$configDir = Join-Path $homeDir '.claude\model-gateway'
$configPath = Join-Path $configDir 'config.json'
$port = Get-Random -Minimum 20000 -Maximum 40000
$secondPort = if ($port -ge 45000) { 45000 } else { $port + 1 }

$firstStdout = Join-Path $tempRoot 'first.stdout.log'
$firstStderr = Join-Path $tempRoot 'first.stderr.log'
$secondStdout = Join-Path $tempRoot 'second.stdout.log'
$secondStderr = Join-Path $tempRoot 'second.stderr.log'

$firstProcess = $null
$secondProcess = $null
$originalHome = $env:HOME
$originalUserProfile = $env:USERPROFILE

function Get-CombinedLog {
    param([string[]]$Paths)

    $parts = @()
    foreach ($path in $Paths) {
        if (Test-Path $path) {
            $parts += Get-Content $path -Raw
        }
    }
    return ($parts -join "`n")
}

function Cleanup {
    if ($secondProcess -and -not $secondProcess.HasExited) {
        Stop-Process -Id $secondProcess.Id -Force -ErrorAction SilentlyContinue
        $secondProcess.WaitForExit()
    }
    if ($firstProcess -and -not $firstProcess.HasExited) {
        Stop-Process -Id $firstProcess.Id -Force -ErrorAction SilentlyContinue
        $firstProcess.WaitForExit()
    }

    $env:HOME = $originalHome
    $env:USERPROFILE = $originalUserProfile

    if (Test-Path $tempRoot) {
        Remove-Item $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

try {
    New-Item -ItemType Directory -Force -Path $configDir | Out-Null

    $config = @{
        port = $port
        bind = '127.0.0.1'
        thread_pool_size = 4
        providers = @(
            @{
                id = 'local-test'
                type = 'openai'
                name = 'Local Test'
                api_key = 'local-test-key'
                base_url = 'http://127.0.0.1:1/v1'
                models = @('mock-model')
            }
        )
        models = @{
            'local-test-model' = @{
                id = 'local-test-model'
                provider = 'local-test'
                upstream_model = 'mock-model'
                protocol = 'openai'
                role = 'test'
            }
        }
        aliases = @{
            test = 'local-test-model'
        }
    }
    $config | ConvertTo-Json -Depth 10 | Set-Content -Path $configPath -Encoding utf8

    $env:HOME = $homeDir
    $env:USERPROFILE = $homeDir

    $firstProcess = Start-Process -FilePath $BinaryPath -ArgumentList @('--show', "$port") -PassThru -RedirectStandardOutput $firstStdout -RedirectStandardError $firstStderr

    $ready = $false
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        try {
            $models = Invoke-RestMethod -Uri "http://127.0.0.1:$port/v1/models" -Method Get
            $ready = $true
            break
        } catch {
            Start-Sleep -Milliseconds 200
        }
    }

    if (-not $ready) {
        throw "first instance did not become ready`n$(Get-CombinedLog @($firstStdout, $firstStderr))"
    }

    if (-not ($models.data | Where-Object { $_.id -eq 'test' })) {
        throw "expected alias 'test' in /v1/models response"
    }

    $monitor = Invoke-RestMethod -Uri "http://127.0.0.1:$port/api/monitor" -Method Get
    if ($monitor.runtime.listener -ne "http://127.0.0.1:$port") {
        throw "unexpected listener in monitor response: $($monitor.runtime.listener)"
    }

    $countTokenBody = @{
        model = 'test'
        messages = @(
            @{
                role = 'user'
                content = 'Count these tokens locally.'
            }
        )
    } | ConvertTo-Json -Depth 10

    $countTokenResult = Invoke-RestMethod -Uri "http://127.0.0.1:$port/v1/messages/count_tokens" -Method Post -ContentType 'application/json' -Body $countTokenBody
    if (-not $countTokenResult.input_tokens -or $countTokenResult.input_tokens -lt 1) {
        throw "count_tokens did not return a positive input_tokens value"
    }

    $secondProcess = Start-Process -FilePath $BinaryPath -ArgumentList @('--show', "$secondPort") -PassThru -RedirectStandardOutput $secondStdout -RedirectStandardError $secondStderr
    $null = $secondProcess.WaitForExit(5000)

    if (-not $secondProcess.HasExited) {
        throw "second instance is still running; expected startup failure`n$(Get-CombinedLog @($secondStdout, $secondStderr))"
    }

    if ($secondProcess.ExitCode -eq 0) {
        throw "second instance exited successfully; expected failure`n$(Get-CombinedLog @($secondStdout, $secondStderr))"
    }

    $secondLog = Get-CombinedLog @($secondStdout, $secondStderr)
    if ($secondLog -notmatch 'another gateway instance is already running') {
        throw "second instance did not fail with the expected lock error`n$secondLog"
    }
}
finally {
    Cleanup
}