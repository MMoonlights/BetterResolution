# CLI

Необязательная утилита SDK для обработки изображений
Для её сборки задайте `BR_BUILD_TOOLS=ON`

## Формат файла

```bash
br format in.png out.jpg
br format in.jpg out.png
br format in.png out.bmp
br format in.png out.qoi
br format in.png out.ppm
br format in.png out.jpg --quality 85
```

Расширение выхода выбирает PNG, JPEG, BMP, QOI или PNM. `convert` - алиас `format`
PNG сохраняет выбранные пиксели без потерь; JPEG сжимает с потерями

## Размер и область

```bash
br resize in.png out.png --width 1280
br resize in.png out.png --width 1280 --height 720
br resize in.png out.png --scale 0.5
br resize in.png out.png --long-edge 1280
br resize in.png crop.png --region 100,50,800,600
```

Один размер сохраняет пропорции; два задают точную геометрию. `--long-edge` ограничивает
длинную сторону. Режимы: `--mode text|quality|balanced|fast`; явный фильтр: `--filter point|triangle|catmull|mitchell|lanczos3`

## OCR и детали оригинала

```bash
br text original.png ocr.png --preset ocr --region 100,80,400,120
br detail original.png text.png --intent text --region 100,80,400,120
br detail original.png icon.png --intent icon --region 20,40,24,24
```

`text` готовит данные для внешнего OCR. `detail` сохраняет цвет и контекст, пишет PNG
и JSON с координатами. Берите область из оригинала, а не из уменьшенного JPEG
Подробнее: [OCR](text-vision.md), [vision](vision-detail.md)

## Информация и изменения

```bash
br version
br info in.png --json
br diff before.png after.png --json
br annotate in.png marked.png --grid
```

`diff`: код 0 - одинаковые кадры, 2 - есть изменения

## Windows capture

```bash
br windows --filter Chrome
br capture --window "Firefox" -o browser.png
br shot --window "Edge" --profile vision -o browser.png
br shot --window "Notepad" --width 1280 -o notepad.png --json
```

Используйте заголовок из `br windows`. `vision` по умолчанию сохраняет native PNG;
`compact` - JPEG q95. Числовые параметры размера применяются явно
Снимки экрана требуют сборки с capture ON. Автоматические тесты - только CPU с
capture OFF: [сборка](getting-started.md#тесты). [Координаты и сессии](capture.md)
