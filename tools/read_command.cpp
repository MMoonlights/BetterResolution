#include <br/br.hpp>
#include <br/br_observation.h>
#include <charconv>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Result = std::unique_ptr<br_observation, decltype(&br_observation_destroy)>;
br_observation_string string(const std::string& s) { return {s.data(),s.size()}; }
br_rect_i32 region(const std::string& text) {
    br_rect_i32 r{}; int32_t* fields[]={&r.x,&r.y,&r.width,&r.height}; size_t start=0;
    for(size_t i=0;i<4;++i) {
        auto end=text.find(',',start); if(end==std::string::npos)end=text.size();
        auto value=std::from_chars(text.data()+start,text.data()+end,*fields[i]);
        if(value.ec!=std::errc() || value.ptr!=text.data()+end || (i<3 && end==text.size()) || (i==3 && end!=text.size()))
            throw std::runtime_error("region must be x,y,width,height");
        start=end+1;
    }
    if(r.width<=0 || r.height<=0)throw std::runtime_error("region must be nonempty");
    return r;
}
}
int br_read_command(const std::vector<std::string>& args) {
    try {
        if(args.size()==1 && args[0]=="--languages") {
            size_t size=0;br::check(br_observation_ocr_languages_json(nullptr,0,&size),"OCR languages");
            std::vector<char> output(size);br::check(br_observation_ocr_languages_json(output.data(),output.size(),&size),"OCR languages");
            std::printf("%s\n",output.data());return 0;
        }
        if(args.empty())throw std::runtime_error("br read in.png [--json] [--region x,y,w,h] [--match TEXT] [--image marked.png] [--mark circle|rect] [--crop crop.png] [--language en-US] [--words] [--max-pixels N] [--tsv words.tsv]; br read --languages");
        bool json=false,words=false; std::string match,image_path,crop_path,tsv_path,text,language,rect;
        auto limits=br_observation_options_default();
        bool custom=false; auto marks=br_observation_render_options_default();
        for(size_t i=1;i<args.size();++i) {
            const auto& key=args[i];
            if(key=="--json"){json=true;continue;} if(key=="--words"){words=true;continue;}
            if(++i>=args.size())throw std::runtime_error("read option requires a value");
            const auto& value=args[i];
            if(key=="--match")match=value; else if(key=="--image")image_path=value; else if(key=="--crop")crop_path=value;
            else if(key=="--tsv")tsv_path=value; else if(key=="--language")language=value;
            else if(key=="--max-pixels") {
                uint64_t n=0;auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);
                if(parsed.ec!=std::errc()||parsed.ptr!=value.data()+value.size()||!n||n>64u*1024u*1024u)
                    throw std::runtime_error("max-pixels must be 1..67108864");
                limits.max_pixels=n;
            }
            else if(key=="--text"){text=value;custom=true;} else if(key=="--region")rect=value;
            else if(key=="--mark") { if(value=="circle")marks.mark=BR_OBSERVATION_CIRCLE;
                else if(value=="rect")marks.mark=BR_OBSERVATION_RECT;else throw std::runtime_error("mark must be circle or rect"); }
            else throw std::runtime_error("unknown read option: "+key);
        }
        if(custom && (!tsv_path.empty() || rect.empty()))throw std::runtime_error("custom text requires --region and cannot use --tsv");
        if(!tsv_path.empty() && !rect.empty())throw std::runtime_error("--region cannot be combined with TSV; TSV coordinates belong to its original image");
        for(const auto& destination:{image_path,crop_path}) {
            if(destination.empty())continue;
            std::error_code error;
            if(std::filesystem::equivalent(args[0],destination,error))throw std::runtime_error("output must not overwrite the input image");
        }
        auto image=br::Image::load(args[0]);auto view=image.view();br_observation* raw=nullptr;
        br_transform map{1,1,0,0};
        if(!custom && !rect.empty()) {
            const auto r=region(rect);
            const int64_t l=std::max(int64_t(0),int64_t(r.x)),t=std::max(int64_t(0),int64_t(r.y));
            const int64_t right=std::min(int64_t(view.width),int64_t(r.x)+r.width);
            const int64_t bottom=std::min(int64_t(view.height),int64_t(r.y)+r.height);
            if(l>=right || t>=bottom)throw std::runtime_error("region is outside image");
            br_image_view selected{};
            br::check(br_image_crop(&view,{int32_t(l),int32_t(t),int32_t(right-l),int32_t(bottom-t)},&selected),"read region");
            view=selected;map.tx=double(l);map.ty=double(t);
        }
        if(custom) {
            br_observation_item item{0,BR_OBSERVATION_CUSTOM,BR_OBSERVATION_TEXT,string(text),{},region(rect),-1};
            br::check(br_observation_create(&item,1,&view,nullptr,&limits,&raw),"custom observation");
        } else if(!tsv_path.empty()) {
            std::ifstream input(tsv_path,std::ios::binary);if(!input)throw std::runtime_error("cannot open TSV");
            std::string data;char block[4096];while(input.read(block,sizeof(block)) || input.gcount()) {
                if(data.size()+size_t(input.gcount())>64u*1024u*1024u)throw std::runtime_error("TSV file is too large");
                data.append(block,size_t(input.gcount()));
            }
            br::check(br_observation_from_tsv(string(data),&view,&map,&limits,&raw),"TSV observation");
        } else {
            auto o=br_observation_ocr_options_default();o.words=words ? 1 : 0;o.language=string(language);
            o.limits=limits;
            br::check(br_observation_ocr_windows(&view,&map,&o,&raw),"Windows OCR");
        }
        Result result(raw,br_observation_destroy);
        if(!match.empty()) {
            auto query=br_observation_query_default();query.contains=string(match);br_observation* selected=nullptr;
            br::check(br_observation_select(result.get(),&query,&selected),"observation selection");result.reset(selected);
        }
        if(!image_path.empty()) {
            br_image drawn{};br::check(br_observation_image(result.get(),&marks,&drawn),"marked image");br::Image output(drawn);
            auto v=output.view();auto png=br_encode_options_default(BR_ENCODE_PNG);br::check(br_save(image_path.c_str(),&v,&png),"marked PNG");
        }
        if(!crop_path.empty()) {
            br_image crop{};br::check(br_observation_crop(result.get(),0,0,&crop,nullptr),"selected crop");br::Image output(crop);
            auto v=output.view();auto png=br_encode_options_default(BR_ENCODE_PNG);br::check(br_save(crop_path.c_str(),&v,&png),"crop PNG");
        }
        size_t size=0;const auto write=json ? br_observation_json : br_observation_text;
        br::check(write(result.get(),nullptr,0,&size),"observation size");std::vector<char> buffer(size);
        br::check(write(result.get(),buffer.data(),buffer.size(),&size),"observation output");
        std::fwrite(buffer.data(),1,size-1,stdout);std::fputc('\n',stdout);return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"br read: %s\n",e.what());return 1;}
}
