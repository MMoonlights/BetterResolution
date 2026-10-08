/* Расширяемый SDK для работы с интерфейсом, без зависимости от модели ИИ и MCP.
 * Вызовы сессии последовательные; параллельно разрешён только cancel.
 * Свои провайдеры выполняются как доверенный native-код.
 */
#ifndef BETTERRESOLUTION_BR_AUTOMATION_H
#define BETTERRESOLUTION_BR_AUTOMATION_H
#include "br.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BR_AUTO_ABI_VERSION 1u

typedef struct br_auto_session br_auto_session;
typedef struct br_auto_snapshot br_auto_snapshot;
typedef struct br_auto_operation br_auto_operation;
typedef struct br_auto_string { const char* data; size_t size; } br_auto_string;

typedef enum br_auto_status {
    BR_AUTO_OK = 0, BR_AUTO_INVALID_ARGUMENT, BR_AUTO_OUT_OF_MEMORY,
    BR_AUTO_UNSUPPORTED, BR_AUTO_NOT_FOUND, BR_AUTO_AMBIGUOUS, BR_AUTO_STALE,
    BR_AUTO_INCOMPLETE, BR_AUTO_TIMEOUT, BR_AUTO_CANCELLED, BR_AUTO_DENIED,
    BR_AUTO_PRECONDITION_FAILED, BR_AUTO_PROVIDER_ERROR, BR_AUTO_BUSY,
    BR_AUTO_LIMIT_EXCEEDED, BR_AUTO_SKIPPED
} br_auto_status;

typedef enum br_auto_state {
    BR_AUTO_ENABLED = 1u, BR_AUTO_VISIBLE = 2u, BR_AUTO_SELECTED = 4u,
    BR_AUTO_TOGGLED = 8u, BR_AUTO_FOCUSED = 16u, BR_AUTO_VALUE_KNOWN = 32u,
    BR_AUTO_ADDRESSABLE = 64u /* Известная стабильная привязка; отсутствие признака не доказывает обратное. */
} br_auto_state;
typedef enum br_auto_action_kind {
    BR_AUTO_INVOKE = 1, BR_AUTO_SET_VALUE, BR_AUTO_TOGGLE, BR_AUTO_SELECT,
    BR_AUTO_FOCUS, BR_AUTO_CUSTOM,
    BR_AUTO_SELECT_NAMED /* Выбор одного дочернего элемента по точному имени в стабильном контейнере. */
} br_auto_action_kind;
#define BR_AUTO_ACTION_BIT(kind) (UINT64_C(1) << (kind))

typedef struct br_auto_node {
    /* Повторное использование native ID другим объектом требует нового incarnation. */
    uint64_t native_id, incarnation, parent_native_id;
    br_auto_string role, name, automation_id, value;
    br_rect_i32 bounds;
    uint32_t known, state;
    uint64_t actions;
} br_auto_node;

typedef struct br_auto_element {
    uint64_t id, revision, parent_id; /* Непрозрачные IDs внутри процесса; в JS используйте BigInt. */
    br_auto_string role, name, automation_id, value;
    br_rect_i32 bounds;
    uint32_t known, state;
    uint64_t actions;
} br_auto_element;

typedef struct br_auto_selector {
    br_auto_string role, name, automation_id; /* Пустое поле не ограничивает поиск; точное совпадение UTF-8. */
    uint32_t required_state, forbidden_state;
} br_auto_selector;

typedef struct br_auto_target {
    uint64_t element_id;
    const br_auto_selector* selector; /* Укажите либо ID, либо selector. */
} br_auto_target;

typedef enum br_auto_property {
    BR_AUTO_EXISTS = 1, BR_AUTO_VALUE_EQUALS, BR_AUTO_IS_ENABLED,
    BR_AUTO_IS_VISIBLE, BR_AUTO_IS_SELECTED, BR_AUTO_IS_TOGGLED, BR_AUTO_IS_FOCUSED
} br_auto_property;
typedef struct br_auto_condition {
    br_auto_target target;
    uint32_t property; /* br_auto_property */
    uint32_t expected; /* 0/1 для логических свойств. */
    br_auto_string value; /* Точное VALUE_EQUALS, включая пустую строку. */
} br_auto_condition;

