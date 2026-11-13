# Wave 嵌入与交互设计

当前框架面向一个宿主界面、一块多通道波形绘图区。宿主负责设备采集、界面布局和消息循环，Wave 负责缓存、按视区查询及波形交互。C++/Win32 与 Python/Tkinter 两个 Demo 用来验证这条接入链。

## 今天可以怎样接入

同进程 C++ 宿主可链接 `Wave::data`，用 `DataStore::configure()` 注册通道，采集线程按批调用 `append()`。等间隔数据按帧交错，例如两通道两帧为 `[A0, B0, A1, B1]`。调用返回后可复用输入内存。使用 ImGui/ImPlot 的宿主再链接 `Wave::view`，在自己的窗口或子区域逐帧调用 `WaveView::draw()`。`WaveView::state()` 可读取用户操作后的范围、暂停和自动 Y 状态。[接入代码和 CMake 用法](../README.md#嵌入上位机)

Windows 上还可使用 `Wave.dll`。导出的 C ABI v1 见 [wave_native.h](../src/wave_native.h)，Python 标准库封装见 [wave_native.py](../python/wave_native.py)。组件创建属于宿主的 `WS_CHILD` 绘图窗口，每个窗口拥有独立的 ImGui/ImPlot 上下文、GPU 资源和显示状态。宿主提供父窗口句柄、位置和渲染定时器，DLL 内每次调用和窗口消息处理结束后恢复之前的 GUI 上下文。C++/Win32 与 Python/Tkinter 两个可运行 Demo 使用同一个 DLL。[ctypes 文档](https://docs.python.org/3/library/ctypes.html) · [Win32 子窗口](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createwindowexw)

DLL 的所有视图使用同一系统单调时钟驱动共享缓存的滚动。直接使用多个 ImGui 上下文的 C++ 宿主可调用 `view.draw(fingers, monotonic_seconds)`，给同一数据会话传入一致的单调时间。单上下文宿主可以继续使用 `draw()`。

### Windows DLL 与 Python 的最小接入

将 `python/` 加到宿主的模块搜索路径，或将其中的接口模块放入宿主项目。给 `Library(path)` 传入匹配宿主位数的 DLL 路径。

```python
from array import array
from wave_native import Library

session = Library("path/to/Wave.dll").session(capacity_frames=50_000)
stream = session.open_stream("motor", [
    {"id": "ia", "name": "Phase A", "unit": "A"},
    {"id": "ib", "name": "Phase B", "unit": "A"},
], sample_rate=50_000)
view = session.create_window(parent_hwnd)  # 宿主 UI 线程
view.set_bounds(0, 0, width_pixels, height_pixels)

# 采集线程。交错 float32，返回后可复用输入数组。
next_frame = stream.append(array("f", [0.1, 0.2, 0.3, 0.4]))

# 宿主 UI 定时器，每帧一次。
view.render()
state = view.state()

# 退出时先停止采集线程和 UI 定时器。
view.close()  # 创建该视图的 UI 线程
stream.close()
session.close()
```

- 同一会话的注册、写入、容量切换和视图读数通过一把锁串行。写入调用直接提交到缓存。每个流保持一个活动写入句柄，重复 ID 的同时注册会报错。关闭后用同一 schema 重开可接着原序号写入。
- `append()` 接受 `array('f')`、连续 float32 NumPy 数组或普通平铺序列。可写连续 float32 缓冲区直接传指针，DLL 在调用内复制到环形缓存。只读缓冲区先作一次 Python 侧拷贝。显式传入 `first_frame` 时拒绝倒退序号和非法样本，失败返回前不接受该批次。
- `sample_rate=0` 的流调用 `append_received(values, seconds)`，每次提交一帧，各来源的秒数由宿主放在同一时间基准上。均匀采样源使用 `first_frame / sample_rate` 的时间。
- 数据会话、流和视图分别持有共享数据的所有权。关闭会话句柄后，已存在的流和视图仍持有数据。修改容量时流句柄在锁内重新定位数据池，暂停快照继续独立存在。释放某个句柄前应结束对它的并发调用，Python 封装还会串行该句柄的调用和 `close()`。
- 创建、改变位置、绘图、配置和销毁原生视图都在父窗口所属 UI 线程执行。宿主负责消息循环与 DPI 策略，尺寸按像素传入。隐藏页面时可停绘，恢复时调用 `render()`。父窗口先销毁后，应继续关闭 Wave 视图句柄以释放图形资源。[Tkinter 线程模型](https://docs.python.org/3/library/tkinter.html#threading-model)
- C 函数返回状态码，错误文字通过 `wave_last_error()` 在当前线程立即读取。异常在 DLL 边界转换为状态码。Python 封装为失败调用抛出异常，并检查 ABI 版本。
- 原始双指坐标按向右、向上为正处理。宿主可用 `set_reverse_scroll()` 调整转换后滚动消息的方向，两个 Demo 提供对应复选框。Demo 默认勾选，匹配当前远程桌面的实测方向。

两个 Demo 都使用后台生成的 2 通道、20 kSa/s 模拟数据，宿主顶栏可切换暂停、自动 Y 和时间跨度。它们分别验证 Win32 与 Tkinter 的实际子控件挂载。Qt/PySide 等其他宿主的框架生命周期和重绘适配待选定后验证。

直接使用 C++ `SignalHub` 时，注册和容量调整由宿主在 UI 线程串行执行。独立 Demo 的 TCP 数据经 `Receiver::poll()` 提交。元数据固定后的 `DataStore` 可从后台写入。DLL 接口在会话内补充串行保护。宿主复用自己的设备连接，外部进程供数可继续使用 TCP 适配器。[数据交互边界](PROTOCOL.md#component-data-design)

## 当前框架的职责

| 层 | 入口 | 职责 |
| --- | --- | --- |
| 数据 | `DataStore` / `SignalHub` | 通道元数据、批次校验、帧序号、共享有界缓存 |
| 接入 | C ABI / Python 封装 / TCP v2 | 把宿主数据交给同一数据层，回复成功或错误 |
| 显示 | `WaveView` / Windows 子控件 | 一块绘图区、多通道、显示范围与暂停状态 |
| 宿主 | C++ 或 Python GUI | 采集线程、布局、帧调度、窗口生命周期 |

采集适配器先注册一组同步通道，之后只提交交错 float32 批次。不同采样率的通道各自注册逻辑流，显示层从这些流读取需要的通道。框架边界与当前协议约定见 [数据交互边界](PROTOCOL.md#component-data-design)。

## 单图交互

| 操作 | 响应 |
| --- | --- |
| 双指或右键拖动 | 由两个方向的位移分别控制 X、Y 缩放，右上看细节，左下看全貌 |
| 左键拖动 | 实时平移 Y，暂停后平移 X/Y |
| 双击绘图区 | 暂停或恢复显示，后台继续接收，恢复时跟上最新缓存 |
| 宿主控件 | 调用与图内交互相同的配置入口，通过 `state()` 读取实际状态 |

原始双指位移向右、向上分别为正，按逻辑像素传给 `TouchpadDelta`。原生控件以子窗口接收位置、焦点、捕获和松开事件。宿主有自己的手势识别时可继续直接使用 C++ `WaveView`。

## 当前验证边界

已用 Windows C++/Win32、Python/Tkinter 验证父窗口内挂载、后台批量写入、缩放、暂停恢复、改变尺寸和销毁。用户界面各自只有一块波形区，Demo 使用两通道模拟采样。PySide/Qt 等宿主选型后可复用相同数据入口，并验证对应框架的生命周期与重绘。
