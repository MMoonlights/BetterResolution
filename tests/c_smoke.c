/* Обычная C-программа использует общую библиотеку: проверяет совместимость br.h с C99 и работу DLL. */
#include <br/br.h>
#include <br/br_automation_vision.h>
#include <br/br_automation_win.h>
#include <br/br_text.h>
#include <br/br_shot.h>
#include <br/br_vision.h>

#include <stdio.h>
#include <string.h>

int main(void) {
#ifdef BR_TEST_AUTOMATION
    {
        br_auto_string path = {"helper", 6};
        br_auto_windows_isolated_options opts = br_auto_windows_isolated_options_default(1, path);
        br_auto_session* session = NULL;
        if (opts.struct_size != sizeof(opts)) return 15;
        opts.max_ipc_bytes = 0;
        if (br_auto_windows_open_isolated(&opts, NULL, &session) != BR_AUTO_INVALID_ARGUMENT || session) return 16;
    }
    {
        br_auto_element element = {0};
        br_auto_vision_plan plan = {0};
        br_transform map = {1, 1, 0, 0};
        if (br_auto_snapshot_find_id(NULL, 1, &element) != BR_AUTO_INVALID_ARGUMENT) return 13;
        if (br_auto_vision_plan_detail(NULL, 1, 10, 10, &map, NULL, &plan) != BR_E_INVALID_ARGUMENT) return 14;
    }
#endif
    {
        br_vision_options options = br_vision_options_default(BR_VISION_ICON);
        br_rect_i32 region = {20,20,12,12};
        br_vision_plan plan;
        memset(&plan, 0, sizeof(plan));
        if (br_vision_plan_detail(100, 80, region, &options, &plan) != BR_OK ||
            plan.scale != 4 || plan.version != BR_VISION_API_VERSION) return 13;
    }
    br_text_options text = br_text_options_default(BR_TEXT_OCR);
    if (text.struct_size != sizeof(text) || !br_build_id()[0]) return 9;
    {
        br_shot_options shot = br_shot_options_default();
        br_shot_result shot_result;
        memset(&shot_result, 0, sizeof(shot_result));
        if (shot.struct_size != sizeof(shot) || shot.encode.format != BR_ENCODE_JPEG) return 10;
        {
            br_shot_options vision = br_shot_options_for(BR_SHOT_VISION);
            if (vision.encode.format != BR_ENCODE_PNG || !(vision.flags & BR_SHOT_REUSE_ENCODED)) return 12;
        }
        shot.struct_size = 0; /* Отклоняется сразу на любой платформе. */
        if (br_shot(NULL, &shot, &shot_result) != BR_E_INVALID_ARGUMENT) return 11;
        br_shot_result_free(&shot_result);
        br_shot_cache_clear();
    }
    br_image img;
    br_image small;
    br_resize_options ro;
    br_encode_options eo;
    uint8_t* png = NULL;
    size_t png_size = 0;
    memset(&img, 0, sizeof(img));
    memset(&small, 0, sizeof(small));

    if (br_image_create(640, 360, BR_PIXEL_BGRA8, &img) != BR_OK) return 1;
    br_image_fill(&img, 0xffffffffu);
    br_draw_text(&img, 10, 10, "BetterResolution C API", 0xff000000u, 0, 2);
    ro = br_resize_options_for(BR_RESIZE_UI_TEXT);
    if (br_resize_to(NULL, (const br_image_view*)&img, 320, 0, &ro, &small) != BR_OK) return 2;
    eo = br_encode_options_default(BR_ENCODE_PNG);
    {
        br_image_view v = br_image_as_view(&small);
        if (br_encode_alloc(&v, &eo, &png, &png_size) != BR_OK) return 3;
    }
    printf("BetterResolution %s (%s): %ux%u -> %ux%u, PNG %u bytes\n", br_version_string(), br_features(), img.width,
           img.height, small.width, small.height, (unsigned)png_size);
    br_free(png);
    br_image_free(&small);
    br_image_free(&img);
    return small.data == NULL && png_size > 0 ? 0 : 4;
}
