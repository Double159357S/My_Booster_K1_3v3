#!/usr/bin/env python3
"""Windows/PuTTY source sync driver and isolated Linux remote agent.

Local side needs only Python's standard library. The remote configuration merge
uses PyYAML already supplied by ROS. Sync alone never builds or starts the Demo;
post-sync build/start actions require an explicit option or interactive choice.
"""
import argparse
import ast
from collections import deque
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
import hashlib
import io
import ipaddress
import json
import os
from pathlib import Path, PurePosixPath
import queue
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import uuid
from datetime import datetime, timezone

CODE_DIRS = ('src', 'scripts', 'setup', 'configs', 'utils', 'distribution')
SKIP_DIRS = {'.git', '.vscode', '.venv', '__pycache__', 'build', 'install', 'log',
             '.robot-sync', '.robot-sync-backups', '.pytest_cache', 'node_modules',
             'logs', 'test_results', 'rrlog', 'vision_log', 'ros_bags'}
SKIP_SUFFIXES = ('.log', '.pyc', '.pyo', '.zip', '.tar', '.tar.gz', '.tgz', '.bag',
                 '.tar.bz2', '.tar.xz', '.7z', '.o', '.obj', '.pdb', '.exe',
                 '.rrd', '.db3', '.mcap')
PROTECTED_FILES = {'configs/fastdds.xml'}
BRAIN_CONFIG = 'src/brain/config/config.yaml'
GC_LAUNCH = 'src/game_controller/launch/launch.py'
TEXT_SUFFIXES = {'.cpp', '.cc', '.c', '.h', '.hpp', '.sh', '.py', '.xml', '.yaml',
                 '.yml', '.cmake', '.md', '.txt', '.json', '.ps1', '.msg', '.srv', '.cfg'}


def validate_ip_prefix(value):
    """Validate the subnet before asking for robot suffixes or connecting."""
    prefix = (value or '').strip().rstrip('.')
    error_message = (f'IP 段应填写前三段数字，例如 192.168.133；'
                     f'.43,.21 这样的机器人尾号请在下一项填写。收到：{value!r}')
    if len(prefix.split('.')) != 3:
        raise ValueError(error_message)
    try:
        ipaddress.IPv4Address(prefix + '.1')
    except ipaddress.AddressValueError as error:
        raise ValueError(error_message) from error
    return prefix


def select_hosts(hosts=None, ip_prefix=None, robot_ids=None, reader=input):
    """Only addresses entered by the operator can become sync targets."""
    if hosts is not None:
        if ip_prefix is not None or robot_ids is not None:
            raise ValueError('不能同时指定完整 IP 和 IP 段/尾号')
        selected = [h for h in re.split(r'[,，;；\s]+', hosts.strip()) if h]
        try:
            selected = [str(ipaddress.IPv4Address(h)) for h in selected]
        except ipaddress.AddressValueError as error:
            raise ValueError('机器人 IP 无效') from error
    else:
        if ip_prefix is None:
            ip_prefix = reader('请输入 IP 段，例如 192.168.133：')
        prefix = validate_ip_prefix(ip_prefix)
        if robot_ids is None:
            robot_ids = reader('请输入机器人尾号，例如 .43 或 .43,.21：')
        suffixes = [s for s in re.split(r'[,，;；\s]+', robot_ids.strip()) if s]
        selected = []
        for suffix in suffixes:
            if not re.fullmatch(r'\.?\d{1,3}', suffix) or not 1 <= int(suffix.lstrip('.')) <= 254:
                raise ValueError('尾号应为 .43 或 43 这种格式，范围为 1–254')
            selected.append(prefix + '.' + str(int(suffix.lstrip('.'))))
    if not selected:
        raise ValueError('必须明确输入至少一台机器人，空输入不会选择全部')
    if len(set(selected)) != len(selected):
        raise ValueError('机器人 IP 不能重复')
    return selected


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def relative_path(name):
    p = PurePosixPath(name)
    if not name or '\\' in name or p.is_absolute() or '..' in p.parts or str(p) != name:
        raise ValueError('Unsafe relative path: ' + repr(name))
    return p


def destination(root, name):
    p = root / str(relative_path(name))
    # Check the parent, not the final symlink: replacing a link must not write
    # into its old referent or follow it outside the workspace.
    if not p.parent.resolve().is_relative_to(root):
        raise ValueError('Path leaves workspace: ' + name)
    return p


def protected(name):
    p = PurePosixPath(name)
    return (name in PROTECTED_FILES or p.name.endswith('_local.yaml') or
            (name.startswith('src/vision/config/') and p.suffix in ('.yaml', '.yml')))


def source_bytes(path):
    data = path.read_bytes()
    if path.suffix.lower() in TEXT_SUFFIXES or path.name in ('CMakeLists.txt', 'Makefile', 'COLCON_IGNORE'):
        try:
            data.decode('utf-8-sig')
        except UnicodeDecodeError:
            return data
        # Linux shell scripts must not receive Windows CRLF or a UTF-8 BOM.
        if path.suffix.lower() != '.ps1':
            data = data.removeprefix(b'\xef\xbb\xbf')
        data = data.replace(b'\r\n', b'\n')
    return data


