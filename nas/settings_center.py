"""Local settings UI; intentionally no user login. Device/agent APIs keep tokens."""
import json,time,re,ipaddress
from pathlib import Path
from urllib.parse import urlsplit
from aiohttp import web
ROOT=Path('/app')

def load(name,default=None):
    try:return json.loads((ROOT/name).read_text())
    except (OSError,ValueError):return {} if default is None else default

def save(name,value):
    target=ROOT/name;temporary=target.with_suffix('.new')
    temporary.write_text(json.dumps(value,ensure_ascii=False));temporary.chmod(0o600);temporary.replace(target)

def codex_options():
    value=load('codex-settings.json')
    return {'notification':value.get('notification','speech'),'usage_interval':value.get('usage_interval',120),'sync_interval':value.get('sync_interval',3)}

def install(app,client_getter,default_url,default_key,device_auth):
    device={}
    def origin(request):
        value=request.headers.get('Origin')
        if value and value!=request.scheme+'://'+request.host:raise web.HTTPForbidden(text='请在设置页面内操作')
    def public_config():
        voice=load('voice-settings.json');connection=load('hermes-connection.private.json')
        return {'hermes':{'url':connection.get('url',default_url),'key_configured':bool(connection.get('api_key',default_key)),
            'model':voice.get('model','deepseek'),'voice':voice.get('voice','zh-CN-XiaoxiaoNeural'),
            'tts':voice.get('tts','edge'),'rate':int(str(voice.get('rate','+5%')).rstrip('%')),'pitch':int(str(voice.get('pitch','+0Hz')).rstrip('Hz'))},'codex':codex_options()}
    async def page(request):return web.FileResponse(ROOT/'settings.html',headers={'Cache-Control':'no-store'})
    async def get_config(request):return web.json_response(public_config(),headers={'Cache-Control':'no-store'})
    async def update(request):
        origin(request);data=await request.json()
        if data.get('section')=='hermes':
            value=data.get('value',{});current=public_config()['hermes'];url=str(value.get('url',current['url'])).strip().rstrip('/')
            parsed=urlsplit(url)
            if parsed.scheme not in ('http','https') or not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment:raise web.HTTPBadRequest(text='请填写有效的 Hermes HTTP/HTTPS 地址，不要在地址中填写密码')
            model=value.get('model',current['model']);tts=value.get('tts',current['tts']);voice=value.get('voice',current['voice'])
            if model not in ('default','deepseek') or tts not in ('edge','local') or voice not in ('zh-CN-XiaoxiaoNeural','zh-CN-YunyangNeural','zh-CN-YunxiNeural'):raise web.HTTPBadRequest(text='请选择提供的模型和音色')
            try:rate=int(value.get('rate',current['rate']));pitch=int(value.get('pitch',current['pitch']))
            except (ValueError,TypeError):raise web.HTTPBadRequest(text='语速或音高无效')
            if not -20<=rate<=30 or not -15<=pitch<=15:raise web.HTTPBadRequest(text='语速或音高超出范围')
            connection=load('hermes-connection.private.json');key=value.get('api_key','')
            if not isinstance(key,str) or len(key)>4096 or '\n' in key or '\r' in key:raise web.HTTPBadRequest(text='API 密钥格式无效')
            if key.strip():connection['api_key']=key.strip()
            elif urlsplit(current['url']).hostname!=parsed.hostname:connection['api_key']=''
            connection['url']=url
            save('hermes-connection.private.json',connection)
            settings=load('voice-settings.json');settings.update(model=model,tts=tts,voice=voice,rate=f'{rate:+d}%',pitch=f'{pitch:+d}Hz');save('voice-settings.json',settings)
        elif data.get('section')=='codex':
            value=data.get('value',{});notification=value.get('notification','speech')
            try:usage=int(value.get('usage_interval',120));sync=int(value.get('sync_interval',3))
            except (ValueError,TypeError):raise web.HTTPBadRequest(text='刷新间隔无效')
            if notification not in ('speech','beep','off') or not 30<=usage<=600 or not 2<=sync<=10:raise web.HTTPBadRequest(text='请检查提醒方式和刷新间隔')
            save('codex-settings.json',{'notification':notification,'usage_interval':usage,'sync_interval':sync})
        else:raise web.HTTPBadRequest(text='无效设置模块')
        return web.json_response({'ok':True,**public_config()})
    async def status(request):
        value=load('codex-status.json');age=int(time.time()-value.get('received_at',0)) if value.get('received_at') else None
        safe={'online':age is not None and age<25,'age':age,'state':value.get('state','offline'),'usage_at':value.get('usage_at',0)}
        return web.json_response({'codex':safe,'device':dict(device,online=time.time()-device.get('received_at',0)<25),'stocks_url':'/stocks'},headers={'Cache-Control':'no-store'})
    async def device_report(request):
        if not device_auth(request):raise web.HTTPUnauthorized()
        value=await request.json()
        device.clear();device.update(ssid=str(value.get('ssid',''))[:64],ip=str(value.get('ip',''))[:48],rssi=int(value.get('rssi',0)),received_at=int(time.time()))
        return web.json_response({'ok':True})
    async def test(request):
        origin(request)
        connection=load('hermes-connection.private.json');url=connection.get('url',default_url);key=connection.get('api_key',default_key)
        try:
            import aiohttp
            async with client_getter().get(url+'/v1/models',headers={'Authorization':'Bearer '+key},timeout=aiohttp.ClientTimeout(total=8)) as response:
                return web.json_response({'ok':response.status==200,'message':'Hermes 连接与 API 密钥正常' if response.status==200 else 'Hermes 返回 HTTP '+str(response.status)})
        except Exception:return web.json_response({'ok':False,'message':'无法连接 Hermes，请检查地址、端口及网络'})
    app.router.add_get('/settings',page);app.router.add_get('/',page)
    app.router.add_get('/settings/api/config',get_config);app.router.add_post('/settings/api/config',update)
    app.router.add_get('/settings/api/status',status);app.router.add_post('/settings/api/hermes/test',test)
    app.router.add_post('/settings/device',device_report)
