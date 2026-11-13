#define WAVE_NATIVE_BUILD
#include "win32_embedded_view.hpp"
#include "wave_protocol.hpp"
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
thread_local char error_text[512]{};
struct WrongThread {};
template<class Action> int invoke(Action action) noexcept {
    error_text[0] = '\0';
    try { action(); return WAVE_OK; }
    catch (const WrongThread&) {
        std::snprintf(error_text, sizeof(error_text), "Window calls must run on the creating UI thread");
        return WAVE_WRONG_THREAD;
    } catch (const std::invalid_argument& error) {
        std::snprintf(error_text, sizeof(error_text), "%s", error.what());
        return WAVE_INVALID_ARGUMENT;
    } catch (const std::exception& error) {
        std::snprintf(error_text, sizeof(error_text), "%s", error.what());
        return WAVE_ERROR;
    } catch (...) {
        std::snprintf(error_text, sizeof(error_text), "Unknown Wave error");
        return WAVE_ERROR;
    }
}
template<class T> T& require(T* value) {
    if (!value) throw std::invalid_argument("Null Wave handle or output pointer");
    return *value;
}
std::string text(const char* value, bool empty = false) {
    const std::string result(value ? value : "");
    if ((!empty && result.empty()) || result.size() > 256 || !wave::tcp::valid_text(result))
        throw std::invalid_argument("Expected UTF-8 text of at most 256 bytes");
    return result;
}
wave::win32::EmbeddedView& window_view(wave_window* window) {
    auto& view = *require(window).view;
    if (!view.correct_thread()) throw WrongThread{};
    return view;
}
}