def git_links(root):
    if not shutil.which('git') or not (root / '.git').exists():
        return set()
    result = subprocess.run(['git', '-C', str(root), 'ls-files', '--stage', '-z'],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    return {item.split(b'\t', 1)[1].decode('utf-8') for item in result.stdout.split(b'\0')
            if item.startswith(b'120000 ') and b'\t' in item}


def validate_link(name, target):
    import posixpath
    combined = posixpath.normpath(str(PurePosixPath(name).parent / target))
    if PurePosixPath(target).is_absolute() or combined == '..' or combined.startswith('../'):
        raise ValueError('Source symlink leaves workspace: ' + name)


def scan_local(root, overwrite_config=False):
    links = git_links(root)
    manifest, skipped = [], []
    for folder in CODE_DIRS:
        start = root / folder
        if not start.is_dir():
            continue
        for directory, dirs, files in os.walk(start, followlinks=False):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
            # Directory symlinks are deliberately rejected, not recursively followed.
            for d in dirs:
                if (Path(directory) / d).is_symlink():
                    raise ValueError('Directory symlink needs explicit handling: ' + str(Path(directory) / d))
            for filename in sorted(files):
                path = Path(directory) / filename
                name = path.relative_to(root).as_posix()
                if name.endswith(SKIP_SUFFIXES):
                    continue
                if not overwrite_config and protected(name):
                    skipped.append(name)
                    continue
                if path.is_symlink() or name in links:
                    target = os.readlink(path) if path.is_symlink() else path.read_text(encoding='utf-8').strip()
                    validate_link(name, target)
                    manifest.append({'path': name, 'kind': 'symlink', 'target': target,
                                     'sha256': digest(target.encode('utf-8')), 'mode': 0o777})
                else:
                    manifest.append({'path': name, 'kind': 'file',
                                     'sha256': digest(source_bytes(path)),
                                     'mode': 0o755 if path.suffix == '.sh' else 0o644})
    if not manifest:
        raise ValueError('No source files found')
    templates = {}
    if not overwrite_config:
        for name in (BRAIN_CONFIG, GC_LAUNCH):
            path = root / name
            if path.is_file():
                templates[name] = source_bytes(path).decode('utf-8')
    return {'files': manifest, 'skipped': skipped, 'templates': templates}


def file_info(path):
    if path.is_symlink():
        target = os.readlink(path)
        return {'kind': 'symlink', 'sha256': digest(target.encode('utf-8')), 'mode': 0o777}
    if not path.exists():
        return None
    if not path.is_file():
        raise ValueError('Expected a file, found directory/special file: ' + str(path))
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return {'kind': 'file', 'sha256': h.hexdigest(), 'mode': stat.S_IMODE(path.stat().st_mode)}


def merge_brain(local_text, remote_text):
    import yaml
    local_doc, remote_doc = yaml.safe_load(local_text), yaml.safe_load(remote_text)
    local = local_doc['brain_node']['ros__parameters']
    remote = remote_doc['brain_node']['ros__parameters']
    if not isinstance(remote, dict) or not isinstance(local, dict):
        raise ValueError('Invalid brain configuration')
    if not isinstance(remote.get('game', {}).get('player_id'), int):
        raise ValueError('Missing robot player_id; refusing to copy another robot identity')
    # Keep the whole robot-specific game block, including future/remote-only
    # fields. All other settings come from local except these two log locations.
    log_locations = ('server_ip', 'log_dir')
    root_node = yaml.compose(local_text)

    def lookup(mapping, path):
        for key in path:
            mapping = mapping[key]
        return mapping

    def node_at(path):
        node = root_node
        for key in ('brain_node', 'ros__parameters') + path:
            found = [v for k, v in node.value if k.value == key]
            if len(found) != 1:
                raise KeyError(key)
            node = found[0]
        return node

    def render(node, value):
        if isinstance(node, yaml.MappingNode) and not node.flow_style:
            # Keep block YAML so setup/set_up.py can still edit individual keys.
            lines = yaml.safe_dump(value, default_flow_style=False, sort_keys=False,
                                   allow_unicode=True, width=100000).rstrip('\n').splitlines()
            indent = ' ' * node.start_mark.column
            text = ('\n' + indent).join(lines) + '\n'
            return text
        return yaml.safe_dump(value, default_flow_style=True, sort_keys=False,
                              allow_unicode=True, width=100000).strip().removesuffix('\n...')

    def replace_value(node, value):
        end = node.end_mark.index
        if isinstance(node, yaml.MappingNode) and not node.flow_style and end < len(local_text):
            # A block mapping's end mark points past the next key's indent.
            # Leave that indent outside the edit, including when adding a block.
            end -= node.end_mark.column
        return node.start_mark.index, end, render(node, value)

    def leaves(value, prefix):
        if isinstance(value, dict):
            for key, child in value.items():
                yield from leaves(child, prefix + (key,))
        else:
            yield '.'.join(prefix)

    replacements = []
    kept = list(leaves(remote['game'], ('game',)))
    game_node = node_at(('game',))
    if not isinstance(local.get('game'), dict):
        raise ValueError('Invalid local game configuration')
    if local['game'] != remote['game']:
        replacements.append(replace_value(game_node, remote['game']))

    local_log, remote_log = local.get('rerunLog', {}), remote.get('rerunLog', {})
    if not isinstance(local_log, dict) or not isinstance(remote_log, dict):
        raise ValueError('Invalid rerunLog configuration')
    kept.extend('rerunLog.' + name for name in log_locations if name in remote_log)
    if 'rerunLog' not in local and any(name in remote_log for name in log_locations):
        # The local template may remove logging settings; retained locations
        # still belong to the robot, so append them without copying other keys.
        params = node_at(())
        indent = ' ' * params.value[0][0].start_mark.column
        retained = {name: remote_log[name] for name in log_locations if name in remote_log}
        text = yaml.safe_dump({'rerunLog': retained}, sort_keys=False, allow_unicode=True)
        text = ''.join(indent + line for line in text.splitlines(keepends=True))
        index = params.end_mark.index
        if index < len(local_text):
            index -= params.end_mark.column
        if index and not local_text[:index].endswith('\n'):
            text = '\n' + text
        replacements.append((index, index, text))
    elif any((name in local_log) != (name in remote_log) for name in log_locations):
        # Preserve absence as well as values: never introduce a local log IP or
        # directory that the robot did not have, and keep remote-only locations.
        merged_log = dict(local_log)
        for name in log_locations:
            if name in remote_log:
                merged_log[name] = remote_log[name]
            else:
                merged_log.pop(name, None)
        node = node_at(('rerunLog',))
        replacements.append(replace_value(node, merged_log))
    else:
        for name in log_locations:
            if name not in remote_log:
                continue
            path = ('rerunLog', name)
            value, original = lookup(remote, path), lookup(local, path)
            node = node_at(path)
            if not isinstance(node, yaml.ScalarNode):
                raise ValueError('Protected brain parameter must be scalar: ' + '.'.join(path))
            if type(value) is type(original) and value == original:
                continue
            replacements.append(replace_value(node, value))
    for start, end, value in sorted(replacements, reverse=True):
        local_text = local_text[:start] + value + local_text[end:]
    yaml.safe_load(local_text)  # The edited text must still be valid YAML.
    return local_text.encode('utf-8'), kept


def merge_gc_launch(local_text, remote_text):
    names = {'enable_ip_white_list', 'ip_white_list'}

    def entries(text):
        tree = ast.parse(text)
        found = {}
        for node in ast.walk(tree):
            if not isinstance(node, ast.Dict):
                continue
            for key, value in zip(node.keys, node.values):
                if isinstance(key, ast.Constant) and key.value in names:
                    if key.value in found:
                        raise ValueError('Multiple GameController whitelist definitions')
                    found[key.value] = (ast.literal_eval(value), value)
        return found

    local, remote = entries(local_text), entries(remote_text)
    # AST columns are UTF-8 byte offsets, not Unicode character offsets.
    lines = local_text.encode('utf-8').splitlines(keepends=True)
    data = b''.join(lines)
    offsets = [0]
    for line in lines:
        offsets.append(offsets[-1] + len(line))
    edits, kept = [], []
    for name in sorted(names):
        if name not in remote or name not in local:
            raise ValueError('Cannot preserve GameController setting: ' + name)
        value, node = remote[name][0], local[name][1]
        kept.append(name)
        if value != local[name][0]:
            edits.append((offsets[node.lineno - 1] + node.col_offset,
                          offsets[node.end_lineno - 1] + node.end_col_offset,
                          repr(value).encode('utf-8')))
    for start, end, value in sorted(edits, reverse=True):
        data = data[:start] + value + data[end:]
    ast.parse(data.decode('utf-8'))
    return data, kept


def prepare_plan(root, request, stage):
    changes, kept = [], {}
    effective = stage / 'effective'
    for spec in request['files']:
        name = spec['path']
        path = destination(root, name)
        before = file_info(path)
        after = dict(spec)
        if name in request.get('templates', {}):
            if before is None or before['kind'] != 'file':
                raise ValueError('Robot configuration is missing or not a regular file: ' + name)
            merge = merge_brain if name == BRAIN_CONFIG else merge_gc_launch
            data, kept[name] = merge(request['templates'][name], path.read_text(encoding='utf-8-sig'))
            variant = effective / str(relative_path(name))
            variant.parent.mkdir(parents=True, exist_ok=True)
            variant.write_bytes(data)
            after['sha256'] = digest(data)
            after['effective'] = True
        if after['kind'] == 'file' and before and before['kind'] == 'file':
            after['mode'] = before['mode'] | (0o111 if name.endswith('.sh') else 0)
        desired = {key: after[key] for key in ('kind', 'sha256', 'mode')}
        if before != desired:
            changes.append({'path': name, 'before': before, 'after': after,
                            'action': 'new' if before is None else 'update'})
    plan = {'root': str(root), 'changes': changes, 'kept': kept,
            'total': len(request['files']), 'skipped': request.get('skipped', [])}
    write_json(stage / 'plan.json', plan)
    return plan


def safe_unpack(archive, payload, expected):
    seen = set()
    with tarfile.open(archive, 'r:gz') as tar:
        for member in tar:
            name = str(relative_path(member.name))
            if name not in expected or name in seen:
                raise ValueError('Unexpected/duplicate archive member: ' + name)
            seen.add(name)
            spec = expected[name]
            target = payload / name
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.issym() and spec['kind'] == 'symlink':
                validate_link(name, member.linkname)
                if member.linkname != spec['target']:
                    raise ValueError('Symlink target mismatch: ' + name)
                target.symlink_to(member.linkname)
            elif member.isfile() and spec['kind'] == 'file':
                with tar.extractfile(member) as source, target.open('wb') as output:
                    shutil.copyfileobj(source, output)
            else:
                raise ValueError('Archive file type mismatch: ' + name)
            if file_info(target)['sha256'] != spec['sha256']:
                raise ValueError('Uploaded file hash mismatch: ' + name)
    if seen != set(expected):
        raise ValueError('Archive is missing requested files')


def atomic_copy(source, target, mode):
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.parent / ('.' + target.name + '.codex-' + uuid.uuid4().hex)
    try:
        if source.is_symlink():
            temporary.symlink_to(os.readlink(source))
        else:
            with source.open('rb') as src, temporary.open('xb') as dst:
                shutil.copyfileobj(src, dst)
                dst.flush()
                os.fsync(dst.fileno())
            temporary.chmod(mode)
        os.replace(temporary, target)
    finally:
        if temporary.exists() or temporary.is_symlink():
            temporary.unlink()


def restore_changes(root, backup, changes):
    for item in reversed(changes):
        target = destination(root, item['path'])
        if item['before'] is None:
            target.unlink(missing_ok=True)
        else:
            atomic_copy(backup / 'files' / item['path'], target, item['before']['mode'])


def apply_plan(root, stage, run_id, archive_hash):
    plan = read_json(stage / 'plan.json')
    if plan['root'] != str(root):
        raise ValueError('Plan workspace does not match')
    changes = plan['changes']
    if not changes:
        return {'changed': 0, 'backup': None}
    expected = {i['path']: i['after'] for i in changes if not i['after'].get('effective')}
    payload = stage / 'payload'
    if expected:
        archive = stage / 'payload.tar.gz'
        if file_info(archive)['sha256'] != archive_hash:
            raise ValueError('Upload archive checksum failed')
        safe_unpack(archive, payload, expected)
    # Check every destination before touching any project source.
    for item in changes:
        if file_info(destination(root, item['path'])) != item['before']:
            raise ValueError('Robot file changed since comparison: ' + item['path'])
        base = stage / 'effective' if item['after'].get('effective') else payload
        if file_info(base / item['path'])['sha256'] != item['after']['sha256']:
            raise ValueError('Staged source changed: ' + item['path'])
    backup = destination(root, '.robot-sync-backups/' + run_id)
    if backup.exists():
        raise ValueError('Backup ID already exists')
    backup.mkdir(parents=True)
    for item in changes:
        if item['before'] is not None:
            saved = backup / 'files' / item['path']
            saved.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(destination(root, item['path']), saved, follow_symlinks=False)
            if file_info(saved) != item['before']:
                raise ValueError('Robot source changed while backing up: ' + item['path'])
    record = {'root': str(root), 'state': 'prepared', 'changes': changes}
    write_json(backup / 'manifest.json', record)
    written = []
    try:
        for item in changes:
            base = stage / 'effective' if item['after'].get('effective') else payload
            if file_info(destination(root, item['path'])) != item['before']:
                raise ValueError('Robot file changed during sync: ' + item['path'])
            atomic_copy(base / item['path'], destination(root, item['path']), item['after']['mode'])
            written.append(item)
            actual = file_info(destination(root, item['path']))
            wanted = {k: item['after'][k] for k in ('kind', 'sha256', 'mode')}
            if actual != wanted:
                raise ValueError('Post-copy verification failed: ' + item['path'])
        record['state'] = 'applied'
        write_json(backup / 'manifest.json', record)
    except Exception:
        restore_changes(root, backup, written)
        record['state'] = 'automatically_rolled_back'
        write_json(backup / 'manifest.json', record)
        raise
    return {'changed': len(changes), 'backup': str(backup)}


def rollback(root, backup_id, dry_run=False):
    if not re.fullmatch(r'[A-Za-z0-9_-]+', backup_id):
        raise ValueError('Rollback must use a backup ID, not an arbitrary path')
    backup = destination(root, '.robot-sync-backups/' + backup_id)
    record = read_json(backup / 'manifest.json')
    if record['root'] != str(root) or record['state'] != 'applied':
        raise ValueError('Backup is not an applied sync for this workspace')
    for item in record['changes']:
        wanted = {k: item['after'][k] for k in ('kind', 'sha256', 'mode')}
        if file_info(destination(root, item['path'])) != wanted:
            raise ValueError('Changed after sync; refusing to overwrite during rollback: ' + item['path'])
        if item['before'] and file_info(backup / 'files' / item['path']) != item['before']:
            raise ValueError('Backup checksum/type/mode mismatch: ' + item['path'])
    if not dry_run:
        restore_changes(root, backup, record['changes'])
        record['state'] = 'rolled_back'
        write_json(backup / 'manifest.json', record)
    return {'restored': len(record['changes']), 'dry_run': dry_run}


class BuildProgress:
    """Show actual package/CMake progress; keep ticking while output is quiet."""

    def __init__(self, host, output=None, clock=time.monotonic, parallel=False, interactive=None):
        self.host, self.output, self.clock = host, output or sys.stdout, clock
        self.interactive = self.output.isatty() if interactive is None else interactive
        self.parallel = parallel
        self.active = set()
        self.started = clock()
        self.last_refresh = float('-inf')
        self.total, self.completed = None, set()
        self.package, self.percent, self.state = 'preparing', None, 'running'
        self.line_width = 0

    def consume(self, line):
        text = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', line).strip()
        total = re.fullmatch(r'__K1_BUILD_TOTAL__=(\d+)', text)
        start = re.match(r'Starting\s+>>>\s+(\S+)', text)
        end = re.match(r'Finished\s+<<<\s+(\S+)', text)
        failed = re.match(r'(Failed|Aborted)\s+<<<\s+(\S+)', text)
        percent = re.match(r'\[\s*(\d{1,3})%\]', text)
        if total:
            self.total = int(total[1])
        elif start:
            self.active.add(start[1])
            self.package, self.percent = start[1], None
        elif end:
            self.completed.add(end[1])
            self.active.discard(end[1])
            self.package, self.percent = 'next package', None
        elif failed:
            self.active.discard(failed[2])
            self.package, self.state = failed[2], 'failed'
        elif percent and not self.parallel and 0 <= int(percent[1]) <= 100:
            self.percent = int(percent[1])
        # The full stream stays in the build log. Compact repetitive Make and
        # installation output here, while keeping diagnostics/start/end visible.
        if not total and not percent and not text.startswith(
                ('-- Installing:', '-- Up-to-date:', '-- Symlinking:')):
            self.clear()
            prefix = self.host + ' | ' if not self.interactive else ''
            self.output.write(prefix + line.rstrip('\r\n') + '\n')
            self.output.flush()
        self.refresh(force=bool(total or start or end or failed))

    def clear(self):
        if self.interactive and self.line_width:
            self.output.write('\r' + ' ' * self.line_width + '\r')
            self.line_width = 0

    def refresh(self, force=False):
        now = self.clock()
        # In redirected output use occasional whole lines instead of carriage
        # returns. Normal PowerShell terminals get an in-place animated bar.
        if not force and now - self.last_refresh < (0.5 if self.interactive else 10):
            return
        elapsed = max(0, int(now - self.started))
        if self.parallel and self.total:
            # Interleaved CMake percentages cannot be assigned to a package.
            # Show completed packages instead of inventing a per-package value.
            filled = min(16, len(self.completed) * 16 // self.total)
            bar = '#' * filled + '-' * (16 - filled)
            value = 'packages'
        elif self.percent is None:
            position = elapsed % 16
            bar = '-' * position + '>' + '-' * (15 - position)
            value = 'wait'
        else:
            filled = self.percent * 16 // 100
            bar = '#' * filled + '-' * (16 - filled)
            value = f'{self.percent}%'
        package = ','.join(sorted(self.active)) if self.parallel and self.active else self.package
        package = package if len(package) <= 30 else package[:27] + '...'
        status = (f'{self.host} | {len(self.completed)}/{self.total if self.total is not None else "?"} pkgs '
                  f'| {package} [{bar}] {value} | {elapsed // 60:02}:{elapsed % 60:02}')
        if self.state != 'running':
            status += ' ' + ('OK' if self.state == 'passed' else 'FAILED')
        if self.interactive:
            width = max(20, shutil.get_terminal_size((100, 20)).columns - 1)
            status = status[:width]
            self.output.write('\r' + status + ' ' * max(0, self.line_width - len(status)))
            self.line_width = len(status)
        else:
            self.output.write(status + '\n')
        self.output.flush()
        self.last_refresh = now

    def finish(self, success):
        self.state = 'passed' if success else 'failed'
        if success:
            self.package, self.percent = 'complete', 100
        self.refresh(force=True)
        if self.interactive:
            self.output.write('\n')
            self.output.flush()
            self.line_width = 0


def stream_build(command, timeout, progress, log_path):
    """Drain SSH output without blocking the progress heartbeat or timeout."""
    events = queue.Queue()
    tail = deque(maxlen=20)
    deadline = time.monotonic() + timeout
    with Path(log_path).open('w', encoding='utf-8') as log:
        with subprocess.Popen(command, stdin=subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, encoding='utf-8', errors='replace', bufsize=1) as process:
            def read_output():
                try:
                    for line in process.stdout:
                        events.put(line)
                except Exception as error:
                    events.put(error)
                finally:
                    events.put(None)

            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            try:
                progress.refresh(force=True)
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise subprocess.TimeoutExpired(command, timeout)
                    try:
                        line = events.get(timeout=min(0.5, remaining))
                    except queue.Empty:
                        progress.refresh()
                        continue
                    if line is None:
                        break
                    if isinstance(line, Exception):
                        raise line
                    log.write(line)
                    log.flush()
                    tail.append(line)
                    progress.consume(line)
                code = process.wait(timeout=max(0, deadline - time.monotonic()))
                progress.finish(code == 0)
                return code, ''.join(tail)
            except BaseException:
                process.kill()
                process.wait()
                progress.finish(False)
                raise
            finally:
                reader.join(timeout=1)


class Transport:
    def __init__(self, args):
        self.args = args

    def authentication(self):
        a = self.args
        auth = ['-i', a.key_file] if a.key_file else ['-pwfile', a.password_file]
        return ['-batch', '-hostkey', a.host_key, '-l', a.user] + auth

    def ssh(self, host, command, timeout=300, stream=False, progress=None, log_path=None):
        # Batch commands authenticate from a key/password file and never need
        # the operator's keyboard. Do not let PuTTY inherit the menu's stdin.
        args = [self.args.plink, '-ssh', '-T', '-no-antispoof'] + self.authentication() + [host, command]
        if progress is not None:
            code, detail = stream_build(args, timeout, progress, log_path)
            if code:
                raise RuntimeError(f'{host}: SSH command failed ({code}): {detail.strip()}')
            return ''
        result = subprocess.run(args, stdin=subprocess.DEVNULL,
                                stdout=None if stream else subprocess.PIPE,
                                stderr=None if stream else subprocess.PIPE,
                                timeout=timeout)
        if result.returncode:
            detail = (result.stderr or b'').decode('utf-8', errors='replace').strip()
            raise RuntimeError(f'{host}: SSH command failed ({result.returncode}): {detail}')
        return (result.stdout or b'').decode('utf-8')

    def upload(self, host, local, remote):
        command = [self.args.pscp, '-q', '-sftp'] + self.authentication() + [str(local), host + ':' + remote]
        result = subprocess.run(command, stdin=subprocess.DEVNULL,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=900)
        if result.returncode:
            raise RuntimeError(host + ': upload failed: ' + result.stderr.decode('utf-8', errors='replace'))


def make_archive(root, changes, path):
    with tarfile.open(path, 'w:gz') as tar:
        for item in changes:
            spec = item['after']
            if spec.get('effective'):
                continue
            info = tarfile.TarInfo(item['path'])
            info.mode = spec['mode']
            if spec['kind'] == 'symlink':
                info.type, info.linkname = tarfile.SYMTYPE, spec['target']
                tar.addfile(info)
            else:
                data = source_bytes(root / item['path'])
                if digest(data) != spec['sha256']:
                    raise ValueError('Local file changed during sync; run again: ' + item['path'])
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))


