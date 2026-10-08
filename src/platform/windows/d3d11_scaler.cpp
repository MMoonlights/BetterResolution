#if defined(_WIN32)

#include "platform/windows/d3d11_scaler.hpp"

#include "core/common.hpp"
#include "core/frame.hpp"
#include "resize/sampler.hpp"

#include <cstring>
#include <vector>

namespace br::win {
namespace {

const char kShader[] = R"HLSL(
cbuffer Params : register(b0) {
    uint inW, inH, outW, outH;
    uint taps, antiring, linearLight, finalPass;
};
Texture2D<float4> Src : register(t0);
StructuredBuffer<int> Start : register(t1);
StructuredBuffer<float> Weight : register(t2);
StructuredBuffer<uint> Lobe : register(t3);
RWTexture2D<float4> Dst : register(u0);

float3 toLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 toSrgb(float3 c) { c = saturate(c); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

[numthreads(16, 16, 1)]
void horizontal(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outW || id.y >= inH) return;
    int s = Start[id.x];
    uint lb = Lobe[id.x] & 0xffff, le = Lobe[id.x] >> 16;
    float4 acc = 0, mn = 1e9, mx = -1e9;
    [loop] for (uint k = 0; k < taps; ++k) {
        float4 v = Src.Load(int3(s + (int)k, id.y, 0));
        v.a = 1.0;
        if (linearLight) v.rgb = toLinear(v.rgb);
        acc += v * Weight[id.x * taps + k];
        if (k >= lb && k < le) { mn = min(mn, v); mx = max(mx, v); }
    }
    if (antiring) acc = clamp(acc, mn, mx);
    Dst[id.xy] = acc;
}

[numthreads(16, 16, 1)]
void vertical(uint3 id : SV_DispatchThreadID) {
    if (id.x >= outW || id.y >= outH) return;
    int s = Start[id.y];
    uint lb = Lobe[id.y] & 0xffff, le = Lobe[id.y] >> 16;
    float4 acc = 0, mn = 1e9, mx = -1e9;
    [loop] for (uint k = 0; k < taps; ++k) {
        float4 v = Src.Load(int3(id.x, s + (int)k, 0));
        acc += v * Weight[id.y * taps + k];
        if (k >= lb && k < le) { mn = min(mn, v); mx = max(mx, v); }
    }
    if (antiring) acc = clamp(acc, mn, mx);
    float3 c = linearLight ? toSrgb(acc.rgb) : saturate(acc.rgb);
    Dst[id.xy] = float4(c.b, c.g, c.r, 1.0); // R8G8B8A8 UAV holding BGRA bytes
}
)HLSL";

struct Params {
    uint32_t inW, inH, outW, outH;
    uint32_t taps, antiring, linear, final_pass;
};

struct AxisBuffers {
    Com<ID3D11Buffer> start, weight, lobe;
    Com<ID3D11ShaderResourceView> start_srv, weight_srv, lobe_srv;
    uint32_t taps{0};
};

br_status make_structured(ID3D11Device* dev, const void* data, UINT count, UINT stride, Com<ID3D11Buffer>& buf,
                          Com<ID3D11ShaderResourceView>& srv) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = count * stride;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA init{data, 0, 0};
    HRESULT hr = dev->CreateBuffer(&bd, &init, buf.put());
    if (FAILED(hr)) return hr_fail(BR_E_OUT_OF_MEMORY, "CreateBuffer", hr);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements = count;
    hr = dev->CreateShaderResourceView(buf.get(), &sd, srv.put());
    if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "CreateShaderResourceView(buffer)", hr);
    return BR_OK;
}

br_status make_axis(ID3D11Device* dev, const resize::AxisSampler& s, AxisBuffers& b) {
    std::vector<uint32_t> lobe(s.out_size);
    for (uint32_t i = 0; i < s.out_size; ++i) lobe[i] = s.lobe_begin[i] | (static_cast<uint32_t>(s.lobe_end[i]) << 16);
    br_status st = make_structured(dev, s.start.data(), s.out_size, 4, b.start, b.start_srv);
    if (st == BR_OK) st = make_structured(dev, s.weights.data(), static_cast<UINT>(s.weights.size()), 4, b.weight, b.weight_srv);
    if (st == BR_OK) st = make_structured(dev, lobe.data(), s.out_size, 4, b.lobe, b.lobe_srv);
    b.taps = s.taps;
    return st;
}

}

