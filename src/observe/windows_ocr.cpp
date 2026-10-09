#include <br/br_observation.h>
#include "observation_internal.hpp"
#include "ocr_tiles.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"
#include <cmath>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#if defined(_WIN32) && defined(BR_HAS_WINDOWS_OCR)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
#endif

extern "C" br_status br_observation_ocr_windows(const br_image_view* image, const br_transform* map,
    const br_observation_ocr_options* options, br_observation** out) {
    if (!out) return br::fail(BR_E_INVALID_ARGUMENT,"null OCR output");
    *out = nullptr;
    if(options && options->struct_size!=sizeof(*options)) return br::fail(BR_E_INVALID_ARGUMENT,"OCR options size mismatch");
    const auto o = options ? *options : br_observation_ocr_options_default();
    if (!image || !br::validate_image(*image) || o.struct_size != sizeof(o) || o.version != BR_OBSERVATION_VERSION || o.words > 1 ||
        !br::observe::valid_options(o.limits) || !br::observe::valid_string(o.language,128) ||
        uint64_t(image->width)*image->height > o.limits.max_pixels ||
        (map && (!std::isfinite(map->sx) || !std::isfinite(map->sy) || !std::isfinite(map->tx) || !std::isfinite(map->ty) || !map->sx || !map->sy)))
        return br::fail(BR_E_INVALID_ARGUMENT,"invalid OCR input");
#if defined(_WIN32) && defined(BR_HAS_WINDOWS_OCR)
    return br::guarded([&] {
        br_status status = BR_E_INTERNAL; std::string error;
        // Все WinRT-объекты создаются и освобождаются в отдельном MTA-потоке.
        std::thread worker([&] {
            bool apartment = false;
            try {
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded); apartment = true;
                {
                    using namespace winrt::Windows::Media::Ocr;
                    using namespace winrt::Windows::Graphics::Imaging;
                    const auto engine_factory=winrt::try_get_activation_factory<OcrEngine,IOcrEngineStatics>();
                    if(!engine_factory)throw br::Error(BR_E_UNSUPPORTED,"Windows OCR factory unavailable");
                    OcrEngine engine{nullptr};
                    if (o.language.size) {
                        const auto language_factory=winrt::try_get_activation_factory<winrt::Windows::Globalization::Language,
                            winrt::Windows::Globalization::ILanguageFactory>();
                        if(!language_factory)throw br::Error(BR_E_UNSUPPORTED,"Windows OCR language factory unavailable");
                        engine=engine_factory.TryCreateFromLanguage(language_factory.CreateLanguage(
                            winrt::to_hstring(std::string_view(o.language.data,o.language.size))));
                    } else engine = engine_factory.TryCreateFromUserProfileLanguages();
                    if (!engine) { status = BR_E_UNSUPPORTED; error = "Windows OCR language is not installed"; }
                    else {
                        const uint32_t edge=engine_factory.MaxImageDimension();
                        const auto writer_factory=winrt::try_get_activation_factory<winrt::Windows::Storage::Streams::DataWriter>();
                        const auto bitmap_factory=winrt::try_get_activation_factory<SoftwareBitmap,ISoftwareBitmapStatics>();
                        if(!writer_factory||!bitmap_factory)throw br::Error(BR_E_UNSUPPORTED,"Windows OCR bitmap factory unavailable");
                        const auto tiles=br::observe::ocr_tiles(image->width,image->height,edge,std::min(128u,edge/8));
                        struct Text { std::string text; br_rect_i32 bounds; };
                        std::vector<Text> lines;size_t bytes=0;
                        std::unordered_map<std::string,std::vector<size_t>> seen;
                        const auto add = [&](std::string text,br_rect_i32 bounds) {
                            if(text.empty() || bounds.width<=0 || bounds.height<=0)return;
                            auto& matches=seen[text];
                            for(auto index:matches) if(br::observe::same_ocr_region(lines[index].bounds,bounds)) {
                                const auto b=lines[index].bounds;
                                if(int64_t(bounds.width)*bounds.height>int64_t(b.width)*b.height)lines[index].bounds=bounds;
                                return;
                            }
                            if(lines.size()>=o.limits.max_items || text.size()>o.limits.max_text_bytes-bytes)
                                throw br::Error(BR_E_BUFFER_TOO_SMALL,"OCR result limit");
                            bytes+=text.size();matches.push_back(lines.size());lines.push_back({std::move(text),bounds});
                        };
                        // Полный кадр читается плитками без уменьшения исходных пикселей.
                        for(const auto tile:tiles) {
                        br_image_view tile_view{};status=br_image_crop(image,tile,&tile_view);
                        if(status!=BR_OK)throw br::Error(status,br_last_error());
                        br_image bgra{};
                        status = br_image_create(tile_view.width,tile_view.height,BR_PIXEL_BGRA8,&bgra);
                        if (status != BR_OK) throw br::Error(status,br_last_error());
                        struct Cleanup { br_image& i; ~Cleanup(){br_image_free(&i);} } cleanup{bgra};
                        status = br_convert(&tile_view,&bgra); if (status != BR_OK) throw br::Error(status,br_last_error());
                        // Прозрачность сводится на белом фоне только для распознавания.
                        for (uint32_t y=0;y<bgra.height;++y) for (uint32_t x=0;x<bgra.width;++x) {
                            auto p=bgra.data+ptrdiff_t(y)*bgra.stride+x*4;
                            for (int k=0;k<3;++k) p[k]=uint8_t((uint32_t(p[k])*p[3]+255u*(255u-p[3])+127u)/255u);
                            p[3]=255;
                        }
                        const size_t stride = size_t(bgra.width)*4;
                        std::vector<uint8_t> pixels(stride*bgra.height);
                        for(uint32_t y=0;y<bgra.height;++y) std::memcpy(pixels.data()+y*stride,bgra.data+ptrdiff_t(y)*bgra.stride,stride);
                        auto writer=writer_factory.ActivateInstance<winrt::Windows::Storage::Streams::DataWriter>();
                        writer.WriteBytes(pixels);
                        auto bitmap = bitmap_factory.CreateCopyFromBuffer(writer.DetachBuffer(),BitmapPixelFormat::Bgra8,
                            int32_t(bgra.width),int32_t(bgra.height),BitmapAlphaMode::Ignore);
                        auto recognized = engine.RecognizeAsync(bitmap).get();
                        for (auto line : recognized.Lines()) {
                            int32_t l=int32_t(tile_view.width),t=int32_t(tile_view.height),r=0,b=0;
                            for (auto word : line.Words()) {
                                const auto box=word.BoundingRect();
                                if(!std::isfinite(box.X) || !std::isfinite(box.Y) || !std::isfinite(box.Width) || !std::isfinite(box.Height) ||
                                    box.Width<0 || box.Height<0) throw br::Error(BR_E_INTERNAL,"invalid Windows OCR bounds");
                                const int32_t wl=int32_t(std::clamp(std::floor(double(box.X)),0.0,double(tile_view.width)));
                                const int32_t wt=int32_t(std::clamp(std::floor(double(box.Y)),0.0,double(tile_view.height)));
                                const int32_t wr=int32_t(std::clamp(std::ceil(double(box.X)+box.Width),0.0,double(tile_view.width)));
                                const int32_t wb=int32_t(std::clamp(std::ceil(double(box.Y)+box.Height),0.0,double(tile_view.height)));
                                l=std::min(l,wl);t=std::min(t,wt);r=std::max(r,wr);b=std::max(b,wb);
                                if(o.words) add(winrt::to_string(word.Text()),{wl+tile.x,wt+tile.y,wr-wl,wb-wt});
                            }
                            if(!o.words && r>l && b>t) add(winrt::to_string(line.Text()),{l+tile.x,t+tile.y,r-l,b-t});
                        }
                        }
                        if(tiles.size()>1 && !o.words)std::stable_sort(lines.begin(),lines.end(),[](const Text& a,const Text& b) {
                            const int64_t ay=int64_t(a.bounds.y)*2+a.bounds.height,by=int64_t(b.bounds.y)*2+b.bounds.height;
                            return ay==by ? a.bounds.x<b.bounds.x : ay<by;
                        });
                        std::vector<br_observation_item> nodes; nodes.reserve(lines.size());
                        for(const auto& n:lines) nodes.push_back({0,BR_OBSERVATION_OCR,BR_OBSERVATION_TEXT,{n.text.data(),n.text.size()},{},n.bounds,-1});
                        status = br_observation_create(nodes.data(),nodes.size(),image,map,&o.limits,out);
                        if(status!=BR_OK) error=br_last_error();
                    }
                }
            } catch(const br::Error& e) { status=e.status; error=e.what(); }
              catch(const std::bad_alloc&) {status=BR_E_OUT_OF_MEMORY;error="out of memory";}
              catch(const winrt::hresult_error& e) {status=BR_E_UNSUPPORTED;error=winrt::to_string(e.message());}
              catch(const std::exception& e) {status=BR_E_INTERNAL;error=e.what();}
              catch(...) {status=BR_E_INTERNAL;error="Windows OCR failed";}
            } catch(...) {status=BR_E_OUT_OF_MEMORY;error.clear();}
            if(apartment) winrt::uninit_apartment();
        });
        worker.join();
        return status==BR_OK ? BR_OK : br::fail(status,error.c_str());
    });
