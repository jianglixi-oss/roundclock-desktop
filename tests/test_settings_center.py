import asyncio,sys,tempfile
from pathlib import Path
sys.path.insert(0,str(Path('nas').resolve()))
import settings_center as center
from aiohttp import web,ClientSession
from aiohttp.test_utils import TestServer,TestClient
async def main():
 with tempfile.TemporaryDirectory(dir='.') as folder:
  center.ROOT=Path(folder);app=web.Application();center.install(app,lambda:None,'http://example.test:8642','private-test-key',lambda r:False)
  async with TestClient(TestServer(app)) as client:
   r=await client.get('/settings/api/config');assert r.status==200;assert 'private-test-key' not in await r.text()
   for mode in ('speech','beep','off'):
    r=await client.post('/settings/api/config',json={'section':'codex','value':{'notification':mode,'usage_interval':60,'sync_interval':2}});assert r.status==200;assert center.codex_options()['notification']==mode
   r=await client.post('/settings/api/config',json={'section':'hermes','value':{'url':'http://another.test:8642'}});assert r.status==200;assert center.load('hermes-connection.private.json')['api_key']==''
   r=await client.post('/settings/device',json={});assert r.status==401
   r=await client.post('/settings/api/config',json={'section':'codex','value':{'usage_interval':1}});assert r.status==400
   r=await client.post('/settings/api/config',headers={'Origin':'http://other.test'},json={});assert r.status==403
 print('Settings contract: 9 checks passed')
asyncio.run(main())
