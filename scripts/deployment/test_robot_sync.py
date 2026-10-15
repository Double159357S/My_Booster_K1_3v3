import importlib.util
import contextlib
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import tarfile
import time
import threading
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

    def test_brain_merge_keeps_entire_game_and_log_locations_and_updates_other_settings(self):
        import yaml
        local = '''brain_node:
  ros__parameters:
    game:
      team_id: 70
      player_id: 3 # identity comment
      player_role: striker
      player_start_pos: left
      field_type: kid_size
      treat_person_as_robot: true
      number_of_players: 3
      local_only: do_not_sync
    robot:
      odom_factor: 1.0
      vx_limit: 1.6
    vision:
      cam_pixel_width: 1280
    game_control_ip: 172.169.133.210
    rerunLog:
      server_ip: local-log-server
      log_dir: /local/logs
      enable_file: false
      img_interval: 2
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
      field_type: adult_size
      treat_person_as_robot: false
      number_of_players: 5
      remote_only:
        formation: preserved
    robot:
      odom_factor: 0.9
      vx_limit: 0.6
    vision:
      cam_pixel_width: 544
    game_control_ip: 192.168.133.210
    rerunLog:
      server_ip: robot-log-server
      log_dir: /robot/logs
      enable_file: true
      img_interval: 5
    strategy:
      cooperation:
        owner_hold_ms: 50.0
'''
        merged, kept = sync.merge_brain(local, remote)
        parsed = yaml.safe_load(merged)['brain_node']['ros__parameters']
        self.assertEqual(parsed['game'], yaml.safe_load(remote)['brain_node']['ros__parameters']['game'])
        self.assertEqual(parsed['robot'], {'odom_factor': 1.0, 'vx_limit': 1.6})
        self.assertEqual(parsed['vision']['cam_pixel_width'], 1280)
        self.assertEqual(parsed['game_control_ip'], '172.169.133.210')
        self.assertEqual(parsed['rerunLog'], {'server_ip': 'robot-log-server', 'log_dir': '/robot/logs',
                                             'enable_file': False, 'img_interval': 2})
        self.assertEqual(parsed['strategy']['cooperation']['owner_hold_ms'], 1500.0)
        self.assertIn(b'# new strategy comment', merged)
        self.assertIn('game.player_id', kept)
        self.assertIn('game.team_id', kept)
        self.assertIn('game.remote_only.formation', kept)
        self.assertIn('rerunLog.server_ip', kept)
        self.assertNotIn('robot.odom_factor', kept)
        self.assertNotIn('game_control_ip', kept)
        self.assertEqual(sync.merge_brain(merged.decode(), merged.decode())[0], merged)

    def test_brain_merge_preserves_missing_and_remote_only_log_locations(self):
        import yaml
        for local_log, remote_log in (
                ({'server_ip': 'local', 'log_dir': '/local', 'enable_file': False}, {'enable_file': True}),
                ({'enable_file': False}, {'server_ip': 'robot', 'log_dir': '/robot', 'enable_file': True}),
                ({'server_ip': 'local', 'enable_file': False}, {'log_dir': '/robot'})):
            with self.subTest(local=local_log, remote=remote_log):
                local = yaml.safe_dump({'brain_node': {'ros__parameters': {
                    'game': {'player_id': 3}, 'rerunLog': local_log, 'enable_com': True}}}, sort_keys=False)
                remote = yaml.safe_dump({'brain_node': {'ros__parameters': {
                    'game': {'player_id': 1}, 'rerunLog': remote_log}}}, sort_keys=False)
                merged, _ = sync.merge_brain(local, remote)
                params = yaml.safe_load(merged)['brain_node']['ros__parameters']
                expected = {'enable_file': False}
                expected.update({key: remote_log[key] for key in ('server_ip', 'log_dir') if key in remote_log})
                self.assertEqual(params['rerunLog'], expected)
                self.assertTrue(params['enable_com'])
                self.assertEqual(sync.merge_brain(merged.decode(), remote)[0], merged)

    def test_brain_merge_retains_log_locations_when_local_log_block_is_absent(self):
        import yaml
        for ending in ('', '\n', '\n  use_sim_time: false\n'):
            with self.subTest(ending=ending):
                local = 'brain_node:\n  ros__parameters:\n    game:\n      player_id: 3' + ending
                remote = '''brain_node:
  ros__parameters:
    game:
      player_id: 1
    rerunLog:
      server_ip: robot
      log_dir: /robot
      enable_file: true
'''
                merged, _ = sync.merge_brain(local, remote)
                params = yaml.safe_load(merged)['brain_node']['ros__parameters']
                self.assertEqual(params['game'], {'player_id': 1})
                self.assertEqual(params['rerunLog'], {'server_ip': 'robot', 'log_dir': '/robot'})
                if ending.endswith('false\n'):
                    self.assertFalse(yaml.safe_load(merged)['brain_node']['use_sim_time'])

    def test_shipped_brain_config_updates_everything_except_game_and_log_locations(self):
        import copy
        import yaml
        config_path = Path(__file__).resolve().parents[2] / sync.BRAIN_CONFIG
        local = config_path.read_text(encoding='utf-8')
        local_params = yaml.safe_load(local)['brain_node']['ros__parameters']
        remote_params = copy.deepcopy(local_params)
        for key, value in local_params.items():
            if key not in ('game', 'rerunLog'):
                remote_params[key] = {'old_robot_only_value': 123} if isinstance(value, dict) else not value
        remote_params['game'].update(team_id=99, player_id=1, field_type='adult_size',
            player_role='goal_keeper', player_start_pos='right', treat_person_as_robot=False, number_of_players=5)
        remote_params['game']['remote_only'] = 'keep'
        remote_params['rerunLog'].update(server_ip='robot-server', log_dir='/robot/logs',
            enable_file=False, enable_tcp=True, img_interval=99)
        remote = yaml.safe_dump({'brain_node': {'ros__parameters': remote_params}}, sort_keys=False)
        merged, _ = sync.merge_brain(local, remote)
        expected = copy.deepcopy(local_params)
        expected['game'] = remote_params['game']
        for key in ('server_ip', 'log_dir'):
            expected['rerunLog'][key] = remote_params['rerunLog'][key]
        self.assertEqual(yaml.safe_load(merged)['brain_node']['ros__parameters'], expected)
        self.assertEqual(sync.merge_brain(merged.decode(), remote)[0], merged)

    def test_gc_launch_keeps_network_constants_and_other_local_changes(self):
        local = "# 中文注释\nsettings={'enable_ip_white_list': False, 'ip_white_list': ['bad-ip'], 'new': 123}\n"
        remote = "settings={'enable_ip_white_list': True, 'ip_white_list': ['192.168.133.210']}\n"
        merged, _ = sync.merge_gc_launch(local, remote)
        self.assertIn(b"'ip_white_list': ['192.168.133.210']", merged)
        self.assertIn(b"'enable_ip_white_list': True", merged)
        self.assertIn(b"'new': 123", merged)
        self.assertEqual(sync.merge_gc_launch(merged.decode(), merged.decode())[0], merged)


