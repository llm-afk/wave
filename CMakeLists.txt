cmake_minimum_required(VERSION 3.20)
project(WaveDemo VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "WaveDemo currently uses Win32 + DirectX 11 and requires Windows.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui-1.92.9b-docking")
set(IMPLOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot-1.0")
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp" OR NOT EXISTS "${IMPLOT_DIR}/implot.cpp")
    message(FATAL_ERROR "Dependencies are missing. Run powershell -ExecutionPolicy Bypass -File scripts/bootstrap.ps1 -SkipToolchain first.")
endif()

add_library(wave_data STATIC src/data_core.cpp)
target_include_directories(wave_data PUBLIC src)

add_library(wave_plot STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMPLOT_DIR}/implot.cpp"
    "${IMPLOT_DIR}/implot_items.cpp")
target_include_directories(wave_plot PUBLIC "${IMGUI_DIR}" "${IMPLOT_DIR}")
target_compile_definitions(wave_plot PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(wave_plot PUBLIC user32 imm32 shell32)

add_library(wave_view STATIC src/wave_view.cpp)
target_link_libraries(wave_view PUBLIC wave_data wave_plot)

add_library(wave_gui STATIC
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(wave_gui PUBLIC "${IMGUI_DIR}/backends")
target_link_libraries(wave_gui PUBLIC wave_plot d3d11 dxgi d3dcompiler dwmapi gdi32)

add_executable(WaveDemo WIN32 src/main.cpp)
target_link_libraries(WaveDemo PRIVATE wave_view wave_gui)
set_target_properties(WaveDemo PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/build")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(WaveDemo PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_SOURCE_DIR}/build")
endforeach()

include(CTest)
set(WAVE_TARGETS wave_data wave_plot wave_view wave_gui WaveDemo)
if(BUILD_TESTING)
    add_executable(data_core_tests tests/data_core_tests.cpp)
    target_link_libraries(data_core_tests PRIVATE wave_data)
    add_test(NAME data_core_tests COMMAND data_core_tests)

    add_executable(plot_navigation_tests tests/plot_navigation_tests.cpp)
    target_include_directories(plot_navigation_tests PRIVATE src)
    add_test(NAME plot_navigation_tests COMMAND plot_navigation_tests)

    add_executable(view_interaction_tests tests/view_interaction_tests.cpp)
    target_link_libraries(view_interaction_tests PRIVATE wave_view)
    add_test(NAME view_interaction_tests COMMAND view_interaction_tests)
    list(APPEND WAVE_TARGETS data_core_tests plot_navigation_tests view_interaction_tests)
endif()

foreach(target IN LISTS WAVE_TARGETS)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 "$<$<CONFIG:Release>:/O2>")
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra "$<$<CONFIG:Release>:-O2>")
    endif()
endforeach()
if(MINGW)
    target_link_options(WaveDemo PRIVATE -static)
    if(BUILD_TESTING)
        target_link_options(data_core_tests PRIVATE -static)
        target_link_options(plot_navigation_tests PRIVATE -static)
        target_link_options(view_interaction_tests PRIVATE -static)
    endif()
endif()
