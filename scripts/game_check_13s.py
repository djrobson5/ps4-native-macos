#!/usr/bin/env python3
"""Checks a game input for the 13 Sentinels port before it is installed: a dumped game folder or a
PS4 fake PKG. The port runs one build only: 13 Sentinels: Aegis Rim CUSA19610, version 01.00.

  game_check_13s.py PATH [--json] [--eboot]
  game_check_13s.py --pick-icon [--game DIR ...] [--data DIR]

Exit status: 0 supported, 1 refused, 2 usage error. --json prints one JSON object: ok, reason (null
when ok, else not_found, not_a_game, unreadable, update_pkg, retail_pkg, other_title, other_version,
wrong_eboot or needs_cryptography), message (English: what is wrong and what to do), input
("folder", "pkg" or null), title_id, title, version, category, content_id, image_checked, has_icon.

A folder is checked down to eboot.bin's loaded image (the runtime refuses any other: src/runtime_
prepare.c). A PKG is read with the standard library only (header, entry table, param.sfo; nothing is
decrypted), so the image is compared only with --eboot, which unpacks eboot.bin alone into a
temporary folder (needs the 'cryptography' package, see pkg_extract.py).

--pick-icon prints the picture for the app's icon: the first sce_sys/icon0.png of the --game
folders, else the newest save data's icon0.png under the data folder (--data; default
$S13_APP_DATA_DIR or ~/Library/Application Support/13 Sentinels), else nothing.
"""
import argparse
import glob
import json
import os
import shutil
import struct
import sys
import tempfile
from pathlib import Path

# Not on sys.path under `python3 -I` (which leaves the script's folder out).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_check  # noqa: E402
import prepare  # noqa: E402

# What the runtime boots; keep in step with src/runtime_prepare.c (SUPPORTED_IMAGE_13S;
# tests/test_game_check_13s.py compares the two).
SUPPORTED_TITLE = 'CUSA19610'
SUPPORTED_VERSION = '01.00'
SUPPORTED_IMAGE_13S = '1623190dc1a74bb9a78739fc50132ade2fc14b3d34ab7488dc1432fd98254c07'

PKG_MAGIC = 0x7F434E54
ENTRY_PARAM_SFO = 0x1000
ENTRY_IMAGE_KEY = 0x20
ENTRY_TARGET_DELTA = 0x407
ENTRY_ORIGIN_DELTA = 0x408
ENTRY_ICON0 = 0x1200
FAKE_IMAGE_KEY_INDEX = 3
MAX_ENTRIES = 4096
MAX_SFO_SIZE = 1 << 20

NAME = '13 Sentinels: Aegis Rim'
FIELDS = ('ok', 'reason', 'message', 'input', 'title_id', 'title', 'version', 'category',
          'content_id', 'image_checked', 'has_icon')


def blank():
    return dict.fromkeys(FIELDS) | {'ok': False, 'image_checked': False, 'has_icon': False}


def refuse(info, reason, message):
    info.update(ok=False, reason=reason, message=message)
    return info


def accept(info, message):
    info.update(ok=True, reason=None, message=message)
    return info


def found(info):
    """'CUSA19610 version 01.00', for the messages."""
    return f"{info['title_id'] or 'unknown title'} version {info['version'] or '?'}"


def fill_from_sfo(info, values):
    for key, field in (('TITLE_ID', 'title_id'), ('TITLE', 'title'), ('APP_VER', 'version'),
                       ('CATEGORY', 'category')):
        if key in values:
            info[field] = str(values[key])
    if 'CONTENT_ID' in values:
        info['content_id'] = str(values['CONTENT_ID'])


def judge_sfo(info, where):
    """The checks that only param.sfo's values decide, in order; a refusal, or None."""
    kind = 'game folder' if info['input'] == 'folder' else 'PKG'
    if not info['title_id'] or not info['version']:
        return refuse(info, 'unreadable',
                      f'{where} has no TITLE_ID and APP_VER: this {kind} is not a complete PS4 game. '
                      f'Dump or download {NAME} ({SUPPORTED_TITLE}) again.')
    if info['category'] == 'gp':
        return refuse(info, 'update_pkg',
                      f'This is a game update ({found(info)}), not the game. Pick the base game '
                      f'{SUPPORTED_TITLE} {SUPPORTED_VERSION} instead: the port runs version '
                      f'{SUPPORTED_VERSION} only, and an update is not merged in.')
    if info['title_id'] != SUPPORTED_TITLE:
        return refuse(info, 'other_title',
                      f'This is {info["title_id"]}'
                      f'{" (" + info["title"] + ")" if info["title"] else ""}, not {NAME} '
                      f'{SUPPORTED_TITLE}: another region\'s release or another game. The port '
                      f'runs {SUPPORTED_TITLE} only.')
    if info['version'] != SUPPORTED_VERSION:
        return refuse(info, 'other_version',
                      f'This is {found(info)}: an update may be merged into it. The port runs '
                      f'version {SUPPORTED_VERSION} only: use the unpatched base game.')
    return None


