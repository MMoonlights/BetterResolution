# Сборка и подключение

Нужны CMake 3.20+ и компилятор C++20. Базовая библиотека не требует сторонних пакетов.

## Product-сборка

Windows, MSVC:

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBR_BUILD_TESTS=OFF
cmake --build build --config Release
cmake --install build --config Release --prefix dist
```

Windows, MinGW:

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DBR_BUILD_TESTS=OFF
cmake --build build
cmake --install build --prefix dist
```

Linux:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBR_BUILD_TESTS=OFF
cmake --build build
cmake --install build --prefix dist
```

Windows capture включён по умолчанию; сборка не запускает захват.
В `dist/bin` находятся CLI и DLL, в `dist/lib` - библиотеки и CMake package,
в `dist/include/br` - `br.h`, `br.hpp`, `br_shot.h`, `br_text.h`, `br_vision.h`.
На Linux shared library находится в `dist/lib`.

## Тесты

Отдельная CPU-сборка, без доступа к рабочему столу:

```bash
cmake -S . -B build-sandbox -DCMAKE_BUILD_TYPE=Release -DBR_ENABLE_WINDOWS_CAPTURE=OFF -DBR_BUILD_TESTS=ON
cmake --build build-sandbox --config Release
ctest --test-dir build-sandbox -C Release --output-on-failure
```

Не используйте один каталог для product и тестовой сборки.

## CMake-проект

```cmake
find_package(BetterResolution CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE BetterResolution::betterresolution)
# или: BetterResolution::betterresolution_static
```

`CMAKE_PREFIX_PATH` указывает на `dist`. Для подпроекта используйте `add_subdirectory`
и targets `BetterResolution::BetterResolution` / `BetterResolution::Static`.
Для прямой линковки добавьте `dist/include`, import library и DLL рядом с приложением.
Статическая сборка требует совместимого компилятора и `BR_STATIC` перед заголовком.

## Первый пример

```c
#include <br/br.h>
#include <stdio.h>

int main(void) {
    br_image input = {0}, output = {0};
    br_image_view view;
    br_resize_options options = br_resize_options_for(BR_RESIZE_UI_TEXT);
    int result = 1;
    if (br_load("in.png", BR_PIXEL_UNKNOWN, &input) != BR_OK) goto done;
    view = br_image_as_view(&input);
    if (br_resize_to(NULL, &view, 1280, 720, &options, &output) != BR_OK) goto done;
    view = br_image_as_view(&output);
    if (br_save("out.png", &view, NULL) != BR_OK) goto done;
    result = 0;
done:
    if (result) fprintf(stderr, "%s\n", br_last_error());
    br_image_free(&output);
    br_image_free(&input);
    return result;
}
```

Выделенные изображения освобождаются `br_image_free`, кодированные байты - `br_free`.
Справочник: [api.md](api.md); команды: [cli.md](cli.md).
