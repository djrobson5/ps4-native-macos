"""scripts/game_check_bb.py: the pre-install check of a Bloodborne game folder or fake PKG (synthetic
param.sfo, PKG headers and eboot.bin: no game data), the app icon's order and the constants shared
with game_check.py and the 13 Sentinels checker.
Run: python3 -I tests/test_game_check_bb.py  (unittest discover runs it too)"""
import json
import os
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

# Not tests/paths.py: -I leaves the tests folder off sys.path.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'tests'))
import game_check  # noqa: E402
import game_check_13s  # noqa: E402
import game_check_bb as bb  # noqa: E402
from test_game_check_13s import param_sfo, pkg as build_pkg  # noqa: E402  (synthetic builders)

SCRIPT = ROOT / 'scripts' / 'game_check_bb.py'
FIELDS = {'ok', 'reason', 'message', 'input', 'title_id', 'title', 'version', 'category',
          'content_id', 'image_checked', 'has_icon'}
CONTENT_ID = 'EP9000-CUSA03173_00-BLOODBORNE0000EU'
# What a real 1.09 dump reads: APP_VER 01.00, VERSION 01.09.
GOOD = {'TITLE_ID': 'CUSA03173', 'TITLE': 'Bloodborne', 'APP_VER': '01.00', 'VERSION': '01.09',
        'CATEGORY': 'gd', 'CONTENT_ID': CONTENT_ID}
BASE = {**GOOD, 'VERSION': '01.00'}


def pkg(sfo=None, **kwargs):
    return build_pkg(sfo=GOOD if sfo is None else sfo, content_id=kwargs.pop('content_id', CONTENT_ID), **kwargs)


class Temp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def folder(self, sfo=None, eboot=True, icon=False, data=True, name='game'):
        path = self.root / name
        (path / 'sce_sys').mkdir(parents=True, exist_ok=True)
        if sfo is not False:
            (path / 'sce_sys' / 'param.sfo').write_bytes(param_sfo(GOOD if sfo is None else sfo))
        if eboot:
            (path / 'eboot.bin').write_bytes(b'not a real eboot')
        if data:
            (path / 'dvdroot_ps4' / 'shader').mkdir(parents=True, exist_ok=True)
        if icon:
            (path / 'sce_sys' / 'icon0.png').write_bytes(b'png')
        return path

    def pkg(self, **kwargs):
        path = self.root / 'game.pkg'
        path.write_bytes(pkg(**kwargs))
        return path


