#include "media/cursor_renderer.h"
#include "rtc/cursor_outbox.h"
#include "viewer_cursor.h"

#include <QTest>

namespace {

evp::CursorSnapshot arrow() {
    evp::CursorSnapshot state;
    state.mode = evp::CursorMode::Separate;
    state.desktop = {1000, 500};
    state.topLeft = {-3, 100};
    state.hotSpot = {4, 2};
    state.image = QImage(16, 16, QImage::Format_ARGB32_Premultiplied);
    state.image.fill(QColor(255, 0, 0, 128));
    return state;
}

evp::CursorPacket packetFor(const evp::CursorSnapshot& state, quint32 sequence, bool full = true) {
    evp::CursorPacket result;
    if (!evp::decodeCursor(evp::encodeCursor(state, 7, sequence, full), result))
        qFatal("Cursor did not round trip");
    return result;
}

class CursorProbe final : public QObject {
    Q_OBJECT

private slots:
    void wireBounds();
    void focusPredictionAndFallback();
    void queueCoalescesAndWaitsForAcknowledgment();
    void d3dOverlayBlendsAndClips();
};

void CursorProbe::wireBounds() {
    const auto state = arrow();
    auto bytes = evp::encodeCursor(state, 7, 1, true);
    evp::CursorPacket decoded;
    QVERIFY(evp::decodeCursor(bytes, decoded));
    QCOMPARE(decoded.epoch, 7u);
    QCOMPARE(decoded.sequence, 1u);
    QCOMPARE(decoded.state.image, state.image);
    QCOMPARE(decoded.state.hotSpot, state.hotSpot);
    QCOMPARE(decoded.state.topLeft, state.topLeft);
    QVERIFY(!evp::decodeCursor(bytes.chopped(1), decoded));
    QVERIFY(!evp::decodeCursor(bytes + QByteArray(1, 'x'), decoded));
    QVERIFY(!evp::decodeCursor(QByteArray(evp::maxCursorPacket + 1, 'x'), decoded));
    bytes[12] = char(0xff);
    QVERIFY(!evp::decodeCursor(bytes, decoded));
    const auto ack = evp::encodeCursorAck(12345);
    QCOMPARE(evp::decodeCursorAck(ack), 12345u);
    QCOMPARE(evp::decodeCursorAck(ack.chopped(1)), 0u);
}

void CursorProbe::focusPredictionAndFallback() {
    evp::ViewerCursor cursor;
    QVERIFY(!cursor.separate());
    QVERIFY(!cursor.hidesLocalPointer(true, true, true));
    auto state = arrow();
    QVERIFY(!cursor.receive(packetFor(state, 1, false)));
    QVERIFY(cursor.receive(packetFor(state, 1)));
    QVERIFY(cursor.separate());
    QVERIFY(cursor.hidesLocalPointer(true, true, true));
    QVERIFY(!cursor.hidesLocalPointer(true, false, true));
    QVERIFY(!cursor.hidesLocalPointer(true, true, false));
    QVERIFY(!cursor.hidesLocalPointer(false, true, true));
    QCOMPARE(cursor.rectangle(false), QRectF(-0.003, 0.2, 0.016, 0.032));

    cursor.predict(cursor.nextMoveSerial(), QPointF(0.5, 0.5));
    cursor.predict(cursor.nextMoveSerial(), QPointF(1.0, 1.0));
    state.appliedMove = 1;
    state.topLeft = {495, 247};
    QVERIFY(cursor.receive(packetFor(state, 2, false)));
    QCOMPARE(cursor.rectangle(true).topLeft(), QPointF(0.995, 0.994));
    QCOMPARE(cursor.rectangle(false).topLeft(), QPointF(0.495, 0.494));
    QVERIFY(!cursor.receive(packetFor(state, 1, false)));
    state.appliedMove = 2;
    state.topLeft = {994, 496};
    QVERIFY(cursor.receive(packetFor(state, 3, false)));
    QCOMPARE(cursor.rectangle(true).topLeft(), QPointF(0.994, 0.992));
    cursor.predict(cursor.nextMoveSerial(), QPointF(0.5, 0.5));
    cursor.expirePrediction();
    QCOMPARE(cursor.rectangle(true).topLeft(), QPointF(0.994, 0.992));

    evp::CursorSnapshot fallback;
    fallback.desktop = state.desktop;
    for (const auto mode : {evp::CursorMode::Composited, evp::CursorMode::Embedded, evp::CursorMode::Hidden}) {
        fallback.mode = mode;
        QVERIFY(cursor.receive(packetFor(fallback, 4 + quint32(mode), false)));
        QVERIFY(!cursor.separate());
        QVERIFY(cursor.rectangle(true).isEmpty());
        QVERIFY(cursor.hidesLocalPointer(true, true, true));
        // The next separate packet may use the same acknowledged bitmap.
        QVERIFY(cursor.receive(packetFor(state, 8 + quint32(mode), false)));
        cursor = {};
        QVERIFY(cursor.receive(packetFor(state, 1)));
    }
    cursor = {};
    QVERIFY(!cursor.hidesLocalPointer(true, true, true));
}

void CursorProbe::queueCoalescesAndWaitsForAcknowledgment() {
    evp::CursorOutbox outbox;
    auto state = arrow();
    for (unsigned i = 1; i <= evp::cursorWindow; ++i) {
        outbox.observe(7, state);
        QVERIFY(outbox.schedule());
        QVERIFY(!outbox.schedule());
        const auto sent = outbox.take(7);
        QVERIFY(sent.has_value());
        QCOMPARE(sent->sequence, i);
        QCOMPARE(sent->includeShape, i == 1);
    }
    for (int i = 0; i < 40; ++i) {
        state.topLeft = {i, i};
        outbox.observe(7, state);
        QVERIFY(!outbox.schedule());
    }
    QVERIFY(!outbox.acknowledge(evp::cursorWindow + 1));
    QVERIFY(outbox.acknowledge(1));
    QVERIFY(outbox.schedule());
    QCOMPARE(outbox.take(7)->state.topLeft, QPoint(39, 39));
    QVERIFY(outbox.acknowledge(evp::cursorWindow));
    state.image.fill(Qt::blue);
    outbox.observe(7, state);
    QVERIFY(outbox.schedule());
    const auto shape = outbox.take(7);
    QVERIFY(shape->includeShape);
    state.image.fill(Qt::red);
    outbox.observe(7, state);
    QVERIFY(!outbox.schedule());
    outbox.reset();
    outbox.observe(8, state);
    QVERIFY(!outbox.schedule());
    QVERIFY(outbox.acknowledge(shape->sequence));
    QVERIFY(outbox.schedule());
    QVERIFY(outbox.take(8)->includeShape);
}

void CursorProbe::d3dOverlayBlendsAndClips() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    evp::CursorRenderer renderer;
    QVERIFY(renderer.initialize(device.Get()));

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 20;
    desc.Height = 12;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11RenderTargetView> target;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &output)));
    QVERIFY(SUCCEEDED(device->CreateRenderTargetView(output.Get(), nullptr, &target)));
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> sampler;
    QVERIFY(SUCCEEDED(device->CreateSamplerState(&samplerDesc, &sampler)));

    constexpr float background[]{0, 0, 1, 1};
    context->ClearRenderTargetView(target.Get(), background);
    context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderer.draw(device.Get(), context.Get(), sampler.Get(), arrow().image, {-0.25, 0.25, 0.5, 0.5}, {2, 2, 16, 8});
    context->OMSetRenderTargets(0, nullptr, nullptr);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> readback;
    QVERIFY(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)));
    context->CopyResource(readback.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapping{};
    QVERIFY(SUCCEEDED(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
    const auto* row = reinterpret_cast<const QRgb*>(static_cast<const uchar*>(mapping.pData) + 5 * mapping.RowPitch);
    const QRgb outsideContent = row[1];
    const QRgb insideCursor = row[2];
    const QRgb outsideCursor = row[6];
    context->Unmap(readback.Get(), 0);
    QCOMPARE(outsideContent, qRgb(0, 0, 255));
    QCOMPARE(insideCursor, qRgb(128, 0, 127));
    QCOMPARE(outsideCursor, qRgb(0, 0, 255));
}

} // namespace

QTEST_APPLESS_MAIN(CursorProbe)
#include "cursor_probe.moc"