def check_folder(folder, info):
    info['input'] = 'folder'
    eboot, param = folder / 'eboot.bin', folder / 'sce_sys' / 'param.sfo'
    for needed in (eboot, param):
        if not needed.is_file():
            return refuse(info, 'not_a_game',
                          f'{folder} is not a dumped PS4 game: {needed.relative_to(folder)} is '
                          f'missing. Pick the folder that holds eboot.bin and sce_sys (the '
                          f'"uroot" folder of a dump).')
    info['has_icon'] = (folder / 'sce_sys' / 'icon0.png').is_file()
    try:
        values = prepare.sfo(param.read_bytes())
    except Exception as error:  # a damaged file raises struct.error, ValueError, IndexError...
        return refuse(info, 'unreadable',
                      f'sce_sys/param.sfo could not be read ({error}): dump the game again.')
    fill_from_sfo(info, values)
    refused = judge_sfo(info, 'sce_sys/param.sfo')
    if refused:
        return refused
    try:
        image = game_check.image_sha256(folder)
    except Exception as error:
        return refuse(info, 'unreadable',
                      f'eboot.bin could not be read as a decrypted PS4 executable ({error}): dump '
                      f'the game again.')
    info['image_checked'] = True
    if image != SUPPORTED_IMAGE_13S:
        return refuse(info, 'wrong_eboot',
                      f'sce_sys/param.sfo says {found(info)} but eboot.bin is not that game\'s '
                      f'executable (it may be patched or from another version): dump the base game '
                      f'again.')
    return accept(info, f'{NAME} ({found(info)}): game folder and eboot.bin match.')


def read_pkg(path):
    """(info, entries) from the header and entry table; raises ValueError when they do not parse."""
    size = path.stat().st_size
    with open(path, 'rb') as f:
        header = f.read(0x1000)
        if len(header) < 0x1000:
            raise ValueError('the header is cut short')
        count, table = struct.unpack_from('>I', header, 0x10)[0], struct.unpack_from('>I', header, 0x18)[0]
        if not 0 < count <= MAX_ENTRIES or table + count * 32 > size:
            raise ValueError('the entry table is out of bounds')
        f.seek(table)
        raw = f.read(count * 32)
        if len(raw) != count * 32:
            raise ValueError('the entry table is cut short')
        entries = {}
        for i in range(count):
            entry_id, _name, flags1, flags2, offset, length = struct.unpack_from('>IIIIII', raw, i * 32)
            entries[entry_id] = dict(flags1=flags1, flags2=flags2, offset=offset, size=length)
        content_id = header[0x40:0x64].rstrip(b'\0').decode('ascii', 'replace')
        sfo = entries.get(ENTRY_PARAM_SFO)
        sfo_data = None
        if sfo and not sfo['flags1'] & 0x80000000:
            if sfo['size'] > MAX_SFO_SIZE or sfo['offset'] + sfo['size'] > size:
                raise ValueError('the param.sfo entry is out of bounds')
            f.seek(sfo['offset'])
            sfo_data = f.read(sfo['size'])
    return content_id, entries, sfo_data


def check_pkg(path, info, eboot):
    info['input'] = 'pkg'
    try:
        with open(path, 'rb') as f:
            magic = f.read(4)
    except OSError as error:
        return refuse(info, 'unreadable', f'{path} could not be read ({error}).')
    if len(magic) < 4 or struct.unpack('>I', magic)[0] != PKG_MAGIC:
        return refuse(info, 'not_a_game',
                      f'{path.name} is neither a dumped game folder nor a PS4 PKG. Pick the game '
                      f'folder (the one that holds eboot.bin and sce_sys) or a .pkg file.')
    try:
        content_id, entries, sfo_data = read_pkg(path)
    except (OSError, ValueError, struct.error) as error:
        return refuse(info, 'unreadable',
                      f'The PKG could not be read ({error}): download or copy it again.')
    info['content_id'] = content_id or None
    info['has_icon'] = ENTRY_ICON0 in entries
    values = {}
    sfo_error = None
    if ENTRY_PARAM_SFO not in entries:
        sfo_error = 'the PKG has no param.sfo entry'
    elif sfo_data is None:
        sfo_error = 'its param.sfo is encrypted'
    else:
        try:
            values = prepare.sfo(sfo_data)
        except Exception as error:
            sfo_error = f'param.sfo does not parse: {error}'
    fill_from_sfo(info, values)
    if info['content_id'] is None and 'CONTENT_ID' in values:
        info['content_id'] = str(values['CONTENT_ID'])

    key = entries.get(ENTRY_IMAGE_KEY)
    if key is None:
        return refuse(info, 'unreadable', 'The PKG has no image key entry: it is not a complete PS4 '
                                          'package. Download or copy it again.')
    index = (key['flags2'] >> 12) & 0xF
    if index != FAKE_IMAGE_KEY_INDEX:
        return refuse(info, 'retail_pkg',
                      'This is a retail PKG: unpacking it needs a console\'s keys, which this port '
                      'does not have. Use a dumped game folder, or a fake PKG made from a dump.')
    if ENTRY_TARGET_DELTA in entries or ENTRY_ORIGIN_DELTA in entries:
        return refuse(info, 'update_pkg',
                      f'This PKG is a game update (it holds delta info). Pick the base game PKG '
                      f'{SUPPORTED_TITLE} {SUPPORTED_VERSION} instead.')
    if sfo_error:
        return refuse(info, 'unreadable', f'The PKG could not be read ({sfo_error}): download or '
                                          f'copy it again.')
    refused = judge_sfo(info, 'The PKG\'s param.sfo')
    if refused:
        return refused
    if not eboot:
        return accept(info, f'{NAME} ({found(info)}): a fake PKG of the supported game. '
                            f'eboot.bin is compared when it is unpacked.')
    return check_pkg_eboot(path, info)


