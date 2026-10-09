# Текст и разметка

BR возвращает текст, координаты, JSON и изображения
Источники: UIA, Windows OCR, TSV Tesseract и ваши детекторы

## CLI

```bat
br read frame.png
br read frame.png --json --language en-US
br read frame.png --match delivered --image marked.png --mark circle
br read frame.png --match delivered --crop detail.png
br read frame.png --tsv words.tsv --json
```

`--words` возвращает отдельные слова Windows OCR
`--match` ищет точную подстроку в тексте или подписи
`--image` сохраняет PNG с рамками или кружками
`--crop` сохраняет область первого выбранного элемента
Оригинал остаётся неизменным

Для своих данных:

```bat
br read frame.png --text Icon --region 20,30,40,40 --image icon.png --mark circle
```

## SDK

Заголовок: [br_observation.h](../include/br/br_observation.h)

| Функция | Результат |
|---|---|
| `br_observation_create` | Свои тексты и области |
| `br_auto_observation_create` | Данные UIA и переданный кадр |
| `br_observation_ocr_windows` | Текст из пикселей |
| `br_observation_from_tsv` | Результат внешнего Tesseract |
| `br_observation_select` | Выбор по тексту, источнику или области |
| `br_observation_text`, `br_observation_json` | Текстовый вывод |
| `br_observation_image` | Исходный кадр или разметка |
| `br_observation_crop` | Область и карта координат |

```c
br_observation* result = NULL;
br_observation_ocr_options options = br_observation_ocr_options_default();
br_status status = br_observation_ocr_windows(&image, NULL, &options, &result);
if (status == BR_OK) {
    size_t size = 0;
    br_observation_text(result, NULL, 0, &size);
    char* text = (char*)br_alloc(size);
    if (text) {
        if (br_observation_text(result, text, size, &size) == BR_OK) puts(text);
        br_free(text);
    }
    br_observation_destroy(result);
}
```

Строки и кадр копируются при создании результата
Фильтрация разделяет исходный кадр без повторного копирования
TEXT и JSON кешируются после первого запроса
Буферы TEXT/JSON включают завершающий NUL
Изображения освобождаются через `br_image_free`, результат - через `br_observation_destroy`
Завершите вызовы перед destroy

Координаты относятся к исходному кадру, `image_to_screen` переводит их на экран
Результат не проверяет свежесть снимка и не запускает действия
Номера на разметке соответствуют полю `number` в JSON
Неизвестная confidence сохраняется как `null`, IDs - как строки
`complete` показывает полноту обхода источника, не гарантирует точность OCR

## OCR

[Windows OCR](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrengine) использует установленные языки и отдельный MTA-поток
Доступен при сборке MSVC с C++/WinRT из Windows SDK
Отключение: `BR_ENABLE_WINDOWS_OCR=OFF`
Вызов синхронный, без принудительного тайм-аута WinRT
Без движка или языка возвращается `BR_E_UNSUPPORTED`
Размер кадра должен укладываться в лимит Windows OCR
Прозрачность для OCR сводится на белом фоне
Модуль не скачивает языковые пакеты

Tesseract запускает ваше приложение, BR читает его TSV для одного изображения
Свои детекторы передают записи TEXT, CONTROL или OBJECT через `br_observation_create`
