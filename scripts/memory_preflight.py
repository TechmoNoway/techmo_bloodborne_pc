"""Probe Windows commit with the native runtime; coordinate temporary safe fallback."""
import math
import subprocess

BASE_MB=5056
LARGE_MB=9152
NATIVE_PIXELS=1920*1080


def parse_size(value):
    if not value:
        return None
    width,height=(int(x) for x in value.lower().split('x'))
    if not (0<width<=16384 and 0<height<=16384):
        raise ValueError('Invalid render size for memory preflight')
    return width,height


def capped_render(size):
    if size[0]*size[1]<=NATIVE_PIXELS:
        return size
    scale=math.sqrt(NATIVE_PIXELS/(size[0]*size[1]))
    return tuple(max(2,int(x*scale)//2*2) for x in size)


def choose_pool(expanded,explicit,probe):
    if explicit is not None:
        try:
            requested=int(explicit)
        except ValueError:
            raise ValueError('BB_DMEM_MB must be an integer from 5056 to 16384') from None
        if not BASE_MB<=requested<=16384:
            raise ValueError('BB_DMEM_MB must be from 5056 to 16384')
        if not probe(requested):
            raise ValueError(f'Windows cannot reserve the explicitly requested {requested} MiB game pool. Free host commit capacity or adjust the page file.')
        return requested,False
    requested=LARGE_MB if expanded else BASE_MB
    if probe(requested):
        return requested,False
    if expanded and probe(BASE_MB):
        return BASE_MB,True
    raise ValueError('Windows cannot reserve the base game memory pool. Close unused applications or increase the Windows page file; game startup was stopped cleanly.')


def configure_memory(probe_path,env,render=None,output=None,live=False,probe=None):
    # Restart recomputes automatic choices; a user's explicit override stays strict.
    if env.pop('BB_AUTO_DMEM',None)=='1':
        env.pop('BB_DMEM_MB',None)
    env.pop('BB_MEMORY_SAFE',None)
    size=parse_size(render)
    expanded=bool(size and size[0]*size[1]>NATIVE_PIXELS and not live)
    explicit=env.get('BB_DMEM_MB')
    if probe is None:
        def probe(mb):
            return subprocess.run([str(probe_path),'--check-memory-pool',str(mb)],env=env,
                                  check=False).returncode==0
    chosen,fallback=choose_pool(expanded,explicit,probe)
    env['BB_DMEM_MB']=str(chosen)
    if explicit is None:
        env['BB_AUTO_DMEM']='1'
    if fallback:
        env['BB_MEMORY_SAFE']='1'
        render='x'.join(map(str,capped_render(size)))
        print('Memory fallback: standard game heaps, internal render size capped to '+render+
              '; custom executable patches skipped for this session. Saved settings are unchanged.',flush=True)
    print(f'Memory plan: {chosen} MiB direct + 1024 MiB flexible; allocation probe passed (advisory).',flush=True)
    return render,output