def prepare_symlink_build(root, run_id):
    """Back up copied ROS message modules that block a symlink build.

    Only touch the generated ament Python module directory, with a matching
    rosidl_generator_py module. Source, install output and robot settings stay
    outside this operation. Existing symlinks already have the desired type.
    """
    if not re.fullmatch(r'[A-Za-z0-9_-]+', run_id or ''):
        raise ValueError('Invalid backup ID')
    root = Path(root).resolve()

    def checked(name):
        path = root / relative_path(name)
        if not path.resolve().is_relative_to(root):
            raise ValueError('Build preparation path escapes workspace: ' + name)
        # Even links pointing inside the workspace could alias source/settings.
        current = root
        for part in path.relative_to(root).parts:
            current = current / part
            if current.is_symlink():
                raise ValueError('Build preparation path contains a symlink: ' + name)
        return path

    build = checked('build')
    backup_name = '.robot-sync-backups/' + run_id + '/build-conflicts'
    changes = []
    if build.is_dir():
        for package in sorted(build.iterdir()):
            if package.is_symlink() or not package.is_dir() or not re.fullmatch(r'[a-z][a-z0-9_]*', package.name):
                continue
            name = package.name
            relative = f'build/{name}/ament_cmake_python/{name}/{name}'
            copied = root / relative
            if copied.is_symlink() or not copied.is_dir():
                continue
            checked(relative)
            generated = checked(f'build/{name}/rosidl_generator_py/{name}')
            if not (generated / '__init__.py').is_file():
                continue
            saved = checked(backup_name + '/' + relative)
            if saved.exists():
                raise ValueError('Build conflict backup already exists: ' + str(saved))
            changes.append({'path': relative, 'backup': str(saved)})
    if not changes:
        return {'moved': 0, 'changes': []}
    # Validate every candidate before moving any directory. Keep a separate
    # artifact record; source rollback must not restore old build directories.
    backup = checked(backup_name)
    backup.mkdir(parents=True, exist_ok=True)
    record = {'state': 'prepared', 'changes': changes}
    write_json(backup / 'manifest.json', record)
    moved = []
    try:
        for item in changes:
            saved = Path(item['backup'])
            saved.parent.mkdir(parents=True, exist_ok=True)
            os.replace(root / item['path'], saved)
            moved.append(item)
    except Exception:
        for item in reversed(moved):
            os.replace(item['backup'], root / item['path'])
        record['state'] = 'automatically_restored'
        write_json(backup / 'manifest.json', record)
        raise
    record['state'] = 'backed_up'
    write_json(backup / 'manifest.json', record)
    return {'moved': len(changes), 'backup': str(backup), 'changes': changes}


