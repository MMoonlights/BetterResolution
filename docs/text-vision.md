# Подготовка для OCR

BR готовит изображение. Текст распознаёт внешний движок, например Tesseract
Windows OCR, текстовый вывод и разметка: [текст и разметка](observation.md)

```sh
br text original.png ocr.png --preset ocr --region 100,80,400,120
br text original.png color.png --preset screenshot --scale 2
```

`ocr`: серый цвет, увеличение 2×, поле 8 пикселей и автоинверсия
`screenshot`: цвет, исходный размер. Резкость и бинаризация по умолчанию выключены

Нужные изменения задаются явно:

```sh
br text original.png ocr.png --preset ocr --scale 3 --threshold otsu
br text original.png ocr.png --preset ocr --invert no --border 8
```

Берите область из оригинала. Повторное увеличение уменьшенного JPEG не восстановит символы

## C API

```c
#include <br/br_text.h>

br_text_options options = br_text_options_default(BR_TEXT_OCR);
br_image output = {0};
br_text_info info = {0};
br_rect_i32 region = {100, 80, 400, 120};
if (br_text_prepare(NULL, &original, region, &options, &output, &info) == BR_OK) {
    /* Использовать output, затем освободить. */
    br_image_free(&output);
}
```

`original` - `br_image_view`. Нулевая область выбирает весь исходник
Результат - opaque RGB8 или GRAY8 sRGB; alpha композитится на белом
`content_rect` исключает добавленное поле; `image_to_source` переводит координаты
подготовленного изображения в оригинал. Применяйте карту один раз

Точные параметры: [br_text.h](../include/br/br_text.h). Цветные детали для ИИ:
[vision-detail.md](vision-detail.md)
