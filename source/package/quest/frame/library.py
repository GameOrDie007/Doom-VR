#!/usr/bin/env python3
"""Game Or Die on the Steam Frame: a game's entry in the Frame's own Steam library.

Run ON THE FRAME by the port's setup-quest.ps1 (frame.ps1) (through library.sh, while Steam is stopped):

  library.py add    --steam S --user U --appid N --name NAME --exe EXE --dir DIR --icon ICON --art DIR
  library.py remove --steam S --user U --appid N
  library.py has    --steam S --user U --appid N        (prints yes or no; Steam may run)

"add" puts a plain (non-devkit) shortcut in userdata/U/config/shortcuts.vdf, maps its app
id to the lepton compatibility tool in config/config.vdf, and copies our art into the
grid folder; "remove" takes all three away.  --tool names another compatibility tool
(a native Linux build: steamlinuxruntime_steamrt4-arm64), or none ('-') to run it on SteamOS itself.  Every file changed is backed up first
(<file>.gameordie-<date>; the last three are kept), so it can be undone by copying one
back.
"""
import argparse, glob, os, re, shutil, struct, sys, time

ART = (('cover.jpg', '%dp.jpg'), ('wide.jpg', '%d.jpg'), ('hero.jpg', '%d_hero.jpg'), ('logo.png', '%d_logo.png'))
stamp = time.strftime('%Y%m%d-%H%M%S')


def backup(p):
    if not os.path.exists(p):
        return
    shutil.copy2(p, p + '.gameordie-' + stamp)
    print('backup', p + '.gameordie-' + stamp)
    old = sorted(glob.glob(p + '.gameordie-*'))
    for f in old[:-3]:
        os.remove(f)


# ---- the binary VDF (shortcuts.vdf) ----
def bread(b, i):
    d = {}
    while True:
        t = b[i]; i += 1
        if t == 8:
            return d, i
        j = b.index(0, i); k = b[i:j].decode('utf-8', 'replace'); i = j + 1
        if t == 0:
            d[k], i = bread(b, i)
        elif t == 1:
            j = b.index(0, i); d[k] = b[i:j].decode('utf-8', 'replace'); i = j + 1
        elif t == 2:
            d[k] = ('i', struct.unpack('<i', b[i:i + 4])[0]); i += 4
        else:
            raise ValueError('unknown type %d' % t)


def bwrite(d):
    out = b''
    for k, v in d.items():
        kb = k.encode() + b'\0'
        if isinstance(v, dict):
            out += b'\0' + kb + bwrite(v)
        elif isinstance(v, tuple):
            out += b'\2' + kb + struct.pack('<i', v[1])
        else:
            out += b'\1' + kb + v.encode() + b'\0'
    return out + b'\x08'


def read_shortcuts(path):
    if os.path.exists(path):
        return bread(open(path, 'rb').read(), 0)[0]
    return {'shortcuts': {}}


STRAY = []   # the second roots shortcuts_node merged away


def shortcuts_node(root):
    """The dict of shortcuts, whatever the file spells its root ('shortcuts' or 'Shortcuts': another
    tool wrote 'Shortcuts' on a player's Frame, our own lower-case root beside it was a second one
    that Steam never reads, and the game was in no library, 2 Nov 2026).  The spelling the file
    already uses is kept; any other root of that name is merged into it and dropped."""
    keys = [k for k in root if k.lower() == 'shortcuts']
    if not keys:
        root['shortcuts'] = {}
        return root['shortcuts']
    target = next((k for k in keys if k != 'shortcuts'), keys[0])
    node = root[target]
    for k in keys:
        if k == target:
            continue
        have = {v.get('appid') for v in node.values() if isinstance(v, dict)}
        for _, v in sorted(root[k].items(), key=lambda kv: int(kv[0]) if kv[0].isdigit() else 1 << 30):
            if isinstance(v, dict) and v.get('appid') not in have:
                node[str(max([int(x) for x in node if x.isdigit()] + [-1]) + 1)] = v
        del root[k]
        STRAY.append(k)
        print('shortcuts.vdf: a second', repr(k), 'root merged into', repr(target))
    return node


def signed(appid):
    return appid - (1 << 32) if appid >= (1 << 31) else appid


def entries_for(shorts, appid):
    return [k for k, v in shorts.items() if isinstance(v, dict) and v.get('appid') == ('i', signed(appid))]


