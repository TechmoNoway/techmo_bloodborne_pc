"""Probe Windows commit with the native runtime; coordinate temporary safe fallback."""
import math
import subprocess
import queue
import threading


class PoolLease:
    """Keep the exact committed section alive until the game has duplicated it."""
    def __init__(self, probe_path, mb, env):
        self.process = subprocess.Popen([str(probe_path), '--hold-memory-pool', str(mb)],
            env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        result = queue.Queue(maxsize=1)
        threading.Thread(target=lambda: result.put(self.process.stdout.readline(256)), daemon=True).start()
        try:
            fields = result.get(timeout=30).decode('ascii').strip().split()
            if len(fields) != 4 or fields[0] != 'BB_POOL':
                raise ValueError('native pool allocation failed')
            pid, handle, size = map(int, fields[1:])
            if pid != self.process.pid or handle <= 0 or size != (mb + 1024) * 1024 * 1024:
                raise ValueError('native pool lease did not match the memory plan')
            self.environment = dict(zip(('BB_POOL_PID', 'BB_POOL_HANDLE', 'BB_POOL_BYTES'), map(str, (pid, handle, size))))
        except (ValueError, UnicodeError, queue.Empty):
            self.close()
            raise ValueError('native memory pool could not be retained') from None

    def close(self):
        if self.process.stdin:
            try:
                self.process.stdin.close()
            except OSError:
                pass
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
        if self.process.stdout:
            self.process.stdout.close()

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


def configure_memory(probe_path,env,render=None,output=None,live=False,probe=None,leases=None,lease_factory=PoolLease):
    # Restart recomputes automatic choices; a user's explicit override stays strict.
    if env.pop('BB_AUTO_DMEM',None)=='1':
        env.pop('BB_DMEM_MB',None)
    env.pop('BB_MEMORY_SAFE',None)
    for key in ('BB_POOL_PID','BB_POOL_HANDLE','BB_POOL_BYTES'):
        env.pop(key,None)
    size=parse_size(render)
    expanded=bool(size and size[0]*size[1]>NATIVE_PIXELS and not live)
    explicit=env.get('BB_DMEM_MB')
    held = []
    if leases is not None and probe is None:
        def probe(mb):
            try:
                held.append(lease_factory(probe_path,mb,env))
                return True
            except (OSError,ValueError):
                return False
    elif probe is None:
        def probe(mb):
            return subprocess.run([str(probe_path),'--check-memory-pool',str(mb)],env=env,
                                  check=False).returncode==0
    chosen,fallback=choose_pool(expanded,explicit,probe)
    if held:
        leases.extend(held)
        env.update(held[-1].environment)
    env['BB_DMEM_MB']=str(chosen)
    if explicit is None:
        env['BB_AUTO_DMEM']='1'
    if fallback:
        env['BB_MEMORY_SAFE']='1'
        render='x'.join(map(str,capped_render(size)))
        print('Memory fallback: standard game heaps, internal render size capped to '+render+
              '; custom executable patches skipped for this session. Saved settings are unchanged.',flush=True)
    detail='committed pool retained for the game' if held else 'allocation probe passed (advisory)'
    print(f'Memory plan: {chosen} MiB direct + 1024 MiB flexible; {detail}.',flush=True)
    return render,output
