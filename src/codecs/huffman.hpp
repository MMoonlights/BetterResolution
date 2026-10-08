#pragma once

#include <cstdint>

namespace br::codec {

// Неиспользуемые длины равны нулю; для полного дерева нужны минимум два символа (сумма Крафта = 1).
void build_code_lengths(const uint32_t* freq, int n, int limit, uint8_t* lens);

}
