#include "video_surface.h"
#include "cursor_renderer.h"

#ifdef EVP_VIDEO_SURFACE_READBACK_PROBE
#include "../tests/video_surface_readback.h"
#endif

#include <QMetaObject>
#include <QTimer>

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace evp {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

constexpr char shader[] = R"(
cbuffer Parameters : register(b0) {
    float4 red;
    float4 green;
    float4 blue;
    uint rotation;
    uint format;
    float2 textureScale;
    float4 textureBounds;
};
cbuffer DownsampleParameters : register(b1) {
    float2 sourceDimensions;
    float2 axis;
    float reduction;
    uint displayOutput;
};
Texture2D plane0 : register(t0);
Texture2D plane1 : register(t1);
Texture2D plane2 : register(t2);
SamplerState linearClamp : register(s0);

struct Vertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Vertex vertexMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    Vertex output;
    output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    if (rotation == 90) uv = float2(uv.y, 1 - uv.x);
    if (rotation == 180) uv = 1 - uv;
    if (rotation == 270) uv = float2(1 - uv.y, uv.x);
    output.uv = uv * textureScale;
    return output;
}

float3 sampleBase(float2 uv) {
    return plane0.SampleLevel(linearClamp, min(uv, textureBounds.xy), 0).rgb;
}

float2 sampleChroma(float2 uv) {
    uv = min(uv, textureBounds.zw);
    float2 chroma = plane1.SampleLevel(linearClamp, uv, 0).rg;
    if (format == 0) chroma.y = plane2.SampleLevel(linearClamp, uv, 0).r;
    return chroma;
}

bool hasChromaPlanes() {
    return format == 0 || format == 2;
}

float3 samplePicture(float2 uv) {
    float3 base = sampleBase(uv);
    if (hasChromaPlanes()) base.gb = sampleChroma(uv);
    return base;
}

float4 displayPicture(float3 picture) {
    if (format == 1) return float4(saturate(picture), 1);
    float4 yuv = float4(picture, 1);
    return float4(saturate(float3(dot(yuv, red), dot(yuv, green), dot(yuv, blue))), 1);
}

float4 pixelMain(Vertex input) : SV_Target {
    return displayPicture(samplePicture(input.uv));
}

float lanczos2(float distance) {
    distance = abs(distance);
    if (distance >= 2) return 0;
    if (distance < 0.0001) return 1;
    float angle = distance * 1.570796327;
    float sine, cosine;
    sincos(angle, sine, cosine);
    float sinc = sine / angle;
    return sinc * sinc * cosine;
}

float3 sampleLanczos(float2 uv) {
    float center = dot(uv * sourceDimensions, axis) - 0.5;
    float firstTap = floor(center - 2 * reduction) + 1;
    float2 texel = axis / sourceDimensions;
    float3 sum = 0;
    float normalization = 0;
    // A 2x box prefilter bounds the remaining reduction to two and the
    // Lanczos radius to four source texels. Merge adjacent same-sign taps
    // into one hardware-linear lookup while retaining the negative lobes.
    [loop] for (int pair = 0; pair < 4; ++pair) {
        float offset = firstTap + pair * 2 - center;
        float left = lanczos2(offset / reduction);
        float right = lanczos2((offset + 1) / reduction);
        float weight = left + right;
        if (left * right > 0) {
            sum += weight * sampleBase(uv + (offset + right / weight) * texel);
        } else {
            if (left != 0) sum += left * sampleBase(uv + offset * texel);
            if (right != 0) sum += right * sampleBase(uv + (offset + 1) * texel);
        }
        normalization += weight;
    }
    return sum / normalization;
}

float4 pixelDownsample(Vertex input) : SV_Target {
    float3 picture;
    if (reduction > 0) {
        picture = sampleLanczos(input.uv);
        // Separate 4:2:0 planes are still being upsampled or mapped 1:1
        // here. Keep their linear reconstruction on the first axis.
        if (hasChromaPlanes()) picture.gb = sampleChroma(input.uv);
    } else {
        picture = samplePicture(input.uv);
    }
    return displayOutput ? displayPicture(picture) : float4(picture, 1);
}
)";

struct ShaderParameters {
    float coefficients[3][4];
    unsigned rotation;
    unsigned format;
    float textureScale[2];
    float textureBounds[4];
};

static_assert(sizeof(ShaderParameters) == 80);

struct DownsampleParameters {
    float sourceDimensions[2];
    float axis[2];
    float reduction;
    unsigned displayOutput;
    float padding[2]{};
};

static_assert(sizeof(DownsampleParameters) == 32);
constexpr unsigned packedYuvFormat = 3;

