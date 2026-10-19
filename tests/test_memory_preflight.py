from paths import ROOT
import unittest
from memory_preflight import configure_memory,choose_pool,capped_render,BASE_MB,LARGE_MB


class MemoryPlanTests(unittest.TestCase):
    def test_native_and_reduced_scenes_keep_standard_pool(self):
        for render in (None,'1280x720','1920x1080'):
            calls=[];env={}
            configure_memory('fixture',env,render,'3840x2160',probe=lambda mb:calls.append(mb) or True)
            self.assertEqual(calls,[BASE_MB]);self.assertEqual(env['BB_DMEM_MB'],str(BASE_MB))

    def test_failed_large_pool_coordinates_safe_render_and_restart(self):
        calls=[];env={}
        render,output=configure_memory('fixture',env,'3840x2160','3840x2160',
            probe=lambda mb:calls.append(mb) or mb==BASE_MB)
        self.assertEqual(calls,[LARGE_MB,BASE_MB])
        self.assertEqual((render,output),('1920x1080','3840x2160'))
        self.assertEqual(env['BB_MEMORY_SAFE'],'1')
        configure_memory('fixture',env,'3840x2160','3840x2160',probe=lambda mb:True)
        self.assertEqual(env['BB_DMEM_MB'],str(LARGE_MB));self.assertNotIn('BB_MEMORY_SAFE',env)

    def test_explicit_override_never_silently_falls_back(self):
        for value in ('0','512','invalid','16385'):
            with self.subTest(value=value),self.assertRaises(ValueError):choose_pool(False,value,lambda mb:True)
        calls=[]
        with self.assertRaisesRegex(ValueError,'explicitly requested'):
            choose_pool(True,'9152',lambda mb:calls.append(mb) or False)
        self.assertEqual(calls,[9152])

    def test_base_failure_stops_before_game_launch(self):
        with self.assertRaisesRegex(ValueError,'stopped cleanly'):
            choose_pool(True,None,lambda mb:False)

    def test_live_resolution_uses_standard_game_heaps(self):
        env={};calls=[]
        configure_memory('fixture',env,'3840x2160','3840x2160',True,
                         lambda mb:calls.append(mb) or True)
        self.assertEqual(calls,[BASE_MB])

    def test_ultrawide_fallback_preserves_aspect_and_fits_game_heap(self):
        size=capped_render((3440,1440))
        self.assertLessEqual(size[0]*size[1],1920*1080)
        self.assertAlmostEqual(size[0]/size[1],3440/1440,delta=.01)
