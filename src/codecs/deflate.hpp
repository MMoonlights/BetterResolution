#pragma once

#include "core/common.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace br::deflate {

// Формат DEFLATE/zlib: уровень 0 сохраняет данные, 1..3 - жадный поиск, 4..9 - ленивый.
class Deflater {
public:
    Deflater(Bytes& out, int level, bool zlib_framing);
    ~Deflater();
    Deflater(const Deflater&) = delete;
    Deflater& operator=(const Deflater&) = delete;

    // Необязательные расстояния совпадений; ноль отключает подсказку.
    void set_hint_distances(int d1, int d2);
    void write(const uint8_t* data, size_t size);
    // Указатель prepare_write действителен до commit_write; между вызовами нельзя выполнять другие операции.
    uint8_t* prepare_write(size_t size);
    void commit_write();
    // Последний блок и необязательный завершающий блок zlib.
    void finish();
    // Промежуточная синхронная очистка с выравниванием по байту; блоки можно соединять.
    void finish_partial();
    // Заимствованный буфер [история|данные]; размер включает исходную историю (<=32768), которая не попадает в результат.
    // Только первая запись; буферы не должны совпадать. При final=false выполняется синхронная очистка.
    void finish_contiguous(const uint8_t* data, size_t size, size_t history = 0, bool final = true);

private:
    struct State;
    std::unique_ptr<State> s_;
};

void zlib_compress(const uint8_t* data, size_t size, int level, Bytes& out);

// Добавляет не более max_output байтов результата; consumed сообщает число использованных входных байтов.
br_status inflate(const uint8_t* data, size_t size, bool zlib_framing, Bytes& out, size_t max_output,
                  size_t* consumed = nullptr);

}
