#include "platform/capture.hpp"

#include <mutex>
#include <utility>

namespace br::capture {
namespace {
std::mutex g_factory_mutex;
SessionFactory g_factory;
}

void set_test_factory(SessionFactory factory) {
    std::lock_guard<std::mutex> lock(g_factory_mutex);
    g_factory = std::move(factory);
}

br_status open(const br_capture_options& options, std::unique_ptr<Session>& out, bool cold_start_first) {
    SessionFactory factory;
    {
        std::lock_guard<std::mutex> lock(g_factory_mutex);
        factory = g_factory;
    }
    if (factory) return factory(options, out);
    return open_native(options, cold_start_first, out);
}

}