uint32_t WAVE_CALL wave_abi_version() { return 1; }
const char* WAVE_CALL wave_last_error() { return error_text; }
int WAVE_CALL wave_session_create(uint32_t capacity, wave_session** result) {
    return invoke([&] {
        require(result) = nullptr;
        auto session = std::make_unique<wave_session>();
        session->data = std::make_shared<wave::NativeSession>();
        session->data->data.set_capacity_frames(capacity);
        *result = session.release();
    });
}
int WAVE_CALL wave_session_destroy(wave_session* session) { return invoke([&] { delete session; }); }
int WAVE_CALL wave_session_set_capacity(wave_session* session, uint32_t capacity) {
    return invoke([&] {
        auto& data = *require(session).data;
        const std::lock_guard lock(data.mutex);
        data.data.set_capacity_frames(capacity);
    });
}
int WAVE_CALL wave_stream_open(wave_session* session, const char* id, const char* name, double rate,
                             const wave_channel* channels, uint32_t count, wave_stream** result) {
    return invoke([&] {
        require(result) = nullptr;
        auto state = require(session).data;
        if (!channels || !count || count > 64 || !std::isfinite(rate) || rate < 0)
            throw std::invalid_argument("Invalid stream rate or channel count");
        auto stream = std::make_unique<wave_stream>();
        stream->session = state; stream->id = text(id);
        const auto source_name = text(name);
        std::vector<wave::ChannelInfo> metadata;
        std::unordered_set<std::string> ids;
        for (uint32_t i = 0; i < count; ++i) {
            auto channel_id = text(channels[i].id);
            if (!ids.insert(channel_id).second) throw std::invalid_argument("Duplicate channel ID");
            metadata.push_back({text(channels[i].name), text(channels[i].unit, true), std::move(channel_id)});
        }
        const std::lock_guard lock(state->mutex);
        if (!state->writers.insert(stream->id).second) throw std::invalid_argument("Stream already has an active writer");
        try {
            auto& source = state->data.register_source(stream->id, source_name, rate, metadata, rate == 0);
            source.online = true;
        } catch (...) { state->writers.erase(stream->id); throw; }
        *result = stream.release();
    });
}
int WAVE_CALL wave_stream_destroy(wave_stream* stream) {
    return invoke([&] {
        if (!stream) return;
        auto state = stream->session;
        { const std::lock_guard lock(state->mutex);
          state->data.find(stream->id)->online = false;
          state->writers.erase(stream->id); }
        delete stream;
    });
}
int WAVE_CALL wave_stream_append(wave_stream* stream, uint64_t first, const float* values,
                               uint64_t count, uint64_t* next) {
    return invoke([&] {
        auto& writer = require(stream); require(next);
        if (!values || !count || count > std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("Invalid sample buffer");
        const std::lock_guard lock(writer.session->mutex);
        auto& source = *writer.session->data.find(writer.id);
        if (first == WAVE_APPEND_NEXT) first = source.next;
        *next = writer.session->data.append_frames(source, first, {values, static_cast<std::size_t>(count)});
    });
}
int WAVE_CALL wave_stream_append_received(wave_stream* stream, const float* values, uint32_t count,
                                        double seconds, uint64_t* next) {
    return invoke([&] {
        auto& writer = require(stream); require(next);
        if (!values || !count) throw std::invalid_argument("Invalid sample buffer");
        const std::lock_guard lock(writer.session->mutex);
        auto& source = *writer.session->data.find(writer.id);
        *next = writer.session->data.append_received(source, {values, count}, seconds);
    });
}
int WAVE_CALL wave_stream_get_state(wave_stream* stream, wave_stream_state* result) {
    return invoke([&] {
        auto& writer = require(stream); require(result);
        const std::lock_guard lock(writer.session->mutex);
        const auto& source = *writer.session->data.find(writer.id);
        const auto range = source.data->range();
        *result = {source.next, range.first_frame, range.frame_count,
            static_cast<uint32_t>(source.data->channels().size()), static_cast<uint32_t>(source.data->capacity_frames()),
            range.begin, range.end};
    });
}
int WAVE_CALL wave_window_create(wave_session* session, uintptr_t parent, wave_window** result) {
    return invoke([&] {
        require(result) = nullptr;
        auto window = std::make_unique<wave_window>();
        window->view = std::make_unique<wave::win32::EmbeddedView>(require(session).data);
        window->view->initialize(reinterpret_cast<HWND>(parent));
        *result = window.release();
    });
}
int WAVE_CALL wave_window_destroy(wave_window* window) {
    return invoke([&] { if (window) { window_view(window); delete window; } });
}
int WAVE_CALL wave_window_get_handle(wave_window* window, uintptr_t* result) {
    return invoke([&] { require(result) = reinterpret_cast<uintptr_t>(window_view(window).handle()); });
}
int WAVE_CALL wave_window_set_bounds(wave_window* window, int x, int y, int width, int height) {
    return invoke([&] { window_view(window).set_bounds(x, y, width, height); });
}
int WAVE_CALL wave_window_render(wave_window* window) {
    return invoke([&] { window_view(window).render(); });
}
int WAVE_CALL wave_window_set_paused(wave_window* window, int paused) {
    return invoke([&] { window_view(window).apply([&](wave::WaveView& view) { view.set_paused(paused != 0); }); });
}
int WAVE_CALL wave_window_set_time_span(wave_window* window, double seconds) {
    return invoke([&] { window_view(window).apply([&](wave::WaveView& view) { view.set_time_span(seconds); }); });
}
int WAVE_CALL wave_window_set_y_range(wave_window* window, double minimum, double maximum) {
    return invoke([&] { window_view(window).apply([&](wave::WaveView& view) { view.set_y_range(minimum, maximum); }); });
}
int WAVE_CALL wave_window_set_auto_y(wave_window* window, int enabled) {
    return invoke([&] { window_view(window).apply([&](wave::WaveView& view) { view.set_auto_y(enabled != 0); }); });
}
int WAVE_CALL wave_window_set_channel_visible(wave_window* window, uint32_t channel, int visible) {
    return invoke([&] { window_view(window).apply([&](wave::WaveView& view) { view.set_channel_visible(channel, visible != 0); }); });
}
int WAVE_CALL wave_window_set_reverse_scroll(wave_window* window, int enabled) {
    return invoke([&] { window_view(window).reverse_scroll(enabled != 0); });
}
int WAVE_CALL wave_window_get_state(wave_window* window, wave_window_state* result) {
    return invoke([&] {
        require(result);
        window_view(window).apply([&](wave::WaveView& view) {
            const auto state = view.state();
            *result = {state.x_min, state.x_max, state.y_min, state.y_max, state.plotted_points, state.paused, state.auto_y};
        });
    });
}
