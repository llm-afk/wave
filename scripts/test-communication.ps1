$ErrorActionPreference = 'Stop'
$waveRoot = Split-Path -Parent $PSScriptRoot
$waveBuild = Join-Path $waveRoot 'build'
$source = $null
$viewer = $null
$blocker = $null
function Start-Viewer {
    return Start-Process -FilePath (Join-Path $waveBuild 'WaveDemo.exe') -ArgumentList '--smoke-test' -WorkingDirectory $waveBuild -WindowStyle Hidden -PassThru
}
function Read-Report($process) {
    if (!$process.WaitForExit(20000)) { throw 'Wave smoke timed out' }
    if ($process.ExitCode -ne 0) { throw "Wave failed: $($process.ExitCode)" }
    return Get-Content (Join-Path $waveBuild 'smoke-test.json') -Raw | ConvertFrom-Json
}
try {
    if (@([System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners() | Where-Object Port -eq 18765).Count) {
        throw 'Close the existing receiver before running communication tests.'
    }
    $viewer = Start-Viewer
    $report = Read-Report $viewer
    if ($report.tcp_frames -ne 0 -or $report.max_submitted_plot_points -ne 0) { throw 'Unexpected internally generated data' }
    Write-Host 'PASS empty Wave does not generate samples.'
    $source = Start-Process -FilePath (Join-Path $waveBuild 'SystemSource.exe') -ArgumentList '5' -WorkingDirectory $waveBuild -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $waveBuild 'source-test.log') -RedirectStandardError (Join-Path $waveBuild 'source-test-error.log')
    Start-Sleep -Seconds 2
    if ($source.HasExited) { throw 'Source did not wait for Wave' }
    $blocker = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback,18765)
    $blocker.Start()
    $viewer = Start-Viewer
    Start-Sleep -Seconds 2
    if ($viewer.HasExited) { throw 'Wave exited on port conflict' }
    $blocker.Stop(); $blocker = $null
    $report = Read-Report $viewer
    if ($report.tcp_frames -lt 5 -or $report.max_submitted_plot_points -ne 0) { throw 'Expected reception without automatically selecting any channel' }
    if ($source.HasExited) { throw 'Source exited with Wave' }
    Write-Host 'PASS source-first startup, port-conflict recovery and reception without auto-selection.'
    $viewer = Start-Viewer
    $report = Read-Report $viewer
    if ($report.tcp_frames -lt 5 -or $source.HasExited) { throw 'Source did not reconnect after Wave restarted' }
    Write-Host 'PASS independent lifetimes and reconnect after receiver restart.'
} finally {
    if ($null -ne $blocker) { $blocker.Stop() }
    foreach ($process in @($viewer,$source)) {
        if ($null -ne $process -and !$process.HasExited) { $process.Kill(); $process.WaitForExit() }
    }
}
