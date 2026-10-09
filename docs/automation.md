# Работа с интерфейсом

BR читает элементы окна, выполняет действия и проверяет условия
Подключайте API к своему приложению или ИИ-агенту

Используйте одну сессию для повторных вызовов
Дельты передают изменения, чтение по ID обновляет нужный элемент
Batch выполняет цепочку действий одним вызовом

## API

Заголовки:

- [br_automation.h](../include/br/br_automation.h) - сессии, провайдеры и действия
- [br_automation.hpp](../include/br/br_automation.hpp) - C++20 обёртка
- [br_automation_win.h](../include/br/br_automation_win.h) - Windows UI Automation
- [br_automation_vision.h](../include/br/br_automation_vision.h) - детали элемента из кадра

| Функция | Назначение |
|---|---|
| `br_auto_observe` | Snapshot интерфейса и изменения |
| `br_auto_find` | Точный поиск элемента |
| `br_auto_snapshot_find_id` | Поиск известного ID |
| `br_auto_act` | Действие с проверками |
| `br_auto_wait` | Ожидание условия |
| `br_auto_batch` | Последовательная цепочка |
| `br_auto_vision_prepare` | Деталь элемента из переданного кадра |

## Windows

```cpp
#include <br/br_automation_win.h>
#include <br/br_automation.hpp>

void inspect(uint64_t hwnd) {
    auto target = br_auto_windows_options_default(hwnd);
    auto options = br_auto_options_default();
    br_auto_session* raw = nullptr;
    auto status = br_auto_windows_open(&target, &options, &raw);
    if (status != BR_AUTO_OK) throw br::AutomationError(status);

    br::AutomationSession session(raw);
    auto snapshot = session.observe();
}
```

Передайте HWND выбранного окна
Сессия привязана к одному окну и процессу
Для действий задайте `options.allow_actions = 1`

Поддержаны Invoke, Value, Toggle, SelectionItem, SelectNamed и Focus
`SELECT_NAMED` выбирает элемент по имени в стабильном контейнере
Имя передаётся через `action.value`
Пустое имя и неоднозначный выбор отвергаются

Узлы без Runtime ID видны как метаданные
У них `ADDRESSABLE=false`, нет действий и новый ID на каждый snapshot
Для выбора таких пунктов используйте адресуемый контейнер и SelectNamed

## Изоляция UIA

`br_auto_windows_open_isolated` запускает UIA в отдельном helper-процессе
`br-uia-helper.exe` устанавливается в `dist/bin`, включая сборку без CLI
Передайте абсолютный UTF-8 путь к helper из той же сборки

```cpp
const char path[] = "C:\\BR\\bin\\br-uia-helper.exe";
auto target = br_auto_windows_isolated_options_default(hwnd, {path, sizeof(path) - 1});
auto options = br_auto_options_default();
br_auto_session* session = nullptr;
auto status = br_auto_windows_open_isolated(&target, &options, &session);
```

Helper работает на desktop вызывающего потока и не переключает его
Timeout или cancel во время запроса завершают helper
Потеря ответа на действие даёт EFFECT_UNKNOWN без повтора
После потери helper сессия возвращает PROVIDER_ERROR
Откройте новую сессию и получите новые IDs

`startup_timeout_ms` ограничивает ожидание запуска и инициализации UIA
`max_ipc_bytes` ограничивает сообщение, по умолчанию 8 MiB
Превышение лимита возвращает LIMIT_EXCEEDED без частичного snapshot
Проверки прав и условий остаются в вызывающем процессе
Это изоляция зависаний, не защитная песочница для чужого кода

## Свой провайдер

Заполните `br_auto_provider` и создайте сессию через `br_auto_session_create`
`observe` передаёт узлы через `emit`
Необязательный `read` обновляет известный элемент без обхода дерева
`wait_event` позволяет ждать события вместо периодического опроса

Пример: [automation_provider.c](../examples/automation_provider.c)

Свой провайдер может читать DOM, accessibility API или состояние вашего приложения
Сохраняйте идентичность элементов и проверяйте её перед действием
Для повторного использования native ID изменяйте incarnation
Обрабатывайте отмену и бюджет операции внутри callbacks

Захват, кодирование и подготовку деталей можно использовать отдельно от UIA
Подключение SDK: [C/C++ и FFI](integration.md)

## Результат действия

`effect` сообщает NOT_DISPATCHED, DISPATCHED или EFFECT_UNKNOWN
`verification` сообщает NOT_REQUESTED, SATISFIED, UNSATISFIED или UNKNOWN

SetValue проверяет значение
Select проверяет selected
SelectNamed проверяет выбранное имя
Для других действий задайте `after`
Ответ DISPATCHED сам по себе не подтверждает выполнение задачи

Batch проверяет вход до первого действия и останавливается при ошибке
Он не откатывает GUI
Dry-run не выполняет действие и не подтверждает результат
BR не повторяет действие после неизвестного исхода

## Память и потоки

Удаляйте snapshots через `br_auto_snapshot_destroy`
Строки элементов живут до удаления snapshot
Snapshot может пережить сессию
Изображения освобождаются через `br_image_free`

Одна сессия требует последовательных вызовов
Параллельный вызов получает BUSY
Cancel разрешён из другого потока
Cancel остаётся активным до вызова `br_auto_reset_cancel`
Перед destroy завершите все вызовы

## Ограничения

Неполный snapshot не доказывает отсутствие или уникальность элемента
Дельта строится относительно последнего опубликованного полного snapshot
Внутренние проверки не сбрасывают эту базу

В обычном `br_auto_windows_open` отмена и timeouts кооперативные
Для защиты от зависшего COM используйте изолированный режим

Windows OCR и вывод наблюдений: [текст и разметка](observation.md)
Встроенного CDP, Linux desktop provider и глобального ввода пока нет
Провайдеры подключаются через C API

## Проверка

Общие тесты запускаются через CTest
Для тестового Windows desktop задайте `BR_BUILD_WINDOWS_AUTOMATION_TESTS=ON`
Стенд не переключает desktop и не использует clipboard или глобальный ввод

Модули отключаются через `BR_ENABLE_AUTOMATION=OFF` и
`BR_ENABLE_WINDOWS_AUTOMATION=OFF`
