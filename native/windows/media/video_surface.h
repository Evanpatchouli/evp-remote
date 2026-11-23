#pragma once

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QWidget>

#include <cstdint>
#include <functional>
#include <memory>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace evp {

class VideoSurface final : public QWidget {
public:
    enum class Matrix { Bt601, Bt709, Bt2020 };

    struct Color {
        Matrix matrix;
        bool fullRange;
    };

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t replaced = 0;
        std::uint64_t presented = 0;
        std::uint64_t presentBusy = 0;
        std::uint64_t failures = 0;
        std::uint64_t lastPresentDelayUs = 0;
    };

    explicit VideoSurface(QWidget* parent = nullptr);
    ~VideoSurface() override;

    // Submission methods are thread-safe. An owner keeps the plane memory alive
    // until presentation. Omit it to copy the planes during submission.
    // Without stream metadata, use limited-range BT.709 for HD and BT.601 for SD.
    bool submitI420(int width, int height,
                    const std::uint8_t* y, int strideY,
                    const std::uint8_t* u, int strideU,
                    const std::uint8_t* v, int strideV,
                    int rotation = 0, std::shared_ptr<const void> owner = {});
    bool submitI420(int width, int height,
                    const std::uint8_t* y, int strideY,
                    const std::uint8_t* u, int strideU,
                    const std::uint8_t* v, int strideV,
                    int rotation, std::shared_ptr<const void> owner, Color color);
    bool submitBgra(int width, int height, const std::uint8_t* pixels, int stride,
                    int rotation = 0, std::shared_ptr<const void> owner = {});
    bool submitNv12(int width, int height, ID3D11Texture2D* texture, unsigned slice,
                    std::shared_ptr<const void> owner, Color color, int rotation = 0);

    // These geometry methods and device acquisition run on the widget thread.
    // The acquired device is AddRef'd for the caller.
    QRectF contentRect() const;
    QSize sourceSize() const;
    ID3D11Device* acquireD3D11Device();
    bool supportsRemoteCursor();
    void setRemoteCursor(QImage image, QRectF normalized);
    void setCursorFailureHandler(std::function<void()> handler);
    Stats stats() const;

protected:
    QPaintEngine* paintEngine() const override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
#ifdef EVP_VIDEO_SURFACE_READBACK_PROBE
    friend struct VideoSurfaceReadback;
#endif
    struct Frame;
    struct State;

    bool submit(Frame frame);
    void requestRender();
    void queueRenderLocked();
    void renderLatest();

    std::unique_ptr<State> state_;
};

}  // namespace evp
