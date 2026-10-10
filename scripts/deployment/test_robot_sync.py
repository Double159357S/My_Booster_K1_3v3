import importlib.util
import contextlib
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tarfile
from types import SimpleNamespace
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('robot_sync', Path(__file__).with_name('robot_sync.py'))
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)


class RobotSyncTests(unittest.TestCase):
    def test_interactive_selection_has_no_default_targets(self):
        reader = mock.Mock(side_effect=['192.168.133', '.43'])
        self.assertEqual(sync.select_hosts(reader=reader), ['192.168.133.43'])
        self.assertEqual(reader.call_count, 2)

    def test_suffix_list_in_prefix_is_rejected_before_suffix_prompt(self):
        reader = mock.Mock(side_effect=AssertionError('Unexpected suffix prompt'))
        with self.assertRaisesRegex(ValueError, '尾号请在下一项'):
            sync.select_hosts(ip_prefix='.43,.21', reader=reader)
        reader.assert_not_called()

    def test_prefix_cli_validates_without_authentication_or_connections(self):
        backend = str(Path(__file__).with_name('robot_sync.py'))
        for prefix, expected in [('192.168.133', 0), ('192.168.133.', 0),
                                 ('.43,.21', 1), ('192.168.999', 1)]:
            with self.subTest(prefix=prefix):
                result = subprocess.run([sys.executable, backend, '--validate-prefix',
                                         '--ip-prefix', prefix], capture_output=True,
                                        encoding='utf-8', timeout=10)
                self.assertEqual(result.returncode, expected)
                if expected == 0:
                    self.assertEqual(result.stdout.strip(), '192.168.133')
                else:
                    self.assertEqual(result.stdout, '')
                    self.assertIn('192.168.133', result.stderr)

    def test_suffix_formats_select_only_explicitly_entered_robots(self):
        self.assertEqual(sync.select_hosts(ip_prefix='192.168.133', robot_ids='.43，.21'),
                         ['192.168.133.43', '192.168.133.21'])
        self.assertEqual(sync.select_hosts(ip_prefix='192.168.133.', robot_ids='43 97'),
                         ['192.168.133.43', '192.168.133.97'])

    def test_empty_selection_never_falls_back_to_three_robots(self):
        for text in ('', '  ', ','):
            with self.subTest(text=text), self.assertRaises(ValueError):
                sync.select_hosts(ip_prefix='192.168.133', robot_ids=text)
        with self.assertRaises(ValueError):
            sync.select_hosts(hosts='')

    def test_invalid_and_duplicate_addresses_are_rejected(self):
        for prefix, suffixes in [('192.168.999', '.43'), ('192.168', '.43'),
                                 ('192.168.133', '.0'), ('192.168.133', '.255'),
                                 ('192.168.133', '.43,.43'), ('192.168.133', 'all'),
                                 ('192.168.133', '192.168.133.43')]:
            with self.subTest(prefix=prefix, suffixes=suffixes), self.assertRaises(ValueError):
                sync.select_hosts(ip_prefix=prefix, robot_ids=suffixes)

    def test_explicit_full_address_does_not_prompt_or_add_targets(self):
        reader = mock.Mock(side_effect=AssertionError('Unexpected prompt'))
        self.assertEqual(sync.select_hosts(hosts='192.168.133.43', reader=reader), ['192.168.133.43'])
        reader.assert_not_called()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'robot'
        (self.root / 'src/brain').mkdir(parents=True)
        self.stage = Path(self.temp.name) / 'stage'
        self.stage.mkdir()

    def prepare(self, name='src/brain/code.cpp', old=b'old\n', new=b'new\n'):
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if old is not None:
            target.write_bytes(old)
            target.chmod(0o644)
        request = {'files': [{'path': name, 'kind': 'file', 'sha256': sync.digest(new), 'mode': 0o644}],
                   'templates': {}, 'skipped': []}
        plan = sync.prepare_plan(self.root, request, self.stage)
        with tarfile.open(self.stage / 'payload.tar.gz', 'w:gz') as tar:
            info = tarfile.TarInfo(name)
            info.size = len(new)
            tar.addfile(info, io.BytesIO(new))
        return target, request, plan, sync.file_info(self.stage / 'payload.tar.gz')['sha256']

    def test_comparison_does_not_write_project_files(self):
        target, _, plan, _ = self.prepare()
        self.assertEqual(target.read_bytes(), b'old\n')
        self.assertEqual(len(plan['changes']), 1)

    def test_apply_verify_idempotence_and_rollback(self):
        target, request, _, archive_hash = self.prepare()
        result = sync.apply_plan(self.root, self.stage, 'test-run', archive_hash)
        self.assertEqual(result['changed'], 1)
        self.assertEqual(target.read_bytes(), b'new\n')
        self.assertEqual((Path(result['backup']) / 'files/src/brain/code.cpp').read_bytes(), b'old\n')
        self.assertEqual(sync.prepare_plan(self.root, request, self.stage)['changes'], [])
        self.assertEqual(sync.rollback(self.root, 'test-run')['restored'], 1)
        self.assertEqual(target.read_bytes(), b'old\n')

    def test_rollback_new_file_only_removes_its_own_addition(self):
        target, _, _, archive_hash = self.prepare(old=None)
        sync.apply_plan(self.root, self.stage, 'test-new', archive_hash)
        unrelated = self.root / 'src/brain/robot_only.cpp'
        unrelated.write_bytes(b'robot-only')
        sync.rollback(self.root, 'test-new')
        self.assertFalse(target.exists())
        self.assertEqual(unrelated.read_bytes(), b'robot-only')

    def test_destination_modified_since_comparison_is_not_overwritten(self):
        target, _, _, archive_hash = self.prepare()
        target.write_bytes(b'operator edit\n')
        with self.assertRaisesRegex(ValueError, 'changed since comparison'):
            sync.apply_plan(self.root, self.stage, 'test-conflict', archive_hash)
        self.assertEqual(target.read_bytes(), b'operator edit\n')

    def test_rollback_refuses_to_overwrite_later_operator_changes(self):
        target, _, _, archive_hash = self.prepare()
        sync.apply_plan(self.root, self.stage, 'test-edited', archive_hash)
        target.write_bytes(b'operator edit\n')
        with self.assertRaisesRegex(ValueError, 'Changed after sync'):
            sync.rollback(self.root, 'test-edited')
        self.assertEqual(target.read_bytes(), b'operator edit\n')

    def test_failed_transaction_restores_files_already_written(self):
        first = self.root / 'src/brain/a.cpp'
        second = self.root / 'src/brain/b.cpp'
        first.write_bytes(b'a-old')
        second.write_bytes(b'b-old')
        request = {'files': [{'path': 'src/brain/' + n + '.cpp', 'kind': 'file',
                              'sha256': sync.digest((n + '-new').encode()), 'mode': 0o644}
                             for n in ('a', 'b')], 'templates': {}}
        sync.prepare_plan(self.root, request, self.stage)
        with tarfile.open(self.stage / 'payload.tar.gz', 'w:gz') as tar:
            for n in ('a', 'b'):
                data = (n + '-new').encode()
                info = tarfile.TarInfo('src/brain/' + n + '.cpp')
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        archive_hash = sync.file_info(self.stage / 'payload.tar.gz')['sha256']
        original = sync.atomic_copy
        def fail_second(source, target, mode):
            if 'payload' in str(source) and target.name == 'b.cpp':
                raise OSError('simulated disk failure')
            return original(source, target, mode)
        with mock.patch.object(sync, 'atomic_copy', side_effect=fail_second):
            with self.assertRaisesRegex(OSError, 'disk failure'):
                sync.apply_plan(self.root, self.stage, 'test-failure', archive_hash)
        self.assertEqual(first.read_bytes(), b'a-old')
        self.assertEqual(second.read_bytes(), b'b-old')

    def test_bad_upload_hash_prevents_source_changes(self):
        target, _, _, _ = self.prepare()
        with self.assertRaisesRegex(ValueError, 'checksum'):
            sync.apply_plan(self.root, self.stage, 'test-hash', '0' * 64)
        self.assertEqual(target.read_bytes(), b'old\n')

    def test_path_and_archive_traversal_are_rejected(self):
        with self.assertRaises(ValueError):
            sync.destination(self.root, '../outside')
        outside = Path(self.temp.name) / 'outside'
        outside.mkdir()
        (self.root / 'src/escape').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'leaves workspace'):
            sync.destination(self.root, 'src/escape/code.cpp')
        archive = self.stage / 'evil.tar.gz'
        with tarfile.open(archive, 'w:gz') as tar:
            info = tarfile.TarInfo('../outside/evil')
            info.size = 1
            tar.addfile(info, io.BytesIO(b'x'))
        with self.assertRaises(ValueError):
            sync.safe_unpack(archive, self.stage / 'payload', {})
        self.assertFalse((outside / 'evil').exists())

    def test_symlink_is_backed_up_and_replaced_without_touching_referent(self):
        referent = self.root / 'src/brain/referent.cpp'
        referent.write_bytes(b'keep me')
        target = self.root / 'src/brain/code.cpp'
        target.symlink_to('referent.cpp')
        _, _, _, archive_hash = self.prepare(old=None)
        sync.apply_plan(self.root, self.stage, 'test-symlink', archive_hash)
        self.assertFalse(target.is_symlink())
        self.assertEqual(referent.read_bytes(), b'keep me')
        sync.rollback(self.root, 'test-symlink')
        self.assertTrue(target.is_symlink())
        self.assertEqual(referent.read_bytes(), b'keep me')

    def test_local_scope_normalization_and_machine_config_protection(self):
        local = Path(self.temp.name) / 'local'
        for folder in ('src/brain/config', 'src/vision/config', 'scripts/build', 'install', 'docs'):
            (local / folder).mkdir(parents=True)
        (local / 'scripts/start.sh').write_bytes(b'\xef\xbb\xbf#!/bin/bash\r\necho hi\r\n')
        (local / 'scripts/build/ignored.cpp').write_bytes(b'generated')
        (local / 'src/brain/config/config_local.yaml').write_bytes(b'player_id: 2')
        (local / 'src/vision/config/vision.yaml').write_bytes(b'camera: calibration')
        binary = local / 'src/brain/model.bin'
        binary.write_bytes(b'\x00\xff\r\n')
        request = sync.scan_local(local)
        names = {item['path'] for item in request['files']}
        self.assertEqual(names, {'scripts/start.sh', 'src/brain/model.bin'})
        self.assertEqual(sync.source_bytes(local / 'scripts/start.sh'), b'#!/bin/bash\necho hi\n')
        self.assertEqual(sync.source_bytes(binary), b'\x00\xff\r\n')
        self.assertEqual(len(request['skipped']), 2)

    def test_brain_merge_keeps_identity_and_calibration_but_updates_strategy(self):
        import yaml
        local = '''brain_node:
  ros__parameters:
    game:
      team_id: 70
      player_id: 3 # identity comment
      player_role: striker
      player_start_pos: left
    robot:
      odom_factor: 1.0
    vision:
      cam_pixel_width: 1280
    game_control_ip: 172.169.133.210
    strategy:
      cooperation:
        owner_hold_ms: 1500.0 # new strategy comment
'''
        remote = '''brain_node:
  ros__parameters:
    game:
      team_id: 99
      player_id: 1
      player_role: goal_keeper
      player_start_pos: right
    robot:
      odom_factor: 0.9
    vision:
      cam_pixel_width: 544
    game_control_ip: 192.168.133.210
    strategy:
      cooperation:
        owner_hold_ms: 50.0
'''
        merged, kept = sync.merge_brain(local, remote)
        parsed = yaml.safe_load(merged)['brain_node']['ros__parameters']
        self.assertEqual(parsed['game'], {'team_id': 70, 'player_id': 1,
                                         'player_role': 'goal_keeper', 'player_start_pos': 'right'})
        self.assertEqual(parsed['robot']['odom_factor'], 0.9)
        self.assertEqual(parsed['vision']['cam_pixel_width'], 544)
        self.assertEqual(parsed['game_control_ip'], '192.168.133.210')
        self.assertEqual(parsed['strategy']['cooperation']['owner_hold_ms'], 1500.0)
        self.assertIn(b'# new strategy comment', merged)
        self.assertIn(b'# identity comment', merged)
        self.assertIn('game.player_id', kept)
        self.assertEqual(sync.merge_brain(merged.decode(), merged.decode())[0], merged)

    def test_gc_launch_keeps_network_constants_and_other_local_changes(self):
        local = "# 中文注释\nsettings={'enable_ip_white_list': False, 'ip_white_list': ['bad-ip'], 'new': 123}\n"
        remote = "settings={'enable_ip_white_list': True, 'ip_white_list': ['192.168.133.210']}\n"
        merged, _ = sync.merge_gc_launch(local, remote)
        self.assertIn(b"'ip_white_list': ['192.168.133.210']", merged)
        self.assertIn(b"'enable_ip_white_list': True", merged)
        self.assertIn(b"'new': 123", merged)
        self.assertEqual(sync.merge_gc_launch(merged.decode(), merged.decode())[0], merged)


