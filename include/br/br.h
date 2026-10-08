/* C ABI. Объекты контекста и захвата требуют последовательного доступа.
 * Представления используют заимствованные пиксели; изображения освобождаются через br_image_free, байты - через br_free.
 * Перед использованием обнуляйте выходные объекты, которыми владеет библиотека. Цвета рисования задаются в формате 0xAARRGGBB.
 */
#ifndef BETTERRESOLUTION_BR_H
#define BETTERRESOLUTION_BR_H

#include <stddef.h>
#include <stdint.h>

#ifndef BR_VERSION_MAJOR
#define BR_VERSION_MAJOR 1
#endif
#ifndef BR_VERSION_MINOR
#define BR_VERSION_MINOR 0
#endif
#ifndef BR_VERSION_PATCH
#define BR_VERSION_PATCH 0
#endif
#ifndef BR_VERSION_STRING
#define BR_VERSION_STRING "1.0.0"
#endif

#if defined(_WIN32) && defined(BR_BUILD_DLL)
#  define BR_API __declspec(dllexport)
#elif defined(_WIN32) && !defined(BR_STATIC)
#  define BR_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(BR_BUILD_DLL)
#  define BR_API __attribute__((visibility("default")))
#else
#  define BR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif


typedef enum br_status {
    BR_OK = 0,
    BR_E_INVALID_ARGUMENT = 1,
    BR_E_OUT_OF_MEMORY = 2,
    BR_E_UNSUPPORTED = 3,
    BR_E_NOT_FOUND = 4,
    BR_E_TIMEOUT = 5,
    BR_E_DEVICE_LOST = 6,
    BR_E_IO = 7,
    BR_E_INTERNAL = 8,
    BR_E_BUFFER_TOO_SMALL = 9,
    BR_E_DECODE = 10
} br_status;

typedef enum br_pixel_format {
    BR_PIXEL_UNKNOWN = 0,
    BR_PIXEL_GRAY8 = 1,
    BR_PIXEL_RGB8 = 2,
    BR_PIXEL_RGBA8 = 3,
    BR_PIXEL_BGRA8 = 4,
    BR_PIXEL_BGR8 = 5
} br_pixel_format;

typedef enum br_color_space {
    BR_COLOR_UNKNOWN = 0, /* считается пространством sRGB */
    BR_COLOR_SRGB = 1,
    BR_COLOR_LINEAR_SRGB = 2
} br_color_space;

typedef struct br_rect_i32 {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} br_rect_i32;

typedef struct br_image_view {
    const uint8_t* data;
    uint32_t width;
    uint32_t height;
    ptrdiff_t stride; /* шаг между строками в байтах; может быть отрицательным */
    br_pixel_format format;
    br_color_space color_space;
    uint8_t premultiplied_alpha;
} br_image_view;

typedef struct br_mut_image_view {
    uint8_t* data;
    uint32_t width;
    uint32_t height;
    ptrdiff_t stride;
    br_pixel_format format;
    br_color_space color_space;
    uint8_t premultiplied_alpha;
} br_mut_image_view;

/* Изображение, память для пикселей которого выделена BR. Освободить через br_image_free(). */
typedef br_mut_image_view br_image;

/* Аффинное преобразование: out = in * s + t (отдельно для x и y). */
typedef struct br_transform {
    double sx;
    double sy;
    double tx;
    double ty;
} br_transform;

typedef struct br_context br_context;
typedef struct br_capture br_capture;


BR_API const char* br_version_string(void);
/* (major << 16) | (minor << 8) | patch */
BR_API uint32_t br_version(void);
BR_API const char* br_status_string(br_status status);
/* Текст последней ошибки в вызывающем потоке. */
BR_API const char* br_last_error(void);
/* Возможности сборки и среды выполнения через запятую, например "avx2,threads,dxgi,wgc,gdi". */
BR_API const char* br_features(void);
/* Идентификатор исходников загруженной библиотеки. */
BR_API const char* br_build_id(void);