class BuildProgressTests(unittest.TestCase):
    class Terminal(io.StringIO):
        def isatty(self):
            return True

    def test_package_count_and_percentage_reset_for_each_package(self):
        output = self.Terminal()
        progress = sync.BuildProgress('192.168.133.43', output)
        progress.consume('__K1_BUILD_TOTAL__=2\n')
        progress.consume('Starting >>> booster_interface\n')
        progress.consume('\x1b[32m[ 72%] Building CXX object main.cpp.o\x1b[0m\n')
        self.assertEqual(progress.percent, 72)
        progress.refresh(force=True)
        self.assertIn('72%', output.getvalue())
        progress.consume('Finished <<< booster_interface [3.2s]\n')
        progress.consume('Starting >>> brain\n')
        self.assertIsNone(progress.percent)
        self.assertEqual(progress.completed, {'booster_interface'})
        self.assertIn('1/2 pkgs', output.getvalue())
        progress.consume('Finished <<< brain [4.0s]\n')
        progress.finish(True)
        self.assertIn('2/2 pkgs', output.getvalue())
        self.assertIn('complete [################] 100%', output.getvalue())

    def test_quiet_build_refreshes_elapsed_time_without_inventing_percentage(self):
        now = [0.0]
        output = self.Terminal()
        progress = sync.BuildProgress('192.168.133.43', output, clock=lambda: now[0])
        progress.consume('Starting >>> brain\n')
        now[0] = 11.0
        progress.refresh()
        self.assertIn('00:11', output.getvalue())
        self.assertIn('wait', output.getvalue())
        self.assertNotIn('%', output.getvalue())
        progress.consume('[ 72%] Building CXX object brain_tree.cpp.o\n')
        now[0] = 20.0
        progress.refresh()
        self.assertIn('72%', output.getvalue())
        self.assertIn('00:20', output.getvalue())
        self.assertEqual(progress.percent, 72)

    def test_failed_package_does_not_count_as_finished_or_show_success(self):
        output = self.Terminal()
        progress = sync.BuildProgress('192.168.133.43', output)
        for line in ('__K1_BUILD_TOTAL__=2', 'Starting >>> brain',
                     '[ 72%] Building CXX object brain.cpp.o',
                     'Failed <<< brain [2.0s, exited with code 2]'):
            progress.consume(line + '\n')
        progress.finish(False)
        self.assertEqual(progress.completed, set())
        self.assertIn('0/2 pkgs', output.getvalue())
        self.assertIn('FAILED', output.getvalue())
        self.assertNotIn('100%', output.getvalue())
        self.assertNotIn(' OK', output.getvalue())

    def test_parallel_progress_keeps_other_active_packages_when_one_finishes(self):
        output = self.Terminal()
        progress = sync.BuildProgress('192.168.133.43', output, parallel=True)
        for line in ('__K1_BUILD_TOTAL__=3', 'Starting >>> brain',
                     'Starting >>> vision', '[ 72%] Building CXX object vision.cpp.o',
                     'Finished <<< vision [2.0s]'):
            progress.consume(line + '\n')
        self.assertEqual(progress.active, {'brain'})
        self.assertEqual(progress.completed, {'vision'})
        self.assertIsNone(progress.percent)
        self.assertIn('1/3 pkgs', output.getvalue())
        self.assertIn('brain [', output.getvalue())
        self.assertNotIn('72%', output.getvalue())

    def test_stream_keeps_log_and_animates_during_silence(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'build.log'
            output = self.Terminal()
            progress = sync.BuildProgress('192.168.133.43', output)
            program = ('import time\nprint("__K1_BUILD_TOTAL__=1", flush=True)\n'
                       'print("Starting >>> brain", flush=True)\ntime.sleep(1.2)\n'
                       'print("[ 72%] Building CXX object brain.cpp.o", flush=True)\n'
                       'print("Finished <<< brain [1.2s]", flush=True)\n')
            code, _ = sync.stream_build([sys.executable, '-u', '-c', program], 5, progress, log)
            self.assertEqual(code, 0)
            self.assertIn('00:01', output.getvalue())
            self.assertIn('1/1 pkgs', output.getvalue())
            self.assertIn('[ 72%] Building CXX object', log.read_text())

    def test_stream_failure_preserves_diagnostics_and_exit_code(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'build.log'
            output = io.StringIO()
            progress = sync.BuildProgress('192.168.133.43', output)
            program = ('import sys\nprint("__K1_BUILD_TOTAL__=1")\n'
                       'print("Starting >>> brain")\nprint("[ 60%] Compiling")\n'
                       'print("error: simulated compiler failure", file=sys.stderr)\n'
                       'sys.exit(2)\n')
            code, detail = sync.stream_build([sys.executable, '-u', '-c', program], 5, progress, log)
            self.assertEqual(code, 2)
            self.assertIn('error: simulated compiler failure', detail)
            self.assertIn('error: simulated compiler failure', log.read_text())
            self.assertIn('FAILED', output.getvalue())
            self.assertNotIn('\r', output.getvalue())
            self.assertNotIn('100%', output.getvalue())

    def test_timeout_stops_local_command_and_reports_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            output = io.StringIO()
            progress = sync.BuildProgress('192.168.133.43', output)
            started = time.monotonic()
            with self.assertRaises(subprocess.TimeoutExpired):
                sync.stream_build([sys.executable, '-c', 'import time; time.sleep(30)'],
                                  0.2, progress, Path(temp) / 'build.log')
            self.assertLess(time.monotonic() - started, 5)
            self.assertIn('FAILED', output.getvalue())

    @unittest.skipUnless(os.environ.get('K1_RUN_COMPILE_TESTS') == '1' and
                         shutil.which('colcon') and shutil.which('cmake') and shutil.which('c++'),
                         'Compilation fixture is opt-in (K1_RUN_COMPILE_TESTS=1)')
    def test_real_colcon_build_supplies_package_count_and_make_progress(self):
        with tempfile.TemporaryDirectory(prefix='k1 build progress ') as temp:
            root = Path(temp)
            packages = ('booster_interface', 'brain', 'vision', 'game_controller')
            (root / 'scripts').mkdir()
            build_script = root / 'scripts/build.sh'
            build_script.write_bytes((Path(__file__).resolve().parents[1] / 'build.sh').read_bytes())
            build_script.chmod(0o755)
            for name in packages:
                folder = root / 'src' / name
                folder.mkdir(parents=True)
                dependency = '<depend>booster_interface</depend>' if name == 'brain' else ''
                (folder / 'package.xml').write_text(
                    f'<package format="3"><name>{name}</name><version>0.0.1</version>'
                    '<description>Progress fixture</description>'
                    '<maintainer email="test@example.com">Test</maintainer>'
                    '<license>Apache-2.0</license>' + dependency +
                    '<export><build_type>cmake</build_type></export></package>')
                (folder / 'CMakeLists.txt').write_text(
                    f'cmake_minimum_required(VERSION 3.16)\nproject({name} LANGUAGES CXX)\n'
                    'add_executable(example main.cpp)\ninstall(TARGETS example DESTINATION bin)\n')
                (folder / 'main.cpp').write_text('int main() { return 0; }\n')
            # The fixture is plain CMake; it needs no robot/ROS installation.
            command = sync.build_command(str(root)).replace('source /opt/ros/humble/setup.bash', 'true')
            progress = sync.BuildProgress('192.168.133.43', self.Terminal())
            log = root / 'build.log'
            code, detail = sync.stream_build(['bash', '-c', command], 60, progress, log)
            self.assertEqual(code, 0, detail)
            self.assertEqual(progress.total, 4)
            self.assertEqual(progress.completed, set(packages))
            self.assertRegex(log.read_text(), r'\[\s*\d+%\]')


class MenuInputTests(unittest.TestCase):
    def test_batch_children_leave_piped_menu_answer_for_parent(self):
        # Fake PuTTY tries to read stdin after source transfer. The parent must
        # still receive the operator's answer; no SSH connection is made.
        with tempfile.TemporaryDirectory() as temp:
            probe = Path(temp) / 'probe.py'
            probe.write_text(f'''
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
from unittest import mock
spec = importlib.util.spec_from_file_location('sync', {str(Path(sync.__file__).resolve())!r})
sync = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sync)
real_popen = subprocess.Popen
def fake_putty(command, **kwargs):
    if command[0] in ('fake-plink', 'fake-pscp'):
        command = [sys.executable, '-u', '-c',
                   'import sys; print("CHILD_INPUT=" + repr(sys.stdin.readline()))']
    return real_popen(command, **kwargs)
args = SimpleNamespace(plink='fake-plink', pscp='fake-pscp', key_file='fake-key',
                       password_file=None, host_key='fake-fingerprint', user='test')
transport = sync.Transport(args)
with mock.patch.object(sync.subprocess, 'Popen', side_effect=fake_putty):
    reply = transport.ssh('fake-host', 'fake-command')
    assert reply.strip() == "CHILD_INPUT=''", reply
    transport.ssh('fake-host', 'fake-command', stream=True)
    transport.upload('fake-host', 'fake-source', 'fake-destination')
    transport.ssh('fake-host', 'fake-command',
                  progress=sync.BuildProgress('fake-host', io.StringIO()),
                  log_path=Path({str(Path(temp) / 'output.log')!r}))
print('CHOICE=' + sync.choose_action())
''', encoding='utf-8')
            result = subprocess.run([sys.executable, str(probe)], input='2\n',
                                    capture_output=True, encoding='utf-8', timeout=15,
                                    env=dict(os.environ, PYTHONUTF8='1'))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('CHOICE=build', result.stdout)

    def windows_kernel(self, mode):
        import ctypes
        from ctypes import wintypes
        def get_mode(handle, pointer):
            ctypes.cast(pointer, ctypes.POINTER(wintypes.DWORD)).contents.value = mode
            return 1
        return SimpleNamespace(
            GetStdHandle=mock.Mock(return_value=42),
            GetConsoleMode=mock.Mock(side_effect=get_mode),
            SetConsoleMode=mock.Mock(return_value=1))

    def test_windows_menu_repairs_line_echo_and_ctrl_c_then_restores_mode(self):
        import ctypes
        original = 0x40 | 0x10  # Quick Edit, mouse input; line/echo/Ctrl+C off.
        kernel = self.windows_kernel(original)
        def answer(prompt):
            editable = kernel.SetConsoleMode.call_args.args[1]
            self.assertEqual(editable & 0x87, 0x87)
            self.assertFalse(editable & 0x40)
            self.assertTrue(editable & 0x10)
            return '2'
        with mock.patch.object(sync.os, 'name', 'nt'), \
                mock.patch.object(ctypes, 'WinDLL', return_value=kernel, create=True), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.choose_action(answer), 'build')
        self.assertEqual(kernel.SetConsoleMode.call_args.args, (42, original))
        self.assertEqual(kernel.SetConsoleMode.call_count, 2)

    def test_redirected_windows_input_needs_no_console_mode_change(self):
        import ctypes
        kernel = self.windows_kernel(0)
        kernel.GetConsoleMode = mock.Mock(return_value=0)
        with mock.patch.object(sync.os, 'name', 'nt'), \
                mock.patch.object(ctypes, 'WinDLL', return_value=kernel, create=True), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.choose_action(lambda prompt: ''), 'sync')
        kernel.SetConsoleMode.assert_not_called()

    def test_cancelled_menu_restores_console_and_selects_sync_only(self):
        import ctypes
        kernel = self.windows_kernel(0x47)
        with mock.patch.object(sync.os, 'name', 'nt'), \
                mock.patch.object(ctypes, 'WinDLL', return_value=kernel, create=True), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.choose_action(mock.Mock(side_effect=KeyboardInterrupt)), 'sync')
        self.assertEqual(kernel.SetConsoleMode.call_args.args, (42, 0x47))