ShaderParameters shaderParameters(VideoSurface::Color color, int rotation, unsigned format,
                                  QSize visible, QSize texture, bool paddedNv12) {
    float kr = 0.299f;
    float kb = 0.114f;
    if (color.matrix == VideoSurface::Matrix::Bt709) {
        kr = 0.2126f;
        kb = 0.0722f;
    } else if (color.matrix == VideoSurface::Matrix::Bt2020) {
        kr = 0.2627f;
        kb = 0.0593f;
    }
    const float kg = 1.f - kr - kb;
    const float yScale = color.fullRange ? 1.f : 255.f / 219.f;
    const float chromaScale = color.fullRange ? 1.f : 255.f / 224.f;
    const float yOffset = color.fullRange ? 0.f : 16.f / 255.f;
    constexpr float chromaOffset = 128.f / 255.f;
    ShaderParameters parameters{
        {{yScale, 0.f, 2.f * (1.f - kr) * chromaScale, 0.f},
         {yScale, -2.f * kb * (1.f - kb) / kg * chromaScale,
                 -2.f * kr * (1.f - kr) / kg * chromaScale, 0.f},
         {yScale, 2.f * (1.f - kb) * chromaScale, 0.f, 0.f}},
        static_cast<unsigned>(rotation), format,
        {static_cast<float>(visible.width()) / texture.width(),
         static_cast<float>(visible.height()) / texture.height()},
        {1.f, 1.f, 1.f, 1.f}};
    if (paddedNv12) {
        // Decoder textures may be larger than the coded picture. Keep bilinear
        // sampling inside the visible luma and chroma texel centers.
        parameters.textureBounds[0] = (visible.width() - 0.5f) / texture.width();
        parameters.textureBounds[1] = (visible.height() - 0.5f) / texture.height();
        parameters.textureBounds[2] = ((visible.width() + 1) / 2 - 0.5f) / (texture.width() / 2);
        parameters.textureBounds[3] = ((visible.height() + 1) / 2 - 0.5f) / (texture.height() / 2);
    }
    for (auto& row : parameters.coefficients) {
        row[3] = -row[0] * yOffset - (row[1] + row[2]) * chromaOffset;
    }
    return parameters;
}

QRectF fit(QSizeF area, QSize source) {
    if (area.isEmpty() || source.isEmpty()) return {};
    const auto scale = std::min(area.width() / source.width(), area.height() / source.height());
    const QSizeF size(source.width() * scale, source.height() * scale);
    return {(area.width() - size.width()) / 2, (area.height() - size.height()) / 2,
            size.width(), size.height()};
}

bool supportedSize(int width, int height, int rotation) {
    constexpr int limit = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    return width > 0 && width <= limit && height > 0 && height <= limit
        && (rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270);
}

class ContextLock {
public:
    explicit ContextLock(ID3D11Multithread* context) : context_(context) {
        if (context_) context_->Enter();
    }
    ~ContextLock() { if (context_) context_->Leave(); }

private:
    ID3D11Multithread* context_;
};

struct CloseEvent {
    void operator()(void* handle) const { CloseHandle(handle); }
};

}  // namespace

struct VideoSurface::Frame {
    enum class Format { I420, Bgra, Nv12 };

    struct Plane {
        const std::uint8_t* pixels = nullptr;
        int stride = 0;
        int widthBytes = 0;
        int height = 0;
    };

    bool ownPlanes() {
        std::size_t bytes = 0;
        bool needsCopy = !owner;
        for (const auto& plane : planes) {
            if (!plane.widthBytes) continue;
            if (!plane.pixels || (plane.stride < plane.widthBytes && plane.stride > -plane.widthBytes)) {
                return false;
            }
            needsCopy = needsCopy || plane.stride < 0;
            bytes += static_cast<std::size_t>(plane.widthBytes) * plane.height;
        }
        if (!needsCopy) return true;

        auto storage = std::make_shared<std::vector<std::uint8_t>>(bytes);
        auto* dst = storage->data();
        for (auto& plane : planes) {
            if (!plane.widthBytes) continue;
            for (int row = 0; row < plane.height; ++row) {
                std::memcpy(dst + static_cast<std::ptrdiff_t>(row) * plane.widthBytes,
                            plane.pixels + static_cast<std::ptrdiff_t>(row) * plane.stride,
                            plane.widthBytes);
            }
            plane.pixels = dst;
            plane.stride = plane.widthBytes;
            dst += static_cast<std::ptrdiff_t>(plane.height) * plane.widthBytes;
        }
        owner = std::move(storage);
        return true;
    }

    QSize orientedSize() const {
        return rotation == 90 || rotation == 270 ? QSize(height, width) : QSize(width, height);
    }