struct GpuScaler::Impl {
    Com<ID3D11Device> dev;
    Com<ID3D11ComputeShader> cs_h, cs_v;
    Com<ID3D11Buffer> cbuf;
    uint32_t in_w{0}, in_h{0}, out_w{0}, out_h{0};
    br_filter filter{BR_FILTER_AUTO};
    AxisBuffers hx, vy;
    Com<ID3D11Texture2D> tmp, out, staging;
    Com<ID3D11UnorderedAccessView> tmp_uav, out_uav;
    Com<ID3D11ShaderResourceView> tmp_srv;
    ID3D11Texture2D* src_seen{nullptr};
    Com<ID3D11ShaderResourceView> src_srv;
};

GpuScaler::GpuScaler() : impl_(new Impl()) {}
GpuScaler::~GpuScaler() { delete impl_; }

br_status GpuScaler::init(ID3D11Device* device) {
    Impl& m = *impl_;
    if (m.cs_h) return BR_OK;
    if (!api().D3DCompile) return fail(BR_E_UNSUPPORTED, "d3dcompiler_47.dll is not available");
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) return fail(BR_E_UNSUPPORTED, "GPU scaler needs feature level 11_0");
    m.dev = Com<ID3D11Device>(device, true);
    for (int i = 0; i < 2; ++i) {
        Com<ID3DBlob> code, errors;
        const HRESULT hr = api().D3DCompile(kShader, sizeof(kShader) - 1, "br_scaler", nullptr, nullptr,
                                            i == 0 ? "horizontal" : "vertical", "cs_5_0", 1u << 15 /* Уровень оптимизации 3. */,
                                            0, code.put(), errors.put());
        if (FAILED(hr)) {
            const char* msg = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown";
            set_errorf("HLSL compile failed: %.400s", msg);
            return BR_E_INTERNAL;
        }
        Com<ID3D11ComputeShader>& cs = i == 0 ? m.cs_h : m.cs_v;
        const HRESULT hr2 = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, cs.put());
        if (FAILED(hr2)) return hr_fail(BR_E_INTERNAL, "CreateComputeShader", hr2);
    }
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Params);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const HRESULT hr = device->CreateBuffer(&cb, nullptr, m.cbuf.put());
    if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "CreateBuffer(constants)", hr);
    return BR_OK;
}

