"""POSIX control locks. Importable for read-only presentation on Windows.

Windows controllers must use Windows handles, never silently emulate flock.
"""
import os

if os.name == 'posix':
    from fcntl import flock, LOCK_EX, LOCK_NB, LOCK_UN, LOCK_SH
else:
    LOCK_EX, LOCK_NB, LOCK_UN, LOCK_SH = 2, 4, 8, 1

    def flock(*args):
        raise OSError('This controller requires POSIX locks; Windows control is unavailable')
