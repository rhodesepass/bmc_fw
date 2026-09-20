import os
from pathlib import Path
import subprocess
import tempfile
import unittest


class TerminalLauncherTests(unittest.TestCase):
    def test_address_selection_and_argument_preservation(self):
        launcher = Path(__file__).resolve().parents[1] / 'tools/bmc-terminal'
        with tempfile.TemporaryDirectory() as directory:
            uv = Path(directory) / 'uv'
            uv.write_text("#!/bin/sh\nprintf '%s\\n' \"$@\"\n")
            uv.chmod(0o755)
            for address in (None, '', '00:11:22:33:44:55'):
                with self.subTest(address=address):
                    env = dict(os.environ, PATH=directory + os.pathsep + os.defpath)
                    env.pop('BMC_ADDRESS', None)
                    if address is not None:
                        env['BMC_ADDRESS'] = address
                    result = subprocess.run(
                        ['/bin/sh', str(launcher), '--log-dir', 'logs with spaces'],
                        env=env, capture_output=True, text=True, check=True,
                    )
                    args = result.stdout.splitlines()
                    expected = ['terminal', '--log-dir', 'logs with spaces']
                    if address:
                        expected = ['--address', address] + expected
                    self.assertEqual(args, [
                        'run', '--project', str(launcher.parent), 'python',
                        str(launcher.parent / 'bmc_ble.py'), *expected,
                    ])


if __name__ == '__main__':
    unittest.main()