class Folder(Temp):
    def run_check(self, path, image=game_check.SUPPORTED_IMAGE, deep=False, broken=()):
        with mock.patch.object(game_check, 'image_sha256', return_value=image), \
                mock.patch.object(game_check, 'broken_files', return_value=list(broken)) as shaders:
            result = bb.check(path, deep=deep)
        self.shaders_checked = shaders.called
        return result

    def test_supported(self):
        result = self.run_check(self.folder())
        self.assertTrue(result['ok'], result)
        self.assertIsNone(result['reason'])
        self.assertEqual((result['input'], result['title_id'], result['version'], result['category']),
                         ('folder', 'CUSA03173', '01.09', 'gd'))
        self.assertEqual(result['content_id'], CONTENT_ID)
        self.assertEqual((result['image_checked'], result['has_icon']), (True, False))
        self.assertTrue(result['message'])
        self.assertFalse(self.shaders_checked, 'the slow shader check runs only with --deep')

    def test_the_executable_decides_the_version(self):
        # A 1.09 executable under an APP_VER 01.00 / VERSION 01.00 param.sfo is accepted, as game_check.
        result = self.run_check(self.folder(sfo=BASE))
        self.assertTrue(result['ok'], result)
        self.assertEqual(result['version'], '01.09')

    def test_every_supported_title(self):
        for title in game_check.SUPPORTED_TITLES:
            with self.subTest(title=title):
                self.assertTrue(self.run_check(self.folder(sfo={**GOOD, 'TITLE_ID': title}))['ok'])

    def test_icon(self):
        self.assertTrue(self.run_check(self.folder(icon=True))['has_icon'])

    def test_other_title(self):
        result = self.run_check(self.folder(sfo={**GOOD, 'TITLE_ID': 'CUSA19610'}), image='0' * 64)
        self.assertEqual((result['ok'], result['reason'], result['image_checked']), (False, 'other_title', True))
        self.assertIn('CUSA03173', result['message'])

    def test_the_executable_decides_over_the_title(self):
        # As game_check.problem(): the supported image is accepted whatever param.sfo names.
        result = self.run_check(self.folder(sfo={**GOOD, 'TITLE_ID': 'CUSA19610'}))
        self.assertEqual((result['ok'], result['image_checked']), (True, True))

    def test_patched_eboot_is_named_before_the_title(self):
        result = self.run_check(self.folder(sfo={**GOOD, 'TITLE_ID': 'CUSA19610'}), image=game_check.LANCE_60FPS_IMAGE)
        self.assertEqual(result['reason'], 'patched_eboot')

    def test_missing_update(self):
        result = self.run_check(self.folder(sfo=BASE), image='0' * 64)
        self.assertEqual((result['ok'], result['reason'], result['version'], result['image_checked']),
                         (False, 'missing_update', '01.00', True))
        self.assertIn('1.09', result['message'])

    def test_wrong_eboot(self):
        # param.sfo from the 1.09 update, eboot.bin from the base game.
        result = self.run_check(self.folder(), image='0' * 64)
        self.assertEqual((result['ok'], result['reason'], result['image_checked']), (False, 'wrong_eboot', True))

    def test_patched_eboot(self):
        result = self.run_check(self.folder(), image=game_check.LANCE_60FPS_IMAGE)
        self.assertEqual((result['ok'], result['reason'], result['image_checked']), (False, 'patched_eboot', True))
        self.assertIn('60 fps', result['message'])

    def test_deep_runs_the_shader_check(self):
        result = self.run_check(self.folder(), deep=True)
        self.assertTrue(result['ok'], result)
        self.assertTrue(self.shaders_checked)

    def test_damaged_files(self):
        result = self.run_check(self.folder(), deep=True, broken=['dvdroot_ps4/shader/a.dcx'])
        self.assertEqual((result['ok'], result['reason'], result['image_checked']), (False, 'damaged_files', True))
        self.assertIn('a.dcx', result['message'])

    def test_damaged_files_are_not_looked_for_without_deep(self):
        self.assertTrue(self.run_check(self.folder(), broken=['dvdroot_ps4/shader/a.dcx'])['ok'])

    def test_deep_on_real_dcx_files(self):
        folder = self.folder()
        sys.path.insert(0, str(ROOT / 'tests'))
        from test_game_check import dcx
        (folder / 'dvdroot_ps4' / 'shader' / 'ok.dcx').write_bytes(dcx(b'shader' * 100))
        with mock.patch.object(game_check, 'image_sha256', return_value=game_check.SUPPORTED_IMAGE):
            self.assertTrue(bb.check(folder, deep=True)['ok'])
            (folder / 'dvdroot_ps4' / 'shader' / 'bad.dcx').write_bytes(dcx(b'shader' * 100, packed=b'junk'))
            self.assertEqual(bb.check(folder, deep=True)['reason'], 'damaged_files')

    def test_missing_eboot(self):
        self.assertEqual(self.run_check(self.folder(eboot=False))['reason'], 'not_a_game')

    def test_missing_param_sfo(self):
        self.assertEqual(self.run_check(self.folder(sfo=False))['reason'], 'not_a_game')

    def test_missing_game_data(self):
        self.assertEqual(self.run_check(self.folder(data=False))['reason'], 'not_a_game')

    def test_damaged_param_sfo(self):
        folder = self.folder()
        (folder / 'sce_sys' / 'param.sfo').write_bytes(b'junk')
        self.assertEqual(self.run_check(folder)['reason'], 'unreadable')

    def test_param_sfo_without_a_title(self):
        self.assertEqual(self.run_check(self.folder(sfo={'CATEGORY': 'gd'}), image='0' * 64)['reason'], 'unreadable')

    def test_unreadable_eboot(self):
        with mock.patch.object(game_check, 'image_sha256', side_effect=ValueError('bad SELF')):
            result = bb.check(self.folder())
        self.assertEqual((result['reason'], result['image_checked']), ('unreadable', False))

    def test_real_eboot_parser_rejects_a_dummy(self):
        self.assertEqual(bb.check(self.folder())['reason'], 'unreadable')

    def test_not_found(self):
        self.assertEqual(bb.check(self.root / 'nope')['reason'], 'not_found')

    def test_a_file_that_is_not_a_pkg(self):
        path = self.root / 'a.txt'
        path.write_text('hello')
        self.assertEqual(bb.check(path)['reason'], 'not_a_game')

    def test_skip_variable_does_not_skip_the_install_check(self):
        with mock.patch.dict(os.environ, {'BB_SKIP_GAME_CHECK': '1'}):
            self.assertEqual(self.run_check(self.folder(), image='0' * 64)['reason'], 'wrong_eboot')


