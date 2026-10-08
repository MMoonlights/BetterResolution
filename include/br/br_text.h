/* Подготовка изображений для внешней системы распознавания текста. */
#ifndef BETTERRESOLUTION_BR_TEXT_H
#define BETTERRESOLUTION_BR_TEXT_H
#include "br.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BR_TEXT_API_VERSION 1

typedef enum br_text_preset {
    BR_TEXT_SCREENSHOT = 0, /* цветное изображение, без изменений UI_TEXT; повышение резкости выключено */
    BR_TEXT_OCR = 1         /* оттенки серого, увеличение 2x, рамка; пороговая обработка выключена */
} br_text_preset;
typedef enum br_text_gray {
    BR_TEXT_KEEP_COLOR = 0,
    BR_TEXT_LUMA = 1,
    BR_TEXT_MEDIAN = 2      /* подавляет цветные ореолы субпикселей RGB, но может удалить цветной текст */
} br_text_gray;
typedef enum br_text_threshold {
    BR_TEXT_THRESHOLD_NONE = 0,
    BR_TEXT_THRESHOLD_OTSU = 1,
    BR_TEXT_THRESHOLD_SAUVOLA = 2
} br_text_threshold;

typedef struct br_text_options {
    uint32_t struct_size;   /* sizeof(br_text_options); инициализируйте через функцию создания */
    uint32_t width;         /* размер содержимого до рамки; при width=0 и height=0 используется scale */
    uint32_t height;        /* если одно значение равно нулю, размер вычисляется с сохранением пропорций */
    float scale;           /* 0.1..8, используется только при width=height=0; округление половин вверх */
    uint32_t border;        /* выходные пиксели, 0..256 */
    br_text_gray grayscale;
    uint32_t normalize;    /* 0..100: ограниченное выравнивание контраста по процентилям; 0 = выкл. */
    uint32_t sharpen;      /* 0..100: повышение резкости 3x3, ограниченное локальным диапазоном и +/-16 */
    int32_t invert;        /* -1 = автоматически по большинству пикселей рамки (только серый); 0 = нет; 1 = да */
    br_text_threshold threshold;
    uint32_t window;       /* размер окна Sauvola: нечётный, 3..127 */
    float threshold_k;     /* коэффициент Sauvola k: 0..1; по умолчанию 0.2 */
    uint32_t threads;      /* 0 = настройка контекста */
    uint32_t reserved;     /* должно быть равно нулю */
    uint64_t max_pixels;   /* предел памяти для каждого входного и выходного изображения; 0 = 64 млн пикселей */
} br_text_options;

typedef struct br_text_info {
    br_rect_i32 source_region; /* область исходного кадра, обрезанная по его границам */
    br_rect_i32 content_rect;  /* без добавленной рамки */
    br_transform image_to_source; /* координаты по границам пикселей; рамка выходит за source_region */
    uint32_t inverted;
    uint32_t threshold;        /* порог Отсу; 0, если порог не применялся или использовался локальный Sauvola */
} br_text_info;

BR_API br_text_options br_text_options_default(br_text_preset preset);
/* Всегда выбирает область из исходного кадра, а не из уменьшенной копии.
 * A zero region selects the whole frame. Output is newly allocated, opaque sRGB,
 * RGB8 or GRAY8. out_image must be zero-initialised/empty; free via br_image_free.
 * При ошибке владение не передаётся, output и info не меняются. */
BR_API br_status br_text_prepare(br_context* context, const br_image_view* original,
    br_rect_i32 region, const br_text_options* options, br_image* out_image, br_text_info* out_info);

/* NULL policy=UI_TEXT. В исходном размере копирует или меняет формат; прозрачность сводится перед изменением размера.
 * При ненулевом значении options->threads заменяет число потоков в policy. Правила владения те же, что у prepare. */
BR_API br_status br_text_prepare_with_resize(br_context* context, const br_image_view* original,
    br_rect_i32 region, const br_text_options* options, const br_resize_options* resize_options,
    br_image* out_image, br_text_info* out_info);

/* Область полностью покрывается плитками в исходном размере; перекрытие меньше размера плитки.
 * Запросить число плиток можно через NULL/0. Частичный результат не возвращается; max_tiles=0 означает 4096. */
typedef struct br_detail_options {
    uint32_t tile_width, tile_height, overlap, max_tiles;
} br_detail_options;
typedef struct br_detail_tile {
    br_rect_i32 source_region;
    br_transform image_to_source; /* исходный размер 1:1, без полей */
} br_detail_tile;
BR_API br_detail_options br_detail_options_default(void);
BR_API br_status br_plan_detail_tiles(uint32_t width, uint32_t height, br_rect_i32 region,
    const br_detail_options* options, br_detail_tile* out_tiles, size_t capacity, size_t* out_count);

/* Переводит целочисленные индексы выборок по центрам пикселей. Для непрерывных
 * координат изображения или рамки используйте br_transform_point. Применяйте только одно из преобразований и ровно один раз. */
BR_API void br_transform_pixel_center(const br_transform* transform, double x, double y,
    double* out_x, double* out_y);
#ifdef __cplusplus
}
#endif
#endif
