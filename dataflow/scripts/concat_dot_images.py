#!/usr/bin/env python3
import os
import sys
import subprocess
from PIL import Image

def main(subdir, prefix):
    # Find all .dot files with the given prefix in the subdirectory
    dot_files = [f for f in os.listdir(subdir) if f.startswith(prefix) and f.endswith('.dot')]
    dot_files.sort()

    png_files = []

    for dot_file in dot_files:
        dot_path = os.path.join(subdir, dot_file)
        png_file = dot_file[:-4] + '.png'  # Replace .dot with .png
        png_path = os.path.join(subdir, png_file)
        # Render .dot file to PNG using Graphviz
        subprocess.run(['dot', '-Tpng', dot_path, '-o', png_path], check=True)
        png_files.append(png_path)

    # Open images and calculate total dimensions
    images = [Image.open(png_file) for png_file in png_files]
    widths, heights = zip(*(img.size for img in images))
    total_height = sum(heights)
    max_width = max(widths)

    # Create a new image with the total dimensions
    result = Image.new('RGB', (max_width, total_height))

    # Paste images one below the other
    y_offset = 0
    for img in images:
        result.paste(img, (0, y_offset))
        y_offset += img.size[1]

    output_image = os.path.join(subdir, f"{prefix}_merged.png")
    result.save(output_image)

    print(f"Merged image saved to {output_image}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python concat_dot_images.py <subdir> <prefix>")
        sys.exit(1)

    subdir = sys.argv[1]
    prefix = sys.argv[2]
    main(subdir, prefix)
