#pragma once
#include "data_core.hpp"
#include <memory>

namespace wave {

// Call once after creating the ImGui and ImPlot contexts.
void apply_style(float dpi_scale = 1.0f);

// Embeddable display component. The caller owns the GUI contexts/event loop
// and a DataStore that must outlive this view. draw() is UI-thread only.
class WaveView {
public:
    explicit WaveView(DataStore& store);
    ~WaveView();
    WaveView(const WaveView&) = delete;
    WaveView& operator=(const WaveView&) = delete;
    void draw();
    std::size_t points_this_frame() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wave
