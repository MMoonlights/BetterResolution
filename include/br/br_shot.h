/* Захват, изменение размера и кодирование с кэшированием сеансов захвата. */
#ifndef BETTERRESOLUTION_BR_SHOT_H
#define BETTERRESOLUTION_BR_SHOT_H
#include "br.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BR_SHOT_API_VERSION 1

typedef enum br_shot_flags {
    BR_SHOT_NO_CACHE = 1,      /* открыть сеанс для этого вызова и закрыть его после */
    BR_SHOT_RETURN_IMAGE = 2,  /* также вернуть изменённые пиксели BGRA8 в br_shot_result.image */
    BR_SHOT_NO_ENCODE = 4,     /* пропустить кодирование (используется с BR_SHOT_RETURN_IMAGE) */
    BR_SHOT_CUSTOM_RESIZE = 8, /* использовать br_shot_options.resize; иначе применяется UI_TEXT */
    BR_SHOT_REUSE_ENCODED = 16 /* при совпадении пикселей и параметров взять результат из кэша; encode_us=0 */
} br_shot_flags;

typedef enum br_shot_profile {
    BR_SHOT_LEGACY = 0,        /* прежние настройки JPEG q90 для совместимости */
    BR_SHOT_VISION = 1,        /* цветной PNG в исходном размере, сжатие без потерь, кэширование неизменного результата */
    BR_SHOT_COMPACT = 2        /* JPEG q95 4:4:4 в исходном размере, кэширование результата; с потерями */
} br_shot_profile;

typedef struct br_shot_options {
    uint32_t struct_size;          /* sizeof(br_shot_options); инициализируйте через br_shot_options_default */
    br_capture_options capture;
    br_fit_options fit;            /* все поля равны нулю: вернуть захваченный размер */
    double scale;                  /* > 0 и != 1: выходной размер = размер кадра * scale (заменяет fit); 0 = выкл. */
    br_resize_options resize;      /* только с BR_SHOT_CUSTOM_RESIZE */
    br_encode_options encode;      /* по умолчанию: JPEG */
    uint32_t timeout_ms;           /* ожидание первого кадра; 0 = 2000 */
    uint32_t settle_ms;            /* 0 = последний кадр; иначе ждать интервал без изменений пикселей */
    uint32_t settle_timeout_ms;    /* максимум ожидания; 0 = 1000 (не ошибка) */
    uint32_t keep_alive_ms;        /* время простоя до закрытия сеанса из кэша; 0 = 30000 */
    uint32_t flags;                /* флаги br_shot_flags */
} br_shot_options;

typedef struct br_shot_timings {
    uint64_t open_us;     /* открытие сеанса; 0, если использован сеанс из кэша */
    uint64_t settle_us;
    uint64_t capture_us;  /* получение кадра (включает уменьшение на GPU, если gpu_scaled) */
    uint64_t resize_us;   /* уменьшение на CPU; 0, если оно выполнено на GPU или не выполнялось */
    uint64_t encode_us;
    uint64_t total_us;
} br_shot_timings;

typedef struct br_shot_result {
    uint8_t* data;                 /* закодированное изображение; освободить через br_free или br_shot_result_free */
    size_t size;
    uint32_t width, height;        /* размер возвращённого изображения */
    br_frame_info frame;           /* frame.image_to_screen переводит пиксели результата в координаты рабочего стола */
    br_shot_timings timings;
    uint8_t session_reused;
    uint8_t gpu_scaled;
    br_image image;                /* заполняется с BR_SHOT_RETURN_IMAGE */
} br_shot_result;

BR_API br_shot_options br_shot_options_default(void);
/* Без fit/scale размер не меняется. Неизвестный профиль использует настройки LEGACY. */
BR_API br_shot_options br_shot_options_for(br_shot_profile profile);
/* result полностью перезаписывается; освободить его через br_shot_result_free. */
BR_API br_status br_shot(br_context* context, const br_shot_options* options, br_shot_result* result);
/* window!=0 выбирает HWND, иначе используется monitor. Нулевые пределы означают отсутствие ограничений.
 * jpeg_quality: 0=PNG, 1..100=JPEG 4:4:4. Используется кэш сеансов. */
BR_API br_status br_shot_simple(uint64_t window, uint32_t monitor, uint32_t max_width, uint32_t max_height,
                                int32_t jpeg_quality, uint32_t settle_ms, br_shot_result* result);
/* Упрощённый FFI: target=br_capture_target; region=int32[4] (обязателен для REGION).
 * scale>0 заменяет fit; jpeg_quality: 0=PNG, 1..100=JPEG 4:4:4.
 * Поддерживаются только флаги NO_CACHE и REUSE_ENCODED. Возвращённые байты освобождаются через br_free.
 * Необязательные выходные данные: rect=int32[4], map=double[4] (sx,sy,tx,ty),
 * timings=uint64[6] (open,settle,capture,resize,encode,total),
 * info=uint32[3] (backend,session_reused,gpu_scaled). */
BR_API br_status br_shot_flat(uint32_t target, uint64_t window, uint32_t monitor, const int32_t* region,
                              double scale, uint32_t max_width, uint32_t max_height, int32_t jpeg_quality,
                              uint32_t settle_ms, uint32_t flags, uint8_t** out_data, size_t* out_size,
                              uint32_t* out_width, uint32_t* out_height, int32_t* out_screen_rect,
                              double* out_image_to_screen, uint64_t* out_timings_us, uint32_t* out_info);
BR_API void br_shot_result_free(br_shot_result* result);
/* Закрывает сеансы в кэше. */
BR_API void br_shot_cache_clear(void);
/* Инициализирует графику; вне Windows возвращает BR_E_UNSUPPORTED. */
BR_API br_status br_capture_prewarm(void);

#ifdef __cplusplus
}
#endif
#endif
