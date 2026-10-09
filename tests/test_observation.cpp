#include "test_framework.hpp"
#include <br/br_observation.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include "observe/ocr_tiles.hpp"
#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
br_observation_string str(const char* s){return {s,std::strlen(s)};}
using Result=std::unique_ptr<br_observation,decltype(&br_observation_destroy)>;
struct Image {
    br_image i{};
    Image(){CHECK_OK(br_image_create(40,30,BR_PIXEL_RGB8,&i));CHECK_OK(br_image_fill(&i,0xff112233));}
    ~Image(){br_image_free(&i);}
};
std::string exported(const br_observation* r,bool json){
    auto fn=json ? br_observation_json : br_observation_text;size_t n=0;CHECK_OK(fn(r,nullptr,0,&n));
    std::vector<char> buf(n);CHECK_OK(fn(r,buf.data(),buf.size(),&n));return buf.data();
}
br_observation_item item(const char* text, uint32_t source=BR_OBSERVATION_CUSTOM){
    return {UINT64_MAX,source,BR_OBSERVATION_TEXT,str(text),{},{8,6,20,14},-1};
}
Result create(const br_observation_item* n,size_t count,const br_image_view* image=nullptr,const br_transform* map=nullptr){
    br_observation* out=nullptr;CHECK_OK(br_observation_create(n,count,image,map,nullptr,&out));return {out,br_observation_destroy};
}
}
TEST(observation_copies_utf8_and_exports_lossless_ids_without_fake_confidence){
    char text[]="\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82 \"\\\n";
    auto n=item(text,BR_OBSERVATION_UIA);n.label=str("Label");auto result=create(&n,1);
    text[0]='x';br_observation_item saved{};CHECK_OK(br_observation_item_at(result.get(),0,&saved));
    CHECK(saved.text.data[0]=='\xD0');CHECK(saved.confidence==-1);CHECK(saved.source==BR_OBSERVATION_UIA);
    auto json=exported(result.get(),true);
    CHECK(json.find("\"id\":\"18446744073709551615\"")!=std::string::npos);
    CHECK(json.find("\"confidence\":null")!=std::string::npos);
    CHECK(json.find("\\\"\\\\\\u000a")!=std::string::npos);
    auto plain=exported(result.get(),false);CHECK(plain.starts_with("Label: "));
    char small[4]={'a','b','c','d'};size_t size=0;
    CHECK(br_observation_json(result.get(),small,sizeof(small),&size)==BR_E_BUFFER_TOO_SMALL);
    CHECK(!std::memcmp(small,"abcd",4) && size==json.size()+1);
    br_observation_item sentinel{};sentinel.id=123;
    CHECK(br_observation_item_at(result.get(),1,&sentinel)==BR_E_NOT_FOUND && sentinel.id==123);
}
TEST(observation_empty_and_metadata_only_results){
    auto r=create(nullptr,0);br_observation_info info{};CHECK_OK(br_observation_get_info(r.get(),&info));
    CHECK(info.item_count==0 && !info.has_image);CHECK(exported(r.get(),false).empty());
    CHECK(exported(r.get(),true).find("\"items\":[]")!=std::string::npos);
    br_image image{};CHECK(br_observation_image(r.get(),nullptr,&image)==BR_E_UNSUPPORTED);CHECK(!image.data);
}
TEST(observation_select_preserves_source_frame_lifetime_and_crop_map){
    Image image;auto view=br_image_as_view(&image.i);br_transform map{2,-3,-100,70};
    br_observation_item nodes[]={item("one"),item("two",BR_OBSERVATION_OCR)};nodes[1].bounds={-2,3,8,7};
    auto parent=create(nodes,2,&view,&map);auto q=br_observation_query_default();q.source=BR_OBSERVATION_OCR;q.contains=str("two");
    q.region={0,0,20,20};br_observation* raw=nullptr;CHECK_OK(br_observation_select(parent.get(),&q,&raw));
    Result selected(raw,br_observation_destroy);parent.reset();br_image_free(&image.i);
    br_observation_info info{};CHECK_OK(br_observation_get_info(selected.get(),&info));CHECK(info.item_count==1 && info.has_image);
    br_image crop{};br_transform cropped{};CHECK_OK(br_observation_crop(selected.get(),0,1,&crop,&cropped));
    CHECK(crop.width==7 && crop.height==9 && cropped.tx==-100 && cropped.ty==64 && cropped.sy==-3);
    CHECK(crop.data[0]==0x11 && crop.data[1]==0x22);br_image_free(&crop);
    q.contains=str("missing");CHECK_OK(br_observation_select(selected.get(),&q,&raw));Result empty(raw,br_observation_destroy);
    CHECK(exported(empty.get(),false).empty());CHECK(br_observation_crop(empty.get(),0,0,&crop,nullptr)==BR_E_NOT_FOUND);
}
TEST(observation_marks_rectangles_and_circles_without_changing_original){
    Image original;auto view=br_image_as_view(&original.i);auto n=item("object");auto r=create(&n,1,&view);
    auto mark=br_observation_render_options_default();mark.padding=0;mark.labels=0;mark.argb=0xffff0000;
    br_image rectangle{},circle{},copy{};CHECK_OK(br_observation_image(r.get(),&mark,&rectangle));
    mark.mark=BR_OBSERVATION_CIRCLE;mark.padding=3;CHECK_OK(br_observation_image(r.get(),&mark,&circle));
    CHECK_OK(br_observation_image(r.get(),nullptr,&copy));
    auto cv=br_image_as_view(&copy);CHECK(brt::images_equal(view,cv));
    CHECK(original.i.data[ptrdiff_t(6)*original.i.stride+8*3]==0x11);
    const auto rect_corner=rectangle.data+ptrdiff_t(6)*rectangle.stride+8*4;
    const auto circle_corner=circle.data+ptrdiff_t(6)*circle.stride+8*4;
    CHECK(rect_corner[2]==255 && circle_corner[2]==0x11);
    const auto center=circle.data+ptrdiff_t(13)*circle.stride+18*4;CHECK(center[2]==0x11);
    const auto edge=circle.data+ptrdiff_t(13)*circle.stride;CHECK(edge[2]==255);
    br_image_free(&rectangle);br_image_free(&circle);br_image_free(&copy);
}
TEST(observation_negative_stride_and_extreme_boxes_are_bounded){
    Image image;auto v=br_image_as_view(&image.i);v.data+=ptrdiff_t(v.height-1)*v.stride;v.stride=-v.stride;
    br_observation_item n=item("far");n.bounds={INT32_MAX,INT32_MAX,INT32_MAX,INT32_MAX};
    auto r=create(&n,1,&v);auto mark=br_observation_render_options_default();mark.mark=BR_OBSERVATION_CIRCLE;
    br_image result{};CHECK_OK(br_observation_image(r.get(),&mark,&result));br_image_free(&result);
    CHECK(br_observation_crop(r.get(),0,0,&result,nullptr)==BR_E_NOT_FOUND);
    CHECK_OK(br_draw_ellipse(&image.i,{INT32_MIN,INT32_MIN,INT32_MAX,INT32_MAX},0xffff0000,2));
}
TEST(observation_rejects_bad_utf8_confidence_versions_and_limits){
    auto n=item("x");br_observation* out=nullptr;auto o=br_observation_options_default();
    const unsigned char bad[]={0xc0,0x80};n.text={reinterpret_cast<const char*>(bad),2};
    CHECK(br_observation_create(&n,1,nullptr,nullptr,nullptr,&out)==BR_E_INVALID_ARGUMENT && !out);
    n=item("abc");n.confidence=std::numeric_limits<double>::quiet_NaN();
    CHECK(br_observation_create(&n,1,nullptr,nullptr,nullptr,&out)==BR_E_INVALID_ARGUMENT);
    n.confidence=-0.1;CHECK(br_observation_create(&n,1,nullptr,nullptr,nullptr,&out)==BR_E_INVALID_ARGUMENT);
    n.confidence=-1;o.max_text_bytes=2;
    CHECK(br_observation_create(&n,1,nullptr,nullptr,&o,&out)==BR_E_BUFFER_TOO_SMALL && !out);
    o=br_observation_options_default();o.version=0;CHECK(br_observation_create(&n,1,nullptr,nullptr,&o,&out)==BR_E_INVALID_ARGUMENT);
    o=br_observation_options_default();o.struct_size=4;
    CHECK(br_observation_create(&n,1,nullptr,nullptr,&o,&out)==BR_E_INVALID_ARGUMENT);
    br_transform map{0,1,0,0};CHECK(br_observation_create(&n,1,nullptr,&map,nullptr,&out)==BR_E_INVALID_ARGUMENT);
    auto r=create(&n,1);auto q=br_observation_query_default();q.region={0,0,0,5};
    CHECK(br_observation_select(r.get(),&q,&out)==BR_E_INVALID_ARGUMENT);
}
TEST(observation_imports_tesseract_words_and_rejects_malformed_rows){
    const char header[]="level\tpage_num\tblock_num\tpar_num\tline_num\tword_num\tleft\ttop\twidth\theight\tconf\ttext\n";
    std::string tsv=header;tsv+="5\t1\t1\t1\t1\t1\t2\t3\t10\t7\t97.5\tHello\n";
    tsv+="5\t1\t1\t1\t1\t2\t14\t3\t8\t7\t-1\tBR\n";
    br_observation* out=nullptr;CHECK_OK(br_observation_from_tsv({tsv.data(),tsv.size()},nullptr,nullptr,nullptr,&out));
    Result r(out,br_observation_destroy);CHECK(exported(r.get(),false)=="Hello\nBR");br_observation_item n{};
    CHECK_OK(br_observation_item_at(r.get(),0,&n));CHECK(n.source==BR_OBSERVATION_OCR && n.confidence==0.975 && n.bounds.x==2);
    CHECK_OK(br_observation_item_at(r.get(),1,&n));CHECK(n.confidence==-1);
    std::string invalid=std::string(header)+"5\t2\t1\t1\t1\t1\t2\t3\t10\t7\t99\twrong page\n";
    CHECK(br_observation_from_tsv({invalid.data(),invalid.size()},nullptr,nullptr,nullptr,&out)==BR_E_INVALID_ARGUMENT && !out);
    invalid=std::string(header)+"5\t1\t1\t1\t1\t1\t2\t3\t10\t7\tnan\tbad\n";
    CHECK(br_observation_from_tsv({invalid.data(),invalid.size()},nullptr,nullptr,nullptr,&out)==BR_E_INVALID_ARGUMENT);
    CHECK(br_observation_from_tsv(str("bad"),nullptr,nullptr,nullptr,&out)==BR_E_INVALID_ARGUMENT);
    auto limit=br_observation_options_default();limit.max_items=1;
    CHECK(br_observation_from_tsv({tsv.data(),tsv.size()},nullptr,nullptr,&limit,&out)==BR_E_BUFFER_TOO_SMALL);
}
TEST(observation_exports_are_safe_for_concurrent_readers){
    auto n=item("readers");auto r=create(&n,1);std::string a,b;
    std::thread one([&]{a=exported(r.get(),true);});std::thread two([&]{b=exported(r.get(),true);});one.join();two.join();CHECK(a==b);
}
TEST(observation_windows_ocr_has_real_text_or_reports_missing_backend){
    br_image image{};CHECK_OK(br_image_create(320,80,BR_PIXEL_RGB8,&image));CHECK_OK(br_image_fill(&image,0xffffffff));
    CHECK_OK(br_draw_text(&image,10,10,"HELLO BR 123",0xff000000,0,4));auto v=br_image_as_view(&image);
    br_observation* out=nullptr;auto o=br_observation_ocr_options_default();o.words=1;
    auto st=br_observation_ocr_windows(&v,nullptr,&o,&out);
    if(st==BR_E_UNSUPPORTED) {CHECK(!out);std::printf("[note] Windows OCR unavailable: %s\n",br_last_error());}
    else {CHECK_OK(st);Result r(out,br_observation_destroy);br_observation_info info{};
        CHECK_OK(br_observation_get_info(r.get(),&info));CHECK(info.item_count>0 && info.has_image);
        for(size_t i=0;i<info.item_count;++i){br_observation_item n{};CHECK_OK(br_observation_item_at(r.get(),i,&n));CHECK(n.source==BR_OBSERVATION_OCR && n.confidence==-1);}}
    br_image_free(&image);
    CHECK(br_observation_ocr_windows(nullptr,nullptr,nullptr,&out)==BR_E_INVALID_ARGUMENT);
}

