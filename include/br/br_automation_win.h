/* Необязательный провайдер Windows UI Automation для br_automation.
 * Использует отдельный MTA-поток и системный COM, без PowerShell/CLR/Node.
 * br_auto_windows_open работает кооперативно; open_isolated прерывает зависший helper.
 */
#ifndef BETTERRESOLUTION_BR_AUTOMATION_WIN_H
#define BETTERRESOLUTION_BR_AUTOMATION_WIN_H
#include "br_automation.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct br_auto_windows_options {
    uint32_t struct_size, abi_version;
    uint64_t window; /* Явный HWND, например из br_windows. */
    uint32_t max_depth; /* Корень имеет глубину 0; ограниченное дерево помечается неполным. */
    uint32_t connection_timeout_ms, transaction_timeout_ms;
} br_auto_windows_options;
BR_API br_auto_windows_options br_auto_windows_options_default(uint64_t window);
/* Привязка к одному окну и процессу. Действия требуют options.allow_actions=1.
 * Область наблюдения: корень окна и UIA ContentView. Виртуальные узлы без runtime ID
 * внутри системного TitleBar не входят в область. Остальные видны как метаданные:
 * ADDRESSABLE=false, actions=0, новый ID на каждый snapshot; ими нельзя управлять по ID.
 * UIA/LegacyIAccessible Invoke, Value, Toggle, SelectionItem, SelectNamed и Focus.
 * Глобальный ввод мыши/клавиатуры не используется. SelectNamed проверяет контейнер,
 * уникальность текущего имени и состояние выбранного элемента до отправки.
 * UIA runtime IDs проверяются вместе с сохранёнными COM-ссылками; чужой провайдер
 * обязан сообщать исчезновение объекта, а не перенаправлять старую ссылку на новый.
 * DPI меняется только в рабочем потоке, настройки процесса не затрагиваются.
 * На других платформах или при отключённом модуле возвращается UNSUPPORTED. */
BR_API br_auto_status br_auto_windows_open(const br_auto_windows_options*, const br_auto_options*, br_auto_session**);
typedef struct br_auto_windows_isolated_options {
    uint32_t struct_size, abi_version;
    br_auto_windows_options target;
    br_auto_string helper_path; /* Абсолютный UTF-8 путь к br-uia-helper.exe, без аргументов. */
    uint32_t startup_timeout_ms;
    uint32_t max_ipc_bytes; /* 4096..64 MiB, ограничение одного сообщения. */
} br_auto_windows_isolated_options;
BR_API br_auto_windows_isolated_options br_auto_windows_isolated_options_default(uint64_t window, br_auto_string helper_path);
/* UIA выполняется в отдельном процессе на desktop вызывающего потока.
 * Timeout или cancel прерывают ожидание и завершают helper, без автоматического повтора.
 * При потере ответа на действие effect=EFFECT_UNKNOWN. После сбоя сессия возвращает
 * PROVIDER_ERROR; для восстановления создайте новую сессию и получите новые IDs.
 * Изоляция зависаний, не защитная песочница для недоверенного кода. */
BR_API br_auto_status br_auto_windows_open_isolated(const br_auto_windows_isolated_options*, const br_auto_options*, br_auto_session**);
#ifdef __cplusplus
}
#endif
#endif