typedef struct br_auto_action {
    uint32_t struct_size, kind; /* Создавайте через br_auto_action_default. */
    br_auto_target target;
    br_auto_string value, custom_action;
    uint64_t expected_revision; /* 0 отключает проверку revision, но не идентичности. */
    const br_auto_condition* before;
    const br_auto_condition* after; /* NULL включает встроенные проверки SET_VALUE/SELECT/SELECT_NAMED. */
    uint32_t timeout_ms; /* 0 = значение сессии. */
    uint32_t dry_run; /* Не выполняет действие и не подтверждает результат. */
} br_auto_action;

typedef enum br_auto_effect {
    BR_AUTO_NOT_DISPATCHED = 0, BR_AUTO_DISPATCHED = 1, BR_AUTO_EFFECT_UNKNOWN = 2
} br_auto_effect;
typedef enum br_auto_verification {
    BR_AUTO_NOT_REQUESTED = 0, BR_AUTO_SATISFIED, BR_AUTO_UNSATISFIED, BR_AUTO_UNKNOWN
} br_auto_verification;
typedef struct br_auto_action_result {
    br_auto_status status;
    uint32_t effect, verification;
    uint64_t element_id, elapsed_us;
} br_auto_action_result;

typedef struct br_auto_observe_request { uint32_t max_nodes; size_t max_text_bytes; } br_auto_observe_request;
typedef struct br_auto_observe_info { uint32_t complete; } br_auto_observe_info;
typedef br_auto_status (*br_auto_emit_fn)(void* sink, const br_auto_node* node);

typedef struct br_auto_provider {
    uint32_t struct_size, abi_version;
    const char* name; /* UTF-8, копируется при создании без обрезки. */
    void* user;
    uint64_t actions;
    /* Одна стабильная область на сессию; BR сразу копирует строки из emit.
     * complete=1 только после полного чтения области; ошибку emit верните сразу. */
    br_auto_status (*observe)(void*, const br_auto_observe_request*, const br_auto_operation*,
                              br_auto_emit_fn, void* sink, br_auto_observe_info*);
    /* Перед отправкой проверьте идентичность и доступность цели; задайте effect на каждом пути.
     * Не заменяйте исчезнувший объект и не повторяйте действие с неизвестным исходом. */
    br_auto_status (*act)(void*, uint64_t native_id, uint64_t incarnation,
                          const br_auto_action*, const br_auto_operation*, uint32_t* effect);
    /* Необязательное чтение только этого объекта через emit, либо STALE/NOT_FOUND.
     * Обновляет известный ID без обхода всего дерева. */
    br_auto_status (*read)(void*, uint64_t native_id, uint64_t incarnation, const br_auto_operation*,
                           br_auto_emit_fn, void* sink);
    /* Необязательное ожидание события с учётом op и лимита, затем BR читает состояние. */
    br_auto_status (*wait_event)(void*, uint32_t max_wait_ms, const br_auto_operation*);
    /* Вызывается один раз при destroy после успешного создания сессии. */
    void (*destroy)(void*);
} br_auto_provider;

typedef br_auto_status (*br_auto_authorize_fn)(void*, const br_auto_action*, const br_auto_element*);
typedef struct br_auto_options {
    uint32_t struct_size, abi_version;
    uint32_t max_nodes, default_timeout_ms;
    size_t max_text_bytes;
    uint32_t allow_actions; /* По умолчанию 0, включается явно. */
    uint32_t max_batch_steps;
    br_auto_authorize_fn authorize; /* Проверка разрешения перед каждым действием, включая batch. */
    void* authorize_user;
} br_auto_options;