TEST(ocr_tiles_cover_all_pixels_and_keep_exact_original_coordinates){
    const auto tiles=br::observe::ocr_tiles(11,9,6,2);std::vector<unsigned> covered(11*9);
    CHECK(tiles.size()==6);
    for(const auto b:tiles){CHECK(b.width<=6 && b.height<=6 && b.x>=0 && b.y>=0 && b.x+b.width<=11 && b.y+b.height<=9);
        for(int32_t y=b.y;y<b.y+b.height;++y)for(int32_t x=b.x;x<b.x+b.width;++x)++covered[size_t(y)*11+x];}
    for(auto n:covered)CHECK(n>0);
    const auto wide=br::observe::ocr_tiles(3000,1000,2560,128);
    CHECK(wide.size()==2 && wide[1].x==2432 && wide[1].width==568);
    CHECK(br::observe::same_ocr_region({100,50,80,20},{102,51,78,20}));
    CHECK(!br::observe::same_ocr_region({100,50,80,20},{200,50,80,20}));
}
TEST(ocr_language_queries_are_repeatable_and_do_not_write_short_buffers){
    size_t size=0;auto st=br_observation_ocr_languages_json(nullptr,0,&size);
    if(st==BR_E_UNSUPPORTED){CHECK(br_observation_ocr_languages_json(nullptr,0,nullptr)==BR_E_INVALID_ARGUMENT);return;}
    CHECK_OK(st);CHECK(size>=3);std::vector<char> output(size);
    for(int run=0;run<4;++run){CHECK_OK(br_observation_ocr_languages_json(output.data(),output.size(),&size));CHECK(output[0]=='[');}
    char tiny[2]={'x','y'};CHECK(br_observation_ocr_languages_json(tiny,2,&size)==BR_E_BUFFER_TOO_SMALL);
    CHECK(tiny[0]=='x' && tiny[1]=='y');
}
TEST(ocr_reads_large_plain_image_corners_and_overlap_once){
#if defined(_WIN32) && defined(BR_HAS_WINDOWS_OCR)
    constexpr int width=4000,height=3000;
    BITMAPINFO bi{};bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi.bmiHeader.biWidth=width;bi.bmiHeader.biHeight=-height;
    bi.bmiHeader.biPlanes=1;bi.bmiHeader.biBitCount=32;bi.bmiHeader.biCompression=BI_RGB;
    void* pixels=nullptr;HDC dc=CreateCompatibleDC(nullptr);HBITMAP bitmap=CreateDIBSection(dc,&bi,DIB_RGB_COLORS,&pixels,nullptr,0);
    CHECK(dc && bitmap && pixels);if(!dc || !bitmap || !pixels){if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);return;}
    auto old_bitmap=SelectObject(dc,bitmap);std::memset(pixels,255,size_t(width)*height*4);
    HFONT font=CreateFontW(-36,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");auto old_font=SelectObject(dc,font);SetTextColor(dc,RGB(0,0,0));SetBkMode(dc,TRANSPARENT);
    TextOutW(dc,80,80,L"TOP LEFT",8);TextOutW(dc,3500,80,L"TOP RIGHT",9);
    TextOutW(dc,80,2700,L"BOTTOM LEFT",11);TextOutW(dc,3500,2700,L"BOTTOM RIGHT",12);
    TextOutW(dc,2440,800,L"EDGE",4);
    GdiFlush();auto bytes=static_cast<uint8_t*>(pixels);for(size_t i=3;i<size_t(width)*height*4;i+=4)bytes[i]=255;
    br_image_view image{};image.data=bytes;image.width=width;image.height=height;image.stride=width*4;
    image.format=BR_PIXEL_BGRA8;image.color_space=BR_COLOR_SRGB;
    auto options=br_observation_ocr_options_default();options.words=1;options.language=str("en-US");br_observation* raw=nullptr;
    const auto st=br_observation_ocr_windows(&image,nullptr,&options,&raw);
    SelectObject(dc,old_font);DeleteObject(font);SelectObject(dc,old_bitmap);DeleteObject(bitmap);DeleteDC(dc);
    if(st==BR_E_UNSUPPORTED){CHECK(!raw);return;}
    CHECK_OK(st);Result result(raw,br_observation_destroy);br_observation_info info{};CHECK_OK(br_observation_get_info(raw,&info));
    CHECK(info.width==width && info.height==height && info.complete);
    bool left_top=false,right_top=false,left_bottom=false,right_bottom=false;size_t edge=0;
    for(size_t i=0;i<info.item_count;++i){br_observation_item n{};CHECK_OK(br_observation_item_at(raw,i,&n));std::string text(n.text.data,n.text.size);
        left_top|=text=="LEFT" && n.bounds.x<500 && n.bounds.y<200;
        right_top|=text=="RIGHT" && n.bounds.x>3400 && n.bounds.y<200;
        left_bottom|=text=="LEFT" && n.bounds.x<500 && n.bounds.y>2600;
        right_bottom|=text=="RIGHT" && n.bounds.x>3400 && n.bounds.y>2600;
        edge+=text=="EDGE";}
    CHECK(left_top && right_top && left_bottom && right_bottom);CHECK(edge==1);
#endif
}
