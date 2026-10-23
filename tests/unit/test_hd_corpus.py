"""Synthetic reference/alpha/resume checks for full-corpus generation; no game art."""
from pathlib import Path
import struct
import sys

import pytest

pytest.importorskip("numpy")
pytest.importorskip("PIL")
scripts = Path(__file__).parents[2] / "scripts"
sys.path.insert(0, str(scripts))
import hd_corpus as corpus


@pytest.mark.parametrize("size", [(1, 8), (8, 1), (4, 2), (16, 32)])
def test_vector_unswizzle_matches_existing_reference(size):
    w, h = size
    for bpp in (1, 4):
        raw = bytes((v*71+11) % 256 for v in range(w*h*bpp))
        assert corpus.fast_unswizzle(raw, w, h, bpp) == corpus.hd.unswizzle(raw, w, h, bpp)


def test_loader_is_ui_but_shop_character_and_environment_keep_their_roles():
    assert corpus.classify("assets/shops.viv::loader.xsh") == "Loading emblems"
    assert corpus.classify("assets/shops.viv::V2IP_099A.xsh") == "Fighters"
    assert corpus.classify("assets/shops.viv::V_ENV01A.xsh") == "Environment maps"
    assert corpus.classify("assets/v2cp_1.viv::V2CP_001A.xsh") == "Customization"
    assert corpus.classify("screens/screens.viv::V2IP_preview.xsh") == "UI and menus"


def test_loader_bleeds_rgb_without_changing_source_or_alpha_and_repairs_corrupt_resume(tmp_path, monkeypatch):
    from PIL import Image
    # Hidden red next to opaque blue makes opacity-contaminated colour obvious.
    original = Image.new("RGBA", (2, 1))
    original.putdata([(255, 0, 0, 0), (0, 0, 255, 255)])
    header = bytearray(16)
    header[0] = 0x7d
    struct.pack_into("<HH", header, 4, 2, 1)
    blob = b"SHPX" + struct.pack("<II", 48, 1) + bytes(4) + b"test" + struct.pack("<I", 24) + header + original.tobytes("raw", "BGRA")
    request = next(corpus.hd.shpx_images(blob, decode=False))[0]
    request["source"] = "assets/shops.viv::loader.xsh"
    for directory in ("originals", "textures", "thumbs", "records"):
        (tmp_path / directory).mkdir()
    # Preserve the slow reference helpers across the worker's local overrides.
    slow_unswizzle, slow_padded = corpus.hd.unswizzle, corpus.hd.padded
    monkeypatch.setattr(corpus.hd, "unswizzle", slow_unswizzle)
    monkeypatch.setattr(corpus.hd, "padded", slow_padded)
    try:
        first = corpus.process_leaf(blob, [request], str(tmp_path), {"id":"synthetic"})[0]
        source = Image.open(tmp_path / first["original"]).convert("RGBA")
        result = Image.open(tmp_path / first["upscaled"]).convert("RGBA")
        assert source.tobytes() == original.tobytes()
        assert result.size == (8, 4)
        assert result.convert("RGB").getextrema() == ((0, 0), (0, 0), (255, 255))
        expected_alpha = slow_padded(original.getchannel("A"), 8).resize((72, 68), Image.Resampling.LANCZOS).crop((32, 32, 40, 36))
        assert result.getchannel("A").tobytes() == expected_alpha.tobytes()
        (tmp_path / first["upscaled"]).write_bytes(b"corrupted cache")
        repaired = corpus.process_leaf(blob, [request], str(tmp_path), {"id":"synthetic"})[0]
        assert repaired["upscaled_hash"] == first["upscaled_hash"]
        assert Image.open(tmp_path / repaired["upscaled"]).size == (8, 4)
        (tmp_path / "records" / (first["id"] + ".json")).write_text('{"recipe_id":')
        resumed = corpus.process_leaf(blob, [request], str(tmp_path), {"id":"synthetic"})[0]
        assert resumed["upscaled_hash"] == first["upscaled_hash"]
        assert not list((tmp_path / "records").glob("*.pending"))
    finally:
        corpus.hd.unswizzle, corpus.hd.padded = slow_unswizzle, slow_padded
