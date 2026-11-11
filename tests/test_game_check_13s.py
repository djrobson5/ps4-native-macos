"""scripts/game_check_13s.py: the pre-install check of a dumped game folder or a fake PKG (synthetic
param.sfo, PKG headers and eboot.bin: no game data), the app icon's order, and the keysets of
pkg_keys.py (skipped without the 'cryptography' package).
Run: python3 tests/test_game_check_13s.py  (ctest runs it too; so does unittest discover)"""
import importlib.util
import json
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

# Not tests/paths.py: ctest runs this with -I, which leaves the tests folder off sys.path.
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import game_check  # noqa: E402
import game_check_13s as check13  # noqa: E402

SCRIPT = ROOT / 'scripts' / 'game_check_13s.py'
FIELDS = {'ok', 'reason', 'message', 'input', 'title_id', 'title', 'version', 'category',
          'content_id', 'image_checked', 'has_icon'}
GOOD = {'TITLE_ID': 'CUSA19610', 'TITLE': '13 Sentinels: Aegis Rim', 'APP_VER': '01.00',
        'VERSION': '01.00', 'CATEGORY': 'gd', 'CONTENT_ID': 'UP0177-CUSA19610_00-13SENTINELSEFIGS'}


def param_sfo(entries):
    """A minimal param.sfo with UTF-8 string entries."""
    keys = b''.join(k.encode() + b'\0' for k in entries)
    values = [v.encode() + b'\0' for v in entries.values()]
    count = len(entries)
    key_table = 20 + 16 * count
    data_table = key_table + len(keys)
    data = bytearray(struct.pack('<4sIIII', b'\0PSF', 0x101, key_table, data_table, count))
    key_off = data_off = 0
    for key, value in zip(entries, values):
        data += struct.pack('<HHIII', key_off, 0x204, len(value), len(value), data_off)
        key_off += len(key) + 1
        data_off += len(value)
    return bytes(data + keys + b''.join(values))


def pkg(sfo=None, index=3, extra=(), encrypted_sfo=False, magic=0x7F434E54, content_id=GOOD['CONTENT_ID']):
    """A PKG: the 0x1000-byte header, the entry table, then the entries' data. Entry ids in `extra`
    are empty entries; 0x1200 is the icon."""
    sfo = param_sfo(GOOD if sfo is None else sfo)
    entries = [(0x20, 0, 0, index << 12, b'\0' * 16),
               (0x1000, 0, 0x80000000 if encrypted_sfo else 0, 0, sfo)]
    entries += [(entry_id, 0, 0, 0, b'\0' * 8) for entry_id in extra]
    header = bytearray(0x1000)
    struct.pack_into('>I', header, 0, magic)
    struct.pack_into('>I', header, 0x10, len(entries))
    struct.pack_into('>I', header, 0x18, 0x1000)
    header[0x40:0x40 + len(content_id)] = content_id.encode()
    table, data = bytearray(), bytearray()
    offset = 0x1000 + 32 * len(entries)
    for entry_id, name, flags1, flags2, payload in entries:
        table += struct.pack('>IIIIII8x', entry_id, name, flags1, flags2, offset + len(data), len(payload))
        data += payload
    return bytes(header + table + data)


class Temp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def folder(self, sfo=None, eboot=True, icon=False, name='game'):
        path = self.root / name
        (path / 'sce_sys').mkdir(parents=True, exist_ok=True)
        if sfo is not False:
            (path / 'sce_sys' / 'param.sfo').write_bytes(param_sfo(GOOD if sfo is None else sfo))
        if eboot:
            (path / 'eboot.bin').write_bytes(b'not a real eboot')
        if icon:
            (path / 'sce_sys' / 'icon0.png').write_bytes(b'png')
        return path

    def pkg(self, **kwargs):
        path = self.root / 'game.pkg'
        path.write_bytes(pkg(**kwargs))
        return path