BR_API void* br_alloc(size_t size);
BR_API void br_free(void* pointer);

/* Контекст владеет кэшами (коэффициенты изменения размера) и пулом рабочих потоков.
 * Во всех функциях вместо br_context* можно передать NULL - тогда выбирается
 * контекст по умолчанию для всего процесса создаётся при первом обращении. */
BR_API br_status br_context_create(br_context** out_context);
BR_API void br_context_destroy(br_context* context);
/* 0 = автоматически (по числу аппаратных потоков). Применяется к следующим вызовам. */
BR_API br_status br_context_set_threads(br_context* context, uint32_t threads);


BR_API uint32_t br_pixel_format_channels(br_pixel_format format);
/* Выделяет память под изображение с плотно упакованными строками и нулевыми пикселями. */
BR_API br_status br_image_create(uint32_t width, uint32_t height, br_pixel_format format, br_image* out_image);
BR_API void br_image_free(br_image* image);
BR_API br_image_view br_image_as_view(const br_mut_image_view* image);
/* Копирует изображение с возможным преобразованием формата (BR_PIXEL_UNKNOWN сохраняет формат). */
BR_API br_status br_image_clone(const br_image_view* src, br_pixel_format format, br_image* out_image);
/* Преобразует формат пикселей между представлениями одинакового размера. */
BR_API br_status br_convert(const br_image_view* src, br_mut_image_view* dst);
/* Создаёт представление без копирования; прямоугольник обрезается по границам изображения. */
BR_API br_status br_image_crop(const br_image_view* src, br_rect_i32 rect, br_image_view* out_view);
BR_API br_status br_image_crop_mut(br_mut_image_view* src, br_rect_i32 rect, br_mut_image_view* out_view);
/* Заливает каждый пиксель значением 0xAARRGGBB вместе с альфа-каналом, без смешивания. */
BR_API br_status br_image_fill(br_mut_image_view* image, uint32_t argb);
/* Устанавливает альфа-канал в 255 (RGBA/BGRA). Полезно для захвата GDI/DXGI с неопределённой прозрачностью. */
BR_API br_status br_image_set_opaque(br_mut_image_view* image);


typedef enum br_filter {
    BR_FILTER_AUTO = 0,
    BR_FILTER_BOX = 1, /* усреднение площади при уменьшении */
    BR_FILTER_TRIANGLE = 2,
    BR_FILTER_CUBIC_BSPLINE = 3,
    BR_FILTER_CATMULL_ROM = 4,
    BR_FILTER_MITCHELL = 5,
    BR_FILTER_LANCZOS2 = 6,
    BR_FILTER_LANCZOS3 = 7,
    BR_FILTER_LANCZOS4 = 8,
    BR_FILTER_POINT = 9 /* ближайший сосед: увеличение без сглаживания */
} br_filter;

typedef enum br_resize_mode {
    BR_RESIZE_QUALITY = 0,  /* фотографии и обычные изображения */
    BR_RESIZE_BALANCED = 1,
    BR_RESIZE_FAST = 2,
    BR_RESIZE_UI_TEXT = 3   /* снимки экрана: сохраняет тонкие штрихи текста тёмными и чёткими */
} br_resize_mode;

typedef struct br_resize_options {
    br_filter filter;       /* AUTO выбирает фильтр по режиму и масштабу */
    br_resize_mode mode;
    uint8_t linear_light;   /* фильтрация в линейном свете (физически точнее, но истончает тёмный текст) */
    uint8_t preserve_alpha; /* фильтрация с учётом альфа-канала (предварительно умноженного) */
    uint8_t antiring;       /* ограничивает выбросы диапазоном главного лепестка ядра */
    uint8_t multistage;     /* предварительно уменьшает изображение целочисленными шагами */
    uint32_t threads;       /* 0 = настройка контекста */
} br_resize_options;