def build_command(remote_root):
    # Use the same entry point as manual builds. Colcon schedules every package
    # in parallel once its dependencies are ready; each package can also use CPUs.
    body = ('set -e -o pipefail\ncd -- ' + shlex.quote(remote_root) + '\n'
            'source /opt/ros/humble/setup.bash\n'
            'if [ -f install/setup.bash ]; then source install/setup.bash; fi\n'
            'task_build_jobs=$(nproc)\n'
            'export CMAKE_BUILD_PARALLEL_LEVEL="$task_build_jobs"\n'
            'export MAKEFLAGS="-j$task_build_jobs"\n'
            'export PYTHONUNBUFFERED=1\n'
            'task_build_count=$(colcon list --names-only | wc -l)\n'
            'printf "__K1_BUILD_TOTAL__=%s\\n" "$task_build_count"\n'
            './scripts/build.sh --symlink-install --executor parallel '
            '--parallel-workers "$task_build_jobs" '
            '--event-handlers console_direct+ console_start_end+ console_cohesion- console_stderr-')
    return 'bash -lc ' + shlex.quote(body)


def start_command(remote_root):
    # Environment loading and startup must happen in the same SSH shell.
    body = ('set -e -o pipefail\ncd -- ' + shlex.quote(remote_root) + '\n'
            'source /opt/ros/humble/setup.bash\n'
            'source install/setup.bash\n'
            './scripts/start.sh')
    return 'bash -lc ' + shlex.quote(body)


