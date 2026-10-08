#pragma once

#include <br/br.h>

#include "core/thread_pool.hpp"
#include "resize/cpu_resizer.hpp"

struct br_context {
    br::ThreadPool pool;
    br::resize::CpuResizer resizer;
};

namespace br {
// Возвращает `c` или лениво создаваемый контекст по умолчанию для всего процесса.
br_context* resolve_context(br_context* c);
}