class Pkg(Temp):
    def test_full_game_pkg(self):
        result = bb.check(self.pkg())
        self.assertTrue(result['ok'], result)
        self.assertEqual((result['input'], result['title_id'], result['version'], result['category'],
                          result['image_checked'], result['has_icon']),
                         ('pkg', 'CUSA03173', '01.09', 'gd', False, False))
        self.assertEqual(result['content_id'], CONTENT_ID)

    def test_every_supported_title(self):
        for title in game_check.SUPPORTED_TITLES:
            with self.subTest(title=title):
                content_id = f'EP9000-{title}_00-BLOODBORNE0000EU'
                path = self.pkg(sfo={**GOOD, 'TITLE_ID': title, 'CONTENT_ID': content_id}, content_id=content_id)
                self.assertTrue(bb.check(path)['ok'])

    def test_app_ver_alone_is_enough(self):
        sfo = {k: v for k, v in GOOD.items() if k != 'VERSION'} | {'APP_VER': '01.09'}
        self.assertTrue(bb.check(self.pkg(sfo=sfo))['ok'])

    def test_base_game_without_the_update(self):
        result = bb.check(self.pkg(sfo=BASE))
        self.assertEqual((result['ok'], result['reason'], result['version']), (False, 'missing_update', '01.00'))
        self.assertIn('01.09', result['message'])

    def test_update_by_category(self):
        result = bb.check(self.pkg(sfo={**GOOD, 'CATEGORY': 'gp'}))
        self.assertEqual(result['reason'], 'update_pkg')
        self.assertIn('base game', result['message'])

    def test_update_by_delta_info(self):
        for entry in (0x407, 0x408):
            with self.subTest(entry=hex(entry)):
                self.assertEqual(bb.check(self.pkg(extra=[entry]))['reason'], 'update_pkg')

    def test_retail(self):
        for index in (0, 1, 2, 4):
            with self.subTest(index=index):
                self.assertEqual(bb.check(self.pkg(index=index))['reason'], 'retail_pkg')

    def test_other_title(self):
        content_id = 'UP0177-CUSA19610_00-13SENTINELSEFIGS'
        result = bb.check(self.pkg(sfo={**GOOD, 'TITLE_ID': 'CUSA19610', 'CONTENT_ID': content_id}, content_id=content_id))
        self.assertEqual(result['reason'], 'other_title')
        self.assertIn('CUSA19610', result['message'])

    def test_content_id_of_another_title(self):
        result = bb.check(self.pkg(content_id='UP0177-CUSA19610_00-13SENTINELSEFIGS'))
        self.assertEqual(result['reason'], 'other_title')

    def test_not_a_full_game(self):
        self.assertEqual(bb.check(self.pkg(sfo={**GOOD, 'CATEGORY': 'ac'}))['reason'], 'not_a_game')

    def test_bad_magic(self):
        self.assertEqual(bb.check(self.pkg(magic=0x12345678))['reason'], 'not_a_game')

    def test_empty_file(self):
        path = self.root / 'empty.pkg'
        path.write_bytes(b'')
        self.assertEqual(bb.check(path)['reason'], 'not_a_game')

    def test_truncated_table(self):
        path = self.root / 'cut.pkg'
        path.write_bytes(pkg()[:0x1000 + 40])
        self.assertEqual(bb.check(path)['reason'], 'unreadable')

    def test_encrypted_param_sfo(self):
        self.assertEqual(bb.check(self.pkg(encrypted_sfo=True))['reason'], 'unreadable')

    def test_icon_entry(self):
        self.assertTrue(bb.check(self.pkg(extra=[0x1200]))['has_icon'])

    def test_header_checks_do_not_need_cryptography(self):
        with mock.patch.dict(sys.modules, {'cryptography': None}):
            self.assertTrue(bb.check(self.pkg())['ok'])

    def test_deep_does_nothing_for_a_pkg(self):
        self.assertTrue(bb.check(self.pkg(), deep=True)['ok'])


