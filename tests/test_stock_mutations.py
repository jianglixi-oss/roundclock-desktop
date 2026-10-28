import sys
sys.path.insert(0,str(__import__("pathlib").Path("nas").resolve()))
import asyncio,importlib.util,tempfile,json
from pathlib import Path
from aiohttp import web
from aiohttp.test_utils import TestClient,TestServer
async def main():
    spec=importlib.util.spec_from_file_location('stock_service','nas/stocks.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    async def quotes(self,symbols):return [{'symbol':s,'name':'测试','code':s[2:],'price':10,'time':'10/23 15:00:00','date':'2026-10-23','stale':False} for s in symbols]
    m.Stocks.quotes=quotes
    with tempfile.TemporaryDirectory(dir='.') as directory:
        m.ROOT=Path(directory);assert m.ROOT.resolve().is_relative_to(Path('.').resolve())
        app=web.Application();m.install(app,'test-device');headers={}
        client=TestClient(TestServer(app));await client.start_server()
        try:
            assert (await client.get('/stocks/snapshot')).status==401
            responses=await asyncio.gather(*[client.post('/stocks/api/watchlist',headers=headers,json={'action':'add','symbol':symbol}) for symbol in ['sz000002','sh600519']])
            assert all(r.status==200 for r in responses)
            assert set(m.config()['symbols'])=={'sz000002','sh600519'}
            await client.post('/stocks/api/watchlist',headers=headers,json={'action':'add','symbol':'sz000002'})
            assert len(m.config()['symbols'])==2
            await client.post('/stocks/api/watchlist',headers=headers,json={'action':'remove','symbol':'sz000002'})
            assert m.config()['symbols']==['sh600519']
            assert (await client.post('/stocks/api/watchlist',headers=headers,json={'symbols':['sz000001']})).status==400
            assert m.config()['symbols']==['sh600519']
            print('PASS: concurrent additions retained, duplicate prevented, single removal retained others, obsolete whole-list saves rejected, device auth enforced.')
        finally:await client.close()
asyncio.run(main())
