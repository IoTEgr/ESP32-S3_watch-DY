"""Prepare independent fully-composited BGRA hashes for the native GIF regression test."""
from pathlib import Path
from PIL import Image, ImageSequence
ROOT = Path(__file__).resolve().parents[2]

def main():
    lines = []
    total = 0
    for number in range(1, 12):
        image = Image.open(ROOT / f'spiffs/watchface/face{number:02}.gif')
        lines.append(f'{number} {image.n_frames}')
        for frame in ImageSequence.Iterator(image):
            value = 2166136261
            for r, g, b, a in frame.convert('RGBA').get_flattened_data():
                for byte in ((b, g, r, 255) if a else (0, 0, 0, 0)):
                    value = ((value ^ byte) * 16777619) & 0xffffffff
            lines.append(f'{frame.info["duration"]} {value}')
            total += 1
    out = ROOT / 'output/watchface/gif-expected.txt'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text('\n'.join(lines) + '\n', encoding='ascii')
    print(f'Independent GIF expected frames: {total}')

if __name__ == '__main__':
    main()
