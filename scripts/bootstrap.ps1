[CmdletBinding()]
param([switch]$SkipToolchain)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProgressPreference = 'SilentlyContinue'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

# PowerShell 5.1 on older Windows installations may not select TLS 1.2 by default.
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

$packages = @(
    @{
        Name = 'Dear ImGui v1.92.9b-docking'
        Url = 'https://codeload.github.com/ocornut/imgui/zip/refs/tags/v1.92.9b-docking'
        Sha256 = '0434445157A575F452FF0F2D1681FDD90EA8939E0C6983E6F8E47C51FBA1BCCD'
        Archive = 'third_party/imgui.zip'
        Destination = 'third_party'
        Sentinel = 'third_party/imgui-1.92.9b-docking/imgui.cpp'
    },
    @{
        Name = 'ImPlot v1.0'
        Url = 'https://codeload.github.com/epezent/implot/zip/refs/tags/v1.0'
        Sha256 = 'C9E697DB6EAAFE5E944EEAAE011ACE1594447EA1D306997EBD4CC16F87CAEF30'
        Archive = 'third_party/implot.zip'
        Destination = 'third_party'
        Sentinel = 'third_party/implot-1.0/implot.cpp'
    }
)

if (-not $SkipToolchain) {
    $packages += @{
        Name = 'LLVM MinGW 20260922 UCRT x86_64 (portable)'
        Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip'
        Sha256 = 'E3AD77D117A4BEA19A7A3B333341824D79A5A371004A10E25B8504E7B3047666'
        Archive = '.tools/llvm-mingw.zip'
        Destination = '.tools'
        Sentinel = '.tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe'
    }
}

try {
    foreach ($package in $packages) {
        $sentinelPath = Join-Path $projectRoot $package.Sentinel
        if (Test-Path -LiteralPath $sentinelPath -PathType Leaf) {
            Write-Host ($package.Name + ' is already available.')
            continue
        }

        $archivePath = Join-Path $projectRoot $package.Archive
        $destinationPath = Join-Path $projectRoot $package.Destination
        New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
        $archiveValid = $false
        if (Test-Path -LiteralPath $archivePath -PathType Leaf) {
            $archiveValid = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -eq $package.Sha256
        }
        if (-not $archiveValid) {
            Write-Host ('Downloading ' + $package.Name + ' ...')
            Invoke-WebRequest -Uri $package.Url -OutFile $archivePath -UseBasicParsing
        }

        $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
        if ($actualHash -ne $package.Sha256) {
            throw ('SHA-256 mismatch for ' + $package.Name + '. Expected ' + $package.Sha256 + ', received ' + $actualHash + '. The archive was not extracted.')
        }
        Write-Host ('Extracting verified archive: ' + $package.Name)
        Expand-Archive -LiteralPath $archivePath -DestinationPath $destinationPath -Force
        if (-not (Test-Path -LiteralPath $sentinelPath -PathType Leaf)) {
            throw ('Archive extraction did not produce the expected file: ' + $package.Sentinel)
        }
    }
    Write-Host 'Dependencies are ready. Nothing was installed system-wide.' -ForegroundColor Green
    Write-Host 'Next: powershell -ExecutionPolicy Bypass -File build.ps1'
    exit 0
} catch {
    Write-Host ('Bootstrap failed: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
