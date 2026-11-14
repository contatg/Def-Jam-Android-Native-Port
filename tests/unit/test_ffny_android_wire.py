import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class AndroidWireTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "requires a C compiler")
    def test_hello_caps_and_hash(self):
        source = r"""
#include <assert.h>
#include "ffny_android_protocol.h"
#include "netplay_check.h"

int main(void)
{
    uint8_t packet[NETCHECK_LEN_FULL];
    const uint8_t hello[] = {
        'D','J','N','P',1,1,2,0,0,0,0,0,0,
        0xf3,0xcf,0x95,0x8a,0x33,0x99,0x60,0xbe,
        4,'T','e','s','t',149,0,0,0,0,0,0,0,1
    };
    const uint8_t token[] = { 0,1,2,3,4,5,6,7 };
    const uint8_t caps[] = { 'D','J','N','P',1,0x20,0,1,2,3,4,5,6,7,0,0,0,0 };
    assert(ffny_android_lobby_hello(packet, "Test", 4, 0) == sizeof(hello));
    assert(!memcmp(packet, hello, sizeof(hello)));
    assert(ffny_android_lobby_hello(packet, "", 0, 0) == 0);
    assert(ffny_android_lobby_hello(packet, "abcdefghijklmnopqrstuvwxy", 25, 0) == 0);
    ffny_android_caps_packet(packet, token);
    assert(!memcmp(packet, caps, sizeof(caps)));

    NetCheck out = { .match = 0xfedcba98, .step = 60, .hash = 0x8899aabb,
                     .has_parts = 1, .has_extra = 1, .crowd = 0x12345678 };
    NetCheck in;
    char detail[700];
    out.parts[0] = 0x11223344;
    out.fields[0][0] = 0xf1234567;
    assert(netcheck_pack(packet, sizeof(packet), 5, &out) == NETCHECK_LEN_FULL);
    assert(netcheck_unpack(packet, NETCHECK_LEN_FULL, &in));
    assert(in.match == out.match && in.step == out.step && in.hash == out.hash);
    assert(in.fields[0][0] == out.fields[0][0] && in.crowd == out.crowd && in.has_extra);
    assert(netcheck_fields_text(detail, sizeof(detail), &out, &in, 2) > 0);
    assert(netcheck_parts_text(detail, sizeof(detail), &out, &in, 2, 2) > 0);
    assert(netcheck_unpack(packet, NETCHECK_LEN_OLD, &in) && !in.has_parts);
    assert(in.hash == out.hash && !netcheck_unpack(packet, NETCHECK_LEN_OLD - 1, &in));
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "test-wire"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-DFFNY_ANDROID_BUILD_NUMBER=149", "-x", "c",
                 "-I", str(ROOT / "src/hooks"), "-o", str(exe), "-"],
                input=source, text=True, check=True,
            )
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
