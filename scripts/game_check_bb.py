#!/usr/bin/env python3
"""Checks a game input for the Bloodborne port before it is installed: a dumped game folder or a PS4
fake PKG. The port runs Bloodborne with the 1.09 update merged in (any retail edition whose 1.09
executable is the supported one: scripts/game_check.py decides what that is).

  game_check_bb.py PATH [--json] [--eboot] [--deep]
  game_check_bb.py --pick-icon [--game DIR ...] [--data DIR]

Exit status: 0 supported, 1 refused, 2 usage error. --json prints one JSON object with the same
fields as game_check_13s.py: ok, reason (null when ok, else not_found, not_a_game, unreadable,
update_pkg, retail_pkg, other_title, wrong_eboot or needs_cryptography as there, plus missing_update
(1.00 game without the 1.09 update), patched_eboot (the 1.09 executable with a 60 fps patch baked in)
or damaged_files (--deep: shaders that do not unpack)), message (English: what is wrong and what to
do), input ("folder", "pkg" or null), title_id, title, version, category, content_id, image_checked,
has_icon. version is param.sfo's VERSION (real 1.09 dumps read APP_VER 01.00 and VERSION 01.09) and
for a folder whose executable matches, the supported 01.09.

A folder is checked down to eboot.bin's loaded image, which decides (as game_check.problem does).
--deep also unpacks the shader .dcx files the game loads first (game_check.broken_files: the
runtime's own check, under a second on a real dump; scripts/game_check.py GAME_DIR scans every .dcx
file, about 50 s). A PKG is read with the
standard library only (header, entry table, param.sfo; nothing is decrypted), so its image is
compared only with --eboot, which unpacks eboot.bin alone into a temporary folder (needs the
'cryptography' package, see pkg_extract.py). --deep does nothing for a PKG.

--pick-icon prints the picture for the app's icon: the first sce_sys/icon0.png of the --game
folders, else the newest save data's icon0.png under the data folder (--data; default
$BB_APP_DATA_DIR or ~/Library/Application Support/Bloodborne), else nothing.
"""
import argparse
import json
import os
import struct
import sys
from pathlib import Path

# Not on sys.path under `python3 -I` (which leaves the script's folder out).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import game_check  # noqa: E402
import game_check_13s as common  # noqa: E402  (PKG reading, result dict, icon search)
import prepare  # noqa: E402

NAME = 'Bloodborne'
# Bloodborne's saves live under this title ID whichever edition it is.
SAVE_TITLE = 'CUSA00207'
SUPPORTED_TITLES = ', '.join(game_check.SUPPORTED_TITLES)


def found(info):
    """'CUSA03173 version 01.00', for the messages."""
    return common.found(info)


def effective_version(values):
    """The game's version from param.sfo: VERSION (real 1.09 dumps read APP_VER 01.00 and VERSION
    01.09), 01.09 when either field says so."""
    app, version = str(values.get('APP_VER', '')), str(values.get('VERSION', ''))
    if game_check.SUPPORTED_VERSION in (app, version):
        return game_check.SUPPORTED_VERSION
    return version or app


def fill_from_sfo(info, values):
    common.fill_from_sfo(info, values)
    info['version'] = effective_version(values) or None


def title_of(content_id):
    """The title ID inside a content ID (EP9000-CUSA03173_00-...), or None."""
    part = (content_id or '')[7:16]
    return part if part[:4] == 'CUSA' else None


def judge_sfo(info, where, header_id=None):
    """The checks that only param.sfo's values (and a PKG's content IDs) decide, in order; a
    refusal, or None. The version is judged by the caller: a folder's executable decides it."""
    kind = 'game folder' if info['input'] == 'folder' else 'PKG'
    if not info['title_id'] or not info['version']:
        return common.refuse(info, 'unreadable',
                             f'{where} has no TITLE_ID and VERSION: this {kind} is not a complete PS4 '
                             f'game. Dump or download {NAME} again.')
    if info['category'] == 'gp' and info['input'] == 'pkg':
        # TODO: merge a base game PKG and its update PKG into one game folder (not done yet).
        return common.refuse(info, 'update_pkg',
                             f'This is a game update ({found(info)}), not the game. Pick the full '
                             f'{NAME} game PKG at version {game_check.SUPPORTED_VERSION} instead: '
                             f'merging a base game and its update is not supported yet.')
    title = info['title_id']
    # A PKG names its title twice more: the header's content ID and param.sfo's CONTENT_ID.
    others = [t for t in (title_of(info['content_id']), title_of(header_id)) if t]
    wrong = [t for t in [title] + others if t not in game_check.SUPPORTED_TITLES]
    if wrong:
        shown = wrong[0]
        return common.refuse(info, 'other_title',
                             f'This is {shown}'
                             f'{" (" + info["title"] + ")" if info["title"] else ""}, not a retail '
                             f'release of {NAME}: the port runs {NAME} (any region, The Old Hunters '
                             f'editions too), title IDs {SUPPORTED_TITLES}.')
    if info['input'] == 'pkg' and info['category'] != 'gd':
        return common.refuse(info, 'not_a_game',
                             f'This PKG is not a full game (category {info["category"] or "unknown"}). '
                             f'Pick the full {NAME} game PKG.')
    return None