    Format format = Format::I420;
    int width = 0;
    int height = 0;
    int rotation = 0;
    Color color{Matrix::Bt601, false};
    std::array<Plane, 3> planes{};
    ComPtr<ID3D11Texture2D> texture;
    unsigned slice = 0;
    std::shared_ptr<const void> owner;
    Clock::time_point received = Clock::now();
    std::uint64_t sequence = 0;
};

struct VideoSurface::State {
    struct Gpu {
        using Resources = std::array<ID3D11ShaderResourceView*, 3>;

        struct FilterTarget {
            QSize size;
            ComPtr<ID3D11RenderTargetView> target;
            ComPtr<ID3D11ShaderResourceView> view;

            bool ensure(ID3D11Device* device, QSize requested) {
                if (size == requested) return view != nullptr;
                target.Reset();
                view.Reset();
                size = requested;
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = size.width();
                desc.Height = size.height();
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> texture;
                return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture))
                    && SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &target))
                    && SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
            }
        };

        HRESULT initializeDevice() {
            if (device) return S_OK;
            constexpr UINT bgra = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                            bgra | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                            nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
            if (FAILED(result)) {
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, bgra,
                                           nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
            }
            if (FAILED(result)) {
                result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, bgra,
                                           nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
            }
            if (SUCCEEDED(result) && SUCCEEDED(context.As(&multithread))) {
                multithread->SetMultithreadProtected(TRUE);
            }
            return result;
        }

        HRESULT initializePipeline() {
            if (sampler) return S_OK;
            ComPtr<ID3DBlob> code;
            auto result = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr,
                                     "vertexMain", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                     &code, nullptr);
            if (FAILED(result)) return result;
            result = device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                nullptr, &vertexShader);
            if (FAILED(result)) return result;
            result = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr,
                                "pixelMain", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                &code, nullptr);
            if (FAILED(result)) return result;
            result = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                               nullptr, &pixelShader);
            if (FAILED(result)) return result;
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = sizeof(ShaderParameters);
            buffer.Usage = D3D11_USAGE_DEFAULT;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            result = device->CreateBuffer(&buffer, nullptr, &parameters);
            if (FAILED(result)) return result;
            D3D11_SAMPLER_DESC sampling{};
            sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sampling.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampling.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampling.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sampling.MaxLOD = D3D11_FLOAT32_MAX;
            return device->CreateSamplerState(&sampling, &sampler);
        }

        bool initializeDownsampler() {
            if (downsampleAttempted) return downsampleParameters != nullptr;
            downsampleAttempted = true;
            ComPtr<ID3DBlob> code;
            auto result = D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr,
                                     "pixelDownsample", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                                     &code, nullptr);
            if (FAILED(result)) return false;
            result = device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(),
                                               nullptr, &downsampleShader);
            if (FAILED(result)) return false;
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = sizeof(DownsampleParameters);
            buffer.Usage = D3D11_USAGE_DEFAULT;
            buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            return SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &downsampleParameters));
        }

        HRESULT resize(HWND hwnd, QSize size) {
            if (swapChain && window != hwnd) {
                context->OMSetRenderTargets(0, nullptr, nullptr);
                target.Reset();
                latencyEvent.reset();
                renderCredit = false;
                swapChain.Reset();
                context->Flush();
            }
            if (!swapChain) {
                ComPtr<IDXGIDevice> dxgiDevice;
                auto result = device.As(&dxgiDevice);
                if (FAILED(result)) return result;
                ComPtr<IDXGIAdapter> adapter;
                result = dxgiDevice->GetAdapter(&adapter);
                if (FAILED(result)) return result;
                ComPtr<IDXGIFactory2> factory;
                result = adapter->GetParent(IID_PPV_ARGS(&factory));
                if (FAILED(result)) return result;

                BOOL allowTearing = FALSE;
                ComPtr<IDXGIFactory5> factory5;
                if (SUCCEEDED(factory.As(&factory5))) {
                    factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                  sizeof(allowTearing));
                }
                presentFlags = DXGI_PRESENT_DO_NOT_WAIT;
                swapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                if (allowTearing) {
                    presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
                    swapFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
                }
                syncInterval = allowTearing ? 0 : 1;

                DXGI_SWAP_CHAIN_DESC1 desc{};
                desc.Width = size.width();
                desc.Height = size.height();
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.Scaling = DXGI_SCALING_STRETCH;
                desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                desc.Flags = swapFlags;
                result = factory->CreateSwapChainForHwnd(device.Get(), hwnd, &desc, nullptr,
                                                         nullptr, &swapChain);
                if (FAILED(result)) return result;
                ComPtr<IDXGISwapChain2> swapChain2;
                if (SUCCEEDED(swapChain.As(&swapChain2))) {
                    swapChain2->SetMaximumFrameLatency(1);
                    latencyEvent.reset(swapChain2->GetFrameLatencyWaitableObject());
                }
                factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
                window = hwnd;
                backBufferSize = size;
            } else if (size != backBufferSize) {
                context->OMSetRenderTargets(0, nullptr, nullptr);
                target.Reset();
                const auto result = swapChain->ResizeBuffers(0, size.width(), size.height(),
                                                             DXGI_FORMAT_UNKNOWN, swapFlags);
                if (FAILED(result)) return result;
                backBufferSize = size;
            }
            if (target) return S_OK;
            ComPtr<ID3D11Texture2D> buffer;
            const auto result = swapChain->GetBuffer(0, IID_PPV_ARGS(&buffer));
            if (FAILED(result)) return result;
            return device->CreateRenderTargetView(buffer.Get(), nullptr, &target);
        }

        HRESULT createTexture(unsigned index, UINT width, UINT height, DXGI_FORMAT format) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            return device->CreateTexture2D(&desc, nullptr, &textures[index]);
        }

        HRESULT createView(unsigned index, ID3D11Texture2D* texture, DXGI_FORMAT format) {
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = format;
            desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipLevels = 1;
            return device->CreateShaderResourceView(texture, &desc, &views[index]);
        }

        HRESULT upload(const Frame& frame) {
            if (frame.sequence == uploaded) return S_OK;
            QSize textureSize(frame.width, frame.height);
            D3D11_TEXTURE2D_DESC source{};
            if (frame.format == Frame::Format::Nv12) {
                frame.texture->GetDesc(&source);
                ComPtr<ID3D11Device> textureDevice;
                frame.texture->GetDevice(&textureDevice);
                if (textureDevice.Get() != device.Get() || source.Format != DXGI_FORMAT_NV12
                    || frame.slice >= source.ArraySize || source.Width < static_cast<UINT>(frame.width)
                    || source.Height < static_cast<UINT>(frame.height)) {
                    return E_INVALIDARG;
                }
                textureSize = QSize(source.Width, source.Height);
            }

            if (textureSize != inputTextureSize || frame.format != inputFormat) {
                textures = {};
                views = {};
                uploaded = 0;
                inputTextureSize = {};
                HRESULT result = S_OK;
                if (frame.format == Frame::Format::Nv12) {
                    result = createTexture(0, source.Width, source.Height, DXGI_FORMAT_NV12);
                    if (SUCCEEDED(result)) result = createView(0, textures[0].Get(), DXGI_FORMAT_R8_UNORM);
                    if (SUCCEEDED(result)) result = createView(1, textures[0].Get(), DXGI_FORMAT_R8G8_UNORM);
                } else {
                    const auto format = frame.format == Frame::Format::Bgra
                        ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8_UNORM;
                    const auto count = frame.format == Frame::Format::Bgra ? 1u : 3u;
                    for (unsigned i = 0; i < count && SUCCEEDED(result); ++i) {
                        const auto size = i == 0 ? textureSize
                            : QSize((frame.width + 1) / 2, (frame.height + 1) / 2);
                        result = createTexture(i, size.width(), size.height(), format);
                        if (SUCCEEDED(result)) result = createView(i, textures[i].Get(), format);
                    }
                }
                if (FAILED(result)) return result;
                inputTextureSize = textureSize;
                inputFormat = frame.format;
            }

            if (frame.format == Frame::Format::Nv12) {
                const auto subresource = D3D11CalcSubresource(0, frame.slice, source.MipLevels);
                context->CopySubresourceRegion(textures[0].Get(), 0, 0, 0, 0,
                                               frame.texture.Get(), subresource, nullptr);
            } else {
                for (unsigned i = 0; i < textures.size() && textures[i]; ++i) {
                    const auto& plane = frame.planes[i];
                    context->UpdateSubresource(textures[i].Get(), 0, nullptr, plane.pixels,
                                               static_cast<UINT>(plane.stride), 0);
                }
            }
            uploaded = frame.sequence;
            return S_OK;
        }

        HRESULT prepare(HWND hwnd, QSize size) {
            auto result = initializeDevice();
            if (FAILED(result)) return result;
            result = initializePipeline();
            if (FAILED(result)) return result;

            ContextLock lock(multithread.Get());
            result = resize(hwnd, size);
            if (FAILED(result) || !latencyEvent || renderCredit) return result;
            const auto ready = WaitForSingleObject(latencyEvent.get(), 0);
            if (ready == WAIT_OBJECT_0) {
                renderCredit = true;
                return S_OK;
            }
            if (ready == WAIT_TIMEOUT) return DXGI_ERROR_WAS_STILL_DRAWING;
            return HRESULT_FROM_WIN32(GetLastError());
        }

        void drawPicture(ID3D11RenderTargetView* destination, const QRectF& area,
                         ID3D11PixelShader* pixel, const ShaderParameters& constants,
                         const Resources& resources) {
            const D3D11_VIEWPORT viewport{static_cast<float>(area.x()), static_cast<float>(area.y()),
                static_cast<float>(area.width()), static_cast<float>(area.height()), 0.f, 1.f};
            context->RSSetViewports(1, &viewport);
            context->OMSetRenderTargets(1, &destination, nullptr);
            context->PSSetShader(pixel, nullptr, 0);
            context->UpdateSubresource(parameters.Get(), 0, nullptr, &constants, 0, 0);
            context->PSSetShaderResources(0, static_cast<UINT>(resources.size()), resources.data());
            context->Draw(3, 0);
            const Resources unbound{};
            context->PSSetShaderResources(0, static_cast<UINT>(unbound.size()), unbound.data());
        }

        void setDownsampleParameters(QSize source, bool horizontal, float reduction, bool display) {
            const DownsampleParameters constants{
                {static_cast<float>(source.width()), static_cast<float>(source.height())},
                {horizontal ? 1.f : 0.f, horizontal ? 0.f : 1.f}, reduction, display ? 1u : 0u};
            context->UpdateSubresource(downsampleParameters.Get(), 0, nullptr, &constants, 0, 0);
        }

        bool drawDownsampled(QSize windowSize, const QRectF& area, const Frame& frame,
                             const ShaderParameters& original, Resources resources) {
            const QSize source(frame.width, frame.height);
            if (source != filterSource) {
                filterSource = source;
                prefilters = {};
                filteredAxis = {};
                filteredSequence = 0;
            }

            // Filter the axis that fills the viewport first. Its intermediate
            // grid matches physical output pixels, even when the letterboxed
            // axis has a fractional origin or size.
            const auto oriented = frame.orientedSize();
            const bool fillsWidth = static_cast<std::int64_t>(windowSize.width()) * oriented.height()
                <= static_cast<std::int64_t>(windowSize.height()) * oriented.width();
            const bool horizontalFirst = fillsWidth != (frame.rotation % 180 != 0);
            const int outputLength = fillsWidth ? windowSize.width() : windowSize.height();
            const int sourceLength = horizontalFirst ? source.width() : source.height();
            if (outputLength >= sourceLength || !initializeDownsampler()) return false;

            // Cursor updates and Present retries can redraw the same upload.
            // Only the final pass depends on the full fitted viewport.
            const bool reuse = uploaded == filteredSequence && horizontalFirst == filteredHorizontal
                && outputLength == filteredLength;

            // Do the wider reductions with exact pairs of source texels. Use
            // ceil division so an odd final texel can be replicated at the edge.
            QSize working = source;
            float reduction = static_cast<float>(sourceLength) / outputLength;
            unsigned levels = 0;
            while (reduction > 2.f) {
                working = QSize((working.width() + 1) / 2, (working.height() + 1) / 2);
                if (!reuse && !prefilters[levels].ensure(device.Get(), working)) return false;
                reduction *= 0.5f;
                ++levels;
            }
            const QSize intermediate = horizontalFirst ? QSize(outputLength, working.height())
                                                       : QSize(working.width(), outputLength);
            const unsigned packedFormat = frame.format == Frame::Format::Bgra ? original.format
                                                                             : packedYuvFormat;
            auto constants = original;
            const auto divisor = static_cast<float>(1u << levels);
            context->PSSetConstantBuffers(1, 1, downsampleParameters.GetAddressOf());
            if (!reuse) {
                filteredSequence = 0;
                if (!filteredAxis.ensure(device.Get(), intermediate)) return false;
                constants.rotation = 0;
                QSize textureSize = inputTextureSize;
                if (levels) setDownsampleParameters(textureSize, horizontalFirst, 0.f, false);
                for (unsigned level = 0; level < levels; ++level) {
                    const auto& output = prefilters[level];
                    constants.textureScale[0] = 2.f * output.size.width() / textureSize.width();
                    constants.textureScale[1] = 2.f * output.size.height() / textureSize.height();
                    drawPicture(output.target.Get(), QRectF(QPointF(), output.size),
                                downsampleShader.Get(), constants, resources);
                    textureSize = output.size;
                    resources = {output.view.Get(), nullptr, nullptr};
                    constants.format = packedFormat;
                    std::fill_n(constants.textureBounds, 4, 1.f);
                }

                // The box levels can have a partial final texel. Use the
                // visible span along the filter axis and keep all texels on
                // the opposite axis for the final pass.
                const float spanX = source.width() / divisor / textureSize.width();
                const float spanY = source.height() / divisor / textureSize.height();
                constants.textureScale[0] = levels && !horizontalFirst ? 1.f : spanX;
                constants.textureScale[1] = levels && horizontalFirst ? 1.f : spanY;
                setDownsampleParameters(textureSize, horizontalFirst, reduction, false);
                drawPicture(filteredAxis.target.Get(), QRectF(QPointF(), filteredAxis.size),
                            downsampleShader.Get(), constants, resources);
                filteredSequence = uploaded;
                filteredHorizontal = horizontalFirst;
                filteredLength = outputLength;
            }

            constants.rotation = frame.rotation;
            constants.format = packedFormat;
            constants.textureScale[0] = horizontalFirst ? 1.f
                : source.width() / divisor / intermediate.width();
            constants.textureScale[1] = horizontalFirst ? source.height() / divisor / intermediate.height()
                                                        : 1.f;
            std::fill_n(constants.textureBounds, 4, 1.f);
            setDownsampleParameters(intermediate, !horizontalFirst, reduction, true);
            drawPicture(target.Get(), area, downsampleShader.Get(), constants,
                        {filteredAxis.view.Get(), nullptr, nullptr});
            return true;
        }

        HRESULT drawVideo(QSize size, const Frame* frame) {
            HRESULT frameResult = S_OK;
            if (frame) frameResult = upload(*frame);

            constexpr float black[4]{0.f, 0.f, 0.f, 1.f};
            context->ClearRenderTargetView(target.Get(), black);
            if (frame && SUCCEEDED(frameResult)) {
                // The native child window uses physical pixels. Input mapping from
                // the parent uses the equivalent fit in Qt logical coordinates.
                const auto area = fit(size, frame->orientedSize());
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context->VSSetShader(vertexShader.Get(), nullptr, 0);
                context->VSSetConstantBuffers(0, 1, parameters.GetAddressOf());
                context->PSSetConstantBuffers(0, 1, parameters.GetAddressOf());
                context->PSSetSamplers(0, 1, sampler.GetAddressOf());
                const auto constants = shaderParameters(frame->color, frame->rotation,
                    static_cast<unsigned>(frame->format), QSize(frame->width, frame->height), inputTextureSize,
                    frame->format == Frame::Format::Nv12);
                const Resources resources{views[0].Get(), views[1].Get(), views[2].Get()};
                if (!drawDownsampled(size, area, *frame, constants, resources)) {
                    drawPicture(target.Get(), area, pixelShader.Get(), constants, resources);
                }
            }
            return frameResult;
        }

        HRESULT draw(QSize size, const Frame* frame, HRESULT& frameResult, const QImage& cursorImage,
                     const QRectF& cursorRect) {
            ContextLock lock(multithread.Get());
            frameResult = drawVideo(size, frame);
            if (frame && SUCCEEDED(frameResult) && !cursorImage.isNull()) {
                cursorRenderer.draw(device.Get(), context.Get(), sampler.Get(), cursorImage, cursorRect,
                                    fit(size, frame->orientedSize()));
            }
            const auto result = swapChain->Present(syncInterval, presentFlags);
            if (result == S_OK) renderCredit = false;
            return result;
        }

        UINT swapFlags = 0;
        UINT presentFlags = 0;
        UINT syncInterval = 1;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Multithread> multithread;
        ComPtr<IDXGISwapChain1> swapChain;
        std::unique_ptr<void, CloseEvent> latencyEvent;
        bool renderCredit = false;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11VertexShader> vertexShader;
        ComPtr<ID3D11PixelShader> pixelShader;
        ComPtr<ID3D11Buffer> parameters;
        ComPtr<ID3D11SamplerState> sampler;
        CursorRenderer cursorRenderer;
        bool downsampleAttempted = false;
        ComPtr<ID3D11PixelShader> downsampleShader;
        ComPtr<ID3D11Buffer> downsampleParameters;
        QSize filterSource;
        std::array<FilterTarget, D3D11_REQ_MIP_LEVELS> prefilters;
        FilterTarget filteredAxis;
        std::uint64_t filteredSequence = 0;
        bool filteredHorizontal = false;
        int filteredLength = 0;
        std::array<ComPtr<ID3D11Texture2D>, 3> textures;
        std::array<ComPtr<ID3D11ShaderResourceView>, 3> views;
        QSize inputTextureSize;
        Frame::Format inputFormat = Frame::Format::I420;
        QSize backBufferSize;
        HWND window = nullptr;
        std::uint64_t uploaded = 0;
    } gpu;

    mutable std::mutex mutex;
    std::optional<Frame> pending;
    bool renderQueued = false;
    bool redrawRequested = false;
    Stats stats;
    std::optional<Frame> current;
    std::uint64_t lastPresented = 0;
    QSize displayedSource;
    QImage cursorImage;
    QRectF cursorRect;
    bool cursorPrepared = false;
    std::function<void()> cursorFailure;
    bool retryScheduled = false;
};