class Folder(Temp):
    def run_check(self, path, image=check13.SUPPORTED_IMAGE_13S):
        with mock.patch.object(game_check, 'image_sha256', return_value=image):
            return check13.check(path)

    def test_supported(self):
        result = self.run_check(self.folder())
        self.assertTrue(result['ok'], result)
        self.assertIsNone(result['reason'])
        self.assertEqual((result['input'], result['title_id'], result['version'], result['category']),
                         ('folder', 'CUSA19610', '01.00', 'gd'))
        self.assertEqual(result['content_id'], GOOD['CONTENT_ID'])
        self.assertTrue(result['image_checked'])
        self.assertFalse(result['has_icon'])
        self.assertTrue(result['message'])

    def test_icon(self):
        self.assertTrue(self.run_check(self.folder(icon=True))['has_icon'])

    def test_other_title(self):
        result = self.run_check(self.folder(sfo={**GOOD, 'TITLE_ID': 'CUSA19611'}))
        self.assertEqual((result['ok'], result['reason'], result['image_checked']), (False, 'other_title', False))
        self.assertIn('CUSA19610', result['message'])

    def test_other_version(self):
        result = self.run_check(self.folder(sfo={**GOOD, 'APP_VER': '01.01'}))
        self.assertEqual((result['ok'], result['reason'], result['version']), (False, 'other_version', '01.01'))

    def test_update(self):
        result = self.run_check(self.folder(sfo={**GOOD, 'CATEGORY': 'gp', 'APP_VER': '01.01'}))
        self.assertEqual(result['reason'], 'update_pkg')

    def test_missing_eboot(self):
        self.assertEqual(self.run_check(self.folder(eboot=False))['reason'], 'not_a_game')

    def test_missing_param_sfo(self):
        self.assertEqual(self.run_check(self.folder(sfo=False))['reason'], 'not_a_game')

    def test_damaged_param_sfo(self):
        folder = self.folder()
        (folder / 'sce_sys' / 'param.sfo').write_bytes(b'junk')
        self.assertEqual(self.run_check(folder)['reason'], 'unreadable')

    def test_wrong_eboot(self):
        result = self.run_check(self.folder(), image='0' * 64)
        self.assertEqual((result['ok'], result['reason'], result['image_checked']), (False, 'wrong_eboot', True))

    def test_unreadable_eboot(self):
        with mock.patch.object(game_check, 'image_sha256', side_effect=ValueError('bad SELF')):
            result = check13.check(self.folder())
        self.assertEqual((result['reason'], result['image_checked']), ('unreadable', False))

    def test_real_eboot_parser_rejects_a_dummy(self):
        # Without the mock the dummy eboot.bin must be refused, not crash.
        self.assertEqual(check13.check(self.folder())['reason'], 'unreadable')

    def test_not_found(self):
        self.assertEqual(check13.check(self.root / 'nope')['reason'], 'not_found')

    def test_a_file_that_is_not_a_pkg(self):
        path = self.root / 'a.txt'
        path.write_text('hello')
        self.assertEqual(check13.check(path)['reason'], 'not_a_game')


class Pkg(Temp):
    def test_fake_base_pkg(self):
        result = check13.check(self.pkg())
        self.assertTrue(result['ok'], result)
        self.assertEqual((result['input'], result['title_id'], result['version'], result['category'],
                          result['image_checked'], result['has_icon']),
                         ('pkg', 'CUSA19610', '01.00', 'gd', False, False))
        self.assertEqual(result['content_id'], GOOD['CONTENT_ID'])

    def test_retail(self):
        for index in (0, 1, 2, 4):
            with self.subTest(index=index):
                self.assertEqual(check13.check(self.pkg(index=index))['reason'], 'retail_pkg')

    def test_update_by_category(self):
        self.assertEqual(check13.check(self.pkg(sfo={**GOOD, 'CATEGORY': 'gp'}))['reason'], 'update_pkg')

    def test_update_by_delta_info(self):
        for entry in (0x407, 0x408):
            with self.subTest(entry=hex(entry)):
                self.assertEqual(check13.check(self.pkg(extra=[entry]))['reason'], 'update_pkg')

    def test_other_title(self):
        self.assertEqual(check13.check(self.pkg(sfo={**GOOD, 'TITLE_ID': 'CUSA19611'}))['reason'], 'other_title')

    def test_other_version(self):
        self.assertEqual(check13.check(self.pkg(sfo={**GOOD, 'APP_VER': '01.02'}))['reason'], 'other_version')

    def test_bad_magic(self):
        self.assertEqual(check13.check(self.pkg(magic=0x12345678))['reason'], 'not_a_game')

    def test_empty_file(self):
        path = self.root / 'empty.pkg'
        path.write_bytes(b'')
        self.assertEqual(check13.check(path)['reason'], 'not_a_game')

    def test_truncated_table(self):
        path = self.root / 'cut.pkg'
        path.write_bytes(pkg()[:0x1000 + 40])
        self.assertEqual(check13.check(path)['reason'], 'unreadable')

    def test_encrypted_param_sfo(self):
        self.assertEqual(check13.check(self.pkg(encrypted_sfo=True))['reason'], 'unreadable')

    def test_garbled_param_sfo(self):
        path = self.root / 'bad.pkg'
        data = bytearray(pkg())
        data[0x1000 + 64 + 16:0x1000 + 64 + 20] = b'XXXX'  # the param.sfo's signature (after 2 table rows and the 16-byte image key)
        path.write_bytes(data)
        self.assertEqual(check13.check(path)['reason'], 'unreadable')

    def test_icon_entry(self):
        self.assertTrue(check13.check(self.pkg(extra=[0x1200]))['has_icon'])

    def test_header_checks_do_not_need_cryptography(self):
        with mock.patch.dict(sys.modules, {'cryptography': None}):
            self.assertTrue(check13.check(self.pkg())['ok'])


