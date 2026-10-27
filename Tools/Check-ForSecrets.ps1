<#
.SYNOPSIS
    发布前自检:扫描仓库里形似凭据的字符串(重点是 API Key),防止密钥被提交进 git。

.DESCRIPTION
    默认只扫描 git 已跟踪的文件,二进制也扫 —— 这一步是必须的:UE 会把 FString 属性以 ASCII 明文
    写进 .uasset,当年这个项目的 LLM_API_Key 就是这样落进
    Content/__ExternalActors__/** 并被提交的。

    命中即返回退出码 1,所以可以直接当 pre-commit 钩子用(-InstallHook 一键安装)。

    只匹配"高置信度"的凭据形态,不做泛化的 token=/key= 匹配,以免误报把钩子变成噪音。
    已知的有意忽略:Config/DefaultEngine.ini 里 AndroidFileServer 的 SecurityToken
    (本机自动生成、用于局域网连接,不是云端凭据),可按需自行加规则。

.PARAMETER ScanHistory
    额外扫描 git 历史里的文本文件。较慢,发布前跑一次即可(二进制历史不做扫描)。

.PARAMETER IncludeUntracked
    额外扫描"未被 ignore 的未跟踪文件",这样新文件在 git add 之前就能被拦住。

.PARAMETER InstallHook
    挂上仓库里的 Tools/githooks/pre-commit(设置 core.hooksPath),之后每次 commit 自动扫描。

.EXAMPLE
    pwsh -NoProfile -ExecutionPolicy Bypass -File Tools\Check-ForSecrets.ps1
    pwsh -NoProfile -ExecutionPolicy Bypass -File Tools\Check-ForSecrets.ps1 -IncludeUntracked -ScanHistory
    pwsh -NoProfile -ExecutionPolicy Bypass -File Tools\Check-ForSecrets.ps1 -InstallHook

.NOTES
    本文件必须保存为 UTF-8 with BOM。Windows PowerShell 5.1 在没有 BOM 时会按系统 ANSI(简体中文
    系统上是 GBK)解析 .ps1,脚本里的中文注释会直接导致语法错误。

    命中后的处理顺序:先去服务商后台吊销/轮换密钥(改文件删提交都不算数,历史里还有),
    再清理文件,最后跑一次 -ScanHistory 确认历史里也没有。
#>
[CmdletBinding()]
param(
    [switch]$ScanHistory,
    [switch]$IncludeUntracked,
    [switch]$InstallHook
)

$ErrorActionPreference = 'Stop'

# 让 git 输出的 UTF-8 路径(含中文)被正确解码
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# git grep 未命中时返回 1,这是正常情况,不能让非零退出码变成异常(PS 7 才需要这行)。
if (Test-Path Variable:PSNativeCommandUseErrorActionPreference) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$Patterns = [ordered]@{
    'OpenAI / DashScope key' = 'sk-[A-Za-z0-9]{16,}'
    'AWS access key id'      = 'AKIA[0-9A-Z]{16}'
    'GitHub token'           = 'gh[pousr]_[A-Za-z0-9]{20,}'
    'Slack token'            = 'xox[baprs]-[A-Za-z0-9-]{10,}'
    'private key block'      = '-----BEGIN [A-Z ]*PRIVATE KEY-----'
}
$Regex = ($Patterns.Values -join '|')

function Get-Masked {
    param([string]$Text)
    $m = [regex]::Match($Text, $Regex)
    if (-not $m.Success) { return '' }
    $v = $m.Value
    if ($v.Length -le 12) { return ('*' * $v.Length) }
    return $v.Substring(0, 8) + ('*' * 6) + $v.Substring($v.Length - 4)
}

function Find-InFile {
    # .uasset 这类二进制里,UE 的 FString 可能是 ASCII,也可能是 UTF-16LE,两种都查。
    param([string]$Path)
    try { $bytes = [System.IO.File]::ReadAllBytes($Path) } catch { return $null }
    foreach ($enc in @([System.Text.Encoding]::ASCII, [System.Text.Encoding]::Unicode)) {
        $text = $enc.GetString($bytes)
        if ([regex]::IsMatch($text, $Regex)) { return (Get-Masked $text) }
    }
    return $null
}

$root = (& git rev-parse --show-toplevel 2>$null)
if (-not $root) { Write-Error 'not inside a git repository.'; exit 2 }
$root = $root.Trim()

Push-Location $root
try {
    if ($InstallHook) {
        # 钩子是受版本控制的仓库文件 Tools/githooks/pre-commit,安装 = 设置 core.hooksPath。
        # 不写 .git/hooks/:那种钩子只存在于本机,克隆后就没了,也无法被 review。
        $hookFile = Join-Path $root 'Tools/githooks/pre-commit'
        if (-not (Test-Path -LiteralPath $hookFile)) {
            Write-Host "[hook] $hookFile is missing - it should be tracked in the repository." -ForegroundColor Red
            exit 2
        }
        & git config --local core.hooksPath 'Tools/githooks'
        if ($LASTEXITCODE -eq 0) {
            Write-Host '[hook] installed: core.hooksPath = Tools/githooks' -ForegroundColor Green
        } else {
            Write-Host '[hook] could not write .git/config here. Run this once yourself:' -ForegroundColor Yellow
            Write-Host '       git config --local core.hooksPath Tools/githooks' -ForegroundColor Yellow
        }
    }

    $findings = New-Object System.Collections.Generic.List[object]

    Write-Host '[scan] git tracked files (binary included)...' -ForegroundColor DarkGray
    $tracked = & git -c core.quotepath=false grep -a -l -E $Regex -- .
    if ($tracked) {
        foreach ($f in $tracked) { $findings.Add((New-Object psobject -Property @{ Kind = 'tracked'; Path = $f; Hit = '' })) }
    }

    if ($IncludeUntracked) {
        Write-Host '[scan] untracked, not ignored...' -ForegroundColor DarkGray
        $others = & git -c core.quotepath=false ls-files --others --exclude-standard
        foreach ($f in $others) {
            $full = Join-Path $root $f
            if (Test-Path -LiteralPath $full -PathType Leaf) {
                $hit = Find-InFile $full
                if ($hit) { $findings.Add((New-Object psobject -Property @{ Kind = 'untracked'; Path = $f; Hit = $hit })) }
            }
        }
    }

    if ($ScanHistory) {
        Write-Host '[scan] git history, text files only (slow)...' -ForegroundColor DarkGray
        $hist = & git -c core.quotepath=false log --all -p --no-color -- '*.ini' '*.h' '*.hpp' '*.cpp' '*.cs' '*.md' '*.json' '*.uproject' '*.txt' '*.yml' '*.yaml'
        foreach ($line in $hist) {
            if ($line -match $Regex) {
                $findings.Add((New-Object psobject -Property @{ Kind = 'history'; Path = '(git log -p)'; Hit = (Get-Masked $line) }))
            }
        }
    }

    Write-Host ''
    if ($findings.Count -eq 0) {
        Write-Host '[OK] no credential-shaped strings found.' -ForegroundColor Green
        exit 0
    }

    Write-Host ("[!] {0} suspected credential(s):" -f $findings.Count) -ForegroundColor Red
    foreach ($f in $findings) {
        if ($f.Hit) {
            Write-Host ("  - [{0}] {1}  ({2})" -f $f.Kind, $f.Path, $f.Hit) -ForegroundColor Yellow
        } else {
            Write-Host ("  - [{0}] {1}" -f $f.Kind, $f.Path) -ForegroundColor Yellow
        }
    }
    Write-Host ''
    Write-Host 'Fix order: 1) revoke/rotate the key at the provider (the only real fix)' -ForegroundColor Cyan
    Write-Host '           2) clean the file   3) re-run with -ScanHistory' -ForegroundColor Cyan
    Write-Host 'Deleting a file or a commit is NOT a fix: the key stays in history.' -ForegroundColor Cyan
    exit 1
}
finally {
    Pop-Location
}