@contextmanager
def console_line_input():
    """Restore line editing/echo for the Windows menu, then restore its mode.

    Native SSH clients can leave the shared console in raw input mode. Quick
    Edit selections can also pause a console application. Disable Quick Edit
    only while reading this menu. Pipes and Linux terminals need no changes.
    """
    restore = None
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetStdHandle.argtypes = [wintypes.DWORD]
        kernel.GetStdHandle.restype = wintypes.HANDLE
        kernel.GetConsoleMode.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel.GetConsoleMode.restype = wintypes.BOOL
        kernel.SetConsoleMode.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.SetConsoleMode.restype = wintypes.BOOL
        handle = kernel.GetStdHandle(-10)  # STD_INPUT_HANDLE
        mode = wintypes.DWORD()
        if kernel.GetConsoleMode(handle, ctypes.byref(mode)):
            original = mode.value
            # PROCESSED_INPUT | LINE_INPUT | ECHO_INPUT | EXTENDED_FLAGS;
            # clear QUICK_EDIT_MODE so selecting console text cannot freeze us.
            editable = (original | 0x01 | 0x02 | 0x04 | 0x80) & ~0x40
            if kernel.SetConsoleMode(handle, editable):
                restore = lambda: kernel.SetConsoleMode(handle, original)
    try:
        yield
    finally:
        if restore is not None:
            restore()