class PkgEboot(Temp):
    """--eboot with pkg_extract replaced by a stand-in that writes the eboot.bin (no real PKG)."""

    def run_eboot(self, image, **kwargs):
        module = types.ModuleType('pkg_extract')
        calls = {}

        class Pkg:
            def __init__(self, path):
                self.f = open(path, 'rb')

        def extract(package, out, progress=None, items=None, only=None):
            calls.update(out=out, only=only)
            (Path(out) / 'eboot.bin').write_bytes(b'x')

        module.Pkg, module.extract = Pkg, extract
        with mock.patch.dict(sys.modules, {'pkg_extract': module, 'cryptography': types.ModuleType('cryptography')}), \
                mock.patch.object(game_check, 'image_sha256', return_value=image):
            result = bb.check(self.pkg(**kwargs), eboot=True)
        self.assertEqual(calls['only'], ['eboot.bin'])
        self.assertFalse(os.path.exists(calls['out']), 'the temporary folder is deleted')
        return result

    def test_matching_image(self):
        result = self.run_eboot(game_check.SUPPORTED_IMAGE)
        self.assertTrue(result['ok'], result)
        self.assertTrue(result['image_checked'])

    def test_other_image(self):
        result = self.run_eboot('1' * 64)
        self.assertEqual((result['reason'], result['image_checked']), ('wrong_eboot', True))

    def test_patched_image(self):
        result = self.run_eboot(game_check.LANCE_60FPS_IMAGE)
        self.assertEqual((result['reason'], result['image_checked']), ('patched_eboot', True))

    def test_needs_cryptography(self):
        with mock.patch.dict(sys.modules, {'cryptography': None}):
            result = bb.check(self.pkg(), eboot=True)
        self.assertEqual((result['ok'], result['reason'], result['image_checked']), (False, 'needs_cryptography', False))

    def test_header_refusal_comes_first(self):
        with mock.patch.dict(sys.modules, {'cryptography': None}):
            self.assertEqual(bb.check(self.pkg(index=0), eboot=True)['reason'], 'retail_pkg')
            self.assertEqual(bb.check(self.pkg(sfo=BASE), eboot=True)['reason'], 'missing_update')

    def test_unpack_failure(self):
        module = types.ModuleType('pkg_extract')
        module.Pkg = mock.Mock()
        module.extract = mock.Mock(side_effect=ValueError('RSA padding check failed'))
        with mock.patch.dict(sys.modules, {'pkg_extract': module, 'cryptography': types.ModuleType('cryptography')}):
            self.assertEqual(bb.check(self.pkg(), eboot=True)['reason'], 'unreadable')