class PkgEboot(Temp):
    """--eboot with pkg_extract replaced by a stand-in that writes the eboot.bin (no real PKG)."""

    def fake_extract(self, written):
        module = types.ModuleType('pkg_extract')
        calls = {}

        class Pkg:
            def __init__(self, path):
                self.f = open(path, 'rb')

        def extract(package, out, progress=None, items=None, only=None):
            calls.update(out=out, only=only)
            (Path(out) / 'eboot.bin').write_bytes(written)

        module.Pkg, module.extract = Pkg, extract
        return module, calls

    def run_eboot(self, image, written=b'x'):
        module, calls = self.fake_extract(written)
        with mock.patch.dict(sys.modules, {'pkg_extract': module, 'cryptography': types.ModuleType('cryptography')}), \
                mock.patch.object(game_check, 'image_sha256', return_value=image):
            result = check13.check(self.pkg(), eboot=True)
        self.assertEqual(calls['only'], ['eboot.bin'])
        self.assertFalse(os.path.exists(calls['out']), 'the temporary folder is deleted')
        return result

    def test_matching_image(self):
        result = self.run_eboot(check13.SUPPORTED_IMAGE_13S)
        self.assertTrue(result['ok'], result)
        self.assertTrue(result['image_checked'])

    def test_other_image(self):
        result = self.run_eboot('1' * 64)
        self.assertEqual((result['reason'], result['image_checked']), ('wrong_eboot', True))

    def test_needs_cryptography(self):
        with mock.patch.dict(sys.modules, {'cryptography': None}):
            result = check13.check(self.pkg(), eboot=True)
        self.assertEqual((result['ok'], result['reason'], result['image_checked']), (False, 'needs_cryptography', False))

    def test_header_refusal_comes_first(self):
        with mock.patch.dict(sys.modules, {'cryptography': None}):
            result = check13.check(self.pkg(index=0), eboot=True)
        self.assertEqual(result['reason'], 'retail_pkg')

    def test_unpack_failure(self):
        module, _ = self.fake_extract(b'')
        module.extract = mock.Mock(side_effect=ValueError('RSA padding check failed'))
        with mock.patch.dict(sys.modules, {'pkg_extract': module, 'cryptography': types.ModuleType('cryptography')}):
            result = check13.check(self.pkg(), eboot=True)
        self.assertEqual(result['reason'], 'unreadable')


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

    def test_usage_errors(self):
        for args in ([], ['--bogus'], ['--pick-icon', 'x'], ['--game', 'x', 'y']):
            with self.subTest(args=args):
                self.assertEqual(self.run_cli(*args).returncode, 2)

    def test_pick_icon_cli(self):
        folder = self.folder(icon=True)
        result = self.run_cli('--pick-icon', '--game', folder, '--data', self.root / 'empty')
        self.assertEqual((result.returncode, result.stdout.strip()), (0, str(folder / 'sce_sys' / 'icon0.png')))
        result = self.run_cli('--pick-icon', '--game', self.root / 'none', '--data', self.root / 'empty')
        self.assertEqual((result.returncode, result.stdout), (0, ''))

    def test_pick_icon_data_default_from_the_environment(self):
        icon = self.root / 'data/user/savedata/1/CUSA19610/SAVEDATA00.sce_sys/icon0.png'
        icon.parent.mkdir(parents=True)
        icon.write_bytes(b'png')
        result = self.run_cli('--pick-icon', env={**os.environ, 'S13_APP_DATA_DIR': str(self.root / 'data')})
        self.assertEqual(result.stdout.strip(), str(icon))


