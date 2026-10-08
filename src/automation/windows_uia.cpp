#include <br/br_automation_win.h>
#include "windows_provider.hpp"

#if defined(_WIN32) && defined(BR_HAS_WINDOWS_AUTOMATION)
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <uiautomation.h>
#include <algorithm>
#include <bit>
#include <climits>
#include <stdexcept>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
template<class T> class Com {
    T* p_ = nullptr;
public:
    Com() = default;
    ~Com() { reset(); }
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    Com(Com&& p) noexcept : p_(std::exchange(p.p_, nullptr)) {}
    Com& operator=(Com&& p) noexcept { if (this != &p) { reset(); p_ = std::exchange(p.p_, nullptr); } return *this; }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T** put() { reset(); return &p_; }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
};
struct Variant {
    VARIANT v;
    Variant() { VariantInit(&v); }
    ~Variant() { VariantClear(&v); }
};
struct Array {
    SAFEARRAY* p = nullptr;
    ~Array() { if (p) SafeArrayDestroy(p); }
};
struct Bstr {
    BSTR p = nullptr;
    explicit Bstr(const std::wstring& value) {
        if (value.size() > UINT_MAX) throw std::runtime_error("input too long");
        p = SysAllocStringLen(value.data(), static_cast<UINT>(value.size()));
        if (!p) throw std::bad_alloc();
    }
    ~Bstr() { SysFreeString(p); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};
struct DpiScope {
    using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    Fn set = std::bit_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
    DPI_AWARENESS_CONTEXT old = set ? set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) : nullptr;
    ~DpiScope() { if (old) set(old); }
};
br_auto_status status_of(HRESULT hr) {
    if (SUCCEEDED(hr)) return BR_AUTO_OK;
    if (hr == E_ACCESSDENIED) return BR_AUTO_DENIED;
    if (hr == E_OUTOFMEMORY) return BR_AUTO_OUT_OF_MEMORY;
    if (hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE)) return BR_AUTO_STALE;
    if (hr == static_cast<HRESULT>(UIA_E_ELEMENTNOTENABLED)) return BR_AUTO_PRECONDITION_FAILED;
    if (hr == static_cast<HRESULT>(UIA_E_NOTSUPPORTED) || hr == E_NOINTERFACE) return BR_AUTO_UNSUPPORTED;
    if (hr == static_cast<HRESULT>(UIA_E_TIMEOUT) || hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) return BR_AUTO_TIMEOUT;
    return BR_AUTO_PROVIDER_ERROR;
}
struct TextLimit {};
std::string utf8(BSTR text, size_t& remaining) {
    const UINT length = text ? SysStringLen(text) : 0;
    if (!length) return {};
    if (length > static_cast<UINT>(INT_MAX)) throw std::runtime_error("UIA string too long");
    if (static_cast<size_t>(length) > remaining) throw TextLimit{};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("invalid UIA UTF-16");
    if (static_cast<size_t>(count) > remaining) throw TextLimit{};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, static_cast<int>(length), result.data(), count, nullptr, nullptr) != count)
        throw std::runtime_error("UTF-16 conversion failed");
    if (result.find('\0') != std::string::npos) throw std::runtime_error("embedded UIA NUL");
    remaining -= result.size();
    return result;
}
std::wstring utf16(br_auto_string s) {
    if (!s.size) return {};
    if (s.size > INT_MAX) throw std::runtime_error("input too long");
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data, static_cast<int>(s.size), nullptr, 0);
    if (count <= 0) throw std::runtime_error("invalid UTF-8");
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data, static_cast<int>(s.size), result.data(), count) != count)
        throw std::runtime_error("UTF-8 conversion failed");
    return result;
}
std::string runtime_id(IUIAutomationElement* e, br_auto_status* status = nullptr) {
    if (status) *status = BR_AUTO_PROVIDER_ERROR;
    Array a;
    const auto id_hr = e->GetRuntimeId(&a.p);
    if (FAILED(id_hr)) { if (status) *status = status_of(id_hr); return {}; }
    if (!a.p) { if (status) *status = BR_AUTO_UNSUPPORTED; return {}; }
    if (SafeArrayGetDim(a.p) != 1) return {};
    VARTYPE type = VT_EMPTY;
    if (FAILED(SafeArrayGetVartype(a.p, &type)) || type != VT_I4) return {};
    LONG low = 0, high = -1;
    if (FAILED(SafeArrayGetLBound(a.p, 1, &low)) || FAILED(SafeArrayGetUBound(a.p, 1, &high))) return {};
    if (high < low) { if (status) *status = BR_AUTO_UNSUPPORTED; return {}; }
    if (static_cast<int64_t>(high) - low > 4096) { if (status) *status = BR_AUTO_LIMIT_EXCEEDED; return {}; }
    std::string key;
    for (LONG i = low; i <= high; ++i) {
        int value = 0;
        if (FAILED(SafeArrayGetElement(a.p, &i, &value))) return {};
        key += std::to_string(value); key += ':';
        if (i == LONG_MAX) break;
    }
    if (status) *status = BR_AUTO_OK;
    return key;
}
std::string cached_text(IUIAutomationElement* e, PROPERTYID id, size_t& remaining, bool* known = nullptr) {
    Variant v;
    const bool have = SUCCEEDED(e->GetCachedPropertyValueEx(id, TRUE, &v.v)) && v.v.vt == VT_BSTR;
    if (known) *known = have;
    return have ? utf8(v.v.bstrVal, remaining) : std::string{};
}
bool cached_bool(IUIAutomationElement* e, PROPERTYID id, bool& value) {
    Variant v;
    if (FAILED(e->GetCachedPropertyValueEx(id, TRUE, &v.v)) || v.v.vt != VT_BOOL) return false;
    value = v.v.boolVal != VARIANT_FALSE; return true;
}
const char* role(CONTROLTYPEID id) {
    switch (id) {
    case UIA_ButtonControlTypeId: return "button";
    case UIA_EditControlTypeId: return "edit";
    case UIA_CheckBoxControlTypeId: return "checkbox";
    case UIA_ComboBoxControlTypeId: return "combobox";
    case UIA_ListControlTypeId: return "list";
    case UIA_ListItemControlTypeId: return "listitem";
    case UIA_RadioButtonControlTypeId: return "radio";
    case UIA_TabControlTypeId: return "tab";
    case UIA_TabItemControlTypeId: return "tabitem";
    case UIA_TextControlTypeId: return "text";
    case UIA_WindowControlTypeId: return "window";
    case UIA_DataGridControlTypeId: return "grid";
    case UIA_DataItemControlTypeId: return "row";
    case UIA_TreeControlTypeId: return "tree";
    case UIA_TreeItemControlTypeId: return "treeitem";
    default: return "control";
    }
}
struct Entry {
    uint64_t id = 0, parent = 0;
    bool addressable = true;
    std::string key;
    Com<IUIAutomationElement> element;
};
struct Native {
    HRESULT apartment_status = E_FAIL;
    Com<IUIAutomation> ui;
    Com<IUIAutomationCacheRequest> cache;
    Com<IUIAutomationTreeWalker> walker;
    Com<IUIAutomationTreeWalker> raw_walker;
    Com<IUIAutomationElement> bound_root;
    HWND window = nullptr;
    HANDLE process = nullptr;
    DWORD pid = 0;
    uint32_t max_depth = 32;
    size_t text_limit = 0;
    uint64_t next = 1;
    std::string root_identity;
    std::unordered_map<std::string, std::shared_ptr<Entry>> entries;
    std::unordered_map<uint64_t, std::shared_ptr<Entry>> tokens;
    ~Native() { if (process) CloseHandle(process); }

