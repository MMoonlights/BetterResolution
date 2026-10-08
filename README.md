# BetterResolution

BetterResolution - библиотека C/C++ и CLI для захвата экрана и обработки изображений

Поддерживает:

- Захват окон, мониторов и областей экрана в Windows
- Чтение, запись, обрезку и изменение размера PNG, JPEG, BMP, QOI и PNM
- Подготовку данных, сравнение кадров, разметку областей и перевод координат

Обработка изображений работает в Windows и Linux, захват экрана - в Windows. Сторонние библиотеки не нужны

## Сборка

Нужны CMake 3.20+ и компилятор C++20

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix dist
```

## Документация

- [CLI](docs/cli.md)
- [C API и заголовки](docs/api.md)
- [Сборка и подключение](docs/getting-started.md)
- [Захват экрана](docs/capture.md)
- [Подготовка изображений](docs/text-vision.md)
- [Цветные детали изображения](docs/vision-detail.md)

## Лицензия

[MIT](LICENSE)