class DriverIntegrationTests(unittest.TestCase):
    """Run the real driver/agent pipeline locally, without SSH or robot motion."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.local = Path(self.temp.name) / 'local'
        self.robot = Path(self.temp.name) / 'robot'
        self.stages = set()
        for root in (self.local, self.robot):
            for folder in ('src/brain/config', 'src/game_controller/launch', 'scripts'):
                (root / folder).mkdir(parents=True)
        self.old_config = '''brain_node:
  ros__parameters:
    game:
      player_id: 1
      player_role: goal_keeper
      player_start_pos: right
    game_control_ip: 192.168.133.210
    strategy:
      cooperation:
        owner_hold_ms: 50.0
'''
        new_config = self.old_config.replace('player_id: 1', 'player_id: 3').replace(
            'goal_keeper', 'striker').replace('right', 'left').replace(
            '192.168.133.210', 'bad-ip').replace('50.0', '1500.0')
        (self.local / sync.BRAIN_CONFIG).write_text(new_config, encoding='utf-8')
        (self.robot / sync.BRAIN_CONFIG).write_text(self.old_config, encoding='utf-8')
        old_launch = "settings={'enable_ip_white_list': True, 'ip_white_list': ['192.168.133.210']}\n"
        new_launch = "settings={'enable_ip_white_list': False, 'ip_white_list': ['bad-ip'], 'new': 7}\n"
        (self.robot / sync.GC_LAUNCH).write_text(old_launch, encoding='utf-8')
        (self.local / sync.GC_LAUNCH).write_text(new_launch, encoding='utf-8')
        (self.local / 'src/brain/code.cpp').write_bytes(b'new source\r\n')
        (self.robot / 'src/brain/code.cpp').write_bytes(b'old source\n')
        (self.local / 'src/brain/new.h').write_bytes(b'new header\n')
        (self.local / 'scripts/start.sh').write_bytes(b'#!/bin/bash\r\necho example\r\n')
        self.protected = {
            'src/brain/config/config_local.yaml': b'player_id: 1\n',
            'src/vision/config/vision.yaml': b'robot calibration\n',
            'configs/fastdds.xml': b'robot network\n',
            'install/keep.txt': b'compiled output\n',
            'log/keep.log': b'robot log\n',
            'src/brain/robot_only.cpp': b'robot only\n',
        }
        for name, data in self.protected.items():
            path = self.robot / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            if sync.protected(name) or name.startswith(('install/', 'log/')):
                local_path = self.local / name
                local_path.parent.mkdir(parents=True, exist_ok=True)
                local_path.write_bytes(b'must not overwrite robot\n')
        self.args = SimpleNamespace(
            local_root=str(self.local), remote_root=str(self.robot),
            hosts='192.168.133.43', ip_prefix=None, robot_ids=None,
            password_file='unused-test-auth', key_file=None,
            rollback=None, build=False, dry_run=False)
        owner = self

        class LocalTransport:
            def __init__(self, args):
                pass

            def ssh(self, host, command, timeout=300, stream=False):
                owner.assertEqual(host, '192.168.133.43')
                result = subprocess.run(['bash', '-c', command], capture_output=True,
                                        timeout=timeout, check=True)
                return result.stdout.decode('utf-8')

            def upload(self, host, local, remote):
                owner.assertEqual(host, '192.168.133.43')
                owner.stages.add(Path(remote).parent)
                shutil.copy2(local, remote)

        self.transport = mock.patch.object(sync, 'Transport', LocalTransport)
        self.transport.start()
        self.addCleanup(self.transport.stop)

    def run_driver(self):
        with contextlib.redirect_stdout(io.StringIO()):
            sync.run_local(self.args)
        reports = sorted((self.local / '.robot-sync').glob('*/result.json'),
                         key=lambda p: p.stat().st_mtime_ns)
        report = sync.read_json(reports[-1])
        self.assertTrue(report['success'])
        self.assertEqual(set(report['robots']), {'192.168.133.43'})
        self.assertTrue(all(not path.exists() for path in self.stages))
        for name, expected in self.protected.items():
            self.assertEqual((self.robot / name).read_bytes(), expected)
        return report

    def test_driver_preview_has_no_project_changes(self):
        self.args.dry_run = True
        report = self.run_driver()
        self.assertEqual(len(report['robots']['192.168.133.43']['plan']['changes']), 5)
        self.assertEqual((self.robot / 'src/brain/code.cpp').read_bytes(), b'old source\n')
        self.assertEqual((self.robot / sync.BRAIN_CONFIG).read_text(), self.old_config)
        self.assertFalse((self.robot / 'src/brain/new.h').exists())
        self.assertFalse((self.robot / '.robot-sync-backups').exists())

    def test_driver_sync_repeat_and_rollback(self):
        import yaml
        report = self.run_driver()
        self.assertEqual(report['robots']['192.168.133.43']['result']['changed'], 5)
        self.assertEqual((self.robot / 'src/brain/code.cpp').read_bytes(), b'new source\n')
        self.assertEqual((self.robot / 'scripts/start.sh').read_bytes(), b'#!/bin/bash\necho example\n')
        self.assertTrue((self.robot / 'scripts/start.sh').stat().st_mode & 0o111)
        config = yaml.safe_load((self.robot / sync.BRAIN_CONFIG).read_text())['brain_node']['ros__parameters']
        self.assertEqual(config['game']['player_id'], 1)
        self.assertEqual(config['game']['player_role'], 'goal_keeper')
        self.assertEqual(config['game_control_ip'], '192.168.133.210')
        self.assertEqual(config['strategy']['cooperation']['owner_hold_ms'], 1500.0)
        self.assertIn("'ip_white_list': ['192.168.133.210']", (self.robot / sync.GC_LAUNCH).read_text())
        self.assertEqual(self.run_driver()['robots']['192.168.133.43']['result']['changed'], 0)
        self.args.rollback = report['run_id']
        self.assertEqual(self.run_driver()['robots']['192.168.133.43']['rollback_result']['restored'], 5)
        self.assertEqual((self.robot / 'src/brain/code.cpp').read_bytes(), b'old source\n')
        self.assertEqual((self.robot / sync.BRAIN_CONFIG).read_text(), self.old_config)
        self.assertFalse((self.robot / 'src/brain/new.h').exists())
        self.assertFalse((self.robot / 'scripts/start.sh').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