    br_auto_status init(const br_auto_windows_options& o, size_t max_text_bytes) {
        DpiScope dpi;
        if (FAILED(apartment_status)) return status_of(apartment_status);
        window = reinterpret_cast<HWND>(static_cast<uintptr_t>(o.window)); max_depth = o.max_depth;
        text_limit = max_text_bytes;
        if (!IsWindow(window) || !GetWindowThreadProcessId(window, &pid)) return BR_AUTO_NOT_FOUND;
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!process) return BR_AUTO_DENIED;
        const CLSID modern = {0xe22ad333, 0xb25f, 0x460c, {0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9}};
        HRESULT hr = CoCreateInstance(modern, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), reinterpret_cast<void**>(ui.put()));
        if (FAILED(hr)) hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                             __uuidof(IUIAutomation), reinterpret_cast<void**>(ui.put()));
        if (FAILED(hr)) return status_of(hr);
#if defined(__IUIAutomation2_INTERFACE_DEFINED__)
        Com<IUIAutomation2> ui2;
        hr = ui->QueryInterface(__uuidof(IUIAutomation2), reinterpret_cast<void**>(ui2.put()));
        if (FAILED(hr) && hr != E_NOINTERFACE) return status_of(hr);
        if (SUCCEEDED(hr)) {
            hr = ui2->put_ConnectionTimeout(o.connection_timeout_ms);
            if (FAILED(hr)) return status_of(hr);
            hr = ui2->put_TransactionTimeout(o.transaction_timeout_ms);
            if (FAILED(hr)) return status_of(hr);
            hr = ui2->put_AutoSetFocus(FALSE);
            if (FAILED(hr)) return status_of(hr);
        }