class PostSyncWorkflowTests(unittest.TestCase):
    def test_action_menu_defaults_retries_and_handles_closed_input(self):
        self.assertEqual(sync.choose_action(mock.Mock(return_value='')), 'sync')
        self.assertEqual(sync.choose_action(mock.Mock(side_effect=EOFError)), 'sync')
        reader = mock.Mock(side_effect=['invalid', '2'])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sync.choose_action(reader), 'build')
        self.assertEqual(reader.call_count, 2)
        self.assertEqual(sync.choose_action(mock.Mock(return_value='3')), 'start')

    def test_build_uses_all_packages_and_parallel_execution(self):
        body = sync.shlex.split(sync.build_command('/tmp/demo with spaces'))[2]
        self.assertIn("cd -- '/tmp/demo with spaces'", body)
        self.assertIn('colcon list --names-only | wc -l', body)
        self.assertIn('./scripts/build.sh', body)
        self.assertIn('--executor parallel', body)
        self.assertIn('--parallel-workers "$task_build_jobs"', body)
        self.assertNotIn('--packages-up-to', body)
        self.assertNotIn('sequential', body)
        self.assertNotIn('MAKEFLAGS=-j1', body)

    def test_start_sources_new_environment_in_the_same_shell_before_starting(self):
        root = "/tmp/demo with a quote's"
        body = sync.shlex.split(sync.start_command(root))[2]
        self.assertIn('cd -- ' + sync.shlex.quote(root), body)
        self.assertLess(body.index('source install/setup.bash'), body.index('./scripts/start.sh'))
        self.assertIn('set -e -o pipefail', body)
        self.assertEqual(sync.manual_commands(root).count('&&'), 2)

    def test_two_selected_robots_build_concurrently_and_all_results_are_saved(self):
        self.check_parallel_build(failed_host=None)

    def test_one_failed_robot_does_not_prevent_the_other_result(self):
        self.check_parallel_build(failed_host='192.168.133.43')

    def check_parallel_build(self, failed_host):
        hosts = ['192.168.133.43', '192.168.133.21']
        barrier = threading.Barrier(2)
        with tempfile.TemporaryDirectory() as temp:
            report = {'run_id': 'test-parallel', 'robots': {host: {} for host in hosts}}
            args = SimpleNamespace(remote_root='/tmp/fake-demo')
            def ssh(host, command, **kwargs):
                if 'progress' not in kwargs:
                    return '{"moved": 0}'
                barrier.wait(timeout=5)  # Fails if targets are built one at a time.
                Path(kwargs['log_path']).write_text('simulated output; no compiler invoked\n')
                if host == failed_host:
                    raise RuntimeError(host + ': simulated build failure')
                return ''
            transport = SimpleNamespace(ssh=mock.Mock(side_effect=ssh))
            prepared = [(host, ['fake-agent'], None) for host in hosts]
            with contextlib.redirect_stdout(io.StringIO()):
                if failed_host:
                    with self.assertRaisesRegex(RuntimeError, 'simulated build failure'):
                        sync.build_robots(transport, args, prepared, report, Path(temp))
                else:
                    sync.build_robots(transport, args, prepared, report, Path(temp))
            for host in hosts:
                robot = report['robots'][host]
                self.assertEqual(robot['build'], 'failed' if host == failed_host else 'passed')
                self.assertTrue(Path(robot['build_log']).is_file())

    @unittest.skipUnless(os.name == 'posix', 'Shell workflow test needs Linux/WSL')
    def test_build_script_preserves_failure_without_printing_build_complete(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            for name, content in [('colcon', '#!/bin/bash\nexit 2\n'),
                                  ('espeak', '#!/bin/bash\necho SHOULD_NOT_RUN\n')]:
                tool = folder / name
                tool.write_text(content)
                tool.chmod(0o755)
            env = dict(os.environ, PATH=str(folder) + os.pathsep + os.environ['PATH'])
            result = subprocess.run(['bash', str(Path(__file__).resolve().parents[1] / 'build.sh')],
                                    env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertNotIn('Build complete', result.stdout)
            self.assertNotIn('SHOULD_NOT_RUN', result.stdout)


@unittest.skipUnless(os.name == 'posix', 'Shell workflow test needs Linux/WSL')
class BuildScriptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'workspace with spaces'
        scripts = self.root / 'scripts'
        (scripts / 'deployment').mkdir(parents=True)
        original = Path(__file__).resolve().parents[1]
        for name in ('build.sh', 'build_brain.sh', 'build_debug.sh', 'deployment/robot_sync.py'):
            # Windows checkouts use CRLF; the robot sync normalizes these too.
            (scripts / name).write_text((original / name).read_text())
        self.copied = self.root / 'build/brain/ament_cmake_python/brain/brain'
        self.generated = self.root / 'build/brain/rosidl_generator_py/brain'
        for path in (self.copied, self.generated):
            path.mkdir(parents=True)
            (path / '__init__.py').write_text('# generated\n')
        (self.copied / 'old-message.so').write_bytes(b'old build artifact')
        self.tools = Path(self.temp.name) / 'bin'
        self.tools.mkdir()
        colcon = self.tools / 'colcon'
        colcon.write_text('''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys
Path('colcon-args.json').write_text(json.dumps(sys.argv[1:]))
if os.environ.get('TEST_COLCON_FAILURE'):
    sys.exit(2)
if os.environ.get('TEST_CREATE_SYMLINK'):
    root = Path.cwd()
    sys.exit(subprocess.run(['cmake', '-E', 'create_symlink',
        str(root / 'build/brain/rosidl_generator_py/brain'),
        str(root / 'build/brain/ament_cmake_python/brain/brain')]).returncode)
''')
        colcon.chmod(0o755)
        espeak = self.tools / 'espeak'
        espeak.write_text('#!/bin/bash\nexit 1\n')
        espeak.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.tools) + os.pathsep + os.environ['PATH'])

    def run_build(self, script='build.sh', *args, **env):
        return subprocess.run(['bash', str(self.root / 'scripts' / script), *args],
                              cwd=self.temp.name, env=dict(self.env, **env),
                              capture_output=True, text=True, timeout=20)

    def test_manual_build_repairs_conflict_with_cached_symlink_mode(self):
        cache = self.root / 'build/brain/CMakeCache.txt'
        cache.write_text('AMENT_CMAKE_SYMLINK_INSTALL:BOOL=1\n')
        source = self.root / 'src/brain/config/config.yaml'
        installed = self.root / 'install/brain/sentinel'
        for path in (source, installed):
            path.parent.mkdir(parents=True)
            path.write_text('preserve me\n')
        # Reproduce the same CMake failure before running the fixed entry point.
        failed = subprocess.run(['cmake', '-E', 'create_symlink', str(self.generated), str(self.copied)],
                                capture_output=True, text=True, timeout=10)
        self.assertNotEqual(failed.returncode, 0)
        self.assertIn('Is a directory', failed.stderr)
        result = self.run_build(TEST_CREATE_SYMLINK='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.copied.is_symlink())
        self.assertEqual(self.copied.resolve(), self.generated)
        saved = list((self.root / '.robot-sync-backups').glob('*/build-conflicts/build/brain/ament_cmake_python/brain/brain/old-message.so'))
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0].read_bytes(), b'old build artifact')
        for path in (source, installed):
            self.assertEqual(path.read_text(), 'preserve me\n')
        self.assertIn('Backed up 1 build conflict(s)', result.stdout)
        repeated = self.run_build(TEST_CREATE_SYMLINK='1')
        self.assertEqual(repeated.returncode, 0, repeated.stderr)
        self.assertNotIn('Backed up', repeated.stdout)

    def test_explicit_symlink_build_repairs_conflict_and_preserves_arguments(self):
        result = self.run_build('build.sh', '--symlink-install', '--packages-select', 'brain',
                                TEST_CREATE_SYMLINK='1')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.copied.is_symlink())
        args = sync.read_json(self.root / 'colcon-args.json')
        self.assertEqual(args[-3:], ['--symlink-install', '--packages-select', 'brain'])

    def test_copy_build_keeps_existing_module_without_backing_it_up(self):
        (self.root / 'build/brain/CMakeCache.txt').write_text('AMENT_CMAKE_SYMLINK_INSTALL:BOOL=OFF\n')
        result = self.run_build()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.copied / 'old-message.so').is_file())
        self.assertFalse((self.root / '.robot-sync-backups').exists())

    def test_all_entry_points_preserve_failure_without_reporting_success(self):
        for script in ('build.sh', 'build_brain.sh', 'build_debug.sh'):
            with self.subTest(script=script):
                result = self.run_build(script, TEST_COLCON_FAILURE='1')
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertNotIn('Build complete', result.stdout)
                args = sync.read_json(self.root / 'colcon-args.json')
                if script == 'build_brain.sh':
                    self.assertEqual(args[-3:], ['--symlink-install', '--packages-select', 'brain'])
                elif script == 'build_debug.sh':
                    self.assertIn('-DCMAKE_BUILD_TYPE=Debug', args)
                    self.assertIn('-DCMAKE_CXX_FLAGS=-g -fno-omit-frame-pointer', args)


class BuildPreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'robot'
        (self.root / 'src/brain').mkdir(parents=True)

    def module(self, name='booster_interface'):
        copied = self.root / f'build/{name}/ament_cmake_python/{name}/{name}'
        generated = self.root / f'build/{name}/rosidl_generator_py/{name}'
        for path in (copied, generated):
            path.mkdir(parents=True)
            (path / '__init__.py').write_bytes(b'# generated message module\n')
        (copied / 'message.so').write_bytes(b'old compiled message\n')
        return copied, generated

    def test_copy_to_symlink_transition_preserves_backup_and_is_repeatable(self):
        copied, generated = self.module()
        with self.assertRaises(FileExistsError):
            copied.symlink_to(generated, target_is_directory=True)
        result = sync.prepare_symlink_build(self.root, 'test-build')
        self.assertEqual(result['moved'], 1)
        saved = Path(result['changes'][0]['backup'])
        self.assertEqual((saved / 'message.so').read_bytes(), b'old compiled message\n')
        copied.symlink_to(generated, target_is_directory=True)
        self.assertEqual(copied.resolve(), generated)
        self.assertEqual(sync.prepare_symlink_build(self.root, 'test-build')['moved'], 0)
        self.assertEqual(list((self.root / 'src/brain').iterdir()), [])

    def test_unrelated_build_directory_without_generated_counterpart_is_preserved(self):
        copied, generated = self.module()
        (generated / '__init__.py').unlink()
        self.assertEqual(sync.prepare_symlink_build(self.root, 'test-build')['moved'], 0)
        self.assertTrue((copied / 'message.so').is_file())
        self.assertFalse((self.root / '.robot-sync-backups').exists())

    def test_cached_only_prepares_only_packages_with_enabled_cmake_cache(self):
        for value in (None, 'OFF', '0', '1', 'ON', 'TRUE'):
            with self.subTest(value=value):
                name = 'cache_' + str(value).lower()
                copied, _ = self.module(name)
                if value is not None:
                    (self.root / f'build/{name}/CMakeCache.txt').write_text(
                        f'AMENT_CMAKE_SYMLINK_INSTALL:BOOL={value}\n')
                result = sync.prepare_symlink_build(self.root, 'cache-' + str(value), cached_only=True)
                enabled = value in ('1', 'ON', 'TRUE')
                self.assertEqual(result['moved'], int(enabled))
                self.assertEqual(copied.exists(), not enabled)

    def test_backup_collision_is_rejected_before_any_directory_is_moved(self):
        first, _ = self.module('aaa_interface')
        last, _ = self.module('zzz_interface')
        saved = self.root / '.robot-sync-backups/test-build/build-conflicts' / last.relative_to(self.root)
        saved.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, 'backup already exists'):
            sync.prepare_symlink_build(self.root, 'test-build')
        self.assertTrue(first.is_dir())
        self.assertTrue(last.is_dir())

    def test_symlink_ancestor_cannot_alias_source_or_escape_workspace(self):
        for outside in (False, True):
            with self.subTest(outside=outside):
                build = self.root / 'build'
                target = Path(self.temp.name) / 'outside' if outside else self.root / 'src/brain'
                target.mkdir(exist_ok=True)
                build.symlink_to(target, target_is_directory=True)
                with self.assertRaises(ValueError):
                    sync.prepare_symlink_build(self.root, 'test-build')
                self.assertTrue(build.is_symlink())
                build.unlink()

    def test_failed_move_restores_already_moved_artifacts(self):
        first, _ = self.module('aaa_interface')
        last, _ = self.module('zzz_interface')
        replace = sync.os.replace

        def fail_second(source, target):
            if Path(source) == last:
                raise OSError('simulated filesystem failure')
            replace(source, target)

        with mock.patch.object(sync.os, 'replace', side_effect=fail_second):
            with self.assertRaisesRegex(OSError, 'simulated filesystem failure'):
                sync.prepare_symlink_build(self.root, 'test-build')
        for path in (first, last):
            self.assertEqual((path / 'message.so').read_bytes(), b'old compiled message\n')