class Cli(Temp):
    def run_cli(self, *args, env=None):
        return subprocess.run([sys.executable, '-I', str(SCRIPT), *map(str, args)], capture_output=True,
                              text=True, env=env)

    def test_json_has_every_field(self):
        result = self.run_cli(self.pkg(), '--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(set(data), FIELDS)
        self.assertTrue(data['ok'])

    def test_same_fields_as_the_13s_checker(self):
        self.assertEqual(set(game_check_13s.FIELDS), FIELDS)
        self.assertEqual(set(bb.common.blank()), FIELDS)

    def test_refused_exit_status_and_json(self):
        result = self.run_cli(self.root / 'nope', '--json')
        self.assertEqual(result.returncode, 1)
        data = json.loads(result.stdout)
        self.assertEqual((set(data), data['reason'], data['input']), (FIELDS, 'not_found', None))

    def test_text_output(self):
        ok = self.run_cli(self.pkg())
        self.assertEqual(ok.returncode, 0)
        self.assertIn('Supported', ok.stdout)
        refused = self.run_cli(self.root / 'nope')
        self.assertEqual(refused.returncode, 1)
        self.assertIn('not_found', refused.stdout)

    def test_deep_flag_is_accepted(self):
        folder = self.root / 'g'
        (folder / 'sce_sys').mkdir(parents=True)
        self.assertEqual(self.run_cli(folder, '--deep', '--json').returncode, 1)

    def test_usage_errors(self):
        for args in ([], ['--bogus'], ['--pick-icon', 'x'], ['--pick-icon', '--deep'], ['--game', 'x', 'y']):
            with self.subTest(args=args):
                self.assertEqual(self.run_cli(*args).returncode, 2)

    def test_pick_icon_cli(self):
        folder = self.folder(icon=True)
        result = self.run_cli('--pick-icon', '--game', folder, '--data', self.root / 'empty')
        self.assertEqual((result.returncode, result.stdout.strip()), (0, str(folder / 'sce_sys' / 'icon0.png')))
        result = self.run_cli('--pick-icon', '--game', self.root / 'none', '--data', self.root / 'empty')
        self.assertEqual((result.returncode, result.stdout), (0, ''))

    def test_pick_icon_data_default_from_the_environment(self):
        icon = self.root / 'data/user/savedata/1/CUSA00207/sdimg_SPRJ0005.sce_sys/icon0.png'
        icon.parent.mkdir(parents=True)
        icon.write_bytes(b'png')
        result = self.run_cli('--pick-icon', env={**os.environ, 'BB_APP_DATA_DIR': str(self.root / 'data')})
        self.assertEqual(result.stdout.strip(), str(icon))

    folder = Temp.folder


class PickIcon(Temp):
    def save_icon(self, user, name, mtime, title='CUSA00207'):
        icon = self.root / f'data/user/savedata/{user}/{title}/{name}.sce_sys/icon0.png'
        icon.parent.mkdir(parents=True, exist_ok=True)
        icon.write_bytes(b'png')
        os.utime(icon, (mtime, mtime))
        return icon

    def test_game_icon_beats_save_icon(self):
        self.save_icon(1, 'sdimg_SPRJ0005', 1000)
        game = self.folder(icon=True)
        self.assertEqual(bb.pick_icon([game], self.root / 'data'), game / 'sce_sys' / 'icon0.png')

    def test_newest_save_icon(self):
        self.save_icon(1, 'sdimg_SPRJ0005', 1000)
        newest = self.save_icon(2, 'SAVEDATA03', 3000)
        self.save_icon(1, 'SAVEDATA01', 2000)
        self.save_icon(1, 'SAVEDATA02', 9999, title='CUSA19610')  # another game's save
        self.assertEqual(bb.pick_icon([self.folder()], self.root / 'data'), newest)

    def test_nothing(self):
        self.assertIsNone(bb.pick_icon([self.folder()], self.root / 'data'))
        self.assertIsNone(bb.pick_icon([], self.root / 'nowhere'))

    def test_data_folder_default(self):
        icon = self.save_icon(1, 'sdimg_SPRJ0005', 1000)
        with mock.patch.dict(os.environ, {'BB_APP_DATA_DIR': str(self.root / 'data')}):
            self.assertEqual(bb.pick_icon([]), icon)
        with mock.patch.dict(os.environ, {'BB_APP_DATA_DIR': ''}), mock.patch.object(Path, 'home', return_value=self.root):
            self.assertEqual(bb.default_data_dir(), self.root / 'Library/Application Support/Bloodborne')

    folder = Temp.folder


class Constants(unittest.TestCase):
    def test_what_bloodborne_accepts_comes_from_game_check(self):
        self.assertIs(bb.game_check, game_check)
        self.assertEqual(game_check.SUPPORTED_VERSION, '01.09')
        self.assertIn('CUSA03173', game_check.SUPPORTED_TITLES)
        self.assertEqual(len(game_check.SUPPORTED_IMAGE), 64)

    def test_save_title(self):
        self.assertEqual(bb.SAVE_TITLE, 'CUSA00207')
        self.assertIn(bb.SAVE_TITLE, game_check.SUPPORTED_TITLES)


if __name__ == '__main__':
    unittest.main()
