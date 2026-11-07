#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["Pillow>=10,<13"]
# ///
"""Overlay Blender orthographic contours on the supplied three-view reference."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


VIEWS = {
    "front": (70, 100, 732, 515),
    "rear": (790, 120, 1462, 525),
    "side": (45, 535, 1438, 955),
}

OBJECT_BOUNDS = {
    "front": (16, 17, 646, 395),
    "rear": (17, 18, 656, 388),
    "side": (18, 15, 1375, 395),
}


def alpha_bounds(image: Image.Image) -> tuple[int, int, int, int]:
    alpha = image.getchannel("A")
    bounds = alpha.getbbox()
    if bounds is None:
        raise RuntimeError("render has no visible pixels")
    return bounds


def silhouette_edge(alpha: Image.Image) -> Image.Image:
    outer = alpha.filter(ImageFilter.MaxFilter(7))
    inner = alpha.filter(ImageFilter.MinFilter(5))
    difference=Image.new("L",alpha.size)
    outer_data = (outer.get_flattened_data() if hasattr(outer,"get_flattened_data")
                  else outer.getdata())
    inner_data = (inner.get_flattened_data() if hasattr(inner,"get_flattened_data")
                  else inner.getdata())
    difference.putdata(tuple(
        max(0,a-b)
        for a,b in zip(outer_data,inner_data)
    ))
    return Image.eval(difference,lambda value:255 if value>18 else 0)


def overlay_view(reference: Image.Image, render_path: Path, name: str) -> tuple[Image.Image, float]:
    crop_box = VIEWS[name]
    panel = reference.crop(crop_box).convert("RGB")
    model = Image.open(render_path).convert("RGBA")
    bounds = alpha_bounds(model)
    model = model.crop(bounds)

    # Match horizontal extent only.  Any height disagreement remains visible
    # and is reported numerically instead of being hidden by anisotropic scaling.
    object_bounds=OBJECT_BOUNDS[name]
    reference_width=object_bounds[2]-object_bounds[0]
    scale = reference_width/model.width
    resized = model.resize((reference_width, round(model.height*scale)), Image.Resampling.LANCZOS)
    alpha = resized.getchannel("A")
    edge = silhouette_edge(alpha)
    expected_height=object_bounds[3]-object_bounds[1]
    height_ratio = resized.height/expected_height

    x=(object_bounds[0]+object_bounds[2]-resized.width)//2
    y=object_bounds[3]-resized.height
    cyan = Image.new("RGBA",resized.size,(0,224,255,0))
    cyan.putalpha(edge)
    panel_rgba = panel.convert("RGBA")
    panel_rgba.alpha_composite(cyan,(x,y))

    draw=ImageDraw.Draw(panel_rgba)
    draw.rectangle((0,0,panel.width-1,panel.height-1),outline=(20,130,165,255),width=2)
    draw.rectangle((8,8,260,34),fill=(0,0,0,190))
    draw.text((15,13),f"{name.upper()}  cyan=model contour",fill=(225,245,255,255))
    return panel_rgba.convert("RGB"),height_ratio


def main() -> None:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference",type=Path,required=True)
    parser.add_argument("--render-dir",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    reference=Image.open(args.reference).convert("RGB")
    panels=[]
    ratios={}
    for name in ("front","rear","side"):
        panel,ratio=overlay_view(reference,args.render_dir/f"{name}.png",name)
        panels.append(panel)
        ratios[name]=ratio
    gap=14
    width=sum(panel.width for panel in panels)+gap*(len(panels)-1)
    height=max(panel.height for panel in panels)+44
    sheet=Image.new("RGB",(width,height),(7,12,20))
    x=0
    for panel in panels:
        sheet.paste(panel,(x,44))
        x+=panel.width+gap
    draw=ImageDraw.Draw(sheet)
    draw.text((12,13),"REFERENCE-CONSTRAINED ORTHOGRAPHIC PROFILE CHECK",fill=(225,240,255))
    draw.text((570,13),"height after width match: "+", ".join(
        f"{name} {ratio:.3f}x" for name,ratio in ratios.items()),fill=(130,225,255))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    sheet.save(args.output)
    print("profile ratios:"," ".join(f"{name}={ratio:.4f}" for name,ratio in ratios.items()))
    print(f"wrote {args.output}")


if __name__=="__main__":
    main()