br_status GpuScaler::run(ID3D11DeviceContext* ctx, ID3D11Texture2D* src, uint32_t in_w, uint32_t in_h, uint32_t out_w,
                         uint32_t out_h, br_filter filter, bool linear, bool antiring, br_mut_image_view& dst) {
    Impl& m = *impl_;
    if (!m.cs_h) return fail(BR_E_UNSUPPORTED, "GPU scaler not initialised");
    ID3D11Device* dev = m.dev.get();
    HRESULT hr;
    if (m.in_w != in_w || m.in_h != in_h || m.out_w != out_w || m.out_h != out_h || m.filter != filter) {
        const resize::AxisSampler hs = resize::build_axis_sampler(in_w, out_w, filter);
        const resize::AxisSampler vs = resize::build_axis_sampler(in_h, out_h, filter);
        br_status st = make_axis(dev, hs, m.hx);
        if (st == BR_OK) st = make_axis(dev, vs, m.vy);
        if (st != BR_OK) return st;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = out_w;
        td.Height = in_h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        hr = dev->CreateTexture2D(&td, nullptr, m.tmp.put());
        if (FAILED(hr)) return hr_fail(BR_E_OUT_OF_MEMORY, "CreateTexture2D(tmp)", hr);
        td.Height = out_h;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        hr = dev->CreateTexture2D(&td, nullptr, m.out.put());
        if (FAILED(hr)) return hr_fail(BR_E_OUT_OF_MEMORY, "CreateTexture2D(out)", hr);
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = dev->CreateTexture2D(&td, nullptr, m.staging.put());
        if (FAILED(hr)) return hr_fail(BR_E_OUT_OF_MEMORY, "CreateTexture2D(staging)", hr);
        if (FAILED(hr = dev->CreateUnorderedAccessView(m.tmp.get(), nullptr, m.tmp_uav.put())) ||
            FAILED(hr = dev->CreateShaderResourceView(m.tmp.get(), nullptr, m.tmp_srv.put())) ||
            FAILED(hr = dev->CreateUnorderedAccessView(m.out.get(), nullptr, m.out_uav.put())))
            return hr_fail(BR_E_INTERNAL, "create views", hr);
        m.in_w = in_w; m.in_h = in_h; m.out_w = out_w; m.out_h = out_h; m.filter = filter;
    }
    if (m.src_seen != src || !m.src_srv) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = dev->CreateShaderResourceView(src, &sd, m.src_srv.put());
        if (FAILED(hr)) return hr_fail(BR_E_UNSUPPORTED, "CreateShaderResourceView(source)", hr);
        m.src_seen = src;
    }

    auto set_params = [&](uint32_t taps, bool final_pass) -> bool {
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (FAILED(ctx->Map(m.cbuf.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
        const Params p{in_w, in_h, out_w, out_h, taps, antiring ? 1u : 0u, linear ? 1u : 0u, final_pass ? 1u : 0u};
        std::memcpy(ms.pData, &p, sizeof(p));
        ctx->Unmap(m.cbuf.get(), 0);
        return true;
    };
    ID3D11Buffer* cbs[] = {m.cbuf.get()};
    ID3D11ShaderResourceView* null_srv[4] = {};
    ID3D11UnorderedAccessView* null_uav[1] = {};

    if (!set_params(m.hx.taps, false)) return fail(BR_E_INTERNAL, "Map(constants) failed");
    {
        ID3D11ShaderResourceView* srvs[] = {m.src_srv.get(), m.hx.start_srv.get(), m.hx.weight_srv.get(), m.hx.lobe_srv.get()};
        ID3D11UnorderedAccessView* uavs[] = {m.tmp_uav.get()};
        ctx->CSSetShader(m.cs_h.get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cbs);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->Dispatch((out_w + 15) / 16, (in_h + 15) / 16, 1);
        ctx->CSSetShaderResources(0, 4, null_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
    }
    if (!set_params(m.vy.taps, true)) return fail(BR_E_INTERNAL, "Map(constants) failed");
    {
        ID3D11ShaderResourceView* srvs[] = {m.tmp_srv.get(), m.vy.start_srv.get(), m.vy.weight_srv.get(), m.vy.lobe_srv.get()};
        ID3D11UnorderedAccessView* uavs[] = {m.out_uav.get()};
        ctx->CSSetShader(m.cs_v.get(), nullptr, 0);
        ctx->CSSetConstantBuffers(0, 1, cbs);
        ctx->CSSetShaderResources(0, 4, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        ctx->Dispatch((out_w + 15) / 16, (out_h + 15) / 16, 1);
        ctx->CSSetShaderResources(0, 4, null_srv);
        ctx->CSSetUnorderedAccessViews(0, 1, null_uav, nullptr);
        ctx->CSSetShader(nullptr, nullptr, 0);
    }
    ctx->CopyResource(m.staging.get(), m.out.get());
    D3D11_MAPPED_SUBRESOURCE ms{};
    hr = ctx->Map(m.staging.get(), 0, D3D11_MAP_READ, 0, &ms);
    if (FAILED(hr)) return hr_fail(BR_E_INTERNAL, "Map(staging)", hr);
    if (dst.data && (dst.width != out_w || dst.height != out_h || dst.format != BR_PIXEL_BGRA8)) free_image(dst);
    if (!dst.data) {
        try {
            dst = alloc_image(out_w, out_h, BR_PIXEL_BGRA8, false);
        } catch (...) {
            ctx->Unmap(m.staging.get(), 0);
            throw;
        }
    }
    for (uint32_t y = 0; y < out_h; ++y)
        std::memcpy(row_ptr(dst, y), static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch, static_cast<size_t>(out_w) * 4);
    ctx->Unmap(m.staging.get(), 0);
    return BR_OK;
}

}

#endif