#ifdef EVP_VIDEO_SURFACE_READBACK_PROBE
QImage VideoSurfaceReadback::render(VideoSurface& surface, QSize size, std::uint64_t* primitives) {
    auto& state = *surface.state_;
    auto& gpu = state.gpu;
    if (state.pending) {
        state.current = std::move(state.pending);
        state.pending.reset();
    }
    if (!state.current || FAILED(gpu.initializeDevice()) || FAILED(gpu.initializePipeline())) return {};

    ComPtr<ID3D11Query> query;
    if (primitives) {
        constexpr D3D11_QUERY_DESC queryDesc{D3D11_QUERY_PIPELINE_STATISTICS, 0};
        if (FAILED(gpu.device->CreateQuery(&queryDesc, &query))) return {};
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = size.width();
    desc.Height = size.height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rendered;
    if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &rendered))
        || FAILED(gpu.device->CreateRenderTargetView(rendered.Get(), nullptr, &gpu.target))) return {};

    const ContextLock lock(gpu.multithread.Get());
    if (query) gpu.context->Begin(query.Get());
    const auto result = gpu.drawVideo(size, &*state.current);
    if (query) gpu.context->End(query.Get());
    gpu.context->OMSetRenderTargets(0, nullptr, nullptr);
    gpu.target.Reset();
    if (FAILED(result)) return {};

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, &staging))) return {};
    gpu.context->CopyResource(staging.Get(), rendered.Get());
    QImage image(size, QImage::Format_ARGB32);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (image.isNull() || FAILED(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    for (int y = 0; y < size.height(); ++y) {
        std::memcpy(image.scanLine(y), static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                    size.width() * sizeof(QRgb));
    }
    gpu.context->Unmap(staging.Get(), 0);
    if (query) {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
        if (gpu.context->GetData(query.Get(), &stats, sizeof(stats), 0) != S_OK) return {};
        *primitives = stats.IAPrimitives;
    }
    return image;
}
#endif

VideoSurface::VideoSurface(QWidget* parent)
    : QWidget(parent), state_(std::make_unique<State>()) {
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_PaintOnScreen);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

VideoSurface::~VideoSurface() = default;

bool VideoSurface::submitI420(int width, int height,
                              const std::uint8_t* y, int strideY,
                              const std::uint8_t* u, int strideU,
                              const std::uint8_t* v, int strideV,
                              int rotation, std::shared_ptr<const void> owner) {
    const Color assumed{height >= 720 || width >= 1280 ? Matrix::Bt709 : Matrix::Bt601, false};
    return submitI420(width, height, y, strideY, u, strideU, v, strideV, rotation,
                      std::move(owner), assumed);
}

bool VideoSurface::submitI420(int width, int height,
                              const std::uint8_t* y, int strideY,
                              const std::uint8_t* u, int strideU,
                              const std::uint8_t* v, int strideV,
                              int rotation, std::shared_ptr<const void> owner, Color color) {
    if (!supportedSize(width, height, rotation)) return false;
    Frame frame;
    frame.width = width;
    frame.height = height;
    frame.rotation = rotation;
    frame.color = color;
    frame.planes = {{{y, strideY, width, height},
                     {u, strideU, (width + 1) / 2, (height + 1) / 2},
                     {v, strideV, (width + 1) / 2, (height + 1) / 2}}};
    frame.owner = std::move(owner);
    return frame.ownPlanes() && submit(std::move(frame));
}

bool VideoSurface::submitBgra(int width, int height, const std::uint8_t* pixels, int stride,
                              int rotation, std::shared_ptr<const void> owner) {
    if (!supportedSize(width, height, rotation)) return false;
    Frame frame;
    frame.format = Frame::Format::Bgra;
    frame.width = width;
    frame.height = height;
    frame.rotation = rotation;
    frame.planes[0] = {pixels, stride, width * 4, height};
    frame.owner = std::move(owner);
    return frame.ownPlanes() && submit(std::move(frame));
}

bool VideoSurface::submitNv12(int width, int height, ID3D11Texture2D* texture, unsigned slice,
                              std::shared_ptr<const void> owner, Color color, int rotation) {
    if (!supportedSize(width, height, rotation) || !texture || !owner) return false;
    Frame frame;
    frame.format = Frame::Format::Nv12;
    frame.width = width;
    frame.height = height;
    frame.rotation = rotation;
    frame.texture = texture;
    frame.slice = slice;
    frame.owner = std::move(owner);
    frame.color = color;
    return submit(std::move(frame));
}

bool VideoSurface::submit(Frame frame) {
    std::optional<Frame> replaced;
    {
        const std::lock_guard lock(state_->mutex);
        frame.sequence = ++state_->stats.submitted;
        if (state_->pending) {
            ++state_->stats.replaced;
            replaced = std::move(state_->pending);
        }
        state_->pending = std::move(frame);
        queueRenderLocked();
    }
    // Release a dropped owner's decoder or WebRTC references outside the mailbox lock.
    return true;
}

void VideoSurface::queueRenderLocked() {
    if (state_->renderQueued) return;
    state_->renderQueued = true;
    QMetaObject::invokeMethod(this, [this] { renderLatest(); }, Qt::QueuedConnection);
}

void VideoSurface::requestRender() {
    const std::lock_guard lock(state_->mutex);
    state_->redrawRequested = true;
    queueRenderLocked();
}

void VideoSurface::renderLatest() {
    {
        const std::lock_guard lock(state_->mutex);
        state_->renderQueued = false;
        if (!state_->pending && !state_->redrawRequested
            && (!state_->current || state_->current->sequence == state_->lastPresented)) return;
    }
    if (!isVisible() || window()->isMinimized()) return;

    const auto hwnd = reinterpret_cast<HWND>(winId());
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return;
    const QSize size(client.right - client.left, client.bottom - client.top);
    if (size.isEmpty()) return;

    HRESULT frameResult = S_OK;
    auto result = state_->gpu.prepare(hwnd, size);
    if (SUCCEEDED(result)) {
        std::optional<Frame> incoming;
        {
            const std::lock_guard lock(state_->mutex);
            state_->redrawRequested = false;
            incoming.swap(state_->pending);
        }
        if (incoming) state_->current = std::move(incoming);
        result = state_->gpu.draw(size, state_->current ? &*state_->current : nullptr, frameResult,
                                  state_->cursorImage, state_->cursorRect);
        if (FAILED(frameResult)) state_->current.reset();
    }

    if ((FAILED(frameResult) || FAILED(result)) && state_->gpu.device
        && FAILED(state_->gpu.device->GetDeviceRemovedReason())) {
        state_->gpu = {};
        if (std::exchange(state_->cursorPrepared, false) && state_->cursorFailure) state_->cursorFailure();
        if (state_->current && state_->current->format == Frame::Format::Nv12) state_->current.reset();
        requestRender();
        const std::lock_guard lock(state_->mutex);
        ++state_->stats.failures;
        return;
    }

    const std::lock_guard lock(state_->mutex);
    if (FAILED(frameResult)) ++state_->stats.failures;
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        ++state_->stats.presentBusy;
        state_->redrawRequested = true;
        if (!state_->retryScheduled) {
            state_->retryScheduled = true;
            QTimer::singleShot(std::chrono::milliseconds(3), Qt::PreciseTimer, this, [this] {
                state_->retryScheduled = false;
                const std::lock_guard lock(state_->mutex);
                queueRenderLocked();
            });
        }
    } else if (FAILED(result)) {
        ++state_->stats.failures;
    } else if (result == S_OK) {
        state_->displayedSource = state_->current ? state_->current->orientedSize() : QSize();
        if (state_->current && state_->lastPresented != state_->current->sequence) {
            ++state_->stats.presented;
            state_->lastPresented = state_->current->sequence;
            state_->stats.lastPresentDelayUs = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - state_->current->received).count();
        }
    }
}

