[CmdletBinding()]
param(
    [switch]$TestOnly,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'
$objectDirectory = Join-Path $buildDirectory 'obj'
$imguiDirectory = Join-Path $projectRoot 'third_party/imgui-1.92.9b-docking'
$implotDirectory = Join-Path $projectRoot 'third_party/implot-1.0'
$portableCompiler = Join-Path $projectRoot '.tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe'

try {
    if ($TestOnly -and $SkipTests) { throw '-TestOnly and -SkipTests cannot be used together.' }
    if (Test-Path -LiteralPath $portableCompiler -PathType Leaf) {
        $compiler = $portableCompiler
    } else {
        $compilerCommand = Get-Command 'clang++.exe' -ErrorAction SilentlyContinue
        if ($null -eq $compilerCommand) {
            throw 'No LLVM MinGW compiler found. Run: powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1 . For Visual Studio, use the CMake instructions in README.md.'
        }
        $compiler = $compilerCommand.Source
        $compilerTarget = (& $compiler -dumpmachine | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $compilerTarget -notmatch 'mingw|windows-gnu') {
            throw "PATH clang++ does not target MinGW ($compilerTarget). Run scripts\bootstrap.ps1 or use CMake with Visual Studio."
        }
    }

    foreach ($requiredPath in @((Join-Path $imguiDirectory 'imgui.cpp'), (Join-Path $implotDirectory 'implot.cpp'))) {
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw 'GUI dependencies are missing. Run: powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1'
        }
    }

    New-Item -ItemType Directory -Path $objectDirectory -Force | Out-Null
    $commonFlags = @('-std=c++20', '-O2', '-g', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DUNICODE', '-D_UNICODE', '-Wall', '-Wextra')
    $includeFlags = @('-I', (Join-Path $projectRoot 'src'), '-I', $imguiDirectory, '-I', (Join-Path $imguiDirectory 'backends'), '-I', $implotDirectory)
    $configurationFile = Join-Path $objectDirectory 'configuration.txt'
    $scriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    $configuration = (@($compiler, $scriptHash) + $commonFlags + $includeFlags) -join "`n"
    $forceRebuild = -not (Test-Path -LiteralPath $configurationFile)
    if (-not $forceRebuild) {
        $forceRebuild = [IO.File]::ReadAllText($configurationFile) -ne $configuration
    }

    # Headers are few; a shared latest timestamp also handles transitive includes.
    $latestHeaderTime = [DateTime]::MinValue
    foreach ($headerRoot in @((Join-Path $projectRoot 'src'), (Join-Path $projectRoot 'tests'), $imguiDirectory, $implotDirectory)) {
        if (Test-Path -LiteralPath $headerRoot -PathType Container) {
            foreach ($header in Get-ChildItem -LiteralPath $headerRoot -Recurse -File | Where-Object { $_.Extension -in @('.h', '.hpp', '.inl') }) {
                if ($header.LastWriteTimeUtc -gt $latestHeaderTime) { $latestHeaderTime = $header.LastWriteTimeUtc }
            }
        }
    }

    function Build-Object([string]$relativeSource) {
        $sourcePath = Join-Path $projectRoot $relativeSource
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { throw "Source file is missing: $relativeSource" }
        $objectName = ($relativeSource -replace '[^a-zA-Z0-9_.-]', '_') + '.o'
        $objectPath = Join-Path $objectDirectory $objectName
        $needsCompile = $forceRebuild -or -not (Test-Path -LiteralPath $objectPath -PathType Leaf)
        if (-not $needsCompile) {
            $objectTime = (Get-Item -LiteralPath $objectPath).LastWriteTimeUtc
            $needsCompile = (Get-Item -LiteralPath $sourcePath).LastWriteTimeUtc -gt $objectTime -or $latestHeaderTime -gt $objectTime
        }
        if ($needsCompile) {
            Write-Host "Compiling $relativeSource"
            & $compiler @commonFlags @includeFlags -c $sourcePath -o $objectPath
            if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $relativeSource (exit $LASTEXITCODE)" }
        }
        return $objectPath
    }

    function Link-Executable([string]$executablePath, [string[]]$objects, [string[]]$extraFlags) {
        $needsLink = $forceRebuild -or -not (Test-Path -LiteralPath $executablePath -PathType Leaf)
        if (-not $needsLink) {
            $executableTime = (Get-Item -LiteralPath $executablePath).LastWriteTimeUtc
            foreach ($object in $objects) {
                if ((Get-Item -LiteralPath $object).LastWriteTimeUtc -gt $executableTime) { $needsLink = $true; break }
            }
        }
        if ($needsLink) {
            Write-Host "Linking $([IO.Path]::GetFileName($executablePath))"
            & $compiler @objects -static @extraFlags -o $executablePath
            if ($LASTEXITCODE -ne 0) { throw "Link failed: $executablePath (exit $LASTEXITCODE). Close the running application before rebuilding." }
        }
    }

    $coreObject = Build-Object 'src/data_core.cpp'
    # Reuse the view/plot objects without native backends in headless tests.
    $viewObjects = @($coreObject)
    foreach ($source in @(
        'src/wave_view.cpp',
        'third_party/imgui-1.92.9b-docking/imgui.cpp',
        'third_party/imgui-1.92.9b-docking/imgui_draw.cpp',
        'third_party/imgui-1.92.9b-docking/imgui_tables.cpp',
        'third_party/imgui-1.92.9b-docking/imgui_widgets.cpp',
        'third_party/implot-1.0/implot.cpp',
        'third_party/implot-1.0/implot_items.cpp'
    )) { $viewObjects += Build-Object $source }

    if (-not $TestOnly) {
        $appObjects = $viewObjects + @(
            (Build-Object 'src/main.cpp'),
            (Build-Object 'third_party/imgui-1.92.9b-docking/backends/imgui_impl_win32.cpp'),
            (Build-Object 'third_party/imgui-1.92.9b-docking/backends/imgui_impl_dx11.cpp')
        )
        Link-Executable (Join-Path $buildDirectory 'WaveDemo.exe') $appObjects @('-mwindows', '-ld3d11', '-ldxgi', '-ld3dcompiler', '-ldwmapi', '-lgdi32', '-luser32', '-limm32', '-lshell32', '-lws2_32')
        Link-Executable (Join-Path $buildDirectory 'SystemSource.exe') @((Build-Object 'examples/system_source.cpp')) @('-lws2_32')
        Link-Executable (Join-Path $buildDirectory 'AudioSource.exe') @((Build-Object 'examples/audio_source.cpp')) @('-lws2_32', '-lole32', '-luuid')
    }

    if (-not $SkipTests) {
        $tcpTest = Join-Path $buildDirectory 'tcp_source_tests.exe'
        Link-Executable $tcpTest @($coreObject, (Build-Object 'tests/tcp_source_tests.cpp')) @('-lws2_32')
        & $tcpTest
        if ($LASTEXITCODE -ne 0) { throw 'TCP source tests failed.' }
        $testObjects = @($coreObject, (Build-Object 'tests/data_core_tests.cpp'), (Build-Object 'tests/simulation_source.cpp'))
        $testExecutable = Join-Path $buildDirectory 'data_core_tests.exe'
        Link-Executable $testExecutable $testObjects @()
        Write-Host 'Running data core tests...'
        & $testExecutable
        if ($LASTEXITCODE -ne 0) { throw "Data core tests failed (exit $LASTEXITCODE)." }

        $clockObjects = @((Build-Object 'tests/display_clock_tests.cpp'))
        $clockExecutable = Join-Path $buildDirectory 'display_clock_tests.exe'
        Link-Executable $clockExecutable $clockObjects @()
        Write-Host 'Running display clock tests...'
        & $clockExecutable
        if ($LASTEXITCODE -ne 0) { throw "Display clock tests failed (exit $LASTEXITCODE)." }

        $viewTestObjects = $viewObjects + @((Build-Object 'tests/view_tests.cpp'))
        $viewTestExecutable = Join-Path $buildDirectory 'view_tests.exe'
        Link-Executable $viewTestExecutable $viewTestObjects @('-luser32', '-limm32', '-lshell32')
        Write-Host 'Running headless view tests...'
        & $viewTestExecutable
        if ($LASTEXITCODE -ne 0) { throw "View tests failed (exit $LASTEXITCODE)." }
    }

    [IO.File]::WriteAllText($configurationFile, $configuration)
    Write-Host 'Build completed.' -ForegroundColor Green
    if (-not $TestOnly) { Write-Host ('Run: ' + (Join-Path $buildDirectory 'WaveDemo.exe')) }
    exit 0
} catch {
    Write-Host ('Build failed: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
