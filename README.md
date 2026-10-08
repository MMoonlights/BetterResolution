# BetterResolution

BR - SDK для изображений, захвата экрана и работы с интерфейсом

Подключайте к своим приложениям и ИИ-агентам через C/C++ API
Модель ИИ и логику задач выбирает ваш проект

- Захват окон, мониторов и областей экрана
- PNG, JPEG, BMP, QOI и PNM
- Ресайз, обрезка и подготовка изображений
- Сравнение кадров и пересчёт координат
- Поиск элементов, действия и проверка результата
- Windows UI Automation и свои провайдеры

Обработка изображений и общий движок работают в Windows и Linux
Захват экрана и встроенный UIA доступны в Windows
Сторонние библиотеки не нужны
CLI - необязательная утилита для изображений

## Сборка

Нужны CMake 3.20+ и компилятор C++20

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBR_BUILD_TOOLS=OFF -DBR_BUILD_TESTS=OFF
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix dist
```

## Документация

- [Подключение SDK](docs/integration.md)
- [Работа с интерфейсом](docs/automation.md)
- [CLI](docs/cli.md)
- [C API](docs/api.md)
- [Сборка и подключение](docs/getting-started.md)
- [Захват экрана](docs/capture.md)
- [Подготовка изображений](docs/text-vision.md)
- [Детали изображения](docs/vision-detail.md)

## Лицензия

[MIT](LICENSE)
