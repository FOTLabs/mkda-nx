"""Match captured Switch PCM and sparse YUV fingerprints to retail intro media.

References: ffmpeg -i intro.xmv -map 0:a:0 -f s16le intro-reference.pcm
            ffmpeg -i intro.xmv -map 0:v:0 -vf scale=16:12:flags=neighbor
                   -pix_fmt gray -f rawvideo intro-reference.grid
Run with a Python environment that already provides numpy.
"""
import argparse
from pathlib import Path
import re
import numpy as np


def audio_envelope(path):
    pcm = np.fromfile(path, dtype='<i2').astype(np.float32)
    # 10 ms stereo energy, independent of phase, volume and the output limiter.
    block = 480 * 2
    pcm = pcm[:len(pcm) // block * block].reshape(-1, block)
    return np.sqrt(np.mean(pcm * pcm, axis=1))


def match_audio(reference, capture, start=10.0):
    ref = audio_envelope(reference)
    got = audio_envelope(capture)
    windows = np.lib.stride_tricks.sliding_window_view(ref, 300)
    centered = windows - windows.mean(axis=1, keepdims=True)
    norms = np.linalg.norm(centered, axis=1)
    for pos in range(0, len(got) - 300, 500):
        sample = got[pos:pos + 300]
        sample = sample - sample.mean()
        score = (centered @ sample) / np.maximum(norms * np.linalg.norm(sample), 1e-10)
        best = int(score.argmax())
        if score[best] < 0.85:
            print(f'audio output {start + pos/100:.2f}s: ambiguous '
                  f'(best correlation {score[best]:.3f})')
            continue
        print(f'audio output {start + pos/100:.2f}s -> intro {best/100:.2f}s '
              f'(correlation {score[best]:.3f})')


def match_video(reference, log):
    ref = np.fromfile(reference, dtype=np.uint8).reshape(-1, 192).astype(np.float32)
    # Luma ranges can differ between the decoder and ffmpeg; compare normalized
    # spatial structure. Black frames are deliberately excluded as ambiguous.
    ref -= ref.mean(axis=1, keepdims=True)
    norms = np.linalg.norm(ref, axis=1)
    ref /= np.maximum(norms[:, None], 1e-10)
    for line in Path(log).read_text(errors='replace').splitlines():
        m = re.search(r'\[\s*([\d.]+)\].*\[AV-TIMING\] movie 720x480 luma ([a-f0-9]+)', line)
        if not m:
            continue
        sample = np.frombuffer(bytes.fromhex(m[2]), dtype=np.uint8).astype(np.float32)
        sample -= sample.mean()
        norm = np.linalg.norm(sample)
        if norm < 80:
            continue
        scores = ref @ (sample / norm)
        best = int(scores.argmax())
        if scores[best] < 0.95:
            continue
        print(f'video log {float(m[1]):.3f}s -> intro {best/30:.3f}s '
              f'(correlation {scores[best]:.3f})')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--audio')
    p.add_argument('--log')
    p.add_argument('--reference', default='logs/intro-reference')
    p.add_argument('--dump-start', type=float, default=10)
    args = p.parse_args()
    if args.audio:
        match_audio(args.reference + '.pcm', args.audio, args.dump_start)
    if args.log:
        match_video(args.reference + '.grid', args.log)
