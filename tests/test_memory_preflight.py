from paths import ROOT
import unittest
from memory_preflight import configure_memory,choose_pool,capped_render,BASE_MB,LARGE_MB


class MemoryPlanTests(unittest.TestCase):
    def test_successful_pool_is_held_and_passed_to_runtime(self):
        class Lease:
            environment = {'BB_POOL_PID':'42','BB_POOL_HANDLE':'88','BB_POOL_BYTES':'6375342080'}
        leases=[];env={'BB_POOL_PID':'stale'};calls=[]
        def factory(path,mb,environment):
            calls.append(mb)
            self.assertNotIn('BB_POOL_PID',environment)
            return Lease()
        configure_memory('fixture',env,leases=leases,lease_factory=factory)
        self.assertEqual(calls,[BASE_MB])
        self.assertEqual(len(leases),1)
        self.assertEqual(env['BB_POOL_PID'],'42')
        self.assertEqual(env['BB_POOL_HANDLE'],'88')

    def test_retained_pool_failure_uses_coordinated_safe_fallback(self):
        class Lease:
            environment = {'BB_POOL_PID':'42','BB_POOL_HANDLE':'88','BB_POOL_BYTES':'6375342080'}
        calls=[];leases=[];env={}
        def factory(path,mb,environment):
            calls.append(mb)
            if mb==LARGE_MB: raise ValueError('host commit exhausted')
            return Lease()
        render,_=configure_memory('fixture',env,'3840x2160',leases=leases,lease_factory=factory)
        self.assertEqual(calls,[LARGE_MB,BASE_MB])
        self.assertEqual(render,'1920x1080')
        self.assertEqual(len(leases),1)
        self.assertEqual(env['BB_MEMORY_SAFE'],'1')

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