def check_pkg_eboot(path, info):
    """Unpacks eboot.bin alone into a temporary folder and compares its loaded image."""
    try:
        import cryptography  # noqa: F401
        import pkg_extract
    except (ImportError, SystemExit):  # pkg_extract exits when it cannot import cryptography
        return refuse(info, 'needs_cryptography',
                      "Checking the PKG's eboot.bin needs the 'cryptography' Python package: "
                      "install it into a virtual environment (python3 -m venv <dir> && "
                      "<dir>/bin/pip install cryptography) and run this with <dir>/bin/python3.")
    temp = tempfile.mkdtemp(prefix='13s-eboot.')
    package = None
    try:
        try:
            package = pkg_extract.Pkg(str(path))
            pkg_extract.extract(package, temp, only=['eboot.bin'])
            if not (Path(temp) / 'eboot.bin').is_file():
                return refuse(info, 'not_a_game',
                              'The PKG holds no eboot.bin: it is not the game. Pick the game\'s '
                              'PKG or its dumped folder.')
            image = game_check.image_sha256(temp)
        except Exception as error:
            return refuse(info, 'unreadable',
                          f"The PKG's eboot.bin could not be unpacked ({error}): download or copy "
                          f"the PKG again.")
    finally:
        if package is not None:
            package.f.close()
        shutil.rmtree(temp, ignore_errors=True)
    info['image_checked'] = True
    if image != SUPPORTED_IMAGE_13S:
        return refuse(info, 'wrong_eboot',
                      f'The PKG says {found(info)} but its eboot.bin is not that game\'s executable '
                      f'(it may be patched or from another version): use the unpatched base game.')
    return accept(info, f'{NAME} ({found(info)}): a fake PKG of the supported game; eboot.bin '
                        f'matches.')


def check(path, eboot=False):
    """The result dict (FIELDS) for a game folder or a PKG file."""
    info = blank()
    path = Path(path)
    if not path.exists():
        return refuse(info, 'not_found', f'{path} does not exist. Pick the game folder or PKG file.')
    if path.is_dir():
        return check_folder(path, info)
    return check_pkg(path, info, eboot)


def default_data_dir():
    return Path(os.environ.get('S13_APP_DATA_DIR')
                or Path.home() / 'Library' / 'Application Support' / '13 Sentinels')


def pick_icon(game_dirs, data_dir=None):
    """The picture for the app's icon: the first sce_sys/icon0.png among the game folders, else the
    most recently modified save icon (<data>/user/savedata/*/CUSA19610/SAVEDATA*.sce_sys/icon0.png),
    else None."""
    for folder in game_dirs:
        icon = Path(folder) / 'sce_sys' / 'icon0.png'
        if icon.is_file():
            return icon
    data = Path(data_dir) if data_dir else default_data_dir()
    pattern = glob.escape(str(data)) + f'/user/savedata/*/{SUPPORTED_TITLE}/SAVEDATA*.sce_sys/icon0.png'
    icons = [Path(p) for p in glob.glob(pattern) if os.path.isfile(p)]
    return max(icons, key=lambda p: (p.stat().st_mtime, str(p)), default=None)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('path', nargs='?', help='a dumped game folder or a PS4 fake PKG')
    parser.add_argument('--json', action='store_true', help='print one JSON object')
    parser.add_argument('--eboot', action='store_true',
                        help='for a PKG: unpack eboot.bin and compare its image (needs cryptography)')
    parser.add_argument('--pick-icon', action='store_true',
                        help="print the app icon's picture (game folder, else save data), or nothing")
    parser.add_argument('--game', action='append', default=[], metavar='DIR',
                        help='with --pick-icon: a game folder to look in first (repeatable)')
    parser.add_argument('--data', metavar='DIR', help='with --pick-icon: the app data folder')
    args = parser.parse_args(argv)
    if args.pick_icon:
        if args.path or args.json or args.eboot:
            parser.error('--pick-icon takes only --game and --data')
        icon = pick_icon(args.game, args.data)
        if icon:
            print(icon)
        return 0
    if args.game or args.data:
        parser.error('--game and --data go with --pick-icon')
    if not args.path:
        parser.error('a game folder or PKG file is required')
    result = check(args.path, eboot=args.eboot)
    if args.json:
        print(json.dumps(result))
    elif result['ok']:
        print(f"Supported: {result['message']}")
    else:
        print(f"Not supported ({result['reason']}): {result['message']}")
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