#endif
        hr = ui->CreateCacheRequest(cache.put());
        if (FAILED(hr)) return status_of(hr);
        hr = cache->put_TreeScope(TreeScope_Element);
        if (FAILED(hr)) return status_of(hr);
        const PROPERTYID properties[] = {UIA_NamePropertyId, UIA_AutomationIdPropertyId, UIA_ControlTypePropertyId, UIA_ProcessIdPropertyId,
            UIA_BoundingRectanglePropertyId, UIA_IsEnabledPropertyId, UIA_IsOffscreenPropertyId,
            UIA_IsPasswordPropertyId, UIA_IsKeyboardFocusablePropertyId, UIA_HasKeyboardFocusPropertyId,
            UIA_ValueValuePropertyId, UIA_SelectionItemIsSelectedPropertyId, UIA_ToggleToggleStatePropertyId,
            UIA_IsInvokePatternAvailablePropertyId, UIA_IsValuePatternAvailablePropertyId,
            UIA_IsTogglePatternAvailablePropertyId, UIA_IsSelectionItemPatternAvailablePropertyId,
            UIA_IsSelectionPatternAvailablePropertyId, UIA_ClassNamePropertyId};
        for (const auto id : properties) { hr = cache->AddProperty(id); if (FAILED(hr)) return status_of(hr); }
        hr = ui->get_ContentViewWalker(walker.put());
        if (FAILED(hr)) return status_of(hr);
        hr = ui->get_RawViewWalker(raw_walker.put());
        if (FAILED(hr)) return status_of(hr);
        Com<IUIAutomationElement> root;
        hr = ui->ElementFromHandleBuildCache(window, cache.get(), root.put());
        if (FAILED(hr)) return status_of(hr);
        if (!root) return BR_AUTO_PROVIDER_ERROR;
        root_identity = runtime_id(root.get());
        bound_root = std::move(root);
        return root_identity.empty() ? BR_AUTO_UNSUPPORTED : BR_AUTO_OK;
    }
    br_auto_status root(Com<IUIAutomationElement>& out, const br_auto_operation* op) {
        auto st = br_auto_operation_status(op);
        if (st != BR_AUTO_OK) return st;
        DWORD current = 0;
        if (!IsWindow(window) || !GetWindowThreadProcessId(window, &current) || current != pid ||
            WaitForSingleObject(process, 0) != WAIT_TIMEOUT) return BR_AUTO_STALE;
        // Старая COM-ссылка должна оставаться живой даже при повторном использовании HWND.
        Com<IUIAutomationElement> original;
        auto hr = bound_root->BuildUpdatedCache(cache.get(), original.put());
        if (FAILED(hr)) return status_of(hr);
        if (!original || runtime_id(original.get()) != root_identity) return BR_AUTO_STALE;
        hr = ui->ElementFromHandleBuildCache(window, cache.get(), out.put());
        if (FAILED(hr)) return status_of(hr);
        if (!out) return BR_AUTO_STALE;
        BOOL same = FALSE;
        hr = ui->CompareElements(original.get(), out.get(), &same);
        if (FAILED(hr)) return status_of(hr);
        return same && runtime_id(out.get()) == root_identity ? br_auto_operation_status(op) : BR_AUTO_STALE;
    }
    br_auto_status emit_entry(const Entry& entry, IUIAutomationElement* e, br_auto_emit_fn emit, void* sink, size_t& remaining) {
        int current_pid = 0;
        if (FAILED(e->get_CachedProcessId(&current_pid))) return BR_AUTO_PROVIDER_ERROR;
        if (static_cast<DWORD>(current_pid) != pid) return BR_AUTO_DENIED;
        br_auto_node node{}; node.native_id = entry.id; node.incarnation = 1; node.parent_native_id = entry.parent;
        CONTROLTYPEID type = 0;
        if (FAILED(e->get_CachedControlType(&type))) return BR_AUTO_PROVIDER_ERROR;
        const std::string r = role(type);
        if (r.size() > remaining) return BR_AUTO_LIMIT_EXCEEDED;
        remaining -= r.size();
        const std::string name = cached_text(e, UIA_NamePropertyId, remaining), aid = cached_text(e, UIA_AutomationIdPropertyId, remaining);
        bool password = false, value_known = false;
        const bool password_known = cached_bool(e, UIA_IsPasswordPropertyId, password);
        std::string value = password_known && !password ? cached_text(e, UIA_ValueValuePropertyId, remaining, &value_known) : "";
        bool selection = false;
        cached_bool(e, UIA_IsSelectionPatternAvailablePropertyId, selection);
        if (!value_known && selection) {
            Com<IUIAutomationSelectionPattern> p;
            if (SUCCEEDED(e->GetCurrentPatternAs(UIA_SelectionPatternId, __uuidof(IUIAutomationSelectionPattern), reinterpret_cast<void**>(p.put()))) && p) {
                Com<IUIAutomationElementArray> selected;
                int count = -1;
                if (SUCCEEDED(p->GetCurrentSelection(selected.put())) && selected && SUCCEEDED(selected->get_Length(&count))) {
                    if (!count) { value_known = true; }
                    else if (count == 1) {
                        Com<IUIAutomationElement> item;
                        if (SUCCEEDED(selected->GetElement(0, item.put())) && item) {
                            BSTR selected_name = nullptr;
                            if (SUCCEEDED(item->get_CurrentName(&selected_name))) {
                                try { value = utf8(selected_name, remaining); value_known = true; }
                                catch (...) { SysFreeString(selected_name); throw; }
                                SysFreeString(selected_name);
                            }
                        }
                    }
                }
            }
        }
        node.role = {r.data(), r.size()}; node.name = {name.data(), name.size()};
        node.automation_id = {aid.data(), aid.size()}; node.value = {value.data(), value.size()};
        if (value_known) node.known |= BR_AUTO_VALUE_KNOWN;
        node.known |= BR_AUTO_ADDRESSABLE;
        if (entry.addressable) node.state |= BR_AUTO_ADDRESSABLE;
        auto flag = [&](PROPERTYID property, uint32_t bit, bool invert = false) {
            bool b = false;
            if (cached_bool(e, property, b)) { node.known |= bit; if (b != invert) node.state |= bit; }
        };
        flag(UIA_IsEnabledPropertyId, BR_AUTO_ENABLED); flag(UIA_IsOffscreenPropertyId, BR_AUTO_VISIBLE, true);
        flag(UIA_SelectionItemIsSelectedPropertyId, BR_AUTO_SELECTED); flag(UIA_HasKeyboardFocusPropertyId, BR_AUTO_FOCUSED);
        Variant toggle;
        if (SUCCEEDED(e->GetCachedPropertyValueEx(UIA_ToggleToggleStatePropertyId, TRUE, &toggle.v)) && toggle.v.vt == VT_I4 &&
            (toggle.v.lVal == ToggleState_On || toggle.v.lVal == ToggleState_Off)) {
            node.known |= BR_AUTO_TOGGLED; if (toggle.v.lVal == ToggleState_On) node.state |= BR_AUTO_TOGGLED;
        }
        const PROPERTYID available[] = {UIA_IsInvokePatternAvailablePropertyId, UIA_IsValuePatternAvailablePropertyId,
            UIA_IsTogglePatternAvailablePropertyId, UIA_IsSelectionItemPatternAvailablePropertyId, UIA_IsKeyboardFocusablePropertyId};
        for (uint32_t i = 0; i < 5; ++i) {
            bool yes = false;
            if (cached_bool(e, available[i], yes) && yes) node.actions |= BR_AUTO_ACTION_BIT(i + 1);
        }
        if (selection) node.actions |= BR_AUTO_ACTION_BIT(BR_AUTO_SELECT_NAMED);
        if (!entry.addressable) node.actions = 0;
        RECT rect{};
        if (SUCCEEDED(e->get_CachedBoundingRectangle(&rect))) {
            const int64_t w = int64_t(rect.right) - rect.left, h = int64_t(rect.bottom) - rect.top;
            if (w < 0 || h < 0 || w > INT32_MAX || h > INT32_MAX) return BR_AUTO_PROVIDER_ERROR;
            node.bounds = {rect.left, rect.top, static_cast<int32_t>(w), static_cast<int32_t>(h)};
        }
        return emit(sink, &node);
    }
    br_auto_status nonclient(IUIAutomationElement* element, IUIAutomationElement* window_root,
                            const br_auto_operation* op, bool& excluded) {
        excluded = false;
        Com<IUIAutomationElement> ancestor;
        auto current = element;
        for (uint32_t depth = 0; depth <= 128; ++depth) {
            const auto st = br_auto_operation_status(op); if (st != BR_AUTO_OK) return st;
            BOOL same = FALSE;
            auto hr = ui->CompareElements(current, window_root, &same);
            if (FAILED(hr)) return status_of(hr);
            if (same) return BR_AUTO_OK;
            CONTROLTYPEID type = 0;
            hr = current->get_CurrentControlType(&type);
            if (FAILED(hr)) return status_of(hr);
            if (type == UIA_TitleBarControlTypeId) { excluded = true; return BR_AUTO_OK; }
            Com<IUIAutomationElement> parent;
            hr = raw_walker->GetParentElement(current, parent.put());
            if (FAILED(hr)) return status_of(hr);
            if (!parent) return BR_AUTO_STALE;
            ancestor = std::move(parent); current = ancestor.get();
        }
        return BR_AUTO_STALE;
    }
    br_auto_status observe(const br_auto_observe_request& request, const br_auto_operation* op,
                           br_auto_emit_fn emit, void* sink, br_auto_observe_info* info) {
        DpiScope dpi;
        struct Visit { Com<IUIAutomationElement> element; uint64_t parent; uint32_t depth; };
        std::vector<Visit> stack;
        Com<IUIAutomationElement> first;
        auto st = root(first, op); if (st != BR_AUTO_OK) return st;
        Com<IUIAutomationElement> window_root;
        first->AddRef(); *window_root.put() = first.get();
        stack.push_back({std::move(first), 0, 0});
        std::unordered_map<std::string, std::shared_ptr<Entry>> fresh;
        std::unordered_map<uint64_t, std::shared_ptr<Entry>> ids;
        size_t remaining = std::min(request.max_text_bytes, text_limit);
        size_t identity_remaining = 64u * 1024u * 1024u;
        uint32_t visited = 0;
        info->complete = 1;
        auto relatives = [&](IUIAutomationElement* element, uint64_t parent, uint64_t child_parent, uint32_t depth) {
            if (depth) {
                Com<IUIAutomationElement> sibling;
                const auto hr = walker->GetNextSiblingElementBuildCache(element, cache.get(), sibling.put());
                if (FAILED(hr)) return status_of(hr);
                if (sibling) stack.push_back({std::move(sibling), parent, depth});
            }
            Com<IUIAutomationElement> child;
            const auto hr = walker->GetFirstChildElementBuildCache(element, cache.get(), child.put());
            if (FAILED(hr)) return status_of(hr);
            if (child) {
                if (depth >= max_depth) info->complete = 0;
                else stack.push_back({std::move(child), child_parent, depth + 1});
            }
            return BR_AUTO_OK;
        };
        while (!stack.empty()) {
            st = br_auto_operation_status(op); if (st != BR_AUTO_OK) return st;
            if (visited >= request.max_nodes) { info->complete = 0; break; }
            ++visited;
            Visit v = std::move(stack.back()); stack.pop_back();
            auto e = std::make_shared<Entry>(); e->key = runtime_id(v.element.get(), &st);
            if (st == BR_AUTO_UNSUPPORTED) {
                bool excluded = false;
                st = nonclient(v.element.get(), window_root.get(), op, excluded);
                if (st != BR_AUTO_OK) return st;
                if (excluded) {
                    st = relatives(v.element.get(), v.parent, v.parent, v.depth);
                    if (st != BR_AUTO_OK) return st;
                    continue;
                }
                // Метаданные наблюдаем полностью; адрес без устойчивого ID действует только в этом снимке.
                if (next == UINT64_MAX) return BR_AUTO_LIMIT_EXCEEDED;
                e->addressable = false;
                e->key = "volatile:" + std::to_string(next);
                st = BR_AUTO_OK;
            }
            if (st != BR_AUTO_OK) return st;
            if (fresh.count(e->key)) return BR_AUTO_PROVIDER_ERROR;
            // Ключ хранится в Entry и в таблице; ограничение независимо от видимого текста.
            if (e->key.size() > identity_remaining / 2) return BR_AUTO_LIMIT_EXCEEDED;
            identity_remaining -= e->key.size() * 2;
            const auto old = entries.find(e->key);
            bool retained = false;
            if (old != entries.end()) {
                Com<IUIAutomationElement> previous;
                auto hr = old->second->element->BuildUpdatedCache(cache.get(), previous.put());
                if (SUCCEEDED(hr) && previous) {
                    BOOL same = FALSE;
                    hr = ui->CompareElements(previous.get(), v.element.get(), &same);
                    if (FAILED(hr)) return status_of(hr);
                    retained = same && runtime_id(previous.get()) == e->key;
                } else if (status_of(hr) != BR_AUTO_STALE) {
                    return FAILED(hr) ? status_of(hr) : BR_AUTO_PROVIDER_ERROR;
                }
            }
            if (!retained && next == UINT64_MAX) return BR_AUTO_LIMIT_EXCEEDED;
            e->id = retained ? old->second->id : next++; e->parent = v.parent;
            e->element = std::move(v.element);
            st = emit_entry(*e, e->element.get(), emit, sink, remaining); if (st != BR_AUTO_OK) return st;
            fresh.emplace(e->key, e); ids.emplace(e->id, e);
            // DFS хранит не более одного следующего соседа на уровень.
            st = relatives(e->element.get(), v.parent, e->id, v.depth);
            if (st != BR_AUTO_OK) return st;
        }
        st = br_auto_operation_status(op); if (st != BR_AUTO_OK) return st;
        entries = std::move(fresh); tokens = std::move(ids); return BR_AUTO_OK;
    }
    br_auto_status lookup(uint64_t id, uint64_t incarnation, const br_auto_operation* op,
                          std::shared_ptr<Entry>& entry, Com<IUIAutomationElement>& live) {
        if (incarnation != 1) return BR_AUTO_STALE;
        Com<IUIAutomationElement> window_root;
        auto st = root(window_root, op); if (st != BR_AUTO_OK) return st;
        const auto it = tokens.find(id); if (it == tokens.end()) return BR_AUTO_STALE;
        entry = it->second;
        if (!entry->addressable) return BR_AUTO_UNSUPPORTED;
        const auto hr = entry->element->BuildUpdatedCache(cache.get(), live.put());
        if (FAILED(hr)) return status_of(hr);
        if (!live) return BR_AUTO_STALE;
        if (runtime_id(live.get()) != entry->key) return BR_AUTO_STALE;
        int current_pid = 0;
        if (FAILED(live->get_CachedProcessId(&current_pid))) return BR_AUTO_PROVIDER_ERROR;
        if (static_cast<DWORD>(current_pid) != pid) return BR_AUTO_DENIED;
        // Перемещённый за границы окна элемент не становится новой целью автоматически.
        Com<IUIAutomationElement> ancestor;
        BOOL same = FALSE;
        auto h = ui->CompareElements(live.get(), window_root.get(), &same);
        if (FAILED(h)) return status_of(h);
        IUIAutomationElement* current = live.get();
        for (uint32_t depth = 0; !same && depth < 128; ++depth) {
            st = br_auto_operation_status(op); if (st != BR_AUTO_OK) return st;
            Com<IUIAutomationElement> parent;
            h = walker->GetParentElement(current, parent.put());
            if (FAILED(h)) return status_of(h);
            if (!parent) return BR_AUTO_STALE;
            h = ui->CompareElements(parent.get(), window_root.get(), &same);
            if (FAILED(h)) return status_of(h);
            ancestor = std::move(parent); current = ancestor.get();
        }
        return same ? br_auto_operation_status(op) : BR_AUTO_STALE;
    }
    br_auto_status read(uint64_t id, uint64_t incarnation, const br_auto_operation* op, br_auto_emit_fn emit, void* sink) {
        DpiScope dpi; std::shared_ptr<Entry> entry; Com<IUIAutomationElement> live;
        const auto st = lookup(id, incarnation, op, entry, live);
        size_t remaining = text_limit;
        return st == BR_AUTO_OK ? emit_entry(*entry, live.get(), emit, sink, remaining) : st;
    }
    template<class T> br_auto_status pattern(IUIAutomationElement* e, PATTERNID id, Com<T>& p) {
        const auto hr = e->GetCurrentPatternAs(id, __uuidof(T), reinterpret_cast<void**>(p.put()));
        return SUCCEEDED(hr) && !p ? BR_AUTO_UNSUPPORTED : status_of(hr);
    }
    br_auto_status act(uint64_t id, uint64_t incarnation, const br_auto_action& action, const br_auto_operation* op, uint32_t* effect) {
        *effect = BR_AUTO_NOT_DISPATCHED;
        DpiScope dpi; std::shared_ptr<Entry> entry; Com<IUIAutomationElement> live;
        auto st = lookup(id, incarnation, op, entry, live); if (st != BR_AUTO_OK) return st;
        BOOL enabled = FALSE;
        HRESULT hr = live->get_CurrentIsEnabled(&enabled);
        if (FAILED(hr)) return status_of(hr);
        if (!enabled) return BR_AUTO_PRECONDITION_FAILED;
        auto dispatch = [&](auto&& fn) {
            const auto ready = br_auto_operation_status(op); if (ready != BR_AUTO_OK) return ready;
            *effect = BR_AUTO_EFFECT_UNKNOWN;
            const HRESULT result = fn();
            if (SUCCEEDED(result)) *effect = BR_AUTO_DISPATCHED;
            return status_of(result);
        };
        switch (action.kind) {
        case BR_AUTO_INVOKE: {
            // WinForms legacy Invoke иногда требует input desktop. MSAA вызывает действие непосредственно.
            size_t budget = text_limit;
            const auto cls = cached_text(live.get(), UIA_ClassNamePropertyId, budget);
            if (cls.rfind("WindowsForms", 0) == 0) {
                Com<IUIAutomationLegacyIAccessiblePattern> legacy;
                const auto have = pattern(live.get(), UIA_LegacyIAccessiblePatternId, legacy);
                if (have == BR_AUTO_OK) return dispatch([&] { return legacy->DoDefaultAction(); });
                if (have != BR_AUTO_UNSUPPORTED) return have;
            }
            Com<IUIAutomationInvokePattern> p; st = pattern(live.get(), UIA_InvokePatternId, p);
            return st == BR_AUTO_OK ? dispatch([&] { return p->Invoke(); }) : st;
        }
        case BR_AUTO_SET_VALUE: {
            Com<IUIAutomationValuePattern> p; st = pattern(live.get(), UIA_ValuePatternId, p);
            if (st != BR_AUTO_OK) return st;
            BOOL read_only = TRUE; hr = p->get_CurrentIsReadOnly(&read_only);
            if (FAILED(hr)) return status_of(hr);
            if (read_only) return BR_AUTO_PRECONDITION_FAILED;
            const Bstr value(utf16(action.value));
            return dispatch([&] { return p->SetValue(value.p); });
        }
        case BR_AUTO_TOGGLE: {
            Com<IUIAutomationTogglePattern> p; st = pattern(live.get(), UIA_TogglePatternId, p);
            return st == BR_AUTO_OK ? dispatch([&] { return p->Toggle(); }) : st;
        }
        case BR_AUTO_SELECT: {
            Com<IUIAutomationSelectionItemPattern> p; st = pattern(live.get(), UIA_SelectionItemPatternId, p);
            return st == BR_AUTO_OK ? dispatch([&] { return p->Select(); }) : st;
        }
        case BR_AUTO_SELECT_NAMED: {
            // Имя является явным запросом к текущим детям контейнера, а не заменой старого ID.
            const auto wanted = utf16(action.value);
            Com<IUIAutomationElement> match, child;
            auto find = [&](IUIAutomationElement* candidate) -> br_auto_status {
                BSTR name = nullptr;
                const auto h = candidate->get_CurrentName(&name);
                if (FAILED(h)) return status_of(h);
                const bool equal = name && std::wstring_view(name, SysStringLen(name)) == wanted;
                SysFreeString(name);
                if (!equal) return BR_AUTO_OK;
                Com<IUIAutomationSelectionItemPattern> p;
                auto supported = pattern(candidate, UIA_SelectionItemPatternId, p);
                if (supported == BR_AUTO_UNSUPPORTED) return BR_AUTO_OK;
                if (supported != BR_AUTO_OK) return supported;
                Com<IUIAutomationElement> container;
                const auto parent_status = p->get_CurrentSelectionContainer(container.put());
                if (FAILED(parent_status)) return status_of(parent_status);
                if (!container) return BR_AUTO_STALE;
                BOOL own = FALSE;
                const auto compared = ui->CompareElements(container.get(), live.get(), &own);
                if (FAILED(compared)) return status_of(compared);
                if (!own) return BR_AUTO_OK;
                if (match) return BR_AUTO_AMBIGUOUS;
                candidate->AddRef(); *match.put() = candidate;
                return BR_AUTO_OK;
            };
            std::vector<Com<IUIAutomationElement>> pending;
            auto h = raw_walker->GetFirstChildElement(live.get(), child.put());
            if (FAILED(h)) return status_of(h);
            if (child) pending.push_back(std::move(child));
            size_t count = 0;
            while (!pending.empty()) {
                auto ready = br_auto_operation_status(op); if (ready != BR_AUTO_OK) return ready;
                if (++count > 4096) return BR_AUTO_LIMIT_EXCEEDED;
                auto current = std::move(pending.back()); pending.pop_back();
                st = find(current.get()); if (st != BR_AUTO_OK) return st;
                Com<IUIAutomationElement> sibling, nested;
                h = raw_walker->GetNextSiblingElement(current.get(), sibling.put());
                if (FAILED(h)) return status_of(h);
                if (sibling) pending.push_back(std::move(sibling));
                h = raw_walker->GetFirstChildElement(current.get(), nested.put());
                if (FAILED(h)) return status_of(h);
                if (nested) pending.push_back(std::move(nested));
                if (pending.size() > 128) return BR_AUTO_LIMIT_EXCEEDED;
            }
            if (!match) return BR_AUTO_NOT_FOUND;
            BOOL selected_enabled = FALSE;
            h = match->get_CurrentIsEnabled(&selected_enabled);
            if (FAILED(h)) return status_of(h);
            if (!selected_enabled) return BR_AUTO_PRECONDITION_FAILED;
            Com<IUIAutomationSelectionItemPattern> p;
            st = pattern(match.get(), UIA_SelectionItemPatternId, p);
            return st == BR_AUTO_OK ? dispatch([&] { return p->Select(); }) : st;
        }
        case BR_AUTO_FOCUS: return dispatch([&] { return live->SetFocus(); });
        default: return BR_AUTO_UNSUPPORTED;
        }
    }
};
// COM-объекты создаются, используются и освобождаются в одном MTA-потоке.
// Зависший чужой COM-вызов задерживает завершение и уничтожение сессии.
class Worker {
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stop_ = false;
    std::function<void(Native&)> pending_;
    std::thread thread_;
public:
    Worker() : thread_([this] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            Native native; native.apartment_status = hr;
            for (;;) {
                std::function<void(Native&)> work;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    ready_.wait(lock, [&] { return stop_ || static_cast<bool>(pending_); });
                    if (stop_ && !pending_) break;
                    work = std::move(pending_); pending_ = {};
                }
                work(native);
            }
        }
        if (SUCCEEDED(hr)) CoUninitialize();
    }) {}
    ~Worker() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; }
        ready_.notify_all(); if (thread_.joinable()) thread_.join();
    }
    template<class F> br_auto_status call(F&& f) {
        auto task = std::make_shared<std::packaged_task<br_auto_status(Native&)>>(std::forward<F>(f));
        auto result = task->get_future();
        { std::lock_guard<std::mutex> lock(mutex_); pending_ = [task](Native& n) { (*task)(n); }; }
        ready_.notify_one();
        try { return result.get(); }
        catch (const TextLimit&) { return BR_AUTO_LIMIT_EXCEEDED; }
    }
};
}
#endif

