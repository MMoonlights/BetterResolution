# Встраивание библиотеки

Базовый C ABI - `<br/br.h>`. Дополнительные заголовки: `br_shot.h`, `br_text.h`,
`br_vision.h`. Структуры имеют естественное C-выравнивание, пути - UTF-8.

## C

[Первый пример](getting-started.md#первый-пример) загружает PNG и меняет размер на 1280×720.
Для собственного буфера заполните `br_image_view`: указатель, размеры, stride, формат,
color space и alpha. Представление не владеет памятью; исходник должен оставаться живым
на время обработки. Отрицательный stride поддерживается.

## C++

```cpp
#include <br/br.hpp>

int main() {
    auto image = br::Image::load("in.png");
    auto resized = image.resize(1280, 720);
    resized.save("out.jpg", br::Encode::jpeg(85));
}
```

`br::Image` освобождает память автоматически; ошибки приходят как `br::Error`.
Windows capture в product-сборке:

```cpp
br::Capture capture(br::Capture::window("Chrome"));
auto frame = capture.grab();
frame.image.save("chrome.png");
```

Этот фрагмент не входит в автоматические CPU-тесты.

## Другие языки

| Язык | Подключение |
|---|---|
| C# / PowerShell | P/Invoke с `CallingConvention.Cdecl`; совместимый класс можно загрузить через `Add-Type` |
| Rust | `extern "C"` / bindgen |
| Go | cgo |
| Delphi | `cdecl; external` |
| Java | JNA / Panama |

После `br_encode_alloc` скопируйте или используйте байты до `br_free`. Изображения,
выделенные BR, освобождаются `br_image_free`. Размеры pointer/size_t/stride зависят
от архитектуры; не подменяйте их 32-битным int на x64.

## Повторная обработка

Переиспользуйте DLL и `br_context`, сериализуйте вызовы одного контекста. Храните
оригинальные пиксели вместе с картой координат; детали берите из оригинала, а не JPEG-обзора.
Готовый кадр не должен ссылаться на буфер, который уже перезаписывается.
Перед завершением освободите изображения, контексты и capture-сессии.
Подробнее: [C API](api.md), [захват экрана](capture.md), [подготовка изображений](text-vision.md), [детали](vision-detail.md).