#else
    (void)map; return br::fail(BR_E_UNSUPPORTED,"Windows OCR is unavailable in this build");
#endif
}

extern "C" br_status br_observation_ocr_languages_json(char* output,size_t capacity,size_t* size) {
    if(!size)return br::fail(BR_E_INVALID_ARGUMENT,"null OCR languages size");
#if defined(_WIN32) && defined(BR_HAS_WINDOWS_OCR)
    return br::guarded([&] {
        std::string json;br_status status=BR_E_INTERNAL;
        std::thread worker([&] {
            bool apartment=false;
            try {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);apartment=true;
                {
                const auto quote=[](const std::string& value) {
                    std::string out="\"";
                    for(unsigned char c:value) {
                        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
                        else if(c<32)throw br::Error(BR_E_INTERNAL,"invalid OCR language name");
                        else out+=char(c);
                    }
                    return out+'"';
                };
                json="[";bool first=true;
                const auto factory=winrt::try_get_activation_factory<winrt::Windows::Media::Ocr::OcrEngine,
                    winrt::Windows::Media::Ocr::IOcrEngineStatics>();
                if(!factory)throw br::Error(BR_E_UNSUPPORTED,"OCR factory unavailable");
                const auto languages=factory.AvailableRecognizerLanguages();
                if(!languages)throw br::Error(BR_E_UNSUPPORTED,"OCR languages unavailable");
                for(uint32_t i=0;i<languages.Size();++i) {
                    auto language=languages.GetAt(i);
                    if(!language)throw br::Error(BR_E_UNSUPPORTED,"OCR language unavailable");
                    if(!first)json+=',';first=false;
                    json+="{\"tag\":"+quote(winrt::to_string(language.LanguageTag()))+
                        ",\"name\":"+quote(winrt::to_string(language.DisplayName()))+'}';
                }
                json+=']';status=BR_OK;
                }
            } catch(const std::bad_alloc&) {status=BR_E_OUT_OF_MEMORY;}
              catch(const winrt::hresult_error&) {status=BR_E_UNSUPPORTED;}
              catch(...) {status=BR_E_INTERNAL;}
            if(apartment)winrt::uninit_apartment();
        });
        worker.join();
        if(status!=BR_OK)return br::fail(status,"Windows OCR language list unavailable");
        *size=json.size()+1;
        if(!output)return BR_OK;
        if(capacity<*size)return br::fail(BR_E_BUFFER_TOO_SMALL,"OCR language buffer too small");
        std::memcpy(output,json.c_str(),*size);return BR_OK;
    });
#else
    (void)output;(void)capacity;return br::fail(BR_E_UNSUPPORTED,"Windows OCR is unavailable in this build");
#endif
}
