"""Use the tiny Win32 fixture, never a multi-gigabyte game pool."""
import os
from pathlib import Path
import subprocess
import unittest


@unittest.skipUnless(os.name=='nt','Windows section handles')
class NativeLeaseTests(unittest.TestCase):
    def test_retained_section_is_adopted_and_bad_size_rejected(self):
        fixture=Path(__file__).resolve().parents[1]/'out'/'memory-probe-test.exe'
        if not fixture.is_file():
            self.skipTest('build the native memory-probe fixture first')
        with subprocess.Popen([str(fixture),'--hold'],stdin=subprocess.PIPE,stdout=subprocess.PIPE) as owner:
            fields=owner.stdout.readline().decode('ascii').split()
            self.assertEqual(fields[0],'BB_POOL')
            env=os.environ.copy()
            env.update(dict(zip(('BB_POOL_PID','BB_POOL_HANDLE','BB_POOL_BYTES'),fields[1:])))
            good=subprocess.run([str(fixture),'--adopt'],env=env,capture_output=True,text=True)
            self.assertEqual(good.returncode,0,good.stderr)
            self.assertIn('adopted preflight section',good.stdout)
            env['BB_POOL_BYTES']='4096'
            bad=subprocess.run([str(fixture),'--adopt'],env=env,capture_output=True,text=True)
            self.assertNotEqual(bad.returncode,0)
            owner.stdin.close()
            self.assertEqual(owner.wait(timeout=5),0)
