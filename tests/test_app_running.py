"""scripts/app_running.py: package-app.sh's check that no program of the app it replaces is running.
Run: python3 -I tests/test_app_running.py"""
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / 'scripts' / 'app_running.py'
sys.path.insert(0, str(ROOT / 'scripts'))
import app_running  # noqa: E402


class AppRunningTest(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix='app-running-'))
        self.addCleanup(shutil.rmtree, self.dir, True)
        # A space in the name, as '13 Sentinels.app' has.
        self.app = self.dir / 'My Game.app'
        (self.app / 'Contents' / 'MacOS').mkdir(parents=True)
        self.program = self.app / 'Contents' / 'MacOS' / 'My Game'
        # A copy of /bin/sleep keeps Apple's platform signature, which macOS refuses outside /bin (the copy is
        # killed and a "damaged" notice shows): signed again ad hoc, it runs as any program of an app would.
        shutil.copy('/bin/sleep', self.program)
        subprocess.run(['codesign', '--force', '--sign', '-', str(self.program)], check=True, capture_output=True)

    def start(self, args):
        process = subprocess.Popen(args)
        self.addCleanup(process.wait)
        self.addCleanup(process.kill)
        time.sleep(0.2)   # exec'd by now
        self.assertIsNone(process.poll(), f'{args[0]} ended at once (exit {process.returncode})')
        return process

    def check(self, app):
        return subprocess.run([sys.executable, '-I', str(SCRIPT), str(app)], capture_output=True, text=True)

    def test_nothing_running(self):
        self.assertEqual(app_running.running(self.app), [])
        self.assertEqual(self.check(self.app).returncode, 1)

    def test_the_apps_program_is_found(self):
        process = self.start([str(self.program), '30'])
        found = app_running.running(self.app)
        self.assertEqual([pid for pid, _ in found], [process.pid])
        result = self.check(self.app)
        self.assertEqual(result.returncode, 0)
        self.assertIn(f'{process.pid}\t', result.stdout)

    def test_a_command_line_that_names_the_path_is_not_the_app(self):
        # What `pgrep -f "<App>.app/Contents/MacOS/"` took for the game: a shell whose arguments name it.
        self.start(['/bin/sh', '-c', f'sleep 30 # {self.program} --compile-shaders', 'watcher'])
        self.assertEqual(app_running.running(self.app), [])

    def test_another_copy_of_the_app_does_not_count(self):
        other = self.dir / 'elsewhere' / 'My Game.app'
        shutil.copytree(self.app, other)
        self.start([str(other / 'Contents' / 'MacOS' / 'My Game'), '30'])
        self.assertEqual(app_running.running(self.app), [])
        self.assertEqual(len(app_running.running(other)), 1)

    def test_a_path_through_a_link_is_the_same_app(self):
        process = self.start([str(self.program), '30'])
        link = self.dir / 'link'
        link.symlink_to(self.dir)
        self.assertEqual([pid for pid, _ in app_running.running(link / 'My Game.app')], [process.pid])

    def test_usage(self):
        self.assertEqual(subprocess.run([sys.executable, '-I', str(SCRIPT)], capture_output=True).returncode, 2)


if __name__ == '__main__':
    unittest.main()
