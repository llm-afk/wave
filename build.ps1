[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = $PSScriptRoot
$buildDirectory = Join-Path $projectRoot 'build'
$objectDirectory = Join-Path $buildDirectory 'obj'
$imguiDirectory = Join-Path $projectRoot 'third_party/imgui-1.92.9b-docking'
$implotDirectory = Join-Path $projectRoot 'third_party/implot-1.0'
$portableCompiler = Join-Path $projectRoot '.tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe'

try {
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
    foreach ($headerRoot in @((Join-Path $projectRoot 'src'), $imguiDirectory, $implotDirectory)) {
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

    function Link-Binary([string]$executablePath, [string[]]$objects, [string[]]$extraFlags) {
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

    $backendObjects = @(
        (Build-Object 'third_party/imgui-1.92.9b-docking/backends/imgui_impl_win32.cpp'),
        (Build-Object 'third_party/imgui-1.92.9b-docking/backends/imgui_impl_dx11.cpp')
    )
    $guiLibraries = @('-ld3d11', '-ldxgi', '-ld3dcompiler', '-ldwmapi', '-lgdi32', '-luser32', '-limm32', '-lshell32', '-ladvapi32')
    $appObjects = $viewObjects + $backendObjects + @((Build-Object 'src/main.cpp'))
    Link-Binary (Join-Path $buildDirectory 'WaveDemo.exe') $appObjects (@('-mwindows', '-lws2_32') + $guiLibraries)
    Link-Binary (Join-Path $buildDirectory 'SystemSource.exe') @((Build-Object 'examples/system_source.cpp')) @('-lws2_32')
    Link-Binary (Join-Path $buildDirectory 'AudioSource.exe') @((Build-Object 'examples/audio_source.cpp')) @('-lws2_32', '-lole32', '-luuid')
    $nativeImport = Join-Path $buildDirectory 'libWave.dll.a'
    if (-not (Test-Path -LiteralPath $nativeImport)) { $forceRebuild = $true }
    $nativeObjects = $viewObjects + $backendObjects + @((Build-Object 'src/wave_native.cpp'))
    Link-Binary (Join-Path $buildDirectory 'Wave.dll') $nativeObjects (@('-shared', ('-Wl,--out-implib,' + $nativeImport)) + $guiLibraries)
    Link-Binary (Join-Path $buildDirectory 'CppHostDemo.exe') @((Build-Object 'examples/cpp_host_demo.cpp'), $nativeImport) @('-mwindows', '-luser32', '-lgdi32')

    [IO.File]::WriteAllText($configurationFile, $configuration)
    Write-Host 'Build completed.' -ForegroundColor Green
    Write-Host ('Run: ' + (Join-Path $buildDirectory 'WaveDemo.exe'))
    exit 0
} catch {
    Write-Host ('Build failed: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
