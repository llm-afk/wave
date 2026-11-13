#pragma once
#include <stdint.h>

#if defined(_WIN32)
# if defined(WAVE_NATIVE_BUILD)
#  define WAVE_API __declspec(dllexport)
# else
#  define WAVE_API __declspec(dllimport)
# endif
# define WAVE_CALL __cdecl
#else
# define WAVE_API
# define WAVE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ABI v1. Text is UTF-8. Input arrays are copied during the call. */
typedef struct wave_session wave_session;
typedef struct wave_stream wave_stream;
typedef struct wave_window wave_window;
typedef struct wave_channel { const char* id; const char* name; const char* unit; } wave_channel;
typedef struct wave_stream_state {
    uint64_t next_frame, first_frame, frame_count;
    uint32_t channel_count, capacity_frames;
    double begin_seconds, end_seconds;
} wave_stream_state;
typedef struct wave_window_state {
    double x_min, x_max, y_min, y_max;
    uint64_t plotted_points;
    uint32_t paused, auto_y;
} wave_window_state;

enum { WAVE_OK = 0, WAVE_INVALID_ARGUMENT = 1, WAVE_ERROR = 2, WAVE_WRONG_THREAD = 3 };
#define WAVE_APPEND_NEXT UINT64_MAX

WAVE_API uint32_t WAVE_CALL wave_abi_version(void);
/* Error text belongs to the DLL and is valid until the next call on this thread. */
WAVE_API const char* WAVE_CALL wave_last_error(void);
WAVE_API int WAVE_CALL wave_session_create(uint32_t capacity_frames, wave_session** result);
/* Streams and windows retain the data independently of this session handle. */
WAVE_API int WAVE_CALL wave_session_destroy(wave_session* session);
WAVE_API int WAVE_CALL wave_session_set_capacity(wave_session* session, uint32_t capacity_frames);
/* sample_rate == 0 selects explicit, monotonically increasing receive times. */
WAVE_API int WAVE_CALL wave_stream_open(wave_session* session, const char* id, const char* name,
    double sample_rate, const wave_channel* channels, uint32_t count, wave_stream** result);
WAVE_API int WAVE_CALL wave_stream_destroy(wave_stream* stream);
/* Interleaved frames. Use WAVE_APPEND_NEXT for an atomic append at the current end. */
WAVE_API int WAVE_CALL wave_stream_append(wave_stream* stream, uint64_t first_frame,
    const float* values, uint64_t value_count, uint64_t* next_frame);
WAVE_API int WAVE_CALL wave_stream_append_received(wave_stream* stream, const float* values,
    uint32_t value_count, double seconds, uint64_t* next_frame);
WAVE_API int WAVE_CALL wave_stream_get_state(wave_stream* stream, wave_stream_state* result);

/* Windows child control. All window calls run on the parent's UI thread.
   The host owns its message loop, DPI policy and render timer. Bounds are pixels.
   Stop concurrent calls before destroying any handle. Destroying a window
   releases its GPU resources and leaves shared acquisition available. */
WAVE_API int WAVE_CALL wave_window_create(wave_session* session, uintptr_t parent, wave_window** result);
WAVE_API int WAVE_CALL wave_window_destroy(wave_window* window);
WAVE_API int WAVE_CALL wave_window_get_handle(wave_window* window, uintptr_t* result);
WAVE_API int WAVE_CALL wave_window_set_bounds(wave_window* window, int x, int y, int width, int height);
WAVE_API int WAVE_CALL wave_window_render(wave_window* window);
WAVE_API int WAVE_CALL wave_window_set_paused(wave_window* window, int paused);
WAVE_API int WAVE_CALL wave_window_set_time_span(wave_window* window, double seconds);
WAVE_API int WAVE_CALL wave_window_set_y_range(wave_window* window, double minimum, double maximum);
WAVE_API int WAVE_CALL wave_window_set_auto_y(wave_window* window, int enabled);
WAVE_API int WAVE_CALL wave_window_set_channel_visible(wave_window* window, uint32_t channel, int visible);
/* Applies to promoted wheel input; raw two-finger coordinates use right/up directly. */
WAVE_API int WAVE_CALL wave_window_set_reverse_scroll(wave_window* window, int enabled);
WAVE_API int WAVE_CALL wave_window_get_state(wave_window* window, wave_window_state* result);

#ifdef __cplusplus
}
#endif
