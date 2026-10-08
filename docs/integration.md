# Подключение SDK

BR встраивается в ваш проект как DLL или статическая библиотека
Подключайте обработку изображений, работу с интерфейсом или оба модуля
Модель ИИ и логику задач выбирает ваш проект

Базовый C ABI - `<br/br.h>`. Дополнительные заголовки: `br_shot.h`, `br_text.h`,
`br_vision.h`, `br_automation.h`. Структуры имеют естественное C-выравнивание, пути - UTF-8

## CMake

```cmake
find_package(BetterResolution CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE BetterResolution::betterresolution)
```

Укажите каталог установленного SDK в `CMAKE_PREFIX_PATH`
Для статической линковки используйте `BetterResolution::betterresolution_static`
Настройка runtime и сборки: [getting-started](getting-started.md)

## C

[Первый пример](getting-started.md#первый-пример) загружает PNG и меняет размер на 1280×720
Для собственного буфера заполните `br_image_view`: указатель, размеры, stride, формат,
color space и alpha. Представление не владеет памятью; исходник должен оставаться живым
на время обработки. Отрицательный stride поддерживается

## C++

```cpp
#include <br/br.hpp>

int main() {
    auto image = br::Image::load("in.png");
    auto resized = image.resize(1280, 720);
    resized.save("out.jpg", br::Encode::jpeg(85));
}
```

`br::Image` освобождает память автоматически; ошибки приходят как `br::Error`
Windows capture в product-сборке:

```cpp
br::Capture capture(br::Capture::window("Chrome"));
auto frame = capture.grab();
frame.image.save("chrome.png");
```

Этот фрагмент не входит в автоматические CPU-тесты

## Другие языки

Подключение через FFI использует C ABI и требует ваших объявлений функций

| Язык | Подключение |
|---|---|
| C# / PowerShell | P/Invoke с `CallingConvention.Cdecl`; совместимый класс можно загрузить через `Add-Type` |
| Rust | `extern "C"` / bindgen |
| Go | cgo |
| Delphi | `cdecl; external` |
| Java | JNA / Panama |
| Python | ctypes / cffi |
| Node.js | Нативный addon через N-API или ваш FFI-слой |

После `br_encode_alloc` скопируйте или используйте байты до `br_free`. Изображения,
выделенные BR, освобождаются `br_image_free`. Размеры pointer/size_t/stride зависят
от архитектуры; не подменяйте их 32-битным int на x64
Сохраняйте IDs как uint64, в JavaScript используйте BigInt вместо Number

## Работа с интерфейсом

1. Откройте сессию выбранного окна или подключите свой `br_auto_provider`
2. Передайте snapshot или подготовленный кадр своему планировщику
3. Выполните выбранное действие через `br_auto_act` или готовую цепочку через `br_auto_batch`
4. Проверьте effect и verification, затем получите изменения

Для Windows UIA используйте [API интерфейса](automation.md)
Для собственного backend начните с [примера провайдера](../examples/automation_provider.c)
Свои OCR, модель ИИ и транспорт подключайте в вызывающем приложении

## Повторная обработка

Переиспользуйте DLL и `br_context`, сериализуйте вызовы одного контекста. Храните
оригинальные пиксели вместе с картой координат; детали берите из оригинала, а не JPEG-обзора
Готовый кадр не должен ссылаться на буфер, который уже перезаписывается
Перед завершением освободите изображения, контексты и capture-сессии
Для UIA переиспользуйте сессию и запрашивайте дельты
В своём провайдере реализуйте read для обновления элемента без обхода дерева
Профилируйте обработку кадров, вызовы UI и модель отдельно в своём проекте
Подробнее: [C API](api.md), [capture](capture.md), [OCR](text-vision.md), [vision](vision-detail.md)
