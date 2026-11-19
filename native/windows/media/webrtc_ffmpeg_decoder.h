#pragma once

struct ID3D11Device;
struct webrtc_VideoDecoderFactory_unique;
struct webrtc_VideoFrameBuffer;

namespace evp {

class DecodedVideoFrame;

// The factory retains the device. Pass nullptr to decode on the CPU.
webrtc_VideoDecoderFactory_unique* makeFfmpegDecoderFactory(ID3D11Device* device);

// The result is valid while its WebRTC frame buffer is retained.
const DecodedVideoFrame* getFfmpegDecodedFrame(const webrtc_VideoFrameBuffer* buffer);

}  // namespace evp
