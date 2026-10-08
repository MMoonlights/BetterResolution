// Используется, если системный захват недоступен или отключён при сборке.

#include "platform/capture.hpp"
#include "core/common.hpp"

#include <chrono>
#include <thread>

namespace br::capture {

static br_status unsupported() { return fail(BR_E_UNSUPPORTED, "native desktop capture is unavailable in this build"); }

br_status list_monitors(std::vector<br_monitor_info>&) { return unsupported(); }
br_status list_windows(std::vector<br_window_info>&) { return unsupported(); }
br_status open_native(const br_capture_options&, bool, std::unique_ptr<Session>&) { return unsupported(); }
br_status prewarm() { return unsupported(); }
void sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
br_status open_dxgi_output(uint32_t, uint32_t, std::unique_ptr<Session>&) { return unsupported(); }
br_status open_window(void*, std::unique_ptr<Session>&) { return unsupported(); }
void release_d3d11_texture(void*) {}
const char* features() noexcept { return ""; }

}