def image_refusal(info, image, where):
    """The refusal for a loaded-image hash that is not the supported one: patched_eboot for the 60
    fps patched 1.09 executable, missing_update for a game that is not at 1.09, else wrong_eboot."""
    if image == game_check.LANCE_60FPS_IMAGE:
        return common.refuse(info, 'patched_eboot',
                             f'{where} has the 60 fps patch by Lance McDonald applied to its eboot.bin, '
                             f'which crashes the game when the gestures menu opens (the port has its '
                             f'own 60 fps): use a clean 1.09 eboot.bin.')
    if info['version'] != game_check.SUPPORTED_VERSION:
        if info['input'] == 'folder':
            return common.refuse(info, 'missing_update',
                                 f'sce_sys/param.sfo says {found(info)}: the port needs the 1.09 update '
                                 f'merged into the game folder. Copy everything from the dumped 1.09 '
                                 f'update (e.g. CUSA03173-patch) into the game folder, replacing '
                                 f'files (eboot.bin and sce_sys too).')
        return common.refuse(info, 'missing_update',
                             f'This is {found(info)}: the port needs version {game_check.SUPPORTED_VERSION}.'
                             f' Pick a full game PKG at that version (the 1.09 update merged in).')
    return common.refuse(info, 'wrong_eboot',
                         f'{where} says {found(info)} but its eboot.bin is not that update\'s '
                         f'executable (it may be from the base game or another version): use the '
                         f'eboot.bin from the dumped 1.09 update.')


def check_folder(folder, info, deep):
    info['input'] = 'folder'
    eboot, param = folder / 'eboot.bin', folder / 'sce_sys' / 'param.sfo'
    for needed in (eboot, param, folder / 'dvdroot_ps4'):
        if not needed.exists():
            return common.refuse(info, 'not_a_game',
                                 f'{folder} is not a dumped PS4 game: {needed.relative_to(folder)} is '
                                 f'missing. Pick the folder that holds eboot.bin, sce_sys and '
                                 f'dvdroot_ps4 (the "uroot" folder of a dump).')
    info['has_icon'] = (folder / 'sce_sys' / 'icon0.png').is_file()
    try:
        values = prepare.sfo(param.read_bytes())
    except Exception as error:  # a damaged file raises struct.error, ValueError, IndexError...
        return common.refuse(info, 'unreadable',
                             f'sce_sys/param.sfo could not be read ({error}): dump the game again.')
    fill_from_sfo(info, values)
    # The executable decides, as in game_check.problem(): the supported image is accepted whatever
    # param.sfo says, the 60 fps patched one is named as such, and only then does the title count.
    try:
        image = game_check.image_sha256(folder)
    except Exception as error:
        refused = judge_sfo(info, 'sce_sys/param.sfo')
        if refused:
            return refused
        return common.refuse(info, 'unreadable',
                             f'eboot.bin could not be read as a decrypted PS4 executable ({error}): '
                             f'dump the game and the 1.09 update again.')
    info['image_checked'] = True
    if image != game_check.SUPPORTED_IMAGE:
        if image != game_check.LANCE_60FPS_IMAGE:
            refused = judge_sfo(info, 'sce_sys/param.sfo')
            if refused:
                return refused
        return image_refusal(info, image, 'This game folder')
    info['version'] = game_check.SUPPORTED_VERSION
    if deep:
        try:
            broken = game_check.broken_files(folder)
        except OSError as error:
            return common.refuse(info, 'unreadable',
                                 f'The game files could not be read ({error}): dump the game again.')
        if broken:
            return common.refuse(info, 'damaged_files',
                                 f'The game files are damaged: {len(broken)} shader files in '
                                 f'{game_check.CHECKED_FOLDER} do not unpack (first: {broken[0]}), and '
                                 f'the game would hang while loading them. The extraction tool left '
                                 f'parts of compressed sectors stale: extract the game and the 1.09 '
                                 f'update again with a fixed tool.')
        return common.accept(info, f'{NAME} ({found(info)}): game folder, eboot.bin and shader files '
                                   f'check out.')
    return common.accept(info, f'{NAME} ({found(info)}): game folder and eboot.bin match. The shader '
                               f'files are checked while the game is installed.')


