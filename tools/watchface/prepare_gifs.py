"""Precompose and prescale user GIFs for the watch; never modify source files.

Each source frame is retained, including visually identical frames and its exact
centisecond duration. The watch no longer scales the transparent canvas at run time.
"""
from pathlib import Path
import hashlib
import json
import numpy as np
from PIL import Image, ImageSequence, GifImagePlugin

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT.parent / '手表素材'
DEST = ROOT / 'spiffs/watchface'
COLORS = 128


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    DEST.mkdir(parents=True, exist_ok=True)
    files = sorted(SOURCE.glob('*.gif'))
    assert len(files) == 11
    records = []
    for number, source in enumerate(files, 1):
        original_hash = sha(source)
        image = Image.open(source)
        rgba = []
        delays = []
        box = None
        for frame in ImageSequence.Iterator(image):
            pixels = frame.convert('RGBA')
            alpha = pixels.getchannel('A')
            assert set(alpha.get_flattened_data()) <= {0, 255}
            part = alpha.getbbox()
            if part:
                box = part if box is None else (min(box[0], part[0]), min(box[1], part[1]),
                                               max(box[2], part[2]), max(box[3], part[3]))
            rgba.append(pixels)
            delays.append(frame.info['duration'])
        # Match the old visual size, but crop away the invisible source margin.
        scale = round(260 / max(box[2] - box[0], box[3] - box[1]) * 256)
        size = (round((box[2] - box[0]) * scale / 256),
                round((box[3] - box[1]) * scale / 256))
        rgba = [frame.crop(box).resize(size, Image.Resampling.NEAREST) for frame in rgba]
        atlas = Image.new('RGB', (size[0], size[1] * len(rgba)))
        for index, frame in enumerate(rgba):
            atlas.paste(frame.convert('RGB'), (0, size[1] * index))
        palette = atlas.quantize(colors=COLORS - 1, method=Image.Quantize.MEDIANCUT,
                                 dither=Image.Dither.NONE)
        lut = palette.getpalette()[:(COLORS - 1) * 3]
        lut += lut[:3] * (256 - (COLORS - 1))
        palette.putpalette(lut)
        encoded = []
        for frame in rgba:
            indices = np.asarray(frame.convert('RGB').quantize(
                palette=palette, dither=Image.Dither.NONE)).copy()
            indices[indices >= COLORS - 1] = 0  # Extra palette entries duplicate color zero.
            indices[np.asarray(frame)[:, :, 3] == 0] = COLORS - 1
            indexed = Image.fromarray(indices)
            indexed.putpalette(lut[:(COLORS - 1) * 3] + lut[:3])
            indexed.info['transparency'] = COLORS - 1
            encoded.append(indexed)
        target = DEST / f'face{number:02d}.gif'
        # Write each frame explicitly. save_all() would merge identical frames.
        with target.open('wb') as stream:
            header, _ = GifImagePlugin.getheader(encoded[0], info={'transparency': COLORS - 1, 'loop': 0})
            for block in header:
                stream.write(block)
            for frame, delay in zip(encoded, delays):
                for block in GifImagePlugin.getdata(frame, duration=delay, disposal=2,
                                                   transparency=COLORS - 1, include_color_table=False):
                    stream.write(block)
            stream.write(b';')
        result = Image.open(target)
        assert result.n_frames == image.n_frames
        error_sum = color_count = max_error = 0
        for actual, expected, reference, delay in zip(ImageSequence.Iterator(result), encoded, rgba, delays):
            assert actual.info['duration'] == delay
            a = np.asarray(actual.convert('RGBA'))
            e = np.asarray(expected.convert('RGBA'))
            r = np.asarray(reference)
            assert np.array_equal(a, e), (source.name, actual.tell(), 'GIF roundtrip changed pixels')
            assert np.array_equal(a[:, :, 3], r[:, :, 3]), (source.name, actual.tell(), 'alpha changed')
            opaque = r[:, :, 3] != 0
            difference = np.abs(a[:, :, :3].astype(np.int16) - r[:, :, :3])
            error_sum += int(difference[opaque].sum())
            color_count += int(opaque.sum()) * 3
            max_error = max(max_error, int(difference[opaque].max()))
        assert sha(source) == original_hash
        records.append(dict(source=source.name, file=target.name, source_sha256=original_hash,
                            staged_sha256=sha(target), frames=image.n_frames, duration_ms=sum(delays),
                            frame_delays_ms=delays, source_bbox=box, source_scale=scale,
                            width=size[0], height=size[1], scale=256, dx=0, dy=0, palette_colors=COLORS,
                            mean_rgb_error=error_sum / color_count, max_rgb_error=max_error,
                            alpha_and_frame_delays='PASS', original_source_unchanged=True))
        print(f'{target.name}: {size[0]}x{size[1]}, {image.n_frames} frames, {sum(delays)}ms; alpha/timing/source OK')
    out = ROOT / 'output/watchface'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'gif-manifest.json').write_text(json.dumps(records, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    header = ('/* Generated by prepare_gifs.py: cropped/prescaled GIFs, no runtime transform. */\n'
              '#pragma once\ntypedef struct { int16_t scale, dx, dy; } face_animation_t;\n'
              'static const face_animation_t FACE_ANIMATIONS[11] = {\n' +
              '    {256,0,0},\n' * 11 + '};\n')
    (ROOT / 'components/watch_shell/include/watch_face_animations.h').write_text(header, encoding='utf-8')
    print('GIF_TOTAL=', sum((DEST / record['file']).stat().st_size for record in records))


if __name__ == '__main__':
    main()
