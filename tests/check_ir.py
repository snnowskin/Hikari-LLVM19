import re, sys


def parse(path):
    txt = open(path, encoding='utf-8', errors='replace').read()
    blocks = {}
    cur = None
    pending = False
    for raw in txt.splitlines():
        line = raw.strip()
        if line.startswith('define') or line.startswith('declare'):
            cur = None
            pending = False
            continue
        m = re.match(r'^([\w.$"-]+):', line)
        if m and not line.startswith(';'):
            cur = m.group(1).strip('"')
            blocks[cur] = {'phis': [], 'succs': []}
            pending = False
            continue
        if cur is None:
            continue
        if ' = phi ' in line or line.startswith('phi '):
            pending = True
            for im in re.finditer(r'\[\s*([^,\]]+?)\s*,\s*%"?([\w.$\'-]+)"?\s*\]', line):
                blocks[cur]['phis'].append(im.group(2).strip('"'))
            continue
        if pending and '[' in line:
            for im in re.finditer(r'\[\s*([^,\]]+?)\s*,\s*%"?([\w.$\'-]+)"?\s*\]', line):
                blocks[cur]['phis'].append(im.group(2).strip('"'))
            continue
        pending = False
        if re.match(r'^(br|indirectbr|switch|invoke|callbr)\b', line):
            for lm in re.finditer(r'label\s+%"?([\w.$\'-]+)"?', line):
                blocks[cur]['succs'].append(lm.group(1).strip('"'))
    return blocks


for p in sys.argv[1:]:
    b = parse(p)
    bad = 0
    for name, blk in b.items():
        for pred in set(blk['phis']):
            if pred not in b:
                print(p, ': phi 指向不存在块 %' + pred, '@', name)
                bad += 1
            elif name not in b[pred]['succs']:
                print(p, ': %' + name, '的 phi 说前驱是 %' + pred, '但 %' + pred, '不跳它')
                bad += 1
    for name, blk in b.items():
        if not blk['phis']:
            continue
        for pred, pb in b.items():
            cnt = pb['succs'].count(name)
            if cnt == 0:
                continue
            have = sum(1 for x in blk['phis'] if x == pred)
            if have != cnt:
                print(p, ': %' + name, '需要 %' + pred, '的', cnt, '条入边，phi 里有', have, '条')
                bad += 1
    print('---', p, bad)
