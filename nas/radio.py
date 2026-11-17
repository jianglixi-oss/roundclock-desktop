"""Internet radio station list management for the settings center.

The station list lives in /music/stations.json, shared with the xiaozhi-nas-music
container (which mounts the same NAS music folder read-only). Edits here take
effect as soon as the voice agent next lists stations.
"""
import json
from pathlib import Path
from aiohttp import web

ROOT = Path('/app')
STATIONS = Path('/music/stations.json')
MAX_STATIONS = 50


def read_stations():
    try:
        data = json.loads(STATIONS.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return []
    if not isinstance(data, list):
        return []
    result = []
    for item in data:
        if not isinstance(item, dict):
            continue
        name = str(item.get('name', '')).strip()
        url = str(item.get('url', '')).strip()
        if name and url.startswith(('http://', 'https://')):
            result.append({'name': name, 'url': url})
    return result


def write_stations(stations):
    STATIONS.parent.mkdir(parents=True, exist_ok=True)
    temporary = STATIONS.with_suffix('.new')
    temporary.write_text(json.dumps(stations, ensure_ascii=False, indent=2), encoding='utf-8')
    temporary.chmod(0o644)
    temporary.replace(STATIONS)


def install(app, device_token):
    def manager(request):
        value = request.headers.get('Origin')
        if value and value != request.scheme + '://' + request.host:
            raise web.HTTPForbidden(text='请在配置页面内操作')

    async def page(request):
        return web.FileResponse(ROOT / 'radio.html', headers={'Cache-Control': 'no-store'})

    async def listing(request):
        manager(request)
        return web.json_response({'stations': read_stations()}, headers={'Cache-Control': 'no-store'})

    async def save(request):
        manager(request)
        body = await request.json()
        stations = read_stations()
        action = body.get('action')
        name = str(body.get('name', '')).strip()
        url = str(body.get('url', '')).strip()
        if action == 'add':
            if not name or len(name) > 40:
                raise web.HTTPBadRequest(text='电台名称无效')
            if not url.startswith(('http://', 'https://')) or len(url) > 2048:
                raise web.HTTPBadRequest(text='请填写有效的 HTTP/HTTPS 电台流地址')
            if any(s['name'] == name for s in stations):
                raise web.HTTPBadRequest(text='电台名称已存在')
            stations.append({'name': name, 'url': url})
        elif action == 'remove':
            stations = [s for s in stations if s['name'] != name]
        else:
            raise web.HTTPBadRequest(text='未知操作')
        if len(stations) > MAX_STATIONS:
            raise web.HTTPBadRequest(text=f'最多 {MAX_STATIONS} 个电台')
        write_stations(stations)
        return web.json_response({'ok': True, 'stations': stations})

    app.router.add_get('/radio', page)
    app.router.add_get('/radio/api/list', listing)
    app.router.add_post('/radio/api/save', save)
