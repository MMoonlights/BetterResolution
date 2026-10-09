#include <br/br_observation.h>
#include "observation_internal.hpp"
#include "core/common.hpp"
#include "core/frame.hpp"
#include <cmath>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>
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
                    OcrEngine engine{nullptr};
                    if (o.language.size) engine = OcrEngine::TryCreateFromLanguage(winrt::Windows::Globalization::Language(
                        winrt::to_hstring(std::string_view(o.language.data,o.language.size))));
                    else engine = OcrEngine::TryCreateFromUserProfileLanguages();
                    if (!engine) { status = BR_E_UNSUPPORTED; error = "Windows OCR language is not installed"; }
                    else if (image->width > OcrEngine::MaxImageDimension() || image->height > OcrEngine::MaxImageDimension()) {
                        status = BR_E_INVALID_ARGUMENT; error = "image exceeds Windows OCR dimension limit";
                    } else {
                        br_image bgra{};
                        status = br_image_create(image->width,image->height,BR_PIXEL_BGRA8,&bgra);
                        if (status != BR_OK) throw br::Error(status,br_last_error());
                        struct Cleanup { br_image& i; ~Cleanup(){br_image_free(&i);} } cleanup{bgra};
                        status = br_convert(image,&bgra); if (status != BR_OK) throw br::Error(status,br_last_error());
                        // Прозрачность сводится на белом фоне только для распознавания.
                        for (uint32_t y=0;y<bgra.height;++y) for (uint32_t x=0;x<bgra.width;++x) {
                            auto p=bgra.data+ptrdiff_t(y)*bgra.stride+x*4;
                            for (int k=0;k<3;++k) p[k]=uint8_t((uint32_t(p[k])*p[3]+255u*(255u-p[3])+127u)/255u);
                            p[3]=255;
                        }
                        const size_t stride = size_t(bgra.width)*4;
                        std::vector<uint8_t> pixels(stride*bgra.height);
                        for(uint32_t y=0;y<bgra.height;++y) std::memcpy(pixels.data()+y*stride,bgra.data+ptrdiff_t(y)*bgra.stride,stride);
                        winrt::Windows::Storage::Streams::DataWriter writer;
                        writer.WriteBytes(pixels);
                        auto bitmap = SoftwareBitmap::CreateCopyFromBuffer(writer.DetachBuffer(),BitmapPixelFormat::Bgra8,
                            int32_t(bgra.width),int32_t(bgra.height),BitmapAlphaMode::Ignore);
                        auto recognized = engine.RecognizeAsync(bitmap).get();
                        struct Text { std::string text; br_rect_i32 bounds; };
                        std::vector<Text> lines; size_t bytes = 0;
                        const auto add = [&](std::string text, br_rect_i32 b) {
                            if (text.empty()) return;
                            if (lines.size()>=o.limits.max_items || text.size()>o.limits.max_text_bytes-bytes)
                                throw br::Error(BR_E_BUFFER_TOO_SMALL,"OCR result limit");
                            bytes+=text.size(); lines.push_back({std::move(text),b});
                        };
                        for (auto line : recognized.Lines()) {
                            int32_t l=int32_t(image->width),t=int32_t(image->height),r=0,b=0;
                            for (auto word : line.Words()) {
                                const auto box=word.BoundingRect();
                                if(!std::isfinite(box.X) || !std::isfinite(box.Y) || !std::isfinite(box.Width) || !std::isfinite(box.Height) ||
                                    box.Width<0 || box.Height<0) throw br::Error(BR_E_INTERNAL,"invalid Windows OCR bounds");
                                const int32_t wl=int32_t(std::clamp(std::floor(double(box.X)),0.0,double(image->width)));
                                const int32_t wt=int32_t(std::clamp(std::floor(double(box.Y)),0.0,double(image->height)));
                                const int32_t wr=int32_t(std::clamp(std::ceil(double(box.X)+box.Width),0.0,double(image->width)));
                                const int32_t wb=int32_t(std::clamp(std::ceil(double(box.Y)+box.Height),0.0,double(image->height)));
                                l=std::min(l,wl);t=std::min(t,wt);r=std::max(r,wr);b=std::max(b,wb);
                                if(o.words) add(winrt::to_string(word.Text()),{wl,wt,wr-wl,wb-wt});
                            }
                            if(!o.words && r>l && b>t) add(winrt::to_string(line.Text()),{l,t,r-l,b-t});
                        }
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