def choose_action(reader=None):
    reader = reader or input
    print('\n文件已同步。输入 1/2/3 后按回车；直接回车或 Ctrl+C 仅结束同步。', flush=True)
    with console_line_input():
        while True:
            try:
                value = reader('同步完成。请选择后续操作：1 仅同步（默认）；2 编译；3 编译并启动 Demo: ').strip()
            except (EOFError, KeyboardInterrupt):
                print('\n已结束操作选择；代码已同步，未执行编译或启动。', flush=True)
                return 'sync'
            if value in ('', '1', '2', '3'):
                return {'': 'sync', '1': 'sync', '2': 'build', '3': 'start'}[value]
            print('请输入 1、2 或 3，再按回车。', flush=True)


def manual_commands(remote_root):
    return ('cd -- ' + shlex.quote(remote_root) + '\n'
            './scripts/build.sh && source install/setup.bash && ./scripts/start.sh')


def build_robots(transport, args, prepared, report, report_dir):
    def build_one(host, base):
        robot = report['robots'][host]
        try:
            result = json.loads(transport.ssh(host, shlex.join(
                base + ['--operation', 'prepare-build', '--run-id', report['run_id']])))
            robot['build_preparation'] = result
            if result['moved']:
                print(f"{host}: backed up {result['moved']} conflicting build directories; "
                      f"backup: {result['backup']}", flush=True)
            print('Building on ' + host + ' (parallel packages) ...', flush=True)
            log_path = report_dir / (host + '.build.log')
            robot['build_log'] = str(log_path)
            progress = BuildProgress(host, parallel=True,
                                     interactive=False if len(prepared) > 1 else None)
            transport.ssh(host, build_command(args.remote_root), timeout=7200,
                          stream=True, progress=progress, log_path=log_path)
            robot['build'] = 'passed'
        except Exception as error:
            robot['build'] = 'failed'
            robot['build_error'] = str(error)
            raise

    errors = []
    # Finish every selected robot and retain every result even if another fails.
    with ThreadPoolExecutor(max_workers=len(prepared)) as pool:
        tasks = {pool.submit(build_one, host, base): host for host, base, _ in prepared}
        for task in as_completed(tasks):
            try:
                task.result()
            except Exception as error:
                errors.append(str(error))
    if errors:
        raise RuntimeError('\n'.join(errors))


