#Requires -Version 5.1
<# Poppy's host-side adapter for yumayo/ai-container's WSL Containers launcher.
   It uses the existing launcher and finally cleanup. No AI container receives
   Docker privileges, and no shared .aicontainer or CLI settings are overwritten.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Repository,
    [Parameter(Mandatory = $true)][string] $Workspace,
    [Parameter(Mandatory = $true)][ValidateSet('codex', 'claude')][string] $Tool
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
[Console]::InputEncoding = [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$notification = $env:POPPY_SESSION_ID
if ($notification -notmatch '^[a-f0-9]{32}$' -or -not $env:POPPY_EVENTS_URL) {
    throw 'Run this adapter in a Poppy terminal with AI notifications enabled.'
}
$prefix = "Local\Poppy.$notification"
$stop = [Threading.EventWaitHandle]::OpenExisting("$prefix.stop")
$launching = [Threading.EventWaitHandle]::OpenExisting("$prefix.launching")
$cleaned = [Threading.EventWaitHandle]::OpenExisting("$prefix.cleaned")
$unconfirmed = [Threading.EventWaitHandle]::OpenExisting("$prefix.unconfirmed")
$null = $unconfirmed.Reset()
$null = $cleaned.Reset()
$null = $launching.Set()
$script:PoppyRunId = $null
$script:PoppyStopJob = $null
$profileDirectory = $null
$confirmed = $false
$enteredLauncher = $false
try {
    $Workspace = (Get-Item -LiteralPath $Workspace).FullName
    $Repository = (Get-Item -LiteralPath $Repository).FullName
    . (Join-Path $Repository 'wsl-containers/wsl-containers.ps1')
    Assert-AiWslPlatform
    # Use the launcher's extension point. The name is obtained directly from
    # the trusted launcher, never from a workspace-controlled state file.
    $script:PoppyAttach = (Get-Command Invoke-AiWslInteractive).ScriptBlock
    function Invoke-AiWslInteractive {
        param([string] $Wslc, [string] $Name)
        if ($Name -notmatch '^yumayo-ai-launch-([a-f0-9]{32})$') { throw 'Unsupported launcher identity.' }
        $script:PoppyRunId = $Matches[1]
        $script:PoppyStopJob = Start-Job -ArgumentList $Wslc, $Name, "$prefix.stop" -ScriptBlock {
            param($Executable, $Runner, $StopName)
            $request = [Threading.EventWaitHandle]::OpenExisting($StopName)
            try {
                while (-not $request.WaitOne(200)) { }
                # Stop only this invocation's trusted runner. Its owner runs
                # the existing finally block to remove children and resources.
                & $Executable rm --force $Runner
                if ($LASTEXITCODE -ne 0) { throw 'Targeted launcher stop failed.' }
            } finally { $request.Dispose() }
        }
        & $script:PoppyAttach -Wslc $Wslc -Name $Name
    }
    foreach ($candidate in @($Workspace, (Join-Path $Workspace '.poppy-runtime'))) {
        if ((Test-Path -LiteralPath $candidate) -and ((Get-Item -LiteralPath $candidate).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Runtime profiles cannot use linked paths.' }
    }
    $profileDirectory = Join-Path $Workspace ('.poppy-runtime\' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $profileDirectory
    $profile = Join-Path $profileDirectory 'launch.json'
    $json = @{ arguments = @(); environment = @{ POPPY_EVENTS_URL = $env:POPPY_EVENTS_URL; POPPY_SESSION_ID = $notification } } | ConvertTo-Json -Depth 4
    [IO.File]::WriteAllText($profile, $json, (New-Object Text.UTF8Encoding($false)))
    $result = Initialize-AiWsl
    if ($result.ExitCode -ne 0) { throw "ai-container initialization failed: $($result.ExitCode)" }
    if (-not (Test-AiWslImages $result.Wslc)) { Build-AiWslImages -Wslc $result.Wslc -Repository $Repository }
    Write-Host 'Poppy notifications require the Codex/Claude hooks installed in the AI container. See ai/README.md.'
    if ($stop.WaitOne(0)) { throw 'Launch cancelled before container creation.' }
    $enteredLauncher = $true
    Start-AiWslContainer -Wslc $result.Wslc -Repository $Repository -Workspace $Workspace -Tool $Tool -LaunchProfileFile $profile
    if ($script:PoppyRunId) {
        $run = $script:PoppyRunId
        # Check the resources owned by exactly this launcher after its finally
        # returns. An empty heartbeat or an exited Windows client is insufficient.
        $native = Read-AiWslCommand $result.Wslc @('ps', '-aq', '--filter', "name=yumayo-ai-launch-$run")
        $carrier = Read-AiWslCommand $result.Wslc @('ps', '-aq', '--filter', "name=yumayo-ai-share-$run")
        $docker = @('system', 'session', 'run', 'docker')
        $children = Read-AiWslCommand $result.Wslc ($docker + @('ps', '-aq', '--filter', "label=io.yumayo.aicontainer.wsl-run=$run"))
        $relay = Read-AiWslCommand $result.Wslc ($docker + @('ps', '-aq', '--filter', "label=io.yumayo.aicontainer.wsl-relay=$run"))
        $networks = Read-AiWslCommand $result.Wslc ($docker + @('network', 'ls', '-q', '--filter', "label=io.yumayo.aicontainer.wsl-run=$run"))
        $volumes = Read-AiWslCommand $result.Wslc @('volume', 'ls', '-q', '--filter', "label=io.yumayo.aicontainer.wsl-run=$run")
        if (($native + $carrier + $children + $relay + $networks + $volumes).Trim()) { throw "Cleanup unconfirmed for launcher $run" }
        $confirmed = $true
    }
    $exitCode = $script:AiWslExitCode
} catch {
    Write-Error $_ -ErrorAction Continue
    $exitCode = 1
} finally {
    if ($script:PoppyStopJob) {
        Stop-Job $script:PoppyStopJob -ErrorAction SilentlyContinue
        Remove-Job $script:PoppyStopJob -Force -ErrorAction SilentlyContinue
    }
    # Before the trusted launcher has been entered, no per-launch resources exist.
    # If it threw during creation, retain the profile and mark cleanup unknown.
    if (-not $enteredLauncher) { $confirmed = $true }
    if ($confirmed) {
        if ($profileDirectory) { Remove-Item -LiteralPath $profileDirectory -Recurse -Force }
        $null = $cleaned.Set()
    } else {
        $null = $unconfirmed.Set()
        Write-Warning "Container cleanup is unconfirmed. Retained profile: $profileDirectory"
    }
    $null = $launching.Reset()
    $stop.Dispose(); $launching.Dispose(); $cleaned.Dispose(); $unconfirmed.Dispose()
}
exit $exitCode
