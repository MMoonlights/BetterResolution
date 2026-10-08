#pragma once
#if defined(_WIN32)

#include "platform/windows/win_common.hpp"

#include <string>
#include <vector>

namespace br::win {

struct MonitorEntry {
    br_monitor_info info;
    HMONITOR handle;
    std::wstring device;
};

br_status enum_monitors(std::vector<MonitorEntry>& out);
br_status enum_windows(std::vector<br_window_info>& out);
br_rect_i32 virtual_desktop();

}

#endif
