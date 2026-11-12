#!/usr/bin/env python3
"""Sync regression test: play every movie of tests/movies.tsv with a Gens build (under Wine, -batch
-hashlog) and compare the per-frame state hashes with the references in tests/hashes/.

  tools/movietest.py run    --exe build/ref/Gens.exe --media <dir> [--cd-bios F] [--32x-bios-dir D]
                            [-j N] [--only name,...] [--update] [--max-frames N]
  tools/movietest.py list

--media      directory the rom/movie paths in movies.tsv are relative to
--cd-bios    Sega CD BIOS (used for US/EU/JP); needed for "cd" movies
--32x-bios-dir  directory with the three 32X BIOS files (names containing 68000 / Master / Slave)
--update     write the hashes of this run as the new references
--max-frames stop each movie after N frames (quick smoke test; compared against the reference prefix)

Requirements: wine (32-bit), an X display (DISPLAY, otherwise xvfb-run is used) and a working sound
device for Wine (e.g. PulseAudio, a null sink is fine): without sound Gens does not update the YM2612
and its hashes differ.

Reference format (tests/hashes/<name>.hash.gz): header lines starting with '#', then one line per
frame: "<frame> <genesis> <segacd> <32x>" with each section hash shortened to 8 hex digits ('-' if
the system is not active).
"""
import argparse, concurrent.futures, glob, gzip, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, 'tests')


def load_manifest():
    tests = []
    for line in open(os.path.join(TESTS, 'movies.tsv'), encoding='utf-8'):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split('\t')
        tests.append({'name': f[0], 'system': f[1], 'rom': f[2], 'movie': f[3],
                      'state': f[4] if len(f) > 4 and f[4] else None})
    return tests


def winpath(p):
    """Unix path -> Wine path on the Z: drive (no winepath call: it is slow and racy when run in parallel)."""
    return 'Z:' + os.path.abspath(p).replace('/', '\\')


def make_cfg(dst, args):
    s = open(os.path.join(TESTS, 'gens-test.cfg'), 'rb').read().decode('latin1')
    cd = winpath(os.path.abspath(args.cd_bios)) if args.cd_bios else '.\\'
    b32 = {}
    if args.x32_bios_dir:
        for f in os.listdir(args.x32_bios_dir):
            for key, pat in (('68K', '68000'), ('MSH2', 'master'), ('SSH2', 'slave')):
                if pat in f.lower():
                    b32[key] = winpath(os.path.abspath(os.path.join(args.x32_bios_dir, f)))
    for k, v in (('CD_BIOS_US', cd), ('CD_BIOS_EU', cd), ('CD_BIOS_JP', cd),
                 ('32X_BIOS_68K', b32.get('68K', '.\\')), ('32X_BIOS_MSH2', b32.get('MSH2', '.\\')),
                 ('32X_BIOS_SSH2', b32.get('SSH2', '.\\'))):
        s = s.replace('@%s@' % k, v)
    open(dst, 'wb').write(s.encode('latin1'))


