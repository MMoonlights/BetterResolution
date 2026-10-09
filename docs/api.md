# C API

Заголовки: [br.h](../include/br/br.h), [br_shot.h](../include/br/br_shot.h),
[br_text.h](../include/br/br_text.h), [br_vision.h](../include/br/br_vision.h)
В них находятся точные сигнатуры, структуры и значения параметров

## Память и ошибки

- `br_image_view` не владеет пикселями. Исходный буфер должен оставаться живым
- Изображения BR освобождаются `br_image_free`, кодированные байты - `br_free`
- Перед первым использованием owning output обнулите его: `br_image image = {0}`
- Успех - `BR_OK`; описание ошибки - `br_status_string` и `br_last_error`
- `NULL` context выбирает общий контекст. Явный context/capture используйте последовательно
- `stride` измеряется в байтах; отрицательный stride поддерживается

## Изображения

| Функция | Назначение |
|---|---|
| `br_image_create` | Создать изображение |
| `br_image_clone`, `br_convert` | Копировать или сменить pixel format |
| `br_image_crop`, `br_image_crop_mut` | Представление области без копирования |
| `br_load`, `br_decode` | Читать файл или байты |
| `br_save`, `br_encode_alloc` | Записать файл или получить кодированные байты |
| `br_image_fill`, `br_image_set_opaque` | Заливка и alpha 255 |

PNG, JPEG, BMP, QOI и PNM. PNG - без потерь при OFF/AUTO palette; JPEG и QUANTIZE - с потерями

## Размеры и координаты

| Функция | Назначение |
|---|---|
| `br_resize`, `br_resize_to` | Задать выходные размеры |
| `br_fit_dimensions`, `br_resize_fit` | Ограничить размер числовыми лимитами |
| `br_zoom` | Область исходника с новым размером |
| `br_transform_point` | Преобразовать непрерывные координаты |
| `br_transform_pixel_center` | Преобразовать индекс пикселя |
| `br_transform_inverse`, `br_transform_compose` | Обратить или объединить карты |

Одна нулевая сторона в `br_resize_to` вычисляется по пропорции. `br_resize_fit`
возвращает карту исходник → результат; `br_zoom` - результат → исходник
`br_transform_compose(a, b)` применяет сначала `a`, затем `b`

Режим выбирается через `br_resize_options_for`: `UI_TEXT`, `QUALITY`, `BALANCED`, `FAST`

## OCR и детали

| Функция | Назначение |
|---|---|
| `br_text_prepare` | Подготовить область для внешнего OCR |
| `br_text_prepare_with_resize` | То же с явным фильтром |
| `br_plan_detail_tiles` | Разбить исходник на перекрывающиеся области |
| `br_vision_plan_detail` | Рассчитать zoom и контекст |
| `br_vision_prepare` | Цветная PNG-деталь из оригинальных пикселей |

Выходы этих функций требуют обнулённого изображения. При ошибке prepare не передаёт
владение и не меняет outputs. Подробнее: [OCR](text-vision.md), [детали](vision-detail.md)

## Windows capture

| Функция | Назначение |
|---|---|
| `br_monitors`, `br_windows`, `br_find_window` | Выбрать цель |
| `br_capture_open`, `br_capture_destroy` | Открыть и закрыть сессию |
| `br_capture_grab`, `br_capture_next` | Получить доступный или изменившийся кадр |
| `br_screenshot` | Разовый снимок |
| `br_shot` | Захват, размер и кодирование одним вызовом |
| `br_shot_simple`, `br_shot_flat` | Варианты для FFI |
| `br_shot_result_free`, `br_shot_cache_clear` | Освободить результат и кеш сессий |

`br_shot_options_for(BR_SHOT_VISION)` сохраняет исходный размер в PNG
Карта `frame.image_to_screen` переводит координаты результата в пиксели экрана
`settle_ms` ждёт тишины пикселей; завершение действия приложения проверяет вызывающий код
Подробнее: [capture](capture.md)

## Работа с интерфейсом

`br_automation.h` предоставляет провайдеры, snapshots, поиск, действия, ожидания и batch
`br_automation_win.h` подключает Windows UIA для выбранного HWND
`br_automation_vision.h` связывает bounds элемента с деталями переданного кадра
Контракты владения, примеры и ограничения: [automation](automation.md)

`br_observation.h` возвращает текст, JSON, области и разметку из UIA, OCR или своих данных
Примеры: [текст и разметка](observation.md)

## Прочее

`br_diff` возвращает области изменений; `br_hash_image_tiles` - хэши областей
`br_draw_rect`, `br_draw_line`, `br_draw_text`, `br_draw_grid`, `br_draw_marks` размечают изображение
Контекст: `br_context_create`, `br_context_set_threads`, `br_context_destroy`
Версия: `br_version_string`, `br_build_id`, `br_features`

[Сборка](getting-started.md) · [C/C++ и FFI](integration.md) · [CLI](cli.md)
