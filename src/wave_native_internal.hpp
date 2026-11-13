#pragma once
#include "wave_native.h"
#include "signal_hub.hpp"
#include <memory>
#include <mutex>
#include <unordered_set>

namespace wave {
struct NativeSession {
    std::mutex mutex;
    SignalHub data;
    std::unordered_set<std::string> writers;
};
namespace win32 { class EmbeddedView; }
}

struct wave_session { std::shared_ptr<wave::NativeSession> data; };
struct wave_stream { std::shared_ptr<wave::NativeSession> session; std::string id; };
struct wave_window { std::unique_ptr<wave::win32::EmbeddedView> view; };