class DriverIntegrationTests(unittest.TestCase):
    """Run the real driver/agent pipeline locally, without SSH or robot motion."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.local = Path(self.temp.name) / 'local'
        self.robot = Path(self.temp.name) / 'robot'
        self.stages = set()
        def cleanup_stages():
            for path in self.stages:
                resolved = path.resolve()
                self.assertEqual(resolved.parent, Path('/tmp').resolve())
                self.assertRegex(resolved.name, r'^k1-source-sync-\d{8}T\d{6}Z-[0-9a-f]{8}$')
                if resolved.exists():
                    shutil.rmtree(resolved)
        self.addCleanup(cleanup_stages)
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
            rollback=None, build=False, start=False, choose_action=False, dry_run=False)
        owner = self

        class LocalTransport:
            def __init__(self, args):
                pass

            def ssh(self, host, command, timeout=300, stream=False, progress=None, log_path=None):
                owner.assertEqual(host, '192.168.133.43')
                if progress is not None:
                    code, detail = sync.stream_build(['bash', '-c', command], timeout, progress, log_path)
                    if code:
                        raise RuntimeError(f'{host}: SSH command failed ({code}): {detail}')
                    return ''
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

    def test_menu_is_after_sync_and_defaults_to_no_build_or_start(self):
        self.args.choose_action = True
        def answer():
            self.assertEqual((self.robot / 'src/brain/code.cpp').read_bytes(), b'new source\n')
            return 'sync'
        with mock.patch.object(sync, 'choose_action', side_effect=answer), \
                mock.patch.object(sync, 'build_command', side_effect=AssertionError('Must not build')), \
                mock.patch.object(sync, 'start_command', side_effect=AssertionError('Must not start')):
            report = self.run_driver()
        self.assertEqual(report['action'], 'sync')
        self.assertNotIn('build', report['robots']['192.168.133.43'])

    def test_cancel_at_menu_retains_synced_code_and_writes_success_report(self):
        self.args.choose_action = True
        with mock.patch('builtins.input', side_effect=KeyboardInterrupt), \
                mock.patch.object(sync, 'build_command', side_effect=AssertionError('Must not build')), \
                mock.patch.object(sync, 'start_command', side_effect=AssertionError('Must not start')):
            report = self.run_driver()
        self.assertEqual(report['action'], 'sync')
        self.assertEqual(report['robots']['192.168.133.43']['state'], 'synced')
        self.assertEqual((self.robot / 'src/brain/code.cpp').read_bytes(), b'new source\n')
        self.assertNotIn('build', report['robots']['192.168.133.43'])
        self.assertNotIn('start', report['robots']['192.168.133.43'])

    def test_build_and_start_loads_environment_only_after_success(self):
        self.args.start = True
        marker = self.robot / 'built.marker'
        started = self.robot / 'started.marker'
        build = 'python3 -c ' + sync.shlex.quote(f'from pathlib import Path; Path({str(marker)!r}).touch()')
        start = 'python3 -c ' + sync.shlex.quote(
            f'from pathlib import Path; assert Path({str(marker)!r}).is_file(); Path({str(started)!r}).touch()')
        with mock.patch.object(sync, 'build_command', return_value=build), \
                mock.patch.object(sync, 'start_command', return_value=start):
            report = self.run_driver()
        self.assertEqual(report['action'], 'start')
        self.assertEqual(report['robots']['192.168.133.43']['build'], 'passed')
        self.assertEqual(report['robots']['192.168.133.43']['start'], 'command_completed')
        self.assertTrue(started.is_file())

    def test_failed_build_never_calls_start_and_cleans_staging(self):
        self.args.start = True
        with mock.patch.object(sync, 'build_command', return_value='exit 2'), \
                mock.patch.object(sync, 'start_command', side_effect=AssertionError('Must not start')):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
                sync.run_local(self.args)
        report = sync.read_json(next((self.local / '.robot-sync').glob('*/result.json')))
        self.assertFalse(report['success'])
        self.assertEqual(report['robots']['192.168.133.43']['build'], 'failed')
        self.assertNotIn('start', report['robots']['192.168.133.43'])
        self.assertTrue(all(not path.exists() for path in self.stages))

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
        self.assertEqual(config['game_control_ip'], 'bad-ip')
        self.assertEqual(config['strategy']['cooperation']['owner_hold_ms'], 1500.0)
        self.assertIn("'ip_white_list': ['192.168.133.210']", (self.robot / sync.GC_LAUNCH).read_text())
        self.assertEqual(self.run_driver()['robots']['192.168.133.43']['result']['changed'], 0)
        self.args.rollback = report['run_id']
        self.assertEqual(self.run_driver()['robots']['192.168.133.43']['rollback_result']['restored'], 5)
        self.assertEqual((self.robot / 'src/brain/code.cpp').read_bytes(), b'old source\n')
        self.assertEqual((self.robot / sync.BRAIN_CONFIG).read_text(), self.old_config)
        self.assertFalse((self.robot / 'src/brain/new.h').exists())
        self.assertFalse((self.robot / 'scripts/start.sh').exists())

    def build_fixture(self):
        name = 'booster_interface'
        copied = self.robot / f'build/{name}/ament_cmake_python/{name}/{name}'
        generated = self.robot / f'build/{name}/rosidl_generator_py/{name}'
        for path in (copied, generated):
            path.mkdir(parents=True)
            (path / '__init__.py').write_bytes(b'# generated\n')
        return copied, generated

    def test_driver_build_prepares_remote_artifacts_before_building(self):
        copied, generated = self.build_fixture()
        self.args.build = True
        command = 'python3 -c ' + sync.shlex.quote(
            f'from pathlib import Path; Path({str(copied)!r}).symlink_to({str(generated)!r})')
        with mock.patch.object(sync, 'build_command', return_value=command):
            report = self.run_driver()
        result = report['robots']['192.168.133.43']
        self.assertEqual(result['build'], 'passed')
        self.assertEqual(result['build_preparation']['moved'], 1)
        self.assertTrue(Path(result['build_log']).is_file())
        self.assertTrue(copied.is_symlink())
        saved = Path(result['build_preparation']['changes'][0]['backup'])
        self.assertEqual((saved / '__init__.py').read_bytes(), b'# generated\n')

    def test_build_flag_does_not_modify_artifacts_in_preview(self):
        copied, _ = self.build_fixture()
        self.args.build = self.args.dry_run = True
        with mock.patch.object(sync, 'build_command', side_effect=AssertionError('Must not build')):
            report = self.run_driver()
        self.assertNotIn('build_preparation', report['robots']['192.168.133.43'])
        self.assertTrue(copied.is_dir())
        self.assertFalse(copied.is_symlink())

    def test_driver_failed_build_keeps_source_sync_result_and_full_log(self):
        self.args.build = True
        command = 'python3 -c ' + sync.shlex.quote(
            'import sys; print("error: fixture build failed", file=sys.stderr); sys.exit(2)')
        with mock.patch.object(sync, 'build_command', return_value=command):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(RuntimeError, 'SSH command failed'):
                sync.run_local(self.args)
        report = sync.read_json(next((self.local / '.robot-sync').glob('*/result.json')))
        result = report['robots']['192.168.133.43']
        self.assertFalse(report['success'])
        self.assertEqual(result['state'], 'synced')
        self.assertEqual(result['build'], 'failed')
        self.assertIn('error: fixture build failed', Path(result['build_log']).read_text())


if __name__ == '__main__':
    unittest.main(verbosity=2)
