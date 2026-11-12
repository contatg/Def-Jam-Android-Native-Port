"""Synthetic safety/identity/image checks; no game bytes or downloaded models."""
import hashlib
import importlib.util
from pathlib import Path
import struct
from types import SimpleNamespace

import pytest

spec = importlib.util.spec_from_file_location("hd_assets", Path(__file__).parents[2] / "scripts/hd_assets.py")
hd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hd)


def test_identity_covers_descriptor_pixels_palette_and_pitch():
    raw, palette = bytes(range(128)), bytes(range(16))
    identity = hd.texture_id(11, 16, 8, raw, palette)
    assert identity == hashlib.sha256(b"XRTEX01\0" + struct.pack("<IIIII", 11, 16, 8, 4, 16) + raw + palette).hexdigest()
    for changed in (hd.texture_id(11, 16, 8, raw[:-1] + b"X", palette),
                    hd.texture_id(11, 16, 8, raw, palette[:-1] + b"X"),
                    hd.texture_id(11, 16, 8, raw, palette, 32),
                    hd.texture_id(12, 16, 8, raw, palette)):
        assert identity != changed


def test_refpack_limits_and_backreferences():
    assert hd.unpack_refpack(b"\x10\xfb\0\0\x04\xe0abcd\xfc") == b"abcd"
    for blob in (b"\x10\xfb\0\0\x03\xe0abcd", b"\x10\xfb\0\0\x03\0\0\xfc",
                 b"\x10\xfb\0\0\x05\xe0abcd\xfc", b"\x90\xfb\xff\xff\xff\xff"):
        with pytest.raises((ValueError, IndexError)):
            hd.unpack_refpack(blob)


def test_rectangular_morton_layout():
    # Explicit 4x2 and 2x4 layouts independently check the rectangular bit mask.
    assert hd.unswizzle(bytes(range(8)), 4, 2, 1) == bytes([0, 1, 4, 5, 2, 3, 6, 7])
    assert hd.unswizzle(bytes(range(8)), 2, 4, 1) == bytes([0, 1, 2, 3, 4, 5, 6, 7])
    with pytest.raises(ValueError):
        hd.unswizzle(b"x", 4, 2, 1)


def test_big_entry_bounds():
    blob = b"BIG4" + struct.pack(">III", 34, 1, 30) + struct.pack(">II", 30, 4) + b"name\0x" + b"DATA"
    assert list(hd.archive_entries(blob)) == [("name", b"DATA")]
    with pytest.raises(ValueError):
        list(hd.archive_entries(blob[:-1]))


def test_category_uses_container_provenance():
    assert hd.category("assets/v2ip.viv::V2IP_003A.xsh")=="fighters"
    assert hd.category("assets/v2cp_0.viv::V2CP_A_A.xsh")=="customization"
    assert hd.category("assets/cincrowd.viv::crowdshapes.viv::V2CM_001.xsh")=="crowd"
    assert hd.category("assets/decalfx.viv::decal_V2BG_07A.xsh")=="effects"
    assert hd.category("screens/screens.viv::V2IP_003A.xsh")=="ui"


def test_padding_and_transparent_colour_preserve_alpha():
    Image = pytest.importorskip("PIL.Image")
    image = Image.new("RGBA", (2, 2))
    image.putdata([(200, 30, 20, 255), (0, 0, 0, 0), (0, 0, 0, 0), (10, 20, 30, 64)])
    spread = hd.bleed_rgb(image)
    assert spread.getchannel("A").tobytes() == image.getchannel("A").tobytes()
    assert spread.getpixel((1, 0))[:3] == (200, 30, 20)
    edge, wrap = hd.padded(image, 1), hd.padded(image, 1, True, True)
    assert edge.getpixel((0, 0)) == image.getpixel((0, 0))
    assert wrap.getpixel((0, 0)) == image.getpixel((1, 1))
    assert hd.padded(image, 1, True, False).getpixel((0, 0)) == image.getpixel((1, 0))


def test_palette_validation_and_alpha():
    pytest.importorskip("PIL.Image")
    header = bytearray(60)
    header[:4] = b"SHPX"
    struct.pack_into("<II", header, 4, 60, 1)
    struct.pack_into("<4sI", header, 16, b"test", 24)
    header[24] = 0x7b
    header[25:28] = (18).to_bytes(3, "little")
    struct.pack_into("<HH", header, 28, 2, 1)
    header[40:42] = b"\0\0"
    header[42] = 0x2a
    struct.pack_into("<H", header, 46, 1)
    header.extend(b"\x0a\x14")  # Extend palette bytes, then correct total size.
    header[58:62] = b"\x0a\x14\x1e\x40"
    struct.pack_into("<I", header, 4, len(header))
    record, image = next(hd.shpx_images(bytes(header)))
    assert "error" not in record
    assert image.getpixel((0, 0)) == (30, 20, 10, 64)
    header[40] = 2
    record, image = next(hd.shpx_images(bytes(header)))
    assert image is None and "Palette index" in record["error"]


