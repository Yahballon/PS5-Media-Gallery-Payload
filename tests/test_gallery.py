#!/usr/bin/env python3
"""End-to-end host checks for HTTP, grouping, icons, traversal, and video ranges."""
import http.client
import concurrent.futures
import struct
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.parse

ROOT = Path(__file__).resolve().parents[1]


def request(port, method, uri, headers=None):
    con = http.client.HTTPConnection('127.0.0.1', port=port, timeout=10)
    con.request(method, uri, headers=headers or {})
    response = con.getresponse()
    result = (response.status, dict(response.getheaders()), response.read())
    con.close()
    return result


with tempfile.TemporaryDirectory(prefix='ps5-gallery-test-') as directory:
    base = Path(directory)
    av = base / 'av_contents'
    image = av / 'photo' / 'NPXS40087' / 'PPSA01234' / '035' / 'Screen 01.png'
    video = av / 'video' / '2026' / 'PPSA01234' / 'video clip.mp4'
    other = av / 'photo' / 'CUSA99999' / 'game.jpg'
    for f in (image, video, other):
        f.parent.mkdir(parents=True, exist_ok=True)
    image.write_bytes(b'\x89PNG\r\n\x1a\nexample-picture')
    video.write_bytes(b'ABCDEFGHIJKLMNOPQRST')
    other.write_bytes(b'\xff\xd8\xffmock')
    (base / 'private.txt').write_text('DO NOT LEAK')
    meta = base / 'apps' / 'PPSA01234' / 'sce_sys'
    meta.mkdir(parents=True)
    (meta / 'param.json').write_text('{"localizedParameters":{"en-US":{"titleName":"Gallery Test Game"}}}')
    icon = meta / 'icon0.png'
    icon.write_bytes(b'\x89PNG\r\n\x1a\nTEST ICON DATA!')
    (image.parent / 'sneaky.jpg').symlink_to(base / 'private.txt')

    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    cmd = [
        'gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pthread',
        f'-DMEDIA_PORT={port}', f'-DAV_BASE="{av}"',
        f'-DAPP_ROOT="{base / "apps"}"',
        f'-DAPPMETA_ROOT="{base / "appmeta"}"',
        '-o', str(base / 'gallery-test'), str(ROOT / 'src/gallery.c'),
    ]
    subprocess.run(cmd, check=True)
    proc = subprocess.Popen([str(base / 'gallery-test')], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        for _ in range(80):
            try:
                status, _, data = request(port, 'GET', '/')
                break
            except (ConnectionError, OSError):
                time.sleep(0.075)
        else:
            raise RuntimeError('Gallery server did not start')
        assert status == 200 and b'PS5 Media Gallery' in data
        status, _, data = request(port, 'GET', '/api/gallery')
        assert status == 200
        catalog = json.loads(data)
        assert catalog['roots'] == {'photo': True, 'video': True}
        assert catalog['version'] == '0.1.3'
        assert len(catalog['items']) == 3, catalog['items']
        groups = {a['id']: a for a in catalog['apps']}
        assert groups['PPSA01234']['title'] == 'Gallery Test Game', groups
        assert groups['PPSA01234']['icon'] is True
        assert 'CUSA99999' in groups
        assert 'NPXS40087' not in groups
        assert not any(item['name'] == 'sneaky.jpg' for item in catalog['items'])
        status, _, data = request(port, 'GET', '/icon/PPSA01234')
        assert status == 200 and data == icon.read_bytes()
        filename = '/media/photo/NPXS40087/PPSA01234/035/Screen%2001.png'
        status, headers, data = request(port, 'GET', filename)
        assert status == 200 and data == image.read_bytes()
        assert headers['Content-Type'] == 'image/png'
        status, _, data = request(port, 'HEAD', filename)
        assert status == 200 and not data
        status, headers, data = request(port, 'GET', '/media/video/2026/PPSA01234/video%20clip.mp4', {'Range': 'bytes=5-9'})
        assert status == 206 and data == b'FGHIJ' and headers['Content-Range'] == 'bytes 5-9/20'
        status, _, data = request(port, 'GET', '/media/video/2026/PPSA01234/video%20clip.mp4', {'Range': 'bytes=-3'})
        assert status == 206 and data == b'RST'
        status, _, data = request(port, 'GET', '/media/video/2026/PPSA01234/video%20clip.mp4', {'Range': 'bytes=300-900'})
        assert status == 416
        status, _, data = request(port, 'POST', '/api/gallery')
        assert status == 405
        for uri in ['/media/photo/..%2f..%2fprivate.txt', '/media/photo/PPSA01234/sneaky.jpg', '/etc/passwd', '/icon/../private.txt']:
            status, _, data = request(port, 'GET', uri)
            assert status in (400, 404), (uri, status)
            assert b'DO NOT LEAK' not in data
        # Large transfers exercise chunking; cancelled streams must not kill the payload.
        large = av / 'video' / 'PPSA01234' / 'large.mp4'
        large.parent.mkdir(parents=True, exist_ok=True)
        payload = bytes(range(256)) * 32768
        large.write_bytes(payload)
        uri = '/media/video/PPSA01234/large.mp4'
        for _ in range(8):
            sock = socket.create_connection(('127.0.0.1', port))
            sock.sendall(f'GET {uri} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode())
            sock.recv(1024)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
            sock.close()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            results = list(pool.map(lambda _: request(port, 'GET', uri, {'Range': 'bytes=65500-200000'}), range(16)))
        assert all(status == 206 and data == payload[65500:200001] for status, _, data in results)
        status, _, data = request(port, 'GET', uri)
        assert status == 200 and data == payload
        assert proc.poll() is None
        confirm={'X-Gallery-Confirm':'delete'}
        status, _, _=request(port,'DELETE','/api/delete/all')
        assert status==403 and image.exists()
        status, _, _=request(port,'DELETE','/api/delete/media/photo/..%2f..%2fprivate.txt',confirm)
        assert status==400
        status, _, data=request(port,'DELETE','/api/delete'+filename,confirm)
        assert status==200 and json.loads(data)['deleted']==1 and not image.exists()
        status, _, data=request(port,'DELETE','/api/delete/app/PPSA01234',confirm)
        assert status==200 and json.loads(data)['deleted']==2 and not video.exists() and not large.exists()
        assert other.exists() and icon.exists()
        status, _, data=request(port,'DELETE','/api/delete/all',confirm)
        assert status==200 and json.loads(data)['deleted']==1 and not other.exists()
        assert icon.exists()
        assert (base / 'private.txt').read_text() == 'DO NOT LEAK'
        print('PASS: UI + JSON + app grouping + param.json title + app icon + image + HEAD + video ranges + read-only + path traversal')
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
