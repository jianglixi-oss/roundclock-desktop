"""Small authenticated relay. No Codex credentials or chat content leave the PC."""
import json,time,hmac
from pathlib import Path
from aiohttp import web
from dotenv import dotenv_values
ROOT=Path('/app')

def install(app,device_token):
    if (ROOT/'codex-ingest.env').exists():(ROOT/'codex-ingest.env').chmod(0o600)
    def allowed(request,key):return bool(key) and hmac.compare_digest(request.headers.get('Authorization',''),'Bearer '+key)
    async def ingest(request):
        key=dotenv_values(ROOT/'codex-ingest.env').get('CODEX_INGEST_TOKEN','')
        if not allowed(request,key):raise web.HTTPUnauthorized()
        value=await request.json()
        if value.get('state') not in ['working','waiting','completed','error','idle','unknown']:raise web.HTTPBadRequest()
        safe={'state':value['state'],'state_at':int(value.get('state_at',time.time())),
              'active':min(max(int(value.get('active',0)),0),99),'received_at':int(time.time()),
              'usage_at':int(value.get('usage_at',0))}
        for name in ['primary','secondary']:
            entry=value.get(name)
            if entry is None:safe[name]=None;continue
            used=entry.get('used')
            safe[name]={'used':min(max(int(used),0),100) if used is not None else None,
                'minutes':max(0,int(entry.get('minutes',0))),'reset':max(0,int(entry.get('reset',0)))}
        temporary=ROOT/'codex-status.new';temporary.write_text(json.dumps(safe));temporary.replace(ROOT/'codex-status.json')
        return web.json_response({'ok':True})
    async def status(request):
        if not allowed(request,device_token):raise web.HTTPUnauthorized()
        try:value=json.loads((ROOT/'codex-status.json').read_text())
        except (OSError,ValueError):value={'state':'offline','received_at':0,'state_at':0,'primary':None,'secondary':None}
        value['online']=time.time()-value['received_at']<25
        if not value['online']:value['state']='offline'
        value['usage_stale']=time.time()-value.get('usage_at',0)>600
        return web.json_response(value,headers={'Cache-Control':'no-store'})
    app.router.add_post('/admin/codex/status',ingest)
    app.router.add_get('/codex/status',status)
