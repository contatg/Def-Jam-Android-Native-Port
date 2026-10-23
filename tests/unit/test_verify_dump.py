"""Unit tests for scripts/verify-dump.py using synthetic data only (no game files)."""
import hashlib, importlib.util, json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
spec = importlib.util.spec_from_file_location("verify_dump", os.path.join(ROOT, "scripts", "verify-dump.py"))
vd = importlib.util.module_from_spec(spec); spec.loader.exec_module(vd)

def _manifest(data: bytes, count=3):
    return {"title": "T", "title_id_catalog": "X", "xbe": {"filename": "default.xbe", "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest()}, "required_paths": ["default.xbe", "assets"],
            "expected_file_count": count}

def _make(tmp_path, data=b"fake-xbe", extra=2):
    (tmp_path / "default.xbe").write_bytes(data)
    (tmp_path / "assets").mkdir()
    for i in range(extra):
        (tmp_path / "assets" / f"f{i}.viv").write_bytes(b"BIGF")
    return tmp_path

def test_ok(tmp_path):
    d = _make(tmp_path)
    assert vd.verify(str(d), _manifest(b"fake-xbe")) == []

def test_hash_mismatch(tmp_path):
    d = _make(tmp_path, data=b"other-xbe")
    probs = vd.verify(str(d), _manifest(b"fake-xbe"))
    assert any("sha256" in p for p in probs) and any("size" in p for p in probs)

def test_missing_dir(tmp_path):
    (tmp_path / "default.xbe").write_bytes(b"fake-xbe")
    probs = vd.verify(str(tmp_path), _manifest(b"fake-xbe", count=1))
    assert probs == ["missing assets"]

def test_manifest_file_is_valid_json():
    with open(os.path.join(ROOT, "config", "dump-manifest.json"), encoding="utf-8") as f:
        m = json.load(f)
    assert len(m["xbe"]["sha256"]) == 64 and m["xbe"]["size"] > 0
