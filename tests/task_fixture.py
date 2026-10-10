"""Development integration child; installed ShellBot has no Python dependency."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time

mode = sys.argv[1]
if mode == 'echo':
    token = ctypes.c_void_p()
    api = ctypes.WinDLL('advapi32', use_last_error=True)
    api.OpenProcessToken.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]
    api.GetTokenInformation.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    elevation, size = ctypes.c_uint32(), ctypes.c_uint32()
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    assert api.OpenProcessToken(ctypes.c_void_p(-1), 8, ctypes.byref(token))
    assert api.GetTokenInformation(token, 20, ctypes.byref(elevation), 4, ctypes.byref(size))
    kernel.CloseHandle(token)
    sys.stdout.buffer.write(json.dumps({'argv': sys.argv[2:], 'cwd': os.getcwd(), 'elevated': bool(elevation.value),
                      'bearer_inherited': 'SHELLBOT_TOKEN' in os.environ or 'SHELLBOT_TEST_SECRET' in os.environ}, ensure_ascii=False).encode('utf-8'))
elif mode == 'raw':
    sys.stdout.buffer.write(b'\x00\xff' + '中文 😀'.encode('utf-8'))
    sys.stderr.buffer.write(b'error\x00\xfe')
    sys.exit(7)
elif mode == 'escaped':
    sys.stdout.buffer.write(b'\x01' * 16384)
elif mode == 'append':
    with Path(sys.argv[2]).open('ab') as output:
        output.write(sys.argv[3].encode('utf-8'))
    print('appended')
elif mode == 'input':
    sys.stdout.buffer.write(sys.stdin.buffer.read())
elif mode == 'flood':
    for _ in range(128):
        sys.stdout.buffer.write(b'x' * 4096)
        sys.stdout.buffer.flush()
elif mode == 'tree':
    child = subprocess.Popen([sys.executable, __file__, 'wait'])
    print(child.pid, flush=True)
elif mode == 'wait':
    print(os.getpid(), flush=True)
    time.sleep(90)
else:
    raise SystemExit(2)
