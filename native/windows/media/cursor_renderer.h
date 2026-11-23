#pragma once

#include "../rtc/cursor_protocol.h"

#include <QImage>
#include <QRectF>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>

namespace evp {

class CursorRenderer {
public:
    bool initialize(ID3D11Device* device) {
        if (attempted_) return bool(textureView_);
        attempted_ = true;
        static constexpr char shader[] = R"(
            Texture2D image : register(t0);
            SamplerState linearClamp : register(s0);
            cbuffer CursorParameters : register(b0) { float4 limits; };
            struct Vertex {
                float4 position : SV_Position;
                float2 uv : TEXCOORD0;
            };
            Vertex vertexMain(uint id : SV_VertexID) {
                Vertex v;
                v.uv = float2((id << 1) & 2, id & 2);
                v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
                return v;
            }
            float4 pixelMain(Vertex v) : SV_Target {
                return image.Sample(linearClamp, min(v.uv * limits.xy, limits.zw));
            }
        )";
        ComPtr<ID3DBlob> code;
        if (FAILED(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "vertexMain", "vs_4_0",
                              0, 0, &code, nullptr)) ||
            FAILED(device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vertex_)))
            return false;
        code.Reset();
        if (FAILED(D3DCompile(shader, sizeof(shader) - 1, nullptr, nullptr, nullptr, "pixelMain", "ps_4_0",
                              0, 0, &code, nullptr)) ||
            FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &pixel_)))
            return false;
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0] = {TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
                                D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
        if (FAILED(device->CreateBlendState(&blend, &blend_))) return false;
        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.ScissorEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rasterizer, &rasterizer_))) return false;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(float) * 4;
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &parameters_))) return false;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = maxCursorEdge;
        desc.Height = maxCursorEdge;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture_)) &&
            SUCCEEDED(device->CreateShaderResourceView(texture_.Get(), nullptr, &textureView_));
    }

    void draw(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11SamplerState* sampler,
              const QImage& image, const QRectF& normalized, const QRectF& content) {
        if (image.isNull() || content.isEmpty() || !initialize(device)) return;
        upload(context, image);
        const float x = static_cast<float>(content.x() + normalized.x() * content.width());
        const float y = static_cast<float>(content.y() + normalized.y() * content.height());
        const float width = static_cast<float>(normalized.width() * content.width());
        const float height = static_cast<float>(normalized.height() * content.height());
        const D3D11_VIEWPORT viewport{x, y, width, height, 0.f, 1.f};
        // Scissor at pixel centers so clipped cursors stay inside the video.
        const D3D11_RECT clip{static_cast<LONG>(std::ceil(content.left() - 0.5)),
            static_cast<LONG>(std::ceil(content.top() - 0.5)), static_cast<LONG>(std::ceil(content.right() - 0.5)),
            static_cast<LONG>(std::ceil(content.bottom() - 0.5))};
        context->RSSetViewports(1, &viewport);
        context->RSSetScissorRects(1, &clip);
        context->RSSetState(rasterizer_.Get());
        context->OMSetBlendState(blend_.Get(), nullptr, 0xffffffff);
        context->VSSetShader(vertex_.Get(), nullptr, 0);
        context->PSSetShader(pixel_.Get(), nullptr, 0);
        context->PSSetSamplers(0, 1, &sampler);
        context->PSSetConstantBuffers(0, 1, parameters_.GetAddressOf());
        context->PSSetShaderResources(0, 1, textureView_.GetAddressOf());
        context->Draw(3, 0);
        ID3D11ShaderResourceView* unbound = nullptr;
        context->PSSetShaderResources(0, 1, &unbound);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->RSSetState(nullptr);
    }

private:
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    void upload(ID3D11DeviceContext* context, const QImage& image) {
        if (shape_ == image.cacheKey()) return;
        const auto width = static_cast<UINT>(image.width());
        const auto height = static_cast<UINT>(image.height());
        const D3D11_BOX area{0, 0, 0, width, height, 1};
        context->UpdateSubresource(texture_.Get(), 0, &area, image.constBits(),
                                   static_cast<UINT>(image.bytesPerLine()), 0);
        constexpr float edge = maxCursorEdge;
        const float limits[]{width / edge, height / edge, (width - 0.5f) / edge, (height - 0.5f) / edge};
        context->UpdateSubresource(parameters_.Get(), 0, nullptr, limits, 0, 0);
        shape_ = image.cacheKey();
    }

    bool attempted_ = false;
    qint64 shape_ = 0;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11VertexShader> vertex_;
    ComPtr<ID3D11PixelShader> pixel_;
    ComPtr<ID3D11Buffer> parameters_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> textureView_;
};

} // namespace evp
