#!/usr/bin/env python3
"""Run the pinned toolkit tests with MSVC for its shared compiled fixture.

The helper prefers Clang (including a hard-coded Windows install) over cl.
Select the port's compiler without changing test sources, results or skips.
"""
import os
import hashlib
import shutil
import sys
from pathlib import Path

import pytest

PINNED_HELPER_SHA256 = 'c33ecc8c573b4690df4b454ace87c99c57f8910323bb0860a8ee6871a6697a1b'


class MSVCFixtures:
    def __init__(self, toolkit, compiler):
        self.helper = (toolkit / 'tools/recomp/test_lifter_result_clobber.py').resolve()
        self.compiler = compiler

    def pytest_collection_modifyitems(self, session, config, items):
        digest = hashlib.sha256(self.helper.read_text(encoding='utf-8').encode('utf-8')).hexdigest()
        if digest != PINNED_HELPER_SHA256:
            raise pytest.UsageError('Pinned fixture changed; review compiler selection and builder before updating its fingerprint')
        helpers = [module for module in tuple(sys.modules.values())
                   if getattr(module, '__file__', None)
                   and Path(module.__file__).resolve() == self.helper]
        if not helpers:
            raise pytest.UsageError('Pinned toolkit compiled-fixture helper was not collected')
        for module in helpers:
            if not callable(getattr(module, '_cc', None)):
                raise pytest.UsageError('Compiler selector changed; review the MSVC runner')
            module._cc = lambda: self.compiler


def main():
    compiler = shutil.which('cl')
    if os.name != 'nt' or not compiler:
        raise SystemExit('Run in an activated Windows MSVC x64 environment')
    toolkit = Path(__file__).resolve().parents[1] / 'tools/xboxrecomp'
    os.chdir(toolkit)
    print('Shared CPU fixture compiler: ' + compiler, flush=True)
    return pytest.main(['-q', '-p', 'no:cacheprovider', 'tools/recomp', 'tools/disasm'],
                       plugins=[MSVCFixtures(toolkit, compiler)])


if __name__ == '__main__':
    raise SystemExit(main())