def run_local(args):
    root = Path(args.local_root).resolve()
    if not (root / 'src/brain').is_dir():
        raise ValueError('Invalid local project root')
    if args.rollback and (args.build or getattr(args, 'start', False)):
        raise ValueError('Do not combine rollback and build/start')
    if not args.key_file and not args.password_file:
        raise ValueError('SSH authentication is required')
    hosts = select_hosts(args.hosts, args.ip_prefix, args.robot_ids)
    run_id = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8]
    report_dir = root / '.robot-sync' / run_id
    report_dir.mkdir(parents=True)
    report = {'run_id': run_id, 'local_root': str(root), 'remote_root': args.remote_root,
              'dry_run': args.dry_run, 'robots': {}}
    transport, prepared = Transport(args), []
    try:
        with tempfile.TemporaryDirectory(prefix='k1-sync-files-') as tmp:
            temp = Path(tmp)
            request = None if args.rollback else scan_local(root)
            if request:
                write_json(temp / 'request.json', request)
                print(f"Local scan: {len(request['files'])} files; protected: {len(request['skipped'])}", flush=True)
            # Preflight all robots before applying source changes to any robot.
            for host in hosts:
                print('Comparing ' + host + ' ...', flush=True)
                stage = '/tmp/k1-source-sync-' + run_id
                create = "python3 -c " + shlex.quote(
                    'import os; from pathlib import Path; '
                    f'p=Path({stage!r}); p.mkdir(mode=0o700, exist_ok=False)')
                transport.ssh(host, create)
                remote_helper = stage + '/robot_sync.py'
                transport.upload(host, Path(__file__).resolve(), remote_helper)
                base = ['python3', remote_helper, '--remote-agent', '--remote-root', args.remote_root, '--stage', stage]
                if args.rollback:
                    command = base + ['--operation', 'rollback-preview', '--rollback', args.rollback]
                    result = json.loads(transport.ssh(host, shlex.join(command)))
                    report['robots'][host] = {'rollback_preview': result}
                    prepared.append((host, base, None))
                    continue
                transport.upload(host, temp / 'request.json', stage + '/request.json')
                plan = json.loads(transport.ssh(host, shlex.join(base + ['--operation', 'compare'])))
                report['robots'][host] = {'plan': plan, 'state': 'compared'}
                write_json(report_dir / (host + '.plan.json'), plan)
                print(f"  {len(plan['changes'])} differences / {plan['total']} compared", flush=True)
                for item in plan['changes']:
                    print('  ' + ('+ ' if item['action'] == 'new' else '~ ') + item['path'], flush=True)
                prepared.append((host, base, plan))
            # Construct every payload from the compared snapshot before any apply.
            if not args.dry_run and not args.rollback:
                for host, base, plan in prepared:
                    archive = temp / (host + '.tar.gz')
                    make_archive(root, plan['changes'], archive)
                    transport.upload(host, archive, base[base.index('--stage') + 1] + '/payload.tar.gz')
                    report['robots'][host]['archive_hash'] = file_info(archive)['sha256']
            for host, base, plan in prepared:
                if args.dry_run:
                    continue
                if args.rollback:
                    result = json.loads(transport.ssh(host, shlex.join(base + ['--operation', 'rollback', '--rollback', args.rollback])))
                    report['robots'][host]['rollback_result'] = result
                    print(f"{host}: restored {result['restored']} files", flush=True)
                else:
                    command = base + ['--operation', 'apply', '--run-id', run_id,
                                      '--archive-hash', report['robots'][host]['archive_hash']]
                    result = json.loads(transport.ssh(host, shlex.join(command)))
                    report['robots'][host].update({'state': 'synced', 'result': result})
                    print(f"{host}: synced {result['changed']} files; backup: {result['backup']}", flush=True)
            try:
                action = 'start' if getattr(args, 'start', False) else 'build' if args.build else 'sync'
                if not args.dry_run and not args.rollback:
                    if getattr(args, 'choose_action', False) and action == 'sync':
                        action = choose_action()
                    report['action'] = action
                    report['manual_commands'] = manual_commands(args.remote_root)
                    if action in ('build', 'start'):
                        build_robots(transport, args, prepared, report, report_dir)
                    if action == 'start':
                        # All selected robots must finish building before any starts.
                        for host, _, _ in prepared:
                            robot = report['robots'][host]
                            try:
                                print('Loading environment and starting Demo on ' + host + ' ...', flush=True)
                                transport.ssh(host, start_command(args.remote_root), stream=True)
                                robot['start'] = 'command_completed'
                            except Exception as error:
                                robot['start'], robot['start_error'] = 'failed', str(error)
                                raise
            finally:
                for host, base, _ in prepared:
                    try:
                        transport.ssh(host, shlex.join(base + ['--operation', 'cleanup']))
                    except Exception as error:
                        report['robots'][host]['cleanup_error'] = str(error)
            report['success'] = True
    except Exception as error:
        report['success'], report['error'] = False, str(error)
        raise
    finally:
        write_json(report_dir / 'result.json', report)
        print('Report: ' + str(report_dir / 'result.json'), flush=True)
    if args.dry_run:
        print('Preview complete; robot source files unchanged.', flush=True)
    elif args.rollback:
        print('Rollback complete.', flush=True)
    elif report['action'] == 'start':
        print('Sync/build complete; Demo startup commands completed on selected robots.', flush=True)
    else:
        print('Sync complete. ' + ('Build complete; load the new environment and start manually.'
              if report['action'] == 'build' else 'No build/start was run.'), flush=True)
        print('机器人上手动执行（编译成功后才会加载环境、启动）：\n' + manual_commands(args.remote_root), flush=True)


