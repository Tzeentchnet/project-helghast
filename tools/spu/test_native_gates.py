#!/usr/bin/env python3
"""Game-free checks for native SPU gate insertion and required lift data."""
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

import lift_spu


ROOT = Path(__file__).resolve().parents[2]
IMAGE = "job_0116d380"


class NativeGatesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="kz3-native-gates-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.table = self.root / "re" / "spu" / "native_gates.tsv"
        self.table.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "re" / "spu" / "native_gates.tsv", self.table)
        self.sub = self.root / "lift" / IMAGE
        self.sub.mkdir(parents=True)
        root_patch = patch.object(lift_spu, "ROOT", str(self.root))
        root_patch.start()
        self.addCleanup(root_patch.stop)

    def test_inserts_every_light_probe_gate_before_lifted_code(self):
        rows = [line.split("\t") for line in self.table.read_text(encoding="utf-8").splitlines()
                if line and not line.startswith("#") and line.split("\t")[0] == IMAGE]
        self.assertEqual({int(row[2]) for row in rows}, {8, 9, 12, 13, 16})
        source = '#include "spu_helpers.h"\n'
        for row in rows:
            source += (f"SPU_ABI void {IMAGE}_spu_func_{int(row[1], 16):08X}(spu_context* ctx) {{\n"
                       "    lifted(ctx);\n}\n")
        target = self.sub / "spu_recomp.c"
        target.write_text(source, encoding="utf-8")
        self.assertEqual(lift_spu.native_gates(str(self.sub), IMAGE), len(rows))
        result = target.read_text(encoding="utf-8")
        self.assertEqual(result.count("extern int (*g_kz3_native_gate[])"), 1)
        for row in rows:
            head = f"SPU_ABI void {IMAGE}_spu_func_{int(row[1], 16):08X}(spu_context* ctx) {{\n"
            body = result.split(head, 1)[1]
            self.assertTrue(body.startswith(f"    if (__builtin_expect(g_kz3_native_gate[{row[2]}]"))
            self.assertLess(body.index("SPU_RET(ctx)"), body.index("lifted(ctx)"))

    def test_missing_table_is_an_error(self):
        self.table.unlink()
        with self.assertRaisesRegex(FileNotFoundError, "native_gates.tsv"):
            lift_spu.native_gates(str(self.sub), IMAGE)

    def test_missing_lifted_function_is_an_error(self):
        (self.sub / "spu_recomp.c").write_text('#include "spu_helpers.h"\n', encoding="utf-8")
        with self.assertRaisesRegex(SystemExit, "no lifted function"):
            lift_spu.native_gates(str(self.sub), IMAGE)

    def test_other_image_has_no_gates(self):
        self.assertEqual(lift_spu.native_gates(str(self.sub), "job_not_in_table"), 0)


if __name__ == "__main__":
    unittest.main()