BR_API br_resize_options br_resize_options_default(void);
BR_API br_resize_options br_resize_options_for(br_resize_mode mode);

/* Пересчитывает src в dst, используя размеры представлений. Форматы могут различаться. */
BR_API br_status br_resize(br_context* context, const br_image_view* src, br_mut_image_view* dst,
                           const br_resize_options* options);
/* Вариант с выделением памяти. */
BR_API br_status br_resize_to(br_context* context, const br_image_view* src, uint32_t width, uint32_t height,
                              const br_resize_options* options, br_image* out_image);

/* Ограничения размера, например для лимита изображения визуальной модели. 0 = без ограничений. */
typedef struct br_fit_options {
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_long_edge;
    uint32_t max_short_edge;
    uint64_t max_pixels;
    uint32_t multiple_of;   /* округляет размер вниз до кратного значения (размер блока); 0/1 = выкл. */
    uint8_t allow_upscale;
} br_fit_options;

/* Вычисляет максимальный размер с сохранением пропорций, удовлетворяющий всем ограничениям. */
BR_API void br_fit_dimensions(uint32_t width, uint32_t height, const br_fit_options* fit,
                              uint32_t* out_width, uint32_t* out_height);
/* Подгоняет размер и изменяет его. out_transform (необязательно) переводит координаты исходника в результат. */
BR_API br_status br_resize_fit(br_context* context, const br_image_view* src, const br_fit_options* fit,
                               const br_resize_options* options, br_image* out_image, br_transform* out_transform);
/* Обрезает region (пиксели исходника) и меняет размер до width x height (0 = сохранить / вычислить по пропорциям).
 * out_transform переводит координаты результата обратно в исходное изображение. */
BR_API br_status br_zoom(br_context* context, const br_image_view* src, br_rect_i32 region,
                         uint32_t width, uint32_t height, const br_resize_options* options,
                         br_image* out_image, br_transform* out_to_source);


BR_API br_transform br_transform_identity(void);
/* Переводит прямоугольник source в прямоугольник output. */
BR_API br_transform br_transform_from_rects(br_rect_i32 source, br_rect_i32 output);
BR_API void br_transform_point(const br_transform* transform, double x, double y, double* out_x, double* out_y);
BR_API br_transform br_transform_inverse(br_transform transform);
/* Результат последовательно применяет first, затем second. */
BR_API br_transform br_transform_compose(br_transform first, br_transform second);
/* Переводит прямоугольник и возвращает наименьший целочисленный прямоугольник, покрывающий его. */
BR_API br_rect_i32 br_transform_rect(const br_transform* transform, br_rect_i32 rect);


typedef enum br_encoded_format {
    BR_ENCODE_RAW = 0,  /* плотно упакованные пиксели без заголовка */
    BR_ENCODE_PNG = 1,
    BR_ENCODE_JPEG = 2,
    BR_ENCODE_BMP = 3,
    BR_ENCODE_QOI = 4,
    BR_ENCODE_PNM = 5   /* двоичный PGM (серый) / PPM (цветной) */
} br_encoded_format;

typedef enum br_png_palette_mode {
    BR_PALETTE_OFF = 0,
    BR_PALETTE_AUTO = 1,     /* без потерь: палитра используется при числе цветов <= 256 */
    BR_PALETTE_QUANTIZE = 2  /* с потерями: сокращает число цветов до max_colors, без дизеринга (чёткий текст) */
} br_png_palette_mode;

typedef struct br_encode_options {
    br_encoded_format format;
    int32_t quality;        /* JPEG 1..100 */
    uint8_t strip_metadata; /* BR не записывает метаданные; поле оставлено для ясности */
    uint8_t force_444;      /* JPEG: 1 = цветность 4:4:4 (текст интерфейса), 0 = 4:2:0 */
    uint8_t png_palette;    /* режим br_png_palette_mode */
    uint8_t jpeg_optimize;  /* оптимальные таблицы Хаффмана JPEG (меньше файл, немного медленнее) */
    int32_t effort;         /* PNG/DEFLATE 0..9 (0 = без сжатия), по умолчанию 1 */
    uint32_t max_colors;    /* BR_PALETTE_QUANTIZE: 2..256 */
} br_encode_options;

