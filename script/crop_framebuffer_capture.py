#!/usr/bin/env python3
"""Crop a known framebuffer from the bottom of a macOS window capture."""
import argparse
from PIL import Image

parser = argparse.ArgumentParser()
parser.add_argument("source")
parser.add_argument("output")
parser.add_argument("--width", required=True, type=int)
parser.add_argument("--height", required=True, type=int)
args = parser.parse_args()

with Image.open(args.source) as image:
    if image.width != args.width or image.height < args.height:
        raise SystemExit(f"capture {image.width}x{image.height} cannot prove requested framebuffer {args.width}x{args.height}")
    image.crop((0, image.height - args.height, args.width, image.height)).save(args.output)
