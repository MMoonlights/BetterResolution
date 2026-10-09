#pragma once
#include <br/br_observation.h>
namespace br::observe {
bool valid_string(br_observation_string, size_t limit);
bool valid_options(const br_observation_options&);
}