BR_API br_encode_options br_encode_options_default(br_encoded_format format);

/* Два вызова: передайте output = NULL, чтобы узнать нужный размер. Рекомендуется br_encode_alloc. */
BR_API br_status br_encode(const br_image_view* image, const br_encode_options* options,
                           uint8_t* output, size_t output_capacity, size_t* output_size);
/* Кодирует в буфер, выделенный BR (освободить через br_free). */
BR_API br_status br_encode_alloc(const br_image_view* image, const br_encode_options* options,
                                 uint8_t** out_data, size_t* out_size);

typedef struct br_image_info {
    br_encoded_format format;
    uint32_t width;
    uint32_t height;
    uint32_t channels;  /* естественное число каналов: 1, 3 или 4 */
    uint8_t bit_depth;
    uint8_t has_alpha;
    uint8_t progressive; /* прогрессивный JPEG / чересстрочный PNG */
} br_image_info;

BR_API br_status br_probe(const void* data, size_t size, br_image_info* out_info);
/* Декодирует PNG, JPEG (обычный и прогрессивный), BMP, QOI, PNM.
 * format = BR_PIXEL_UNKNOWN возвращает естественный формат (GRAY8 / RGB8 / RGBA8). */
BR_API br_status br_decode(const void* data, size_t size, br_pixel_format format, br_image* out_image);
/* Пути в кодировке UTF-8. */
BR_API br_status br_load(const char* path, br_pixel_format format, br_image* out_image);
/* options = NULL выбирает формат по расширению файла. */
BR_API br_status br_save(const char* path, const br_image_view* image, const br_encode_options* options);

/* Возвращает нужное число символов вместе с завершающим NUL.
 * Записывает результат с NUL только при достаточном out_capacity. */
BR_API size_t br_base64_encode(const void* data, size_t size, char* out, size_t out_capacity);


BR_API uint64_t br_hash_image_tiles(const br_image_view* image, uint32_t tile_width, uint32_t tile_height,
                                    uint64_t* out_hashes, size_t hash_capacity, size_t* out_hash_count);

typedef struct br_diff_options {
    uint32_t tile_size;  /* шаг проверки в пикселях (по умолчанию 16) */
    uint8_t threshold;   /* различия каналов <= threshold игнорируются (по умолчанию 0) */
    uint32_t merge_gap;  /* области изменений на расстоянии меньше этого значения объединяются (по умолчанию 8) */
} br_diff_options;

typedef struct br_diff_result {
    uint64_t changed_pixels;
    double changed_fraction;
    br_rect_i32 bounds;   /* объединённая область всех изменений (пустая, если изображения совпадают) */
    uint32_t rect_count;  /* общее число прямоугольников изменений (может превышать capacity) */
} br_diff_result;

BR_API br_diff_options br_diff_options_default(void);
/* Размер изображений должен совпадать; форматы могут различаться. Прямоугольники точно покрывают изменённые пиксели. */
BR_API br_status br_diff(const br_image_view* before, const br_image_view* after, const br_diff_options* options,
                         br_rect_i32* out_rects, size_t rect_capacity, br_diff_result* out_result);


/* thickness <= 0 заполняет прямоугольник. */
BR_API br_status br_draw_rect(br_mut_image_view* image, br_rect_i32 rect, uint32_t argb, int32_t thickness);
BR_API br_status br_draw_line(br_mut_image_view* image, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t argb);
/* Встроенный шрифт ASCII 5x7. scale >= 1 увеличивает символы. При alpha=0 у background_argb фон прозрачный. */
BR_API br_status br_draw_text(br_mut_image_view* image, int32_t x, int32_t y, const char* text,
                              uint32_t argb, uint32_t background_argb, uint32_t scale);
