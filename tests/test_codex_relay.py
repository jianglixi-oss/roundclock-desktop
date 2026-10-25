import asyncio,importlib.util,tempfile,time,json
from pathlib import Path
from aiohttp.test_utils import TestClient,TestServer
from aiohttp import web
async def main():
    spec=importlib.util.spec_from_file_location('codex_relay','nas/codex_status.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    with tempfile.TemporaryDirectory(dir='.') as directory:
        module.ROOT=Path(directory);assert module.ROOT.resolve().is_relative_to(Path('.').resolve())
        (module.ROOT/'codex-ingest.env').write_text('CODEX_INGEST_TOKEN=test-ingest\n')
        app=web.Application();module.install(app,'test-device');client=TestClient(TestServer(app));await client.start_server()
        try:
            assert (await client.get('/codex/status')).status==401
            assert (await client.post('/admin/codex/status',json={'state':'working'},headers={'Authorization':'Bearer test-device'})).status==401
            headers={'Authorization':'Bearer test-ingest'}
            assert (await client.post('/admin/codex/status',json={'state':'invalid'},headers=headers)).status==400
            response=await client.post('/admin/codex/status',json={'state':'waiting','primary':{'used':None},'secondary':{'used':145}},headers=headers);assert response.status==200
            response=await client.get('/codex/status',headers={'Authorization':'Bearer test-device'});data=await response.json()
            assert data['state']=='waiting' and data['online'] and data['primary']['used'] is None and data['secondary']['used']==100
            stored=json.loads((module.ROOT/'codex-status.json').read_text());stored['received_at']=int(time.time())-30
            (module.ROOT/'codex-status.json').write_text(json.dumps(stored))
            data=await (await client.get('/codex/status',headers={'Authorization':'Bearer test-device'})).json();assert data['state']=='offline' and not data['online']
            print('PASS: ingest/read credentials isolated; missing quota stays unknown; stale PC becomes offline; invalid states rejected.')
        finally:await client.close()
asyncio.run(main())
