/* Цветные детали, взятые из исходного изображения. */
#ifndef BETTERRESOLUTION_BR_VISION_H
#define BETTERRESOLUTION_BR_VISION_H
#include "br.h"
#ifdef __cplusplus
extern "C" {
#endif

#define BR_VISION_API_VERSION 1

typedef enum br_vision_preset {
    BR_VISION_NATIVE = 0, /* исходный RGB без контекстного поля; для заданного увеличения используется POINT */
    BR_VISION_TEXT = 1,   /* увеличение 2x фильтром Catmull-Rom в гамме интерфейса, поле 4 исходных пикселя */
    BR_VISION_ICON = 2    /* увеличение 4x фильтром POINT без сглаживания, поле 16 исходных пикселей */
} br_vision_preset;

typedef struct br_vision_options {
    uint32_t struct_size;    /* sizeof(br_vision_options); используйте функцию инициализации */
    uint32_t version;        /* BR_VISION_API_VERSION */
    br_vision_preset preset;
    uint32_t scale;          /* запрошенное целое увеличение, 1..8 */
    uint32_t context_margin; /* исходные пиксели с каждой стороны, 0..256; обрезаются по краям */
    uint32_t border;         /* выходные пиксели с каждой стороны, 0..256 */
    uint32_t max_long_edge;  /* максимальная длина итогового изображения с полем; 0 = без ограничения */
    uint32_t reserved;       /* должно быть равно нулю */
    uint64_t max_pixels;     /* максимум пикселей итогового изображения с полем; 0 = 2 млн пикселей */
} br_vision_options;

typedef struct br_vision_plan {
    uint32_t struct_size;    /* sizeof(br_vision_plan) */
    uint32_t version;        /* BR_VISION_API_VERSION */
    br_rect_i32 requested_region; /* запрошенная область, обрезанная по исходному изображению; нулевая область означает всё изображение */
    br_rect_i32 source_region;    /* запрошенная область с контекстным полем, обрезанная по исходному изображению */
    br_rect_i32 content_rect;     /* всё увеличенное содержимое в выходных координатах, без поля */
    br_transform image_to_source; /* координаты по границам пикселей; применяйте ровно один раз */
    uint32_t scale;          /* фактическое целое увеличение, всегда >= 1 */
    uint32_t desired_scale;  /* запрошенное увеличение до применения ограничений */
    uint32_t chose_limited;  /* 1, если ограничения по пикселям, сторонам или памяти уменьшили увеличение */
    uint32_t reserved;
} br_vision_plan;

BR_API br_vision_options br_vision_options_default(br_vision_preset preset);

/* Максимальное целое увеличение в заданных пределах, не меньше исходного размера.
 * BR_E_UNSUPPORTED, если даже увеличение 1 не помещается. При options=NULL используется NATIVE.
 * out_plan меняется только при успехе; пиксели не читаются, память не выделяется. */
BR_API br_status br_vision_plan_detail(uint32_t width, uint32_t height, br_rect_i32 region,
    const br_vision_options* options, br_vision_plan* out_plan);

/* Непрозрачный RGB8 sRGB; прозрачность накладывается на белый фон. TEXT сглаживает, ICON/NATIVE копируют пиксели.
 * Перед вызовом обнулите out_image; освободите его через br_image_free. out_plan может быть NULL.
 * Поддерживается отрицательный шаг строк. При ошибке исходник и результат не меняются. */
BR_API br_status br_vision_prepare(br_context* context, const br_image_view* original,
    br_rect_i32 region, const br_vision_options* options, br_image* out_image, br_vision_plan* out_plan);

#ifdef __cplusplus
}
#endif
#endif