BR_API void br_measure_text(const char* text, uint32_t scale, uint32_t* out_width, uint32_t* out_height);

typedef struct br_grid_options {
    uint32_t step;                  /* расстояние между линиями в пикселях изображения; 0 = авто */
    uint32_t line_argb;
    uint32_t label_argb;
    uint32_t label_background_argb;
    uint32_t label_scale;           /* 0 = авто */
    uint8_t labels;
    br_transform label_transform;   /* подписи показывают преобразованные координаты изображения, например координаты экрана */
} br_grid_options;

BR_API br_grid_options br_grid_options_default(void);
BR_API br_status br_draw_grid(br_mut_image_view* image, const br_grid_options* options);
/* Нумерованные рамки (разметка Set-of-Mark). Нумерация начинается с first_label. */
BR_API br_status br_draw_marks(br_mut_image_view* image, const br_rect_i32* rects, size_t count,
                               uint32_t first_label, uint32_t argb);


BR_API br_status br_brf_pack_raw(const br_image_view* image, const br_rect_i32* rois, size_t roi_count,
                                 uint8_t* output, size_t output_capacity, size_t* output_size);


typedef struct br_monitor_info {
    uint32_t index;
    uint32_t adapter_index;   /* адаптер / выход DXGI; UINT32_MAX, если неизвестен */
    uint32_t output_index;
    br_rect_i32 bounds;       /* физические пиксели виртуального рабочего стола */
    br_rect_i32 work_area;
    uint32_t dpi;
    float scale;              /* dpi / 96 */
    uint8_t primary;
    int32_t rotation;         /* градусы поворота */
    char name[64];            /* имя устройства в UTF-8 */
} br_monitor_info;

typedef struct br_window_info {
    uint64_t handle;          /* HWND */
    uint32_t process_id;
    br_rect_i32 bounds;       /* видимая рамка окна (границы DWM), физические пиксели */
    br_rect_i32 client;       /* клиентская область в экранных координатах */
    uint32_t dpi;
    uint8_t visible;
    uint8_t minimized;
    uint8_t maximized;
    uint8_t foreground;
    char title[256];          /* UTF-8 */
    char class_name[128];
} br_window_info;

/* Перечисляет мониторы и видимые окна верхнего уровня (от переднего к заднему плану).
 * Включает режим DPI для каждого монитора, поэтому все значения указаны в физических пикселях. */
BR_API br_status br_monitors(br_monitor_info* out, size_t capacity, size_t* out_count);
BR_API br_status br_windows(br_window_info* out, size_t capacity, size_t* out_count);
/* Находит первое видимое окно, заголовок которого содержит title_substring (без учёта регистра, UTF-8). */
BR_API br_status br_find_window(const char* title_substring, br_window_info* out_info);

typedef enum br_capture_backend {
    BR_BACKEND_AUTO = 0,
    BR_BACKEND_DXGI = 1, /* Desktop Duplication: мониторы, области и изменённые прямоугольники */
    BR_BACKEND_WGC = 2,  /* Windows.Graphics.Capture: окна, в том числе перекрытые и отрисованные на GPU */
    BR_BACKEND_GDI = 3,  /* BitBlt для открытого окна, иначе PrintWindow; RDP и старые системы */
    BR_BACKEND_GDI_SCREEN = 4, /* BitBlt рабочего стола без подготовки; захватывает также то, что перекрывает окно */
    BR_BACKEND_GDI_PRINT = 5   /* PrintWindow(PW_RENDERFULLCONTENT): захватывает и перекрытое окно, самый медленный вариант */
} br_capture_backend;

