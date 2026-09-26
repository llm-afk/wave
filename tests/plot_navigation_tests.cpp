#include "plot_navigation.hpp"
#include "overview_navigation.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double span(wave::AxisRange range) { return range.max - range.min; }
double fraction(wave::AxisRange range, double anchor) { return (anchor - range.min) / span(range); }
bool near(double a, double b, double tolerance = 1e-10) {
    return std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}
bool same(wave::PlotRange a, wave::PlotRange b) {
    return a.x.min == b.x.min && a.x.max == b.x.max &&
           a.y.min == b.y.min && a.y.max == b.y.max;
}
bool finite(wave::AxisRange range) {
    return std::isfinite(range.min) && std::isfinite(range.max) &&
           std::isfinite(span(range)) && range.max > range.min;
}

void direction_and_anchor() {
    const wave::PlotRange initial{{-2, 8}, {-10, 10}};
    const wave::ZoomAnchor anchor{1, 4};
    const auto zoomed = wave::zoom_from_drag(initial, anchor, 150, -90);
    require(span(zoomed.x) < span(initial.x) && span(zoomed.y) < span(initial.y),
            "Right/up dragging must zoom into X/Y");
    require(near(fraction(initial.x, anchor.x), fraction(zoomed.x, anchor.x)) &&
            near(fraction(initial.y, anchor.y), fraction(zoomed.y, anchor.y)),
            "The pressed data anchor must keep its viewport position");
    const auto expanded = wave::zoom_from_drag(initial, anchor, -150, 90);
    require(span(expanded.x) > span(initial.x) && span(expanded.y) > span(initial.y),
            "Left/down dragging must zoom out of X/Y");
    require(near(span(zoomed.x) * span(expanded.x), span(initial.x) * span(initial.x)),
            "Opposite drag distances must produce reciprocal exponential scales");
}

void return_to_origin_and_event_independence() {
    const wave::PlotRange initial{{10, 40}, {-5, 15}};
    const wave::ZoomAnchor anchor{13, 9};
    const auto direct = wave::zoom_from_drag(initial, anchor, 180, -90);
    wave::PlotRange staged = initial;
    for (int step = 1; step <= 60; ++step)
        staged = wave::zoom_from_drag(initial, anchor, 3.0 * step, -1.5 * step);
    require(same(direct, staged), "Final zoom must not depend on frame/event subdivision");
    const auto back_at_press = wave::zoom_from_drag(initial, anchor, 0, 0);
    require(same(back_at_press, initial), "Dragging back to the press must exactly restore initial bounds");
    const auto x_only = wave::zoom_from_drag(initial, anchor, 200, -200, wave::ZoomAxes::XOnly);
    const auto y_only = wave::zoom_from_drag(initial, anchor, 200, -200, wave::ZoomAxes::YOnly);
    require(x_only.y.min == initial.y.min && x_only.y.max == initial.y.max &&
            y_only.x.min == initial.x.min && y_only.x.max == initial.x.max,
            "Axis modifiers must leave the unselected axis unchanged");
}

void extreme_deltas_and_limits() {
    const wave::PlotRange initial{{0, 10}, {-10, 10}};
    const wave::ZoomAnchor anchor{3, 2};
    wave::DragZoomLimits limits;
    limits.sample_rate = 20'000;
    limits.max_x_span = 120;
    limits.min_y_span = 0.001;
    limits.max_y_span = 500;
    const auto zoomed = wave::zoom_from_drag(initial, anchor, 1e308, -1e308,
                                            wave::ZoomAxes::Both, limits);
    const auto expanded = wave::zoom_from_drag(initial, anchor, -1e308, 1e308,
                                              wave::ZoomAxes::Both, limits);
    require(finite(zoomed.x) && finite(zoomed.y) && finite(expanded.x) && finite(expanded.y),
            "Extreme mouse deltas must always produce finite, ordered bounds");
    require(near(span(zoomed.x), 8.0 / limits.sample_rate) &&
            near(span(zoomed.y), limits.min_y_span) &&
            near(span(expanded.x), limits.max_x_span) &&
            near(span(expanded.y), limits.max_y_span),
            "X must respect eight samples and configured maximum; Y must respect its limits");
    require(near(fraction(zoomed.x, anchor.x), fraction(initial.x, anchor.x)) &&
            near(fraction(expanded.y, anchor.y), fraction(initial.y, anchor.y)),
            "Hitting a span limit must still retain the anchor");
    require(same(wave::zoom_from_drag(initial, anchor, 0, 0, wave::ZoomAxes::Both, limits), initial),
            "Dragging back from a clamped limit must have no accumulated zoom state");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto invalid = wave::zoom_from_drag({{nan, nan}, {5, 5}}, {nan, nan}, nan, nan);
    require(finite(invalid.x) && finite(invalid.y), "Invalid initial limits must recover to usable bounds");
}

