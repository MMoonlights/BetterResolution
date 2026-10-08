# Подготовка изображений

BR подготавливает изображения к дальнейшей обработке: выбирает область, меняет масштаб, формат цвета и границы.

```sh
br text original.png prepared.png --preset screenshot --region 100,80,400,120
br text original.png color.png --preset screenshot --scale 2
```

`screenshot` сохраняет цвет; масштаб и обработку можно задать явно.

Например, можно увеличить изображение и выбрать порог:

```sh
br text original.png prepared.png --preset screenshot --scale 3 --threshold otsu
br text original.png prepared.png --preset screenshot --invert no --border 8
```

Берите область из оригинала: повторное увеличение JPEG не восстановит потерянные детали.

## C API

```c
#include <br/br_text.h>

br_text_options options = br_text_options_default(BR_TEXT_SCREENSHOT);
br_image output = {0};
br_text_info info = {0};
br_rect_i32 region = {100, 80, 400, 120};
if (br_text_prepare(NULL, &original, region, &options, &output, &info) == BR_OK) {
    /* Использовать output, затем освободить. */
    br_image_free(&output);
}
```

`original` - `br_image_view`. Нулевая область выбирает весь исходник.
Результат - opaque RGB8 или GRAY8 sRGB; alpha композитится на белом.
`content_rect` исключает добавленное поле; `image_to_source` переводит координаты
подготовленного изображения в оригинал. Применяйте карту один раз.

Точные параметры: [br_text.h](../include/br/br_text.h). Цветные детали:
[vision-detail.md](vision-detail.md).
