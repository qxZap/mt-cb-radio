"""CB Radio sound preview: the same chain the plugin runs, on Kokoro lines, at three signal strengths.

python preview.py [outdir]  -> cb_preview.ogg (S9 nearby, S5 across town, S1 edge of range) + one wav per take.
The chain (mirrored in plugins/cb_radio.cpp): 8 kHz mono, 300-2800 Hz band, hard AM-style compression,
then by signal s (1 strong .. 0 gone): hiss, crackle, slow fading, dropouts. Key-up kerchunk, roger beep,
squelch tail on release.
"""
import os, sys, subprocess
import numpy as np
from scipy.signal import butter, lfilter, resample_poly

SR = 8000
rng = np.random.default_rng(19)
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
MODEL = os.path.expanduser(r"~/.cache/hyperframes/tts/models/kokoro-v1.0.onnx")
VOICES = os.path.expanduser(r"~/.cache/hyperframes/tts/voices/voices-v1.0.bin")


def tts(text, voice):
    from kokoro_onnx import Kokoro
    k = getattr(tts, "k", None) or Kokoro(MODEL, VOICES); tts.k = k
    a, sr = k.create(text, voice=voice, speed=1.05, lang="en-us")
    return resample_poly(a, SR, sr).astype(np.float32)


BP = butter(4, [300 / (SR / 2), 2800 / (SR / 2)], btype="band")
HISS = butter(2, [400 / (SR / 2), 3400 / (SR / 2)], btype="band")


def radio(x, s):
    """Voice through the rig at signal s (0..1)."""
    x = lfilter(*BP, x)
    x = x / (np.max(np.abs(x)) + 1e-9)
    x = np.tanh(3.2 * x) / np.tanh(3.2)          # the mic preamp slammed, like every CB
    n = len(x)
    t = np.arange(n) / SR
    fade = 1 - (1 - s) * 0.55 * (0.5 + 0.5 * np.sin(2 * np.pi * (0.7 + 1.3 * rng.random()) * t + rng.random() * 6))
    hiss = lfilter(*HISS, rng.standard_normal(n)) * (0.03 + 0.32 * (1 - s) ** 1.6)
    crack = np.zeros(n)
    for i in np.nonzero(rng.random(n) < (0.0004 + 0.006 * (1 - s) ** 2))[0]:
        k = min(n, i + int(SR * 0.004)); crack[i:k] += rng.standard_normal(k - i) * (0.3 + 0.7 * rng.random())
    y = x * fade * (0.35 + 0.65 * s) + hiss + crack * (1 - s * 0.7)
    if s < 0.35:                                  # the squelch chatters open and shut
        hop = int(SR * 0.05); gate = np.ones(n)
        for i in range(0, n, hop):
            if rng.random() < (0.35 - s) * 1.4: gate[i:i + hop] = 0.06
        y *= lfilter([0.15], [1, -0.85], gate)
    return y


def tone(f, ms, amp=0.4):
    t = np.arange(int(SR * ms / 1000)) / SR
    env = np.minimum(1, np.minimum(t, t[-1] - t) / 0.004)
    return amp * env * np.sin(2 * np.pi * f * t)


def squelch(ms, s):
    n = int(SR * ms / 1000)
    return lfilter(*HISS, rng.standard_normal(n)) * (0.35 + 0.25 * (1 - s)) * np.linspace(1, 0.2, n)


def over(line, voice, s, roger=True):
    v = radio(np.concatenate([np.zeros(int(SR * 0.06)), tts(line, voice), np.zeros(int(SR * 0.1))]), s)
    parts = [squelch(35, s) * 0.8, v]
    if roger:
        parts.append(np.concatenate([tone(1200, 70), tone(900, 90)]) * (0.5 + 0.5 * s))
    parts += [squelch(170, s), np.zeros(int(SR * 0.5))]
    return np.concatenate(parts)


def channel_beep(ch):
    return np.concatenate([tone(1750, 45, 0.3), np.zeros(int(SR * 0.04)), tone(1750 if ch != 9 else 2400, 45, 0.3), np.zeros(int(SR * 0.4))])


TAKES = [
    ("S9 nearby", 1.0, "Breaker one nine, this is Night Owl, eastbound on the coast road. How's it looking up by the bridge?", "am_adam"),
    ("S5 across town", 0.55, "Copy that Night Owl. Bear's parked at the bridge with the radar out, keep it slow. Over.", "af_bella"),
    ("S1 edge of range", 0.18, "Night Owl, you're breaking up, say again your twenty?", "am_michael"),
]


def main():
    os.makedirs(OUT, exist_ok=True)
    import wave
    def save(name, y):
        y = np.clip(y, -1, 1); p = os.path.join(OUT, name)
        with wave.open(p, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes((y * 32000).astype("<i2").tobytes())
        return p
    allp = [channel_beep(19)]
    for label, s, line, voice in TAKES:
        y = over(line, voice, s); allp.append(y)
        save(label.split()[0] + ".wav", y)
    p = save("cb_preview.wav", np.concatenate(allp))
    ogg = p[:-4] + ".ogg"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", p, "-ar", "48000", "-c:a", "libopus", "-b:a", "48k", ogg], check=True)
    print(ogg)


def demo():
    y = radio(np.sin(np.arange(SR) / SR * 2 * np.pi * 1000).astype(np.float32), 1.0)
    assert np.max(np.abs(y)) < 2 and len(y) == SR
    lo = radio(np.zeros(SR, np.float32), 0.1); hi = radio(np.zeros(SR, np.float32), 1.0)
    assert np.std(lo) > 3 * np.std(hi), (np.std(lo), np.std(hi))   # weaker signal, more noise
    print("demo ok")


if __name__ == "__main__":
    demo() if sys.argv[1:] == ["demo"] else main()