typedef enum br_auto_change_kind { BR_AUTO_PRESENT = 0, BR_AUTO_ADDED, BR_AUTO_UPDATED, BR_AUTO_REMOVED } br_auto_change_kind;
typedef struct br_auto_change { uint32_t kind; br_auto_element element; } br_auto_change;
typedef struct br_auto_snapshot_info {
    uint64_t id, base_id, elapsed_us;
    size_t element_count, change_count;
    uint32_t is_delta, complete;
} br_auto_snapshot_info;

typedef enum br_auto_step_kind { BR_AUTO_STEP_ACT = 1, BR_AUTO_STEP_WAIT, BR_AUTO_STEP_ASSERT } br_auto_step_kind;
typedef struct br_auto_step {
    uint32_t kind;
    br_auto_action action; /* Только для ACT. */
    br_auto_condition condition; /* Для WAIT или ASSERT. */
} br_auto_step;
typedef struct br_auto_batch_result {
    br_auto_status status;
    size_t completed_steps, failed_index; /* SIZE_MAX, если ошибок не было. */
    uint64_t elapsed_us;
} br_auto_batch_result;

BR_API br_auto_options br_auto_options_default(void);
BR_API br_auto_action br_auto_action_default(uint32_t kind);
BR_API const char* br_auto_status_string(br_auto_status status);
BR_API br_auto_status br_auto_session_create(const br_auto_provider*, const br_auto_options*, br_auto_session**);
/* До destroy завершите все вызовы, включая cancel. */
BR_API void br_auto_session_destroy(br_auto_session*);
BR_API const char* br_auto_session_provider(const br_auto_session*);
BR_API uint64_t br_auto_session_actions(const br_auto_session*);
BR_API void br_auto_cancel(br_auto_session*);
BR_API br_auto_status br_auto_reset_cancel(br_auto_session*);
/* Кооперативная проверка бюджета и отмены; не прерывает чужой код принудительно. */
BR_API br_auto_status br_auto_operation_status(const br_auto_operation*);
BR_API uint32_t br_auto_operation_remaining_ms(const br_auto_operation*);
/* Дельта относительно последнего опубликованного полного снимка этой сессии.
 * Неизвестный или старый since возвращает полный снимок. */
BR_API br_auto_status br_auto_observe(br_auto_session*, uint64_t since, br_auto_snapshot**);
BR_API void br_auto_snapshot_destroy(br_auto_snapshot*);
BR_API br_auto_snapshot_info br_auto_snapshot_get_info(const br_auto_snapshot*);
BR_API br_auto_status br_auto_snapshot_element(const br_auto_snapshot*, size_t index, br_auto_element*);
/* Поиск известного ID без обхода дерева. Строки заимствуются из снимка.
 * Отсутствие в неполном снимке возвращает INCOMPLETE, в полном - NOT_FOUND. */
BR_API br_auto_status br_auto_snapshot_find_id(const br_auto_snapshot*, uint64_t element_id, br_auto_element*);
BR_API br_auto_status br_auto_snapshot_change(const br_auto_snapshot*, size_t index, br_auto_change*);
/* Строки живут до snapshot_destroy; снимок может пережить сессию. */
BR_API br_auto_status br_auto_find(const br_auto_snapshot*, const br_auto_selector*, br_auto_element*);
BR_API br_auto_status br_auto_act(br_auto_session*, const br_auto_action*, br_auto_action_result*);
BR_API br_auto_status br_auto_wait(br_auto_session*, const br_auto_condition*, uint32_t timeout_ms, uint32_t* verification);
/* Проверяет всю цепочку до отправки, выполняет последовательно до первой ошибки.
 * Без отката, повторов и лишних задержек; capacity должна быть не меньше count.
 * Буферы остаются у вызывающего кода. */
BR_API br_auto_status br_auto_batch(br_auto_session*, const br_auto_step*, size_t count, uint32_t timeout_ms,
                                   br_auto_action_result* steps, size_t capacity, br_auto_batch_result*);
#ifdef __cplusplus
}
#endif
#endif
