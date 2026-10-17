from paths import ROOT
import struct
import os
import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch
from patches import RENDER_POOL_CODE, RENDER_POOL_SITES, render_pool_writes


class RenderPoolTests(unittest.TestCase):
    @staticmethod
    def elf():
        data=bytearray(512)
        struct.pack_into('<Q',data,0x20,64)
        struct.pack_into('<HH',data,0x36,56,2)
        for index,site in enumerate(RENDER_POOL_SITES):
            offset=384+index*16
            struct.pack_into('<IIQQQQQQ',data,64+index*56,1,5,offset,site,0,5,5,4096)
            data[offset:offset+5]=RENDER_POOL_CODE
        return data

    def test_real_elf_translation_and_patch_stream_conflicts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary)
            (root/'eboot.elf').write_bytes(self.elf());(root/'settings.ini').write_text('')
            external=root/'external';external.mkdir()
            xml=external/'test.xml'
            xml.write_text('<Patch><Metadata Name="Overlap" AppVer="01.09" AppElf="eboot.bin" isEnabled="true"><PatchList><Line Type="bytes" Address="'+hex(RENDER_POOL_SITES[0]+0x400000)+'" Value="90"/></PatchList></Metadata></Patch>')
            command=[sys.executable,str(ROOT/'scripts/patches.py'),'--out',str(root),'--fps','30',
                     '--settings',str(root/'settings.ini'),'--game-dir',str(root),'--patches-dir',str(external)]
            env=dict(os.environ,BB_INTEL_TONEMAP_FIX='0',BB_RENDER_POOLS='1')
            for conflict,safe,count in ((False,False,2),(True,False,1),(True,True,2)):
                with self.subTest(conflict=conflict,safe=safe):
                    env['BB_MEMORY_SAFE']='1' if safe else '0'
                    (root/'settings.ini').write_text('model_lod=-2\n' if safe else '')
                    if conflict:
                        if xml.name!='test.xml':
                            xml.rename(external/'test.xml')
                        xml=external/'test.xml'
                    else:
                        xml.rename(external/'test.disabled');xml=external/'test.disabled'
                    result=subprocess.run(command,env=env,capture_output=True,text=True,timeout=15)
                    self.assertEqual(result.returncode,0,result.stderr)
                    blob=(root/'patches.bin').read_bytes()
                    self.assertEqual(struct.unpack_from('<Q',blob,16)[0],count)
                    if not conflict or safe:
                        at=24
                        for site in RENDER_POOL_SITES:
                            offset,size=struct.unpack_from('<QQ',blob,at);at+=16
                            self.assertEqual((offset,size),(site+1,4))
                            self.assertEqual(blob[at:at+size],struct.pack('<I',1024*1024));at+=size
                    else:
                        self.assertIn('overlaps',result.stdout)
    def test_both_sites_checked_before_any_patch(self):
        for bad in (None,0,1):
            with self.subTest(bad=bad):
                def read(elf,site,size):
                    return b'\x90'*size if bad is not None and site==RENDER_POOL_SITES[bad] else RENDER_POOL_CODE
                with patch('frame_rate.original_bytes',side_effect=read):
                    result=render_pool_writes(b'fixture')
                self.assertEqual(result, [] if bad is not None else
                    [(site+1,struct.pack('<I',1024*1024)) for site in RENDER_POOL_SITES])

    def test_opt_out_does_not_read_executable(self):
        with patch('frame_rate.original_bytes',side_effect=AssertionError('must not read')):
            self.assertEqual(render_pool_writes(b'',False),[])