QRectF VideoSurface::contentRect() const {
    return fit(size(), sourceSize());
}

QSize VideoSurface::sourceSize() const {
    return state_->displayedSource;
}

bool VideoSurface::supportsRemoteCursor() {
    state_->cursorPrepared = SUCCEEDED(state_->gpu.initializeDevice()) &&
        state_->gpu.cursorRenderer.initialize(state_->gpu.device.Get());
    return state_->cursorPrepared;
}

void VideoSurface::setRemoteCursor(QImage image, QRectF normalized) {
    if (state_->cursorImage.cacheKey() == image.cacheKey() && state_->cursorRect == normalized) return;
    state_->cursorImage = std::move(image);
    state_->cursorRect = normalized;
    requestRender();
}

void VideoSurface::setCursorFailureHandler(std::function<void()> handler) {
    state_->cursorFailure = std::move(handler);
}

ID3D11Device* VideoSurface::acquireD3D11Device() {
    if (FAILED(state_->gpu.initializeDevice())) return nullptr;
    state_->gpu.device->AddRef();
    return state_->gpu.device.Get();
}

VideoSurface::Stats VideoSurface::stats() const {
    const std::lock_guard lock(state_->mutex);
    return state_->stats;
}

QPaintEngine* VideoSurface::paintEngine() const { return nullptr; }
void VideoSurface::paintEvent(QPaintEvent*) { requestRender(); }
void VideoSurface::resizeEvent(QResizeEvent*) { requestRender(); }
void VideoSurface::showEvent(QShowEvent*) { requestRender(); }

}  // namespace evp
