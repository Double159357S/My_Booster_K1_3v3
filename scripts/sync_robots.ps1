#requires -Version 5.1
<#
Ask for the local project, IP prefix and robot suffixes, then sync sources.
Use -DryRun to preview or -Build to compile after syncing.
#>
[CmdletBinding()]
param(
    [string]$LocalRoot,
    [string]$RemoteRoot = '/home/booster/Workspace/K1_5v5_demo_1.7',
    [string]$IpPrefix,
    [string]$RobotIds,
    [string[]]$Robots,
    [string]$UserName = 'booster',
    [string]$HostKey = 'SHA256:nYXfRrYeTIq8Wpf1z5spmyHnRevFkOJCuIyF/urWwcY',
    [string]$PuttyDirectory = 'D:\putty',
    [string]$KeyFile,
    [string]$RollbackBackup,
    [switch]$DryRun,
    [switch]$Build
)
$ErrorActionPreference = 'Stop'
$taskTemporaryDirectory = $null
$taskExitCode = 1
$previousPythonUtf8 = $env:PYTHONUTF8
$env:PYTHONUTF8 = '1'

function Find-PuttyTool([string]$Name) {
    $candidates = @(
        (Join-Path $PuttyDirectory $Name),
        (Join-Path $env:ProgramFiles "PuTTY\$Name")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    throw "找不到 $Name，请用 -PuttyDirectory 指定 PuTTY 所在目录。"
}

try {
    $backend = Join-Path $PSScriptRoot 'deployment\robot_sync.py'
    if (-not (Test-Path -LiteralPath $backend -PathType Leaf)) {
        throw "缺少脚本配套文件：$backend"
    }
    if ([string]::IsNullOrWhiteSpace($LocalRoot)) {
        $defaultLocalRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
        $LocalRoot = Read-Host ("请输入本地工程路径（直接回车使用 $defaultLocalRoot）")
        if ([string]::IsNullOrWhiteSpace($LocalRoot)) { $LocalRoot = $defaultLocalRoot }
    }
    # Accept paths pasted with surrounding quotes, including paths with spaces.
    $LocalRoot = $LocalRoot.Trim().Trim('"').Trim("'")
    $LocalRoot = (Resolve-Path -LiteralPath $LocalRoot -ErrorAction Stop).ProviderPath
    if (-not (Test-Path -LiteralPath (Join-Path $LocalRoot 'src\brain') -PathType Container)) {
        throw "本地工程目录无效：$LocalRoot"
    }
    Write-Host ("本地工程：$LocalRoot") -ForegroundColor Cyan
    $python = Get-Command python.exe -ErrorAction Stop
    $plink = Find-PuttyTool 'plink.exe'
    $pscp = Find-PuttyTool 'pscp.exe'
    # There are no default targets. An empty suffix list never selects all.
    if ($Robots) {
        if ($IpPrefix -or $RobotIds) { throw '不能同时指定 -Robots 和 IP 段/尾号。' }
        $selectionArguments = @('--hosts', ($Robots -join ','))
    } else {
        if (-not $IpPrefix) { $IpPrefix = Read-Host '请输入 IP 段，例如 192.168.133' }
        if (-not $RobotIds) { $RobotIds = Read-Host '请输入要同步的机器人尾号，例如 .43 或 .43,.21' }
        $selectionArguments = @('--ip-prefix', $IpPrefix, '--robot-ids', $RobotIds)
    }
    $selectionJson = & $python.Source $backend '--select-only' @selectionArguments
    if ($LASTEXITCODE -ne 0) { throw '机器人 IP 输入无效，尚未连接任何机器人。' }
    [string[]]$selectedRobots = ConvertFrom-Json -InputObject ($selectionJson -join "`n")
    Write-Host ('本次目标：' + ($selectedRobots -join ', ')) -ForegroundColor Cyan
    $arguments = @(
        '-u', $backend, '--local-root', $LocalRoot, '--remote-root', $RemoteRoot,
        '--hosts', ($selectedRobots -join ','), '--user', $UserName, '--host-key', $HostKey,
        '--plink', $plink, '--pscp', $pscp
    )
    if ($DryRun) { $arguments += '--dry-run' }
    if ($Build) { $arguments += '--build' }
    if ($RollbackBackup) { $arguments += @('--rollback', $RollbackBackup) }

    if ($KeyFile) {
        $arguments += @('--key-file', (Resolve-Path -LiteralPath $KeyFile).Path)
    } else {
        # PuTTY reads the password from a temporary file restricted to this
        # Windows user. The password never appears in process arguments/logs.
        $taskTemporaryDirectory = Join-Path ([IO.Path]::GetTempPath()) ('k1-sync-auth-' + [Guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($taskTemporaryDirectory) | Out-Null
        $taskAcl = New-Object System.Security.AccessControl.DirectorySecurity
        $taskAcl.SetAccessRuleProtection($true, $false)
        $taskIdentity = [Security.Principal.WindowsIdentity]::GetCurrent().User
        $taskRule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            $taskIdentity, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $taskAcl.AddAccessRule($taskRule)
        Set-Acl -LiteralPath $taskTemporaryDirectory -AclObject $taskAcl
        $passwordFile = Join-Path $taskTemporaryDirectory 'password.txt'
        $securePassword = Read-Host '请输入机器人 SSH 密码（本次运行只输入一次）' -AsSecureString
        $passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($securePassword)
        try {
            $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer)
            [IO.File]::WriteAllText($passwordFile, $plainPassword + "`n", (New-Object Text.UTF8Encoding($false)))
        } finally {
            [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer)
            $plainPassword = $null
            $securePassword.Dispose()
        }
        $arguments += @('--password-file', $passwordFile)
    }
    & $python.Source @arguments
    $taskExitCode = $LASTEXITCODE
} catch {
    Write-Host ("同步失败：" + $_.Exception.Message) -ForegroundColor Red
} finally {
    $env:PYTHONUTF8 = $previousPythonUtf8
    if ($taskTemporaryDirectory) {
        # Delete only the credential file and its now-empty, exact directory.
        if (Test-Path -LiteralPath (Join-Path $taskTemporaryDirectory 'password.txt')) {
            Remove-Item -LiteralPath (Join-Path $taskTemporaryDirectory 'password.txt') -Force
        }
        Remove-Item -LiteralPath $taskTemporaryDirectory -Force
    }
}
exit $taskExitCode