typedef enum br_capture_target {
    BR_TARGET_MONITOR = 0,
    BR_TARGET_WINDOW = 1,
    BR_TARGET_REGION = 2,  /* прямоугольная область виртуального рабочего стола */
    BR_TARGET_DESKTOP = 3, /* весь виртуальный рабочий стол (все мониторы) */
    BR_TARGET_WINDOW_FULL = 4 /* Полный GetWindowRect через PrintWindow, включая невидимые края. */
} br_capture_target;

typedef struct br_capture_options {
    br_capture_target target;
    uint32_t monitor;          /* индекс из br_monitors (BR_TARGET_MONITOR) */
    uint64_t window;           /* HWND (BR_TARGET_WINDOW) */
    br_rect_i32 region;        /* BR_TARGET_REGION: пиксели рабочего стола; иначе необязательная область (width > 0) */
    br_capture_backend backend;
    uint8_t include_cursor;
    uint8_t client_area;       /* для окна: только клиентская область */
    uint8_t border;            /* WGC: 0 просит Windows скрыть жёлтую рамку захвата */
} br_capture_options;

typedef struct br_frame_info {
    uint64_t frame_id;
    uint64_t timestamp_us;     /* монотонное время */
    br_rect_i32 screen_rect;   /* область виртуального рабочего стола, покрытая изображением */
    br_transform image_to_screen;
    br_capture_backend backend;
    uint32_t dirty_count;      /* прямоугольники, изменившиеся после предыдущего захвата */
    uint8_t dirty_known;       /* 0: область изменений неизвестна, считать изменённым весь кадр */
    uint8_t changed;           /* 0: совпадает с предыдущим захватом */
} br_frame_info;

BR_API br_capture_options br_capture_options_default(void);
BR_API br_status br_capture_open(br_context* context, const br_capture_options* options, br_capture** out_capture);
/* Возвращает текущее изображение в BGRA8 (без прозрачности). out_image используется повторно, если совпадает размер.
 * Тайм-аут учитывается, только если кадр ещё ни разу не возвращался. */
BR_API br_status br_capture_grab(br_capture* capture, uint32_t timeout_ms, br_image* out_image,
                                 br_frame_info* out_info, br_rect_i32* dirty_rects, size_t dirty_capacity);
/* Захват, подгонка размера и изменение масштаба за один шаг (если доступно, сначала уменьшение на GPU). */
BR_API br_status br_capture_grab_fit(br_capture* capture, uint32_t timeout_ms, const br_fit_options* fit,
                                     const br_resize_options* options, br_image* out_image, br_frame_info* out_info);
/* Разовый снимок экрана. */
BR_API br_status br_screenshot(const br_capture_options* options, br_image* out_image, br_frame_info* out_info);

/* Низкоуровневый API (Windows). */
BR_API br_status br_capture_create_monitor(br_context* context, uint32_t adapter_index, uint32_t output_index,
                                           br_capture** out_capture);
BR_API br_status br_capture_create_window(br_context* context, void* hwnd, br_capture** out_capture);
BR_API br_status br_capture_size(br_capture* capture, uint32_t* out_width, uint32_t* out_height);
/* Ждёт НОВЫЙ кадр (BR_E_TIMEOUT, если экран не изменился). dst должен быть BGRA8 размера захвата. */
BR_API br_status br_capture_next(br_capture* capture, uint32_t timeout_ms, br_mut_image_view* dst,
                                 br_rect_i32* dirty_rects, size_t dirty_capacity, size_t* dirty_count);
/* Возвращает ID3D11Texture2D* с AddRef; указатель действителен до следующего вызова для этого объекта захвата. */
BR_API br_status br_capture_next_d3d11(br_capture* capture, uint32_t timeout_ms, void** out_texture,
                                       uint32_t* width, uint32_t* height,
                                       br_rect_i32* dirty_rects, size_t dirty_capacity, size_t* dirty_count);
BR_API void br_d3d11_texture_release(void* texture);
BR_API void br_capture_destroy(br_capture* capture);

#ifdef __cplusplus
}
#endif

#endif
