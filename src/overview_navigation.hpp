#pragma once

#include "plot_navigation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace wave {

enum class OverviewDrag { Move, ResizeMin, ResizeMax };

// initial and extent are captured at press time. delta_time is the total drag
// displacement using that same fixed overview mapping, not a per-frame delta.
// Normalization preserves the initial span where it fits, shifting the whole
// range inside extent before applying the gesture. Following always anchors
// the result at extent.max; Move then behaves like dragging the minimum edge.
// A reversed/degenerate/nonfinite extent has no usable mapping and returns the
// finite fallback [0, 1]; the UI should disable dragging such an overview.
inline AxisRange overview_range_from_drag(AxisRange initial, AxisRange extent,
                                          double delta_time, OverviewDrag drag,
                                          double min_span, bool follow_latest) noexcept {
    const double extent_span = extent.max - extent.min;
    if (!std::isfinite(extent.min) || !std::isfinite(extent.max) ||
        !std::isfinite(extent_span) || extent_span <= 0)
        return {0, 1};

    if (std::isnan(min_span) || min_span <= 0) min_span = 1e-12;
    // Keep endpoints distinguishable even at large absolute timestamps. At
    // ordinary timestamps the caller's minimum (e.g. eight samples) dominates.
    const double quantum = std::max(
        std::nextafter(extent.min, std::numeric_limits<double>::infinity()) - extent.min,
        extent.max - std::nextafter(extent.max, -std::numeric_limits<double>::infinity()));
    const double minimum = std::min(extent_span, std::max(min_span, quantum));

    if (!std::isfinite(initial.min) || !std::isfinite(initial.max) ||
        !std::isfinite(initial.max - initial.min) || initial.max <= initial.min)
        initial = extent;
    const double span = std::clamp(initial.max - initial.min, minimum, extent_span);
    initial.min = std::clamp(initial.min, extent.min, std::max(extent.min, extent.max - span));
    initial.max = std::min(extent.max, initial.min + span);
    if (std::isnan(delta_time)) delta_time = 0;

    AxisRange result = initial;
    if (follow_latest) {
        const double change = drag == OverviewDrag::ResizeMax ? delta_time : -delta_time;
        const double new_span = std::clamp(span + change, minimum, extent_span);
        result = {std::max(extent.min, extent.max - new_span), extent.max};
    } else if (drag == OverviewDrag::Move) {
        result.min = std::clamp(initial.min + delta_time, extent.min,
                                std::max(extent.min, extent.max - span));
        result.max = std::min(extent.max, result.min + span);
    } else if (drag == OverviewDrag::ResizeMin) {
        result.min = std::clamp(initial.min + delta_time, extent.min,
                                std::max(extent.min, initial.max - minimum));
    } else {
        result.max = std::clamp(initial.max + delta_time,
                                std::min(extent.max, initial.min + minimum), extent.max);
    }
    // At exceptional floating-point magnitudes, use the valid extent rather
    // than let rounding collapse or invert a range passed to the plot.
    if (!std::isfinite(result.min) || !std::isfinite(result.max) || result.max <= result.min)
        return extent;
    return result;
}

} // namespace wave