class PickIcon(Temp):
    def save_icon(self, user, slot, mtime, title='CUSA19610'):
        icon = self.root / f'data/user/savedata/{user}/{title}/SAVEDATA{slot:02d}.sce_sys/icon0.png'
        icon.parent.mkdir(parents=True, exist_ok=True)
        icon.write_bytes(b'png')
        os.utime(icon, (mtime, mtime))
        return icon

    def test_game_icon_beats_save_icon(self):
        self.save_icon(1, 0, 1000)
        game = self.folder(icon=True)
        self.assertEqual(check13.pick_icon([game], self.root / 'data'), game / 'sce_sys' / 'icon0.png')

    def test_first_game_folder_with_an_icon(self):
        bare = self.folder(name='bare')
        second = self.folder(icon=True, name='second')
        third = self.folder(icon=True, name='third')
        self.assertEqual(check13.pick_icon([self.root / 'missing', bare, second, third], self.root / 'data'),
                         second / 'sce_sys' / 'icon0.png')

    def test_newest_save_icon(self):
        self.save_icon(1, 0, 1000)
        newest = self.save_icon(2, 3, 3000)
        self.save_icon(1, 1, 2000)
        self.save_icon(1, 2, 9999, title='CUSA00000')  # another game's save
        self.assertEqual(check13.pick_icon([self.folder()], self.root / 'data'), newest)

    def test_nothing(self):
        self.assertIsNone(check13.pick_icon([self.folder()], self.root / 'data'))
        self.assertIsNone(check13.pick_icon([], self.root / 'nowhere'))

    def test_data_folder_default(self):
        icon = self.save_icon(1, 0, 1000)
        with mock.patch.dict(os.environ, {'S13_APP_DATA_DIR': str(self.root / 'data')}):
            self.assertEqual(check13.pick_icon([]), icon)
        with mock.patch.dict(os.environ, {'S13_APP_DATA_DIR': ''}), mock.patch.object(Path, 'home', return_value=self.root):
            self.assertEqual(check13.default_data_dir(), self.root / 'Library/Application Support/13 Sentinels')


class Constants(unittest.TestCase):
    def test_image_hash_matches_the_runtime(self):
        source = (ROOT / 'src' / 'runtime_prepare.c').read_text(encoding='utf-8')
        found = re.search(r'SUPPORTED_IMAGE_13S\[\]\s*=\s*"([0-9a-f]{64})"', source)
        self.assertIsNotNone(found)
        self.assertEqual(check13.SUPPORTED_IMAGE_13S, found.group(1))

    def test_title_and_version(self):
        self.assertEqual((check13.SUPPORTED_TITLE, check13.SUPPORTED_VERSION), ('CUSA19610', '01.00'))


@unittest.skipUnless(importlib.util.find_spec('cryptography'),
                     "the 'cryptography' package is not installed (pkg_extract needs it)")
class Keys(unittest.TestCase):
    def test_every_keyset_is_an_rsa_pair(self):
        import pkg_extract
        import pkg_keys
        prefixes = sorted(name[:-2] for name in vars(pkg_keys) if name.endswith('_N'))
        self.assertTrue(prefixes)
        rng = random.Random(1)
        for prefix in prefixes:
            with self.subTest(keyset=prefix):
                n, d = getattr(pkg_keys, prefix + '_N'), getattr(pkg_keys, prefix + '_D')
                size = (n.bit_length() + 7) // 8
                self.assertEqual(size, 256)
                message = bytes(rng.randrange(256) for _ in range(32))
                padding = bytes(rng.randrange(1, 256) for _ in range(size - 3 - len(message)))
                block = b'\0\2' + padding + b'\0' + message  # PKCS#1 v1.5, type 2
                encrypted = pow(int.from_bytes(block, 'big'), 65537, n).to_bytes(size, 'big')
                self.assertEqual(pkg_extract.rsa_pkcs1_decrypt(encrypted, n, d), message)


if __name__ == '__main__':
    unittest.main()
