# Third-party components

The GUI dependencies and optional MinGW toolchain are pinned by version and
downloaded from their official GitHub repositories by `scripts/bootstrap.ps1`.
The script checks SHA-256 before extracting each downloaded archive. No
system-wide software is installed.

| Component | Version | Source | License |
| --- | --- | --- | --- |
| Dear ImGui | v1.92.9b-docking | https://github.com/ocornut/imgui | MIT; `third_party/imgui-1.92.9b-docking/LICENSE.txt` |
| ImPlot | v1.0 | https://github.com/epezent/implot | MIT; `third_party/implot-1.0/LICENSE` |
| LLVM MinGW portable toolchain | 20260922, UCRT x86_64 | https://github.com/mstorsjo/llvm-mingw | Toolchain components have their own licenses; retain the distribution's license notices when redistributing it. |
| Microsoft C++ Build Tools (local build alternative) | MSVC toolset 14.44.35207, compiler 19.44.35229 | https://visualstudio.microsoft.com/visual-cpp-build-tools/ | Microsoft license terms apply; build tool only, not included in the application. |
| Windows SDK (local build alternative) | 10.0.26100.0 | https://developer.microsoft.com/windows/downloads/windows-sdk/ | Microsoft SDK license terms apply; build headers and libraries only. |

`build-msvc.ps1` uses the Microsoft compiler and SDK packages already prepared
inside `.tools` on this machine. It also accepts explicit MSVC and SDK paths.
`scripts/bootstrap.ps1` downloads the MinGW alternative; it does not provision
MSVC or the Windows SDK. Windows application-control policy can reject the
unsigned MinGW tools on some machines; the working local build uses MSVC.

The build scripts statically link the selected C++ runtime (`/MT` for MSVC or
`-static` for MinGW) and use Windows system DLLs for Win32 and DirectX 11. The
downloaded compiler is a build tool, not an application runtime dependency.
Include the ImGui and ImPlot license files when redistributing a compiled
application. Do not bundle the `.tools` directory with the application.