def movie_frames(path):
    return max(0, (os.path.getsize(path) - 64) // 3)


def parse_hashlog(path):
    """-> (frames: list of (frame, g, c, x), notes: list of str, complete: bool)"""
    frames, notes, complete = [], [], False
    if not os.path.exists(path):
        return frames, notes, complete
    for line in open(path, encoding='latin1'):
        line = line.strip()
        if not line:
            continue
        if line.startswith('#'):
            notes.append(line[1:].strip())
        elif line.startswith('END'):
            complete = True
        else:
            p = line.split()
            short = lambda h: h if h == '-' else h[:8]
            frames.append((int(p[0]), short(p[1]), short(p[2]), short(p[3])))
    return frames, notes, complete


def ref_path(name):
    return os.path.join(TESTS, 'hashes', name + '.hash.gz')


def load_ref(name):
    p = ref_path(name)
    if not os.path.exists(p):
        return None
    frames = []
    for line in gzip.open(p, 'rt', encoding='latin1'):
        if line.startswith('#') or not line.strip():
            continue
        f, g, c, x = line.split()
        frames.append((int(f), g, c, x))
    return frames


def save_ref(t, frames, notes, exe):
    with gzip.GzipFile(ref_path(t['name']), 'wb', mtime=0) as gz:
        out = ['# gens64 sync reference: %s' % t['name'], '# movie: %s' % t['movie'],
               '# frames: %d' % len(frames), '# generated with: %s' % os.path.basename(exe)]
        out += ['# note: %s' % n for n in notes]
        out += ['%d %s %s %s' % f for f in frames]
        gz.write(('\n'.join(out) + '\n').encode('latin1'))


def run_one(t, args):
    try:
        return run_one_inner(t, args)
    except Exception as e:
        import traceback
        return t, 'ERROR', '%s: %s' % (type(e).__name__, e) + '\n' + traceback.format_exc(), [], [], 0


def run_one_inner(t, args):
    media = os.path.abspath(args.media)
    rom, movie = os.path.join(media, t['rom']), os.path.join(media, t['movie'])
    for p in (rom, movie):
        if not os.path.exists(p):
            return t, 'MISSING', 'file not found: %s' % p, [], [], 0
    rundir = os.path.join(os.path.abspath(args.workdir), t['name'])
    shutil.rmtree(rundir, ignore_errors=True)
    os.makedirs(rundir)
    shutil.copy(args.exe, os.path.join(rundir, 'Gens.exe'))
    make_cfg(os.path.join(rundir, 'Gens.cfg'), args)
    cmd = ['wine', 'Gens.exe', '-rom', winpath(rom), '-play', winpath(movie), '-batch', '-hashlog', 'hash.txt']
    if t['state']:
        cmd += ['-moviestate', winpath(os.path.join(media, t['state']))]
    if args.max_frames:
        cmd += ['-stopframe', str(args.max_frames)]  # note: frame numbers of state movies start mid-way
    if not os.environ.get('DISPLAY') and shutil.which('xvfb-run'):
        cmd = ['xvfb-run', '-a'] + cmd
    env = dict(os.environ, WINEDEBUG='-all')
    nframes = movie_frames(movie)
    timeout = 120 + nframes / 50  # generous: batch mode runs well above real time
    t0 = time.time()
    try:
        with open(os.path.join(rundir, 'stderr.txt'), 'w') as err:
            subprocess.run(cmd, cwd=rundir, env=env, stdout=err, stderr=err, timeout=timeout)
    except subprocess.TimeoutExpired:
        pass
    frames, notes, complete = parse_hashlog(os.path.join(rundir, 'hash.txt'))
    elapsed = time.time() - t0
    if not complete:
        return t, 'INCOMPLETE', 'run did not finish (%d frames logged, %.0fs); see %s' % (
            len(frames), elapsed, rundir), frames, notes, elapsed
    return t, 'DONE', '', frames, notes, elapsed


def compare(t, frames, ref, prefix_only):
    if ref is None:
        return 'NOREF', 'no reference hashes (run with --update to create them)'
    n = min(len(frames), len(ref)) if prefix_only else max(len(frames), len(ref))
    for i in range(n):
        a = frames[i] if i < len(frames) else None
        b = ref[i] if i < len(ref) else None
        if a != b:
            if a is None or b is None:
                return 'FAIL', 'length differs: %d frames, reference %d' % (len(frames), len(ref))
            sections = [s for s, x, y in zip(('genesis', 'segacd', '32x'), a[1:], b[1:]) if x != y]
            return 'FAIL', 'first difference at frame %d (line %d): %s' % (a[0], i + 1, ', '.join(sections) or 'frame number')
    return 'OK', '%d frames' % n


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('--exe', required=True)
    r.add_argument('--media', required=True)
    r.add_argument('--cd-bios')
    r.add_argument('--32x-bios-dir', dest='x32_bios_dir')
    r.add_argument('-j', '--jobs', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    r.add_argument('--only')
    r.add_argument('--update', action='store_true')
    r.add_argument('--max-frames', type=int)
    r.add_argument('--workdir', default=os.path.join(ROOT, 'build', 'movietest'))
    sub.add_parser('list')
    args = ap.parse_args()

    tests = load_manifest()
    if args.cmd == 'list':
        for t in tests:
            ref = load_ref(t['name'])
            print('%-12s %-4s %s  (%s)' % (t['name'], t['system'], t['movie'],
                                          '%d ref frames' % len(ref) if ref else 'no reference'))
        return 0
    if args.only:
        names = set(args.only.split(','))
        tests = [t for t in tests if t['name'] in names]
    if args.update and args.max_frames:
        ap.error('--update with --max-frames would store truncated references')
    if subprocess.run(['pactl', 'info'], capture_output=True).returncode != 0:
        print('warning: no PulseAudio server found; without sound YM2612 hashes will differ', file=sys.stderr)

    failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = [pool.submit(run_one, t, args) for t in tests]
        for fut in concurrent.futures.as_completed(futures):
            t, status, msg, frames, notes, elapsed = fut.result()
            if status == 'DONE':
                if args.update:
                    save_ref(t, frames, notes, args.exe)
                    status, msg = 'UPDATED', '%d frames' % len(frames)
                else:
                    status, msg = compare(t, frames, load_ref(t['name']), bool(args.max_frames))
            if status not in ('OK', 'UPDATED'):
                failed += 1
            print('%-10s %-12s %s  [%.0fs]' % (status, t['name'], msg, elapsed), flush=True)
    print('%d of %d failed' % (failed, len(tests)) if failed else 'all %d passed' % len(tests))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
