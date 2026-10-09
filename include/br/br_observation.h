/* Результаты UIA, OCR и своих детекторов: текст, области и изображения. */
#ifndef BETTERRESOLUTION_BR_OBSERVATION_H
#define BETTERRESOLUTION_BR_OBSERVATION_H
#include "br.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BR_OBSERVATION_VERSION 1u
typedef struct br_observation br_observation;
typedef struct br_observation_string { const char* data; size_t size; } br_observation_string;
typedef enum br_observation_source {
    BR_OBSERVATION_ANY = 0, BR_OBSERVATION_UIA = 1, BR_OBSERVATION_OCR = 2, BR_OBSERVATION_CUSTOM = 3
} br_observation_source;
typedef enum br_observation_kind {
    BR_OBSERVATION_TEXT = 1, BR_OBSERVATION_CONTROL = 2, BR_OBSERVATION_OBJECT = 3
} br_observation_kind;
typedef struct br_observation_item {
    uint64_t id; /* 0 = нет ID; OCR и свои IDs не являются целями br_auto_act. */
    uint32_t source, kind;
    br_observation_string text, label;
    br_rect_i32 bounds; /* Пиксели исходного кадра; нулевой размер = только текст. */
    double confidence; /* -1 = неизвестно, иначе 0..1. */
} br_observation_item;
typedef struct br_observation_options {
    uint32_t struct_size, version, complete, max_items; /* complete=0 для частичных входных данных. */
    size_t max_text_bytes;
    uint64_t max_pixels;
} br_observation_options;
typedef struct br_observation_query {
    uint32_t struct_size, version, source, kind; /* kind=0 = любой. */
    br_observation_string contains; /* Точная UTF-8 подстрока в text или label. */
    br_rect_i32 region; /* Область пересечения; нулевой размер = весь кадр. */
} br_observation_query;
typedef struct br_observation_info {
    size_t item_count;
    uint32_t width, height, has_image, complete; /* Полнота обхода источника, не точность распознавания. */
    br_transform image_to_screen;
} br_observation_info;
typedef enum br_observation_mark { BR_OBSERVATION_RECT = 1, BR_OBSERVATION_CIRCLE = 2 } br_observation_mark;
typedef struct br_observation_render_options {
    uint32_t struct_size, version, mark, argb, thickness, padding, labels;
} br_observation_render_options;
typedef struct br_observation_ocr_options {
    uint32_t struct_size, version, words; /* 0 = строки, 1 = отдельные слова. */
    br_observation_string language; /* Пусто = язык профиля Windows. */
    br_observation_options limits;
} br_observation_ocr_options;
BR_API br_observation_options br_observation_options_default(void);
BR_API br_observation_query br_observation_query_default(void);
BR_API br_observation_render_options br_observation_render_options_default(void);
BR_API br_observation_ocr_options br_observation_ocr_options_default(void);
/* Копирует строки и необязательный кадр; снимок не захватывает и актуальность не проверяет.
 * out обнулите заранее; при ошибке он получает NULL. map=NULL = единичная карта. */
BR_API br_status br_observation_create(const br_observation_item*, size_t count, const br_image_view*,
    const br_transform* image_to_screen, const br_observation_options*, br_observation** out);
BR_API void br_observation_destroy(br_observation*);
BR_API br_status br_observation_get_info(const br_observation*, br_observation_info*);
/* Строки заимствуются до destroy. */
BR_API br_status br_observation_item_at(const br_observation*, size_t index, br_observation_item*);
/* Выбранные элементы сохраняют координаты; кадр разделяется без повторного копирования. */
BR_API br_status br_observation_select(const br_observation*, const br_observation_query*, br_observation** out);
/* TEXT/JSON буферы завершаются NUL; размер включает NUL; output=NULL запрашивает размер.
 * Недостаточный буфер остаётся неизменным. JSON сохраняет uint64 IDs строками. */
BR_API br_status br_observation_text(const br_observation*, char* output, size_t capacity, size_t* size);
BR_API br_status br_observation_json(const br_observation*, char* output, size_t capacity, size_t* size);
/* Выделяет независимую копию кадра или разметку; original остаётся неизменным.
 * Обнулите output, освободите через br_image_free. Кружки - овалы по bounds. */
BR_API br_status br_observation_image(const br_observation*, const br_observation_render_options*, br_image* output);
/* Копия выбранной области; карта учитывает обрезку. */
BR_API br_status br_observation_crop(const br_observation*, size_t index, uint32_t padding,
    br_image* output, br_transform* crop_to_screen);
/* Импорт TSV Tesseract для одного изображения, без запуска внешнего процесса. */
BR_API br_status br_observation_from_tsv(br_observation_string, const br_image_view*, const br_transform*,
    const br_observation_options*, br_observation** out);
/* OCR переданного изображения, локально через установленный Windows OCR.
 * Большой кадр читается плитками с перекрытием без уменьшения, bounds относятся ко всему кадру.
 * Синхронный вызов; WinRT работает в отдельном MTA-потоке. Без загрузки языковых пакетов.
 * На других платформах, без модуля или языка возвращает UNSUPPORTED. */
BR_API br_status br_observation_ocr_windows(const br_image_view*, const br_transform*,
    const br_observation_ocr_options*, br_observation** out);
/* Установленные языки OCR: JSON-массив {tag,name}. Размер включает NUL.
 * output=NULL запрашивает размер; недостаточный буфер не меняется. */
BR_API br_status br_observation_ocr_languages_json(char* output,size_t capacity,size_t* size);
#ifdef __cplusplus
}
#endif
#endif
