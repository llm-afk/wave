# Wave：可嵌入的基础波形组件

C++20 / Dear ImGui / ImPlot。独立演示程序使用 Windows / DirectX 11。

第一阶段保留单图实时显示、坐标轴、通道名称与颜色、采样点、有界环形缓存、按像素降采样和批量数据的平滑显示时钟。数据显示采用原始采样或首点/最小值/最大值/末点摘要，缩小时保留尖峰，绘图区边缘保留相邻真实点以裁剪连线。

支持轴区域和绘图区滚轮缩放、右键拖动缩放、左键平移和双击暂停。演示程序显示单一实时波形，绘图组件由宿主安排布局，图例显示名称和颜色。

默认每通道保留 50,000 帧。总计 20 通道、每通道最高 50 kSa/s 是暂定的设计负载。通道与批次结构、迁移顺序和现行 v2 边界见 [组件数据交互设计草案](examples/PROTOCOL.md#component-data-design)。

## 运行演示程序

双击 `run.cmd`，或运行 `build/WaveDemo.exe`。它只启动一个波形窗口，默认监听本机 TCP 18765，等待外部数据，不生成模拟信号。

单独运行 `run-source.cmd` 可发送真实 CPU/物理内存占用率；数据注册后默认显示所有通道，无需右键选择。两边独立启动、独立退出，数据源在 Wave 未启动时等待并重连。见 [系统数据源](examples/SYSTEM_SOURCE.md)。

第一笔波形从左侧开始，横轴保留原始时间刻度。默认视窗为 10 秒，等间隔数据取 10 秒和“缓存帧数 ÷ 采样率”中的较小值。同图有多个在线来源时，由可见来源中最短的缓存时长决定上限。50,000 帧在 1、10、50 kSa/s 下分别对应 10、5、1 秒的默认视窗。接收时间不等间隔的来源先用 10 秒，缓存满后结合实际首末时间取较小值。视窗填满后，波形末端贴着右边滚动。

Y 轴自动适配当前视窗里所有可见波形。最高点到最低点占绘图区高度的目标为 80%，上下各留 10%。新峰值靠近边缘时立即调整，持续约半秒的范围变化会触发收紧或重新居中，小幅波动保留当前刻度。恒定值居中显示。音频示例见 [音频数据源](examples/AUDIO_SOURCE.md)，通信格式见 [TCP v2](examples/PROTOCOL.md)。

| 操作区域 | 滚轮 | 右键拖动 |
| --- | --- | --- |
| X 轴下方刻度区域 | 只缩放 X | 向右放大、向左缩小 X，按下后锁定轴向 |
| Y 轴左侧刻度区域 | 只缩放 Y | 向上放大、向下缩小 Y，按下后锁定轴向 |
| 绘图区 | 同时缩放 X/Y | 横向缩放 X、纵向缩放 Y，可斜向同时操作 |

滚轮向上放大、向下缩小。实时状态左键拖动平移 Y，X 持续跟随；暂停后左键可平移 X/Y。轴刻度区域开始拖动时锁定该轴。右键拖动以按下时的尺度和锚点计算，拖回原位恢复尺度，松开不弹菜单。各实例互不联动。

在绘图区双击可暂停或恢复当前视图。暂停时保存只读数据，后台缓存继续接收并覆盖。再次双击回到最新缓存。自动 Y 随可见波形重新适配，手动 Y 保留用户设置。宿主可用 `set_paused()` 控制暂停，用 `set_pan_enabled()`、`set_pause_on_double_click()` 和 `set_zoom_enabled()` 分别控制交互。

波形用 55% 不透明度的细实线连接。采样点直径沿用缩小 30% 的设置。相邻采样的横向间距从 3 个点径缩到 0.75 个点径时，抗锯齿小点从 95% 不透明度逐渐收细、淡出。放大时反向过渡。不等间隔的数据按局部采样间距显点，密集突发和稀疏采样可以显示在同一屏。单个样本直接显示为点。

## 嵌入上位机

显示组件依赖 ImGui 和 ImPlot，当前可直接复用于同一 GUI 技术栈。宿主负责创建 ImGui/ImPlot 上下文、窗口或子区域、图形后端、帧循环以及自己的通信层；数据池必须比 WaveView 存活更久。Qt、WinForms 等上位机需要另做相应的渲染/窗口适配，这轮没有提供这些封装。

```cpp
#include "data_core.hpp"
#include "wave_view.hpp"
#include "imgui.h"

// 在初始化时创建，保持到窗口结束：
wave::DataStore data(20'000.0, 5.0, {
    {"Current", "A"}, {"Speed", "rpm"}
});
wave::WaveView view(data, {
    .time_span = 0.2,
    .y_min = -10.0,
    .y_max = 10.0
});
view.set_channel_visible(1, false); // 此图只显示电流

// 接收线程按批写入，帧优先、通道交错；调用返回后可复用输入内存：
std::vector<float> batch = {1.0f, 1000.0f, 1.1f, 1001.0f};
data.append(0, batch); // 下一批从第 2 帧开始

// 每一帧在宿主自己的窗口/子区域中绘制：
ImGui::Begin("Motor monitor");
ImGui::TextUnformatted("Phase current");
ImGui::BeginChild("Wave area", {0, 300});
view.draw();
ImGui::EndChild();
ImGui::End();

// 宿主需要更改显示范围时调用（UI 线程）：
view.set_time_span(1.0);
view.set_y_range(-20.0, 20.0);
```

直接构造 `WaveView(data)` 使用自动时间跨度和自动 Y。构造时显式提供 Y 范围、调用 `set_y_range()` 或手动平移、缩放 Y 会进入固定量程，`set_auto_y(true)` 恢复自动适配。只操作 X 轴时，自动 Y 继续生效。用鼠标或 `set_time_span()` 选择的时间跨度按用户设定显示。每个实例独立保存显示范围和可见通道，宿主可用 `set_channel_visible(index, bool)` 选择通道，索引对应 `channels()` 顺序。

`draw()` 只填充当前窗口或子区域的可用内容区。不要在同一帧重复绘制同一个实例；多个实例有各自的 ID，不需要组件内部的窗口管理。组件不自动应用全局主题，`apply_style()` 仅供宿主主动使用，演示程序在启动/DPI 改变时调用。

## 数据边界与保留能力

- `DataStore` 只负责采样缓存与查询，默认通道列表为空，不附带模拟信号或采集线程。配置在生产者开始前完成；数据写入与查询通过锁保护。
- 单个数据池接受固定频率的同步通道，时间为 `frame_index / sample_rate`；序号跳跃保留真实数据并连接缺口两侧，不补零。输入必须有限、长度必须是通道数的整数倍。
- `SignalHub` 支持不同频率/接收时间模式的多来源，映射到公共时间轴。通信线程与注册流程的约束见协议。可直接构造 `WaveView(hub, options)`，显示组件不依赖 TCP。
- 高密度查询按像素分桶、分层维护极值摘要；连续数据每条曲线通常最多约 `4 × 绘图区宽度` 个点，缺口/边缘可额外保留真实端点；放大尺度由宿主指定后可显示原始采样。
- 保留数据层的清空、容量和只读快照 API，供现有通信和宿主按需使用。WaveView 不提供清空或容量设置界面；双击暂停会建立一次只读快照，恢复后释放。TCP 的 CLEAR_CHANNEL/CLEAR_SOURCE 保持可用。
- `DisplayClock` 平滑规则批量数据的滚动显示时间，输入停止后停在最后一个真实采样点。

## CMake 集成

上位机只需链接 `Wave::view`：

```cmake
# 子目录方式默认只构建核心库，不启动/构建演示程序和测试。
add_subdirectory(path/to/wave)
target_link_libraries(MyHost PRIVATE Wave::view)
```

`Wave::data` 是无 GUI 的数据层。只有独立演示程序和通信测试链接 Winsock；仅链接 `Wave::view` 不会带入 TCP 接收器、Win32/DX11 后端或示例采集程序。

默认使用固定版本的 ImGui v1.92.9b-docking / ImPlot v1.0。若宿主已有兼容的 ImGui/ImPlot 合并目标，在 `add_subdirectory` 前设置 `WAVE_PLOT_TARGET` 为该目标名，以避免编译第二份 GUI 库；它须公开这两个库的头文件与链接依赖。组件使用该固定版本的 ImPlotSpec API，升级需要验证兼容性。

独立配置时也可显式关闭可执行程序：

```powershell
cmake -S . -B build/core-only -DWAVE_BUILD_DEMO=OFF -DWAVE_BUILD_EXAMPLES=OFF -DWAVE_BUILD_TESTS=OFF
cmake --build build/core-only --config Release
```

## 项目结构

```text
src/data_core.hpp/.cpp     环形缓存、校验、极值查询及数据层 API
src/wave_view.hpp/.cpp     单图实时显示与宿主配置 API
src/signal_hub.hpp         多来源注册与公共时间轴
src/display_clock.hpp      批量数据的平滑显示时间
src/plot_navigation.hpp    轴区域/绘图区缩放的纯数学计算
src/main.cpp               独立演示：原生窗口、DX11、帧循环和 TCP
src/tcp_source.hpp         可选 TCP 接收适配器
src/wave_protocol.hpp      TCP v2 编解码
examples/                  外部数据源、Python 客户端及协议
tests/                     数据、时钟、嵌入显示、TCP 测试
tests/simulation_source.*  仅测试使用的模拟源，不编入核心库
```

## 构建与验证

本项目已准备 LLVM-MinGW 工具链：

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

默认构建演示程序、两个独立数据源及四组测试，并运行测试。`-TestOnly` 只构建/运行测试，不编译原生图形后端；`-SkipTests` 只构建程序。有对应微软工具链时也可使用 `build-msvc.ps1`，或通过 CMake 构建。首次准备依赖见 `scripts/bootstrap.ps1`。发布时保留第三方许可证，见 [THIRD_PARTY.md](THIRD_PARTY.md)。

验证包含：真实 TCP 注册/重连/校验、缓存覆盖/极值/线程安全、显示时钟，以及无原生窗口的真实 ImGui/ImPlot 集成测试。后者检查空图/快速音频尺度的布局稳定、快照限速/跳过积压、同源通道读取一致、暂停后台覆盖/恢复最新、默认显示、宿主通道筛选、轴区域/绘图区滚轮与右键拖动、已移除的其他输入操作不再生效、多个独立实例、宿主控件仍可交互以及按像素限制绘图点数。

`WaveDemo.exe --smoke-test` 使用实际 DX11 渲染约 12 秒并在工作目录输出 `smoke-test.json`。无来源时点数为零，有来源时默认绘制。Python 接入检查：`python tests/python_client_test.py`（可加 `--receiver-time`）。这些检查不代表长期负载或真实设备链路验证。