br_auto_status br::automation::windows_provider(const br_auto_windows_options& o, size_t text_limit, br_auto_provider& p) {
    p = {};
    if (o.struct_size != sizeof(o) || o.abi_version != BR_AUTO_ABI_VERSION || !o.window ||
        o.window > UINTPTR_MAX || o.max_depth > 128 || !o.connection_timeout_ms || !o.transaction_timeout_ms) return BR_AUTO_INVALID_ARGUMENT;
#if defined(_WIN32) && defined(BR_HAS_WINDOWS_AUTOMATION)
    try {
        auto worker = std::make_unique<Worker>();
        auto st = worker->call([&](Native& n) { return n.init(o, text_limit); });
        if (st != BR_AUTO_OK) return st;
        p.struct_size = sizeof(p); p.abi_version = BR_AUTO_ABI_VERSION; p.name = "windows-uia"; p.user = worker.get();
        p.actions = BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE) | BR_AUTO_ACTION_BIT(BR_AUTO_SET_VALUE) |
            BR_AUTO_ACTION_BIT(BR_AUTO_TOGGLE) | BR_AUTO_ACTION_BIT(BR_AUTO_SELECT) | BR_AUTO_ACTION_BIT(BR_AUTO_FOCUS) |
            BR_AUTO_ACTION_BIT(BR_AUTO_SELECT_NAMED);
        p.observe = [](void* u, const br_auto_observe_request* r, const br_auto_operation* op,
                       br_auto_emit_fn emit, void* sink, br_auto_observe_info* info) {
            return static_cast<Worker*>(u)->call([&](Native& n) { return n.observe(*r, op, emit, sink, info); });
        };
        p.act = [](void* u, uint64_t id, uint64_t gen, const br_auto_action* a, const br_auto_operation* op, uint32_t* effect) {
            return static_cast<Worker*>(u)->call([&](Native& n) { return n.act(id, gen, *a, op, effect); });
        };
        p.read = [](void* u, uint64_t id, uint64_t gen, const br_auto_operation* op, br_auto_emit_fn emit, void* sink) {
            return static_cast<Worker*>(u)->call([&](Native& n) { return n.read(id, gen, op, emit, sink); });
        };
        p.destroy = [](void* u) { delete static_cast<Worker*>(u); };
        worker.release();
        return BR_AUTO_OK;
    } catch (const std::bad_alloc&) { return BR_AUTO_OUT_OF_MEMORY; }
      catch (...) { return BR_AUTO_PROVIDER_ERROR; }
#else
    (void)text_limit;
    return BR_AUTO_UNSUPPORTED;
#endif
}

extern "C" {
br_auto_windows_options br_auto_windows_options_default(uint64_t window) {
    return {sizeof(br_auto_windows_options), BR_AUTO_ABI_VERSION, window, 32, 1000, 1000};
}
br_auto_status br_auto_windows_open(const br_auto_windows_options* o, const br_auto_options* options, br_auto_session** out) {
    if (!out) return BR_AUTO_INVALID_ARGUMENT;
    *out = nullptr;
    if (!o || (options && (options->struct_size != sizeof(*options) || options->abi_version != BR_AUTO_ABI_VERSION)))
        return BR_AUTO_INVALID_ARGUMENT;
    br_auto_provider p{};
    auto st = br::automation::windows_provider(*o, options ? options->max_text_bytes : br_auto_options_default().max_text_bytes, p);
    if (st != BR_AUTO_OK) return st;
    st = br_auto_session_create(&p, options, out);
    if (st != BR_AUTO_OK) p.destroy(p.user);
    return st;
}
}
