[CmdletBinding()]
param(
    [string]$MsvcRoot = '',
    [string]$SdkRoot = '',
    [string]$SdkVersion = '',
    [string]$SdkLibRoot = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDirectory = Join-Path $projectRoot 'build'
$objectDirectory = Join-Path $buildDirectory 'obj-msvc'
$imguiDirectory = Join-Path $projectRoot 'third_party/imgui-1.92.9b-docking'
$implotDirectory = Join-Path $projectRoot 'third_party/implot-1.0'
if ([string]::IsNullOrWhiteSpace($MsvcRoot)) {
    $MsvcRoot = Join-Path $projectRoot '.tools/recovery-msvc/Contents/VC/Tools/MSVC/14.44.35207'
}
if ([string]::IsNullOrWhiteSpace($SdkRoot)) {
    $SdkRoot = Join-Path $projectRoot '.tools/recovery-winsdk-cpp/c'
}
$originalPath = $env:PATH
$originalInclude = $env:INCLUDE
$originalLib = $env:LIB
$buildExitCode = 0

try {
    $MsvcRoot = [IO.Path]::GetFullPath($MsvcRoot)
    $SdkRoot = [IO.Path]::GetFullPath($SdkRoot)
    $compilerDirectory = Join-Path $MsvcRoot 'bin/Hostx64/x64'
    $compiler = Join-Path $compilerDirectory 'cl.exe'
    $linker = Join-Path $compilerDirectory 'link.exe'

    if ([string]::IsNullOrWhiteSpace($SdkVersion)) {
        $sdkIncludeRoot = Join-Path $SdkRoot 'Include'
        if (Test-Path -LiteralPath $sdkIncludeRoot -PathType Container) {
            $versions = @(Get-ChildItem -LiteralPath $sdkIncludeRoot -Directory | Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } | Sort-Object { [Version]$_.Name } -Descending)
            if ($versions.Count -gt 0) { $SdkVersion = $versions[0].Name }
        }
        if ([string]::IsNullOrWhiteSpace($SdkVersion)) {
            throw 'No Windows SDK version found. Supply -SdkRoot <Windows Kits\10 root> and -SdkVersion <version>.'
        }
    }

    $includeDirectories = @(
        (Join-Path $MsvcRoot 'include'),
        (Join-Path $SdkRoot "Include/$SdkVersion/ucrt"),
        (Join-Path $SdkRoot "Include/$SdkVersion/shared"),
        (Join-Path $SdkRoot "Include/$SdkVersion/um")
    )
    foreach ($optionalSdkInclude in @('winrt', 'cppwinrt')) {
        $optionalIncludePath = Join-Path $SdkRoot "Include/$SdkVersion/$optionalSdkInclude"
        if (Test-Path -LiteralPath $optionalIncludePath -PathType Container) { $includeDirectories += $optionalIncludePath }
    }
    if ([string]::IsNullOrWhiteSpace($SdkLibRoot)) {
        $standardSdkLibRoot = Join-Path $SdkRoot "Lib/$SdkVersion"
        if (Test-Path -LiteralPath $standardSdkLibRoot -PathType Container) {
            $SdkLibRoot = $standardSdkLibRoot
        } else {
            $SdkLibRoot = Join-Path $projectRoot '.tools/recovery-winsdk-x64/c'
        }
    }
    $SdkLibRoot = [IO.Path]::GetFullPath($SdkLibRoot)
    $libraryDirectories = @(
        (Join-Path $MsvcRoot 'lib/x64'),
        (Join-Path $SdkLibRoot 'ucrt/x64'),
        (Join-Path $SdkLibRoot 'um/x64')
    )
    foreach ($requiredPath in (@($compiler, $linker) + $includeDirectories + $libraryDirectories)) {
        if (-not (Test-Path -LiteralPath $requiredPath)) { throw "MSVC/Windows SDK component is missing: $requiredPath" }
    }
    foreach ($requiredPath in @((Join-Path $imguiDirectory 'imgui.cpp'), (Join-Path $implotDirectory 'implot.cpp'))) {
        if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
            throw 'GUI dependencies are missing. Run: powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1 -SkipToolchain'
        }
    }

    # All changes are confined to this build process; nothing is installed or
    # persisted in the user's or system's environment.
    $env:PATH = $compilerDirectory + ';' + $originalPath
    $env:INCLUDE = $includeDirectories -join ';'
    $env:LIB = $libraryDirectories -join ';'
    New-Item -ItemType Directory -Path $objectDirectory -Force | Out-Null
    $commonFlags = @('/nologo', '/c', '/std:c++20', '/O2', '/MT', '/EHsc', '/utf-8', '/W4', '/permissive-', '/Zc:__cplusplus', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DUNICODE', '/D_UNICODE')
    $includeFlags = @(('/I' + (Join-Path $projectRoot 'src')), ('/I' + $imguiDirectory), ('/I' + (Join-Path $imguiDirectory 'backends')), ('/I' + $implotDirectory))
    $configurationFile = Join-Path $objectDirectory 'configuration.txt'
    $scriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash
    $configuration = (@($compiler, $env:INCLUDE, $env:LIB, $scriptHash) + $commonFlags + $includeFlags) -join "`n"
    $forceRebuild = -not (Test-Path -LiteralPath $configurationFile)
    if (-not $forceRebuild) { $forceRebuild = [IO.File]::ReadAllText($configurationFile) -ne $configuration }

    $latestHeaderTime = [DateTime]::MinValue
    foreach ($headerRoot in @((Join-Path $projectRoot 'src'), $imguiDirectory, $implotDirectory)) {
        if (Test-Path -LiteralPath $headerRoot -PathType Container) {
            foreach ($header in Get-ChildItem -LiteralPath $headerRoot -Recurse -File | Where-Object { $_.Extension -in @('.h', '.hpp', '.inl') }) {
                if ($header.LastWriteTimeUtc -gt $latestHeaderTime) { $latestHeaderTime = $header.LastWriteTimeUtc }
            }
        }
    }

    function Build-MsvcObject([string]$relativeSource) {
        $sourcePath = Join-Path $projectRoot $relativeSource
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) { throw "Source file is missing: $relativeSource" }
        $objectName = ($relativeSource -replace '[^a-zA-Z0-9_.-]', '_') + '.obj'
        $objectPath = Join-Path $objectDirectory $objectName
        $needsCompile = $forceRebuild -or -not (Test-Path -LiteralPath $objectPath -PathType Leaf)
        if (-not $needsCompile) {
            $objectTime = (Get-Item -LiteralPath $objectPath).LastWriteTimeUtc
            $needsCompile = (Get-Item -LiteralPath $sourcePath).LastWriteTimeUtc -gt $objectTime -or $latestHeaderTime -gt $objectTime
        }
        if ($needsCompile) {
            Write-Host "Compiling $relativeSource"
            & $compiler @commonFlags @includeFlags $sourcePath ('/Fo' + $objectPath) | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $relativeSource (exit $LASTEXITCODE)" }
        }
        return $objectPath
    }

    function Link-MsvcBinary([string]$executablePath, [string[]]$objects, [string[]]$extraFlags) {
        $needsLink = $forceRebuild -or -not (Test-Path -LiteralPath $executablePath -PathType Leaf)
        if (-not $needsLink) {
            $executableTime = (Get-Item -LiteralPath $executablePath).LastWriteTimeUtc
            foreach ($object in $objects) {
                if ((Get-Item -LiteralPath $object).LastWriteTimeUtc -gt $executableTime) { $needsLink = $true; break }
            }
        }
        if ($needsLink) {
            Write-Host "Linking $([IO.Path]::GetFileName($executablePath))"
            & $linker /NOLOGO /MACHINE:X64 /INCREMENTAL:NO /OPT:REF /OPT:ICF @objects @extraFlags ('/OUT:' + $executablePath) | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "Link failed: $executablePath (exit $LASTEXITCODE). Close the running application before rebuilding." }
        }
    }

    $coreObject = Build-MsvcObject 'src/data_core.cpp'
    $viewObjects = @($coreObject)
    foreach ($source in @(
        'src/wave_view.cpp',
        'third_party/imgui-1.92.9b-docking/imgui.cpp',
        'third_party/imgui-1.92.9b-docking/imgui_draw.cpp',
        'third_party/imgui-1.92.9b-docking/imgui_tables.cpp',
        'third_party/imgui-1.92.9b-docking/imgui_widgets.cpp',
        'third_party/implot-1.0/implot.cpp',
        'third_party/implot-1.0/implot_items.cpp'
    )) { $viewObjects += Build-MsvcObject $source }

    $backendObjects = @(
        (Build-MsvcObject 'third_party/imgui-1.92.9b-docking/backends/imgui_impl_win32.cpp'),
        (Build-MsvcObject 'third_party/imgui-1.92.9b-docking/backends/imgui_impl_dx11.cpp')
    )
    $guiLibraries = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'dwmapi.lib', 'gdi32.lib', 'user32.lib', 'imm32.lib', 'shell32.lib', 'advapi32.lib')
    $appObjects = $viewObjects + $backendObjects + @((Build-MsvcObject 'src/main.cpp'))
    Link-MsvcBinary (Join-Path $buildDirectory 'WaveDemo.exe') $appObjects (@('/SUBSYSTEM:WINDOWS', 'ws2_32.lib') + $guiLibraries)
    $nativeImport = Join-Path $buildDirectory 'Wave.lib'
    if (-not (Test-Path -LiteralPath $nativeImport)) { $forceRebuild = $true }
    $nativeObjects = $viewObjects + $backendObjects + @((Build-MsvcObject 'src/wave_native.cpp'))
    Link-MsvcBinary (Join-Path $buildDirectory 'Wave.dll') $nativeObjects (@('/DLL', ('/IMPLIB:' + $nativeImport)) + $guiLibraries)
    Link-MsvcBinary (Join-Path $buildDirectory 'CppHostDemo.exe') @((Build-MsvcObject 'examples/cpp_host_demo.cpp'), $nativeImport) @('/SUBSYSTEM:WINDOWS', 'user32.lib', 'gdi32.lib')
    [IO.File]::WriteAllText($configurationFile, $configuration)
    Write-Host 'MSVC build completed.' -ForegroundColor Green
    Write-Host ('Run: ' + (Join-Path $buildDirectory 'WaveDemo.exe'))
} catch {
    Write-Host ('MSVC build failed: ' + $_.Exception.Message) -ForegroundColor Red
    $buildExitCode = 1
} finally {
    $env:PATH = $originalPath
    $env:INCLUDE = $originalInclude
    $env:LIB = $originalLib
}
exit $buildExitCode