void overview_range_constraints() {
    constexpr double minimum = 8.0 / 20'000;
    const wave::AxisRange extent{10, 40};
    const wave::AxisRange initial{18, 22};
    const auto move_left = wave::overview_range_from_drag(initial, extent, -100,
        wave::OverviewDrag::Move, minimum, false);
    const auto move_right = wave::overview_range_from_drag(initial, extent, 100,
        wave::OverviewDrag::Move, minimum, false);
    require(near(move_left.min, extent.min) && near(move_right.max, extent.max) &&
            near(span(move_left), span(initial)) && near(span(move_right), span(initial)),
            "Overview movement must preserve span and stop at both extent boundaries");

    const auto shrink_left = wave::overview_range_from_drag(initial, extent, 100,
        wave::OverviewDrag::ResizeMin, minimum, false);
    const auto shrink_right = wave::overview_range_from_drag(initial, extent, -100,
        wave::OverviewDrag::ResizeMax, minimum, false);
    require(near(shrink_left.max, initial.max) && near(shrink_right.min, initial.min) &&
            span(shrink_left) >= minimum * (1 - 1e-9) && span(shrink_right) >= minimum * (1 - 1e-9) &&
            near(span(shrink_left), minimum) && near(span(shrink_right), minimum),
            "Overview edge resizing must retain its opposite edge and respect the sample minimum");
    const auto expand_left = wave::overview_range_from_drag(initial, extent,
        -std::numeric_limits<double>::infinity(), wave::OverviewDrag::ResizeMin, minimum, false);
    const auto expand_right = wave::overview_range_from_drag(initial, extent,
        std::numeric_limits<double>::infinity(), wave::OverviewDrag::ResizeMax, minimum, false);
    require(finite(expand_left) && finite(expand_right) &&
            expand_left.min == extent.min && expand_right.max == extent.max,
            "Extreme overview drags must clamp to finite extent endpoints");

    const auto whole = wave::overview_range_from_drag({-10, 60}, extent, 100,
        wave::OverviewDrag::ResizeMin, 100, false);
    require(whole.min == extent.min && whole.max == extent.max,
            "Oversized initial ranges and minimum spans must normalize to the available extent");
    const auto following = wave::overview_range_from_drag(initial, extent, 1,
        wave::OverviewDrag::Move, minimum, true);
    require(following.max == extent.max && near(span(following), 3),
            "Following overview movement must resize the minimum edge and retain the latest endpoint");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto neutral = wave::overview_range_from_drag(initial, extent, nan,
        wave::OverviewDrag::Move, minimum, false);
    const auto recovered = wave::overview_range_from_drag({nan, nan}, extent, nan,
        wave::OverviewDrag::ResizeMax, nan, false);
    require(neutral.min == initial.min && neutral.max == initial.max && finite(recovered) &&
            recovered.min >= extent.min && recovered.max <= extent.max,
            "NaN input must not escape into overview bounds or move a valid neutral range");
}
} // namespace

int main() {
    try {
        direction_and_anchor();
        return_to_origin_and_event_independence();
        extreme_deltas_and_limits();
        overview_range_constraints();
        std::cout << "PASS: drag direction, fixed anchor, reversible zoom, event independence, axis modifiers, finite limits, overview bounds\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
