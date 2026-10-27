"""0.9.37 builds title rows natively; exercise its real builder with guest-call mocks."""
from pathlib import Path
import re
import os
import subprocess
import unittest
ROOT=Path(__file__).resolve().parents[1]
class MenuPatchScopeTests(unittest.TestCase):
    def test_menu_patch_is_separate_from_renderer_and_input_changes(self):
        paths=set(re.findall(r'^\+\+\+ b/(.+)$',(ROOT/'engine/techmo-menu.patch').read_text(encoding='utf-8'),re.MULTILINE))
        self.assertEqual(paths,{'gpu/shim/engine/option_menu.cpp','start/main.cpp','tests/test_option_rows.cpp'})
class NativeTitleRowsTests(unittest.TestCase):
    def test_native_title_row_preserves_previous_rows_and_confirmation(self):
        tool=ROOT/'engine/native/out/gpu/option-rows-test.exe'
        if not tool.is_file():self.skipTest('Native menu fixture runs after the native build')
        env=os.environ.copy()
        env['PATH']=str(ROOT/'out')+os.pathsep+env.get('PATH','')
        result=subprocess.run([str(tool)],capture_output=True,text=True,timeout=20,env=env)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS: title Exit Game preserves the first five rows',result.stdout)
if __name__=='__main__':unittest.main()