def test_pack_resume_checks_source_and_output(tmp_path):
    Image = pytest.importorskip("PIL.Image")
    original = tmp_path / "original"; original.mkdir()
    ident = "a" * 64
    Image.new("RGBA", (2, 3), (200, 40, 30, 50)).save(original / (ident + ".png"))
    args = SimpleNamespace(input=original, output=tmp_path / "pack", tool=None, model="lanczos",
                           scale=4, padding=8, bleed=True, wrap_u=False, wrap_v=False, limit=20)
    hd.upscale(args)
    destination = args.output / "textures" / (ident + ".png")
    first = destination.read_bytes()
    hd.upscale(args)
    assert destination.read_bytes() == first
    destination.write_bytes(b"corrupt")
    hd.upscale(args)
    assert destination.read_bytes() == first
    assert Image.open(destination).size == (8, 12)


def test_batch_review_default_and_explicit_override(tmp_path):
    import json
    Image = pytest.importorskip("PIL.Image")
    runtime=tmp_path/"runtime"; runtime.mkdir()
    ident="b"*64
    Image.new("RGBA",(2,2),(200,30,20,64)).save(runtime/(ident+".png"))
    (runtime/"textures.jsonl").write_text(json.dumps(dict(id=ident))+"\n")
    recipes=tmp_path/"recipes.json"; output=tmp_path/"pack"
    recipes.write_text(json.dumps(dict(schema=1)))
    args=SimpleNamespace(runtime=runtime,output=output,recipes=recipes,inventory=None,tool=None)
    hd.batch(args)
    assert ident not in (output/"manifest.ini").read_text()
    assert json.loads((output/"review.json").read_text())[0]["id"]==ident
    recipes.write_text(json.dumps(dict(schema=1,png_mips="channels",overrides={ident:dict(approved=True,scale=2)})))
    hd.batch(args)
    assert ident in (output/"manifest.ini").read_text()
    assert "png_mips=channels" in (output/"manifest.ini").read_text()
    outputs=list((output/"jobs").glob("*/textures/*.png"))
    assert len(outputs)==1 and Image.open(outputs[0]).size==(4,4)
    previous=(output/"manifest.ini").read_bytes()
    recipes.write_text(json.dumps(dict(schema=1,png_mips="invalid")))
    with pytest.raises(ValueError,match="PNG mip policy"):
        hd.batch(args)
    assert (output/"manifest.ini").read_bytes()==previous


def test_aux_export_uses_hash_names_and_deduplicates(tmp_path):
    import json
    root=tmp_path/"source";root.mkdir()
    raw=b"MADk"+b"synthetic payload"
    (root/"clip.mad").write_bytes(raw)
    (root/"alias.mad").write_bytes(raw)
    args=SimpleNamespace(root=root,output=tmp_path/"output",kind="video",select=[],limit=3)
    hd.export_aux(args)
    manifest=json.loads((args.output/"exports.json").read_text())
    assert manifest["unique"]==1 and len(manifest["records"])==2
    files=list(args.output.glob("*.mad"))
    assert files[0].name==hd.digest(raw)+".mad" and files[0].read_bytes()==raw


def test_exact_selection_decodes_only_requested_image_and_rejects_escape(tmp_path):
    import json
    pytest.importorskip("PIL.Image")
    root=tmp_path/"source"; root.mkdir()
    blob=bytearray(72); blob[:4]=b"SHPX"
    struct.pack_into("<II",blob,4,len(blob),2)
    struct.pack_into("<4sI4sI",blob,16,b"bad!",32,b"good",52)
    blob[32]=0x65  # Unselected unsupported image must not be decoded.
    blob[52]=0x7d;struct.pack_into("<HH",blob,56,1,1)
    blob[68:72]=bytes((10,20,30,64))
    (root/"test.xsh").write_bytes(blob)
    selection=tmp_path/"selection.json"
    selection.write_text(json.dumps(dict(schema=1,assets=[dict(source="test.xsh",index=1,label="test",category="UI") ])))
    args=SimpleNamespace(root=root,output=tmp_path/"output",selection=selection)
    hd.select_assets(args)
    records=json.loads((args.output/"selection.json").read_text())["assets"]
    assert len(records)==1 and records[0]["sample"]=="S01"
    assert records[0]["alpha_extrema"]==[64,64]
    assert records[0]["index"]==1 and (args.output/records[0]["original"]).exists()
    (tmp_path/"outside.xsh").write_bytes(blob)
    selection.write_text(json.dumps(dict(schema=1,assets=[dict(source="../outside.xsh",index=1)])))
    with pytest.raises(ValueError,match="source container"):
        hd.select_assets(args)


def test_selected_walk_skips_unrelated_compressed_members():
    first=b"DATA";bad=b"\x10\xfb\0\0\x04\xe0x"  # Truncated unrelated RefPack.
    directory=struct.pack(">II",50,len(first))+b"keep.xsh\0"+struct.pack(">II",54,len(bad))+b"skip.xsh\0"
    blob=b"BIG4"+struct.pack(">III",50+len(first)+len(bad),2,50)+directory+first+bad
    assert list(hd.walk(blob,"root.viv",selected={"root.viv::keep.xsh"}))==[("root.viv::keep.xsh",first)]
    with pytest.raises((ValueError,IndexError)):
        list(hd.walk(blob,"root.viv"))
