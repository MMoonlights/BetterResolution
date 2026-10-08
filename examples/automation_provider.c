/* Пример подключения своего провайдера к SDK без desktop и сети.
 * Замените callbacks на UI/DOM/accessibility backend своего приложения. */
#include <br/br_automation.h>
#include <stdio.h>
#include <string.h>

typedef struct app { char field[256]; unsigned submits, tree_reads, target_reads; } app;
static br_auto_string text(const char* s) { br_auto_string v = {s, strlen(s)}; return v; }
static br_auto_status emit_one(app* a, uint64_t id, br_auto_emit_fn emit, void* sink) {
    br_auto_node node = {0};
    node.native_id = id; node.incarnation = 1;
    node.role = text(id == 1 ? "edit" : "button");
    node.name = text(id == 1 ? "Query" : "Submit");
    node.known = BR_AUTO_ENABLED | BR_AUTO_VISIBLE | BR_AUTO_VALUE_KNOWN;
    node.state = BR_AUTO_ENABLED | BR_AUTO_VISIBLE;
    node.value = text(id == 1 ? a->field : "");
    node.actions = BR_AUTO_ACTION_BIT(id == 1 ? BR_AUTO_SET_VALUE : BR_AUTO_INVOKE);
    return emit(sink, &node);
}
static br_auto_status observe(void* u, const br_auto_observe_request* request, const br_auto_operation* op,
                              br_auto_emit_fn emit, void* sink, br_auto_observe_info* info) {
    app* a = (app*)u; br_auto_status status; uint64_t id;
    ++a->tree_reads; info->complete = 0;
    for (id = 1; id <= 2 && id <= request->max_nodes; ++id) {
        status = br_auto_operation_status(op); if (status != BR_AUTO_OK) return status;
        status = emit_one(a, id, emit, sink); if (status != BR_AUTO_OK) return status;
    }
    info->complete = id > 2; return BR_AUTO_OK;
}
static br_auto_status read_target(void* u, uint64_t id, uint64_t gen, const br_auto_operation* op,
                                  br_auto_emit_fn emit, void* sink) {
    app* a = (app*)u; br_auto_status status = br_auto_operation_status(op);
    if (status != BR_AUTO_OK) return status;
    if (gen != 1 || id < 1 || id > 2) return BR_AUTO_STALE;
    ++a->target_reads; return emit_one(a, id, emit, sink);
}
static br_auto_status act(void* u, uint64_t id, uint64_t gen, const br_auto_action* action,
                          const br_auto_operation* op, uint32_t* effect) {
    app* a = (app*)u; br_auto_status status = br_auto_operation_status(op);
    *effect = BR_AUTO_NOT_DISPATCHED;
    if (status != BR_AUTO_OK) return status;
    if (gen != 1 || id < 1 || id > 2) return BR_AUTO_STALE;
    if (id == 1 && action->kind == BR_AUTO_SET_VALUE) {
        if (action->value.size >= sizeof(a->field)) return BR_AUTO_LIMIT_EXCEEDED;
        if (action->value.size) memcpy(a->field, action->value.data, action->value.size);
        a->field[action->value.size] = 0;
    } else if (id == 2 && action->kind == BR_AUTO_INVOKE) ++a->submits;
    else return BR_AUTO_UNSUPPORTED;
    *effect = BR_AUTO_DISPATCHED; return BR_AUTO_OK;
}
int main(void) {
    app a = {0}; br_auto_provider p = {0}; br_auto_session* session = NULL;
    br_auto_snapshot* snapshot = NULL; br_auto_element field = {0}, submit = {0};
    br_auto_options options = br_auto_options_default();
    br_auto_step steps[2] = {0}; br_auto_action_result results[2] = {0}; br_auto_batch_result batch = {0};
    int success = 0;
    p.struct_size = sizeof(p); p.abi_version = BR_AUTO_ABI_VERSION; p.name = "example-app"; p.user = &a;
    p.actions = BR_AUTO_ACTION_BIT(BR_AUTO_SET_VALUE) | BR_AUTO_ACTION_BIT(BR_AUTO_INVOKE);
    p.observe = observe; p.act = act; p.read = read_target;
    options.allow_actions = 1;
    if (br_auto_session_create(&p, &options, &session) != BR_AUTO_OK) goto done;
    if (br_auto_observe(session, 0, &snapshot) != BR_AUTO_OK) goto done;
    if (br_auto_snapshot_element(snapshot, 0, &field) != BR_AUTO_OK ||
        br_auto_snapshot_element(snapshot, 1, &submit) != BR_AUTO_OK) goto done;
    steps[0].kind = BR_AUTO_STEP_ACT; steps[0].action = br_auto_action_default(BR_AUTO_SET_VALUE);
    steps[0].action.target.element_id = field.id; steps[0].action.value = text("BR provider API");
    steps[1].kind = BR_AUTO_STEP_ACT; steps[1].action = br_auto_action_default(BR_AUTO_INVOKE);
    steps[1].action.target.element_id = submit.id;
    if (br_auto_batch(session, steps, 2, 1000, results, 2, &batch) != BR_AUTO_OK) goto done;
    /* Проверяем состояние приложения независимо от ответа SDK. */
    success = strcmp(a.field, "BR provider API") == 0 && a.submits == 1 && a.tree_reads == 1 &&
        a.target_reads == 3 && results[0].verification == BR_AUTO_SATISFIED &&
        results[1].verification == BR_AUTO_NOT_REQUESTED;
    printf("provider=%s tree_reads=%u target_reads=%u submits=%u verified=%u\n",
        br_auto_session_provider(session), a.tree_reads, a.target_reads, a.submits, results[0].verification);
done:
    br_auto_snapshot_destroy(snapshot); br_auto_session_destroy(session);
    return success ? 0 : 1;
}
