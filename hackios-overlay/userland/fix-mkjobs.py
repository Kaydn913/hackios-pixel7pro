"""Fix HackiOS's userland/mkjobs.py: its RWA_PACKED16 loop reused the variable `name`, which also holds
the job's plist file name, so every job of the REST set was written to one file called RWA_PACKED16,
each over the last, and none of them ran (configd among them, so Wi-Fi never got an address).

Usage: python3 -I fix-mkjobs.py path/to/userland/mkjobs.py
"""
import sys

path = sys.argv[1]
text = open(path).read()
old = """        for name in ('RWA_PACKED16',):
            if os.environ.get(name):
                job.setdefault('EnvironmentVariables', {})[name] = os.environ[name]"""
new = """        # (not "name": that is the plist's own file name, written below -- reused here, every job of
        # the rest went into one file called RWA_PACKED16, each over the last, and none of them ran)
        for variable in ('RWA_PACKED16',):
            if os.environ.get(variable):
                job.setdefault('EnvironmentVariables', {})[variable] = os.environ[variable]"""
if text.count(old) != 1:
    sys.exit(f'fix-mkjobs: the loop is not there once ({text.count(old)})')
open(path, 'w').write(text.replace(old, new))
print('fix-mkjobs: done')
