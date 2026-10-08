/* Связь элемента интерфейса с оригинальными пикселями кадра.
 * Функции не захватывают экран и не проверяют актуальность снимка интерфейса.
 * Провайдер обязан возвращать bounds в физических координатах экрана.
 */
#ifndef BETTERRESOLUTION_BR_AUTOMATION_VISION_H
#define BETTERRESOLUTION_BR_AUTOMATION_VISION_H
#include "br_automation.h"
#include "br_vision.h"
#ifdef __cplusplus
extern "C" {
#endif
#define BR_AUTO_VISION_VERSION 1u

typedef struct br_auto_vision_plan {
    uint32_t struct_size, version;
    uint64_t snapshot_id, element_id, element_revision;
    br_rect_i32 screen_bounds;
    br_vision_plan detail;
    br_transform image_to_screen;
    uint32_t clipped, reserved;
} br_auto_vision_plan;

/* План без доступа к пикселям. image_to_screen берётся из br_frame_info.
 * Масштабы должны быть конечными и ненулевыми. Нулевая область не означает весь кадр.
 * NOT_FOUND: элемент отсутствует в полном снимке либо область вне кадра.
 * UNSUPPORTED: неполный снимок не доказывает отсутствие элемента.
 * Выход меняется только при успехе. */
BR_API br_status br_auto_vision_plan_detail(const br_auto_snapshot*, uint64_t element_id,
    uint32_t width, uint32_t height, const br_transform* image_to_screen,
    const br_vision_options*, br_auto_vision_plan* out_plan);

/* Точная деталь из переданного кадра, без нового захвата и повторного кодирования.
 * Обнулите out_image; освободите через br_image_free. Отрицательный stride допустим.
 * out_plan необязателен. При ошибке outputs сохраняются. */
BR_API br_status br_auto_vision_prepare(br_context*, const br_auto_snapshot*, uint64_t element_id,
    const br_image_view* original, const br_transform* image_to_screen,
    const br_vision_options*, br_image* out_image, br_auto_vision_plan* out_plan);
#ifdef __cplusplus
}
#endif
#endif