def remote_agent(args):
    os.nice(10)
    root = Path(args.remote_root).resolve()
    stage = Path(args.stage).resolve()
    if not root.is_dir() or not (root / 'src/brain').is_dir() or root == Path('/'):
        raise ValueError('Invalid robot project root: ' + str(root))
    if not stage.is_dir() or stage.parent != Path('/tmp') or not re.fullmatch(
            r'k1-source-sync-\d{8}T\d{6}Z-[0-9a-f]{8}', stage.name):
        raise ValueError('Invalid isolated staging directory')
    if args.operation == 'compare':
        result = prepare_plan(root, read_json(stage / 'request.json'), stage)
    elif args.operation == 'apply':
        if not re.fullmatch(r'[A-Za-z0-9_-]+', args.run_id or ''):
            raise ValueError('Invalid backup ID')
        result = apply_plan(root, stage, args.run_id, args.archive_hash)
    elif args.operation in ('rollback', 'rollback-preview'):
        result = rollback(root, args.rollback, args.operation == 'rollback-preview')
    elif args.operation == 'prepare-build':
        result = prepare_symlink_build(root, args.run_id)
    elif args.operation == 'cleanup':
        # stage is an exact, resolved directory under /tmp created by this run.
        shutil.rmtree(stage)
        result = {'staging_removed': True}
    else:
        raise ValueError('Unknown remote operation')
    print(json.dumps(result, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--local-root', default=r'E:\GIthubCode\My_Booster_K1_3v3')
    parser.add_argument('--remote-root', default='/home/booster/Workspace/K1_5v5_demo_1.7')
    parser.add_argument('--hosts')
    parser.add_argument('--ip-prefix')
    parser.add_argument('--robot-ids')
    parser.add_argument('--select-only', action='store_true')
    parser.add_argument('--validate-prefix', action='store_true')
    parser.add_argument('--user', default='booster')
    parser.add_argument('--host-key')
    parser.add_argument('--plink')
    parser.add_argument('--pscp')
    parser.add_argument('--password-file')
    parser.add_argument('--key-file')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--build', action='store_true')
    parser.add_argument('--start', action='store_true', help='Build all selected robots, then load environment and start Demo')
    parser.add_argument('--choose-action', action='store_true', help='Ask for sync/build/start after syncing; default is sync only')
    parser.add_argument('--rollback')
    parser.add_argument('--remote-agent', action='store_true')
    parser.add_argument('--operation')
    parser.add_argument('--stage')
    parser.add_argument('--run-id')
    parser.add_argument('--archive-hash')
    args = parser.parse_args()
    try:
        if args.validate_prefix:
            print(validate_ip_prefix(args.ip_prefix))
        elif args.select_only:
            print(json.dumps(select_hosts(args.hosts, args.ip_prefix, args.robot_ids)))
        else:
            (remote_agent if args.remote_agent else run_local)(args)
    except Exception as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
