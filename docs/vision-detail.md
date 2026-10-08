# Детали изображения для ИИ

`br detail` берёт область из оригинала, добавляет контекст и сохраняет цветной PNG

```sh
br detail original.png text.png --intent text --region 100,80,400,120
br detail original.png icon.png --intent icon --region 20,40,24,24
br detail original.png native.png --intent native --region 100,80,400,120
```

| Режим | Zoom | Контекст с каждой стороны |
|---|---:|---:|
| `native` | 1× | 0 px |
| `text` | 2×, Catmull-Rom | 4 px |
| `icon` | 4×, точное повторение пикселей | 16 px |

Параметры задаются явно:

```sh
br detail original.png out.png --intent icon --region 20,40,24,24 --scale 4 --context 8
```

По умолчанию бюджет - 2 Mi пикселей. При нехватке бюджета zoom уменьшается целыми
ступенями, всегда до 1× или выше. Если исходная область не помещается, возвращается ошибка
JSON в stdout содержит итоговые размеры, `source_region` и `image_to_source`

## C API

```c
#include <br/br_vision.h>

br_vision_options options = br_vision_options_default(BR_VISION_ICON);
br_image detail = {0};
br_vision_plan plan = {0};
br_rect_i32 region = {20, 40, 24, 24};
if (br_vision_prepare(NULL, &original, region, &options, &detail, &plan) == BR_OK) {
    br_image_free(&detail);
}
```

`original` - `br_image_view`. Выход - opaque RGB8 sRGB; alpha композитится на белом
Нельзя строить деталь из JPEG-обзора. Храните оригинал вместе с картой координат
Для экранных координат объедините `plan.image_to_source` с картой оригинал → экран

[br_vision.h](../include/br/br_vision.h) · [C API](api.md) · [Захват](capture.md)
