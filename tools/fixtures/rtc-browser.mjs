// This fixture runs in each browser. Only the host substitutes capture devices.
// The peers, codecs, ICE, signaling and application event handlers are real.
export function installFixture({ host, unreachable }) {
  const diagnostics = window.smoke = { peers: [], filtered: 0 };
  function candidateForTest(line) {
    if (!unreachable || !/^(?:a=)?candidate:/i.test(line) || /\btyp relay\b/i.test(line)) return line;
    diagnostics.filtered++;
    const fields = line.split(' ');
    if (fields[2]?.toLowerCase() !== 'udp') return '';
    fields[4] = unreachable.address;
    fields[5] = String(unreachable.port);
    return fields.join(' ');
  }
  const Peer = window.RTCPeerConnection;
  window.RTCPeerConnection = class extends Peer {
    constructor(config) {
      super(config);
      diagnostics.peers.push(this);
    }
    async setRemoteDescription(description) {
      if (unreachable && description?.sdp) {
        const lines = description.sdp.split('\r\n');
        const mapped = lines.flatMap(line => {
          const candidate = candidateForTest(line);
          return candidate || !line ? [candidate] : [];
        });
        description = { type: description.type, sdp: mapped.join('\r\n') };
      }
      return super.setRemoteDescription(description);
    }
    async addIceCandidate(candidate) {
      if (unreachable && candidate?.candidate) {
        const line = candidateForTest(candidate.candidate);
        if (!line) return;
        candidate = { ...candidate, candidate: line };
      }
      return super.addIceCandidate(candidate);
    }
  };
  if (!host) return;
  navigator.mediaDevices.getDisplayMedia = async () => {
    const canvas = document.createElement('canvas');
    canvas.width = 640;
    canvas.height = 360;
    const context = canvas.getContext('2d');
    let count = 0;
    function draw() {
      count++;
      context.fillStyle = count % 2 ? '#42609c' : '#407859';
      context.fillRect(0, 0, canvas.width, canvas.height);
      context.fillStyle = 'white';
      context.font = '32px sans-serif';
      context.fillText(String(count), 16, 48);
    }
    draw();
    const stream = canvas.captureStream(15);
    const video = stream.getVideoTracks()[0];
    const settings = video.getSettings.bind(video);
    video.getSettings = () => ({ ...settings(), displaySurface: 'monitor' });
    const timer = setInterval(draw, 65);
    const audio = new AudioContext();
    await audio.resume();
    const tone = audio.createOscillator();
    const output = audio.createMediaStreamDestination();
    tone.frequency.value = 440;
    tone.connect(output);
    tone.start();
    stream.addTrack(output.stream.getAudioTracks()[0]);
    diagnostics.fixture = { stream, audio, tone, canvas, timer };
    return stream;
  };
}
