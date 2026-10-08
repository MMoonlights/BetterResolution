#pragma once

#include <br/br.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <utility>

namespace br {

// Сообщение об ошибке для текущего потока, возвращаемое через br_last_error().
void set_error(const char* message) noexcept;
void set_errorf(const char* fmt, ...) noexcept;
const char* last_error() noexcept;

inline br_status fail(br_status status, const char* message) noexcept {
    set_error(message);
    return status;
}

// Исключение с кодом br_status; возникает во внутреннем коде и преобразуется на границе C API.
struct Error : std::runtime_error {
    br_status status;
    Error(br_status s, const char* message) : std::runtime_error(message), status(s) {}
};

[[noreturn]] inline void raise(br_status status, const char* message) { throw Error(status, message); }

// Вызывает `fn` и преобразует любое исключение в код br_status.
template <class F>
br_status guarded(F&& fn) noexcept {
    try {
        return fn();
    } catch (const Error& e) {
        return fail(e.status, e.what());
    } catch (const std::bad_alloc&) {
        return fail(BR_E_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception& e) {
        return fail(BR_E_INTERNAL, e.what());
    } catch (...) {
        return fail(BR_E_INTERNAL, "unknown internal error");
    }
}

// Расширяемый буфер байтов на malloc; память можно передать вызывающему коду на C и освободить через br_free.
class Bytes {
public:
    Bytes() = default;
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
    Bytes(Bytes&& o) noexcept : data_(o.data_), size_(o.size_), cap_(o.cap_) { o.data_ = nullptr; o.size_ = o.cap_ = 0; }
    Bytes& operator=(Bytes&& o) noexcept {
        if (this != &o) {
            std::free(data_);
            data_ = o.data_; size_ = o.size_; cap_ = o.cap_;
            o.data_ = nullptr; o.size_ = o.cap_ = 0;
        }
        return *this;
    }
    ~Bytes() { std::free(data_); }

    uint8_t* data() noexcept { return data_; }
    const uint8_t* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    uint8_t& operator[](size_t i) noexcept { return data_[i]; }
    uint8_t operator[](size_t i) const noexcept { return data_[i]; }

    void reserve(size_t n) {
        if (n <= cap_) return;
        size_t c = cap_ ? cap_ : 256;
        while (c < n) c = c + c / 2 + 64;
        void* p = std::realloc(data_, c);
        if (!p) throw std::bad_alloc();
        data_ = static_cast<uint8_t*>(p);
        cap_ = c;
    }
    void resize(size_t n) { reserve(n); size_ = n; }
    void clear() noexcept { size_ = 0; }
    void push(uint8_t b) {
        if (size_ == cap_) reserve(size_ + 1);
        data_[size_++] = b;
    }
    void append(const void* p, size_t n) {
        if (!n) return;
        reserve(size_ + n);
        std::memcpy(data_ + size_, p, n);
        size_ += n;
    }
    void be16(uint32_t v) { push(uint8_t(v >> 8)); push(uint8_t(v)); }
    void be32(uint32_t v) { push(uint8_t(v >> 24)); push(uint8_t(v >> 16)); push(uint8_t(v >> 8)); push(uint8_t(v)); }
    void le16(uint32_t v) { push(uint8_t(v)); push(uint8_t(v >> 8)); }
    void le32(uint32_t v) { push(uint8_t(v)); push(uint8_t(v >> 8)); push(uint8_t(v >> 16)); push(uint8_t(v >> 24)); }
    // Передаёт владение; освобождать через std::free или br_free.
    uint8_t* release() noexcept {
        uint8_t* p = data_;
        data_ = nullptr;
        size_ = cap_ = 0;
        return p;
    }

private:
    uint8_t* data_{};
    size_t size_{};
    size_t cap_{};
};

inline uint32_t load_be32(const uint8_t* p) noexcept {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t load_be16(const uint8_t* p) noexcept { return (uint32_t(p[0]) << 8) | p[1]; }
inline uint32_t load_le32(const uint8_t* p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint32_t load_le16(const uint8_t* p) noexcept { return uint32_t(p[0]) | (uint32_t(p[1]) << 8); }

// CRC-32 (ISO-HDLC, используется в PNG и gzip/zlib).
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size) noexcept;
inline uint32_t crc32(const uint8_t* data, size_t size) noexcept { return crc32_update(0, data, size); }
uint32_t adler32_update(uint32_t adler, const uint8_t* data, size_t size) noexcept;

uint64_t monotonic_us() noexcept;

}