def check_pkg(path, info, eboot):
    info['input'] = 'pkg'
    try:
        with open(path, 'rb') as f:
            magic = f.read(4)
    except OSError as error:
        return common.refuse(info, 'unreadable', f'{path} could not be read ({error}).')
    if len(magic) < 4 or struct.unpack('>I', magic)[0] != common.PKG_MAGIC:
        return common.refuse(info, 'not_a_game',
                             f'{path.name} is neither a dumped game folder nor a PS4 PKG. Pick the '
                             f'game folder (the one that holds eboot.bin and sce_sys) or a .pkg file.')
    try:
        content_id, entries, sfo_data = common.read_pkg(path)
    except (OSError, ValueError, struct.error) as error:
        return common.refuse(info, 'unreadable',
                             f'The PKG could not be read ({error}): download or copy it again.')
    info['content_id'] = content_id or None
    info['has_icon'] = common.ENTRY_ICON0 in entries
    values = {}
    sfo_error = None
    if common.ENTRY_PARAM_SFO not in entries:
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

    key = entries.get(common.ENTRY_IMAGE_KEY)
    if key is None:
        return common.refuse(info, 'unreadable', 'The PKG has no image key entry: it is not a complete '
                                                 'PS4 package. Download or copy it again.')
    if (key['flags2'] >> 12) & 0xF != common.FAKE_IMAGE_KEY_INDEX:
        return common.refuse(info, 'retail_pkg',
                             'This is a retail PKG: unpacking it needs a console\'s keys, which this '
                             'port does not have. Use a dumped game folder, or a fake PKG made from '
                             'a dump.')
    if common.ENTRY_TARGET_DELTA in entries or common.ENTRY_ORIGIN_DELTA in entries:
        # TODO: merge a base game PKG and its update PKG into one game folder (not done yet).
        return common.refuse(info, 'update_pkg',
                             f'This PKG is a game update (it holds delta info). Pick the full {NAME} '
                             f'game PKG at version {game_check.SUPPORTED_VERSION} instead: merging a '
                             f'base game and its update is not supported yet.')
    if sfo_error:
        return common.refuse(info, 'unreadable', f'The PKG could not be read ({sfo_error}): download or '
                                                 f'copy it again.')
    refused = judge_sfo(info, 'The PKG\'s param.sfo', header_id=content_id)
    if refused:
        return refused
    if info['version'] != game_check.SUPPORTED_VERSION:
        return image_refusal(info, None, 'This PKG')
    if not eboot:
        return common.accept(info, f'{NAME} ({found(info)}): a fake PKG of the supported game. '
                                   f'eboot.bin is compared when it is unpacked.')
    image, refused = common.pkg_image(path, info)
    if refused:
        return refused
    if image != game_check.SUPPORTED_IMAGE:
        return image_refusal(info, image, 'The PKG')
    return common.accept(info, f'{NAME} ({found(info)}): a fake PKG of the supported game; eboot.bin '
                               f'matches.')


def check(path, eboot=False, deep=False):
    """The result dict (common.FIELDS) for a game folder or a PKG file."""
    info = common.blank()
    path = Path(path)
    if not path.exists():
        return common.refuse(info, 'not_found', f'{path} does not exist. Pick the game folder or PKG file.')
    if path.is_dir():
        return check_folder(path, info, deep)
    return check_pkg(path, info, eboot)


def default_data_dir():
    return Path(os.environ.get('BB_APP_DATA_DIR')
                or Path.home() / 'Library' / 'Application Support' / 'Bloodborne')


def pick_icon(game_dirs, data_dir=None):
    """The picture for the app's icon: the first sce_sys/icon0.png among the game folders, else the
    most recently modified save icon (<data>/user/savedata/*/CUSA00207/*.sce_sys/icon0.png), else
    None."""
    return common.find_icon(game_dirs, Path(data_dir) if data_dir else default_data_dir(), SAVE_TITLE,
                            slot='*')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('path', nargs='?', help='a dumped game folder or a PS4 fake PKG')
    parser.add_argument('--json', action='store_true', help='print one JSON object')
    parser.add_argument('--eboot', action='store_true',
                        help='for a PKG: unpack eboot.bin and compare its image (needs cryptography)')
    parser.add_argument('--deep', action='store_true',
                        help='for a folder: also unpack the shader .dcx files (the runtime\'s own check)')
    parser.add_argument('--pick-icon', action='store_true',
                        help="print the app icon's picture (game folder, else save data), or nothing")
    parser.add_argument('--game', action='append', default=[], metavar='DIR',
                        help='with --pick-icon: a game folder to look in first (repeatable)')
    parser.add_argument('--data', metavar='DIR', help='with --pick-icon: the app data folder')
    args = parser.parse_args(argv)
    if args.pick_icon:
        if args.path or args.json or args.eboot or args.deep:
            parser.error('--pick-icon takes only --game and --data')
        icon = pick_icon(args.game, args.data)
        if icon:
            print(icon)
        return 0
    if args.game or args.data:
        parser.error('--game and --data go with --pick-icon')
    if not args.path:
        parser.error('a game folder or PKG file is required')
    result = check(args.path, eboot=args.eboot, deep=args.deep)
    if args.json:
        print(json.dumps(result))
    elif result['ok']:
        print(f"Supported: {result['message']}")
    else:
        print(f"Not supported ({result['reason']}): {result['message']}")
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    sys.exit(main())
