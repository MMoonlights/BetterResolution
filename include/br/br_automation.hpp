#pragma once
#include "br_automation.h"
#include <stdexcept>
#include <string_view>
#include <utility>

namespace br {
class AutomationError : public std::runtime_error {
    br_auto_status status_;
public:
    explicit AutomationError(br_auto_status status) : std::runtime_error(br_auto_status_string(status)), status_(status) {}
    br_auto_status status() const noexcept { return status_; }
};
inline br_auto_string auto_string(std::string_view s) noexcept { return {s.data(), s.size()}; }
class AutomationSnapshot {
    br_auto_snapshot* p_ = nullptr;
public:
    explicit AutomationSnapshot(br_auto_snapshot* p = nullptr) noexcept : p_(p) {}
    ~AutomationSnapshot() { br_auto_snapshot_destroy(p_); }
    AutomationSnapshot(const AutomationSnapshot&) = delete;
    AutomationSnapshot& operator=(const AutomationSnapshot&) = delete;
    AutomationSnapshot(AutomationSnapshot&& other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
    AutomationSnapshot& operator=(AutomationSnapshot&& other) noexcept {
        if (this != &other) { br_auto_snapshot_destroy(p_); p_ = std::exchange(other.p_, nullptr); } return *this;
    }
    br_auto_snapshot* get() const noexcept { return p_; }
    br_auto_snapshot_info info() const noexcept { return br_auto_snapshot_get_info(p_); }
    br_auto_element element(size_t index) const {
        br_auto_element e{}; const auto st = br_auto_snapshot_element(p_, index, &e);
        if (st != BR_AUTO_OK) throw AutomationError(st);
        return e; // Строки заимствуются из этого снимка.
    }
    br_auto_element find_id(uint64_t id) const {
        br_auto_element e{};
        const auto st = br_auto_snapshot_find_id(p_, id, &e);
        if (st != BR_AUTO_OK) throw AutomationError(st);
        return e;
    }
};
class AutomationSession {
    br_auto_session* p_ = nullptr;
public:
    explicit AutomationSession(const br_auto_provider& provider, const br_auto_options& options = br_auto_options_default()) {
        const auto st = br_auto_session_create(&provider, &options, &p_);
        if (st != BR_AUTO_OK) throw AutomationError(st);
    }
    /* Принимает владение успешно созданной C-сессией. */
    explicit AutomationSession(br_auto_session* owned) noexcept : p_(owned) {}
    ~AutomationSession() { br_auto_session_destroy(p_); }
    AutomationSession(const AutomationSession&) = delete;
    AutomationSession& operator=(const AutomationSession&) = delete;
    AutomationSession(AutomationSession&& other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
    AutomationSession& operator=(AutomationSession&& other) noexcept {
        if (this != &other) { br_auto_session_destroy(p_); p_ = std::exchange(other.p_, nullptr); } return *this;
    }
    br_auto_session* get() const noexcept { return p_; }
    AutomationSnapshot observe(uint64_t since = 0) {
        br_auto_snapshot* raw = nullptr; const auto st = br_auto_observe(p_, since, &raw);
        if (st != BR_AUTO_OK) throw AutomationError(st);
        return AutomationSnapshot(raw);
    }
    br_auto_action_result act(const br_auto_action& action) {
        br_auto_action_result r{}; br_auto_act(p_, &action, &r);
        return r; // Сохраняет effect и verification, включая ошибки.
    }
    void cancel() noexcept { br_auto_cancel(p_); }
};
}