# ---- the text VDF (config.vdf): CompatToolMapping ----
def tparse(s):
    toks = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', s)

    def node(i):
        d = {}
        while i < len(toks):
            q, br = toks[i]
            if br == '}':
                return d, i + 1
            key = q; i += 1
            q2, br2 = toks[i]
            if br2 == '{':
                d[key], i = node(i + 1)
            else:
                d[key] = q2; i += 1
        return d, i
    return node(0)[0]


def tdump(d, ind=0):
    out = ''
    for k, v in d.items():
        if isinstance(v, dict):
            out += '\t' * ind + '"%s"\n' % k + '\t' * ind + '{\n' + tdump(v, ind + 1) + '\t' * ind + '}\n'
        else:
            out += '\t' * ind + '"%s"\t\t"%s"\n' % (k, v)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('what', choices=('add', 'remove', 'has'))
    ap.add_argument('--steam', required=True)
    ap.add_argument('--user', required=True)
    ap.add_argument('--appid', required=True, type=int)
    ap.add_argument('--name', default='')
    ap.add_argument('--exe', default='')
    ap.add_argument('--dir', default='')
    ap.add_argument('--icon', default='')
    ap.add_argument('--art', default='')
    ap.add_argument('--tool', default='lepton')
    a = ap.parse_args()
    tool = '' if a.tool in ('', '-', 'none') else a.tool

    cfg = os.path.join(a.steam, 'userdata', a.user, 'config')
    sc = os.path.join(cfg, 'shortcuts.vdf')
    root = read_shortcuts(sc)
    shorts = shortcuts_node(root)

    if a.what == 'has':
        # a stray second root is not "there": Steam never read it, so Setup must write the file again
        print('yes' if entries_for(shorts, a.appid) and not STRAY else 'no')
        return 0

    for k in entries_for(shorts, a.appid):
        del shorts[k]
    if a.what == 'add':
        idx = str(max([int(k) for k in shorts if k.isdigit()] + [-1]) + 1)
        shorts[idx] = {
            'appid': ('i', signed(a.appid)), 'AppName': a.name, 'Exe': a.exe, 'StartDir': '"' + a.dir + '"',
            'icon': a.icon, 'ShortcutPath': '', 'LaunchOptions': '', 'IsHidden': ('i', 0),
            'AllowDesktopConfig': ('i', 1), 'AllowOverlay': ('i', 1), 'OpenVR': ('i', 1),
            'Devkit': ('i', 0), 'DevkitGameID': '', 'DevkitOverrideAppID': ('i', 0),
            'LastPlayTime': ('i', 0), 'FlatpakAppID': '', 'tags': {},
        }
    data = bwrite(root)
    assert bread(data, 0)[0] == root
    os.makedirs(cfg, exist_ok=True)
    backup(sc)
    open(sc, 'wb').write(data)
    print('shortcuts.vdf:', 'entry added' if a.what == 'add' else 'entry removed')

    cp = os.path.join(a.steam, 'config', 'config.vdf')
    conf = tparse(open(cp, encoding='utf-8').read())
    assert tparse(tdump(conf)) == conf, 'config.vdf round trip failed'
    m = conf['InstallConfigStore']['Software']['Valve']['Steam'].setdefault('CompatToolMapping', {})
    if a.what == 'add' and tool:
        m[str(a.appid)] = {'name': tool, 'config': '', 'priority': '250'}
    else:
        m.pop(str(a.appid), None)
    out = tdump(conf)
    assert tparse(out) == conf
    backup(cp)
    open(cp, 'w', encoding='utf-8').write(out)
    print('config.vdf: CompatToolMapping', a.appid, ('-> ' + tool) if a.what == 'add' and tool else 'none')

    grid = os.path.join(cfg, 'grid')
    if a.what == 'add':
        os.makedirs(grid, exist_ok=True)
        n = 0
        for ours, theirs in ART:
            src = os.path.join(a.art, ours)
            if os.path.exists(src):
                shutil.copy2(src, os.path.join(grid, theirs % a.appid)); n += 1
        print('art: %d pictures in %s' % (n, grid))
    else:
        for ours, theirs in ART:
            p = os.path.join(grid, theirs % a.appid)
            if os.path.exists(p):
                os.remove(p)
        print('art: removed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
