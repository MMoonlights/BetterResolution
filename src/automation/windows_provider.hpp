#pragma once
#include <br/br_automation_win.h>

namespace br::automation {
br_auto_status windows_provider(const br_auto_windows_options&, size_t text_limit, br_auto_provider&);
using OperationFn = br_auto_status (*)(void*, const br_auto_operation*);
br_auto_status with_operation(br_auto_session*, uint32_t timeout_ms, OperationFn, void*);
int windows_helper(int argc, char** argv);
}
