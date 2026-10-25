"""Personal A-share watchlist and timestamped quote relay."""
import asyncio,json,re,time,hmac,secrets
from datetime import datetime
from zoneinfo import ZoneInfo
from pathlib import Path
from aiohttp import web,ClientSession,ClientTimeout
ROOT=Path('/app');ZONE=ZoneInfo('Asia/Shanghai')
PATTERN=re.compile(r'^(sh|sz|bj)\d{6}$')

def config():
    path=ROOT/'stocks-config.json'
    if not path.exists():
        path.write_text(json.dumps({'pin':str(secrets.randbelow(900000)+100000),'symbols':[]}));path.chmod(0o600)
    return json.loads(path.read_text())

def parse_quotes(text):
    result={}
    for symbol,body in re.findall(r'v_((?:sh|sz|bj)\d{6})="([^"]*)"',text):
        values=body.split('~')
        if len(values)<35 or not re.fullmatch(r'\d{14}',values[30]):continue
        try:
            price=float(values[3]);previous=float(values[4]);change=price-previous
            if price<=0 or previous<=0:continue
            stamp=datetime.strptime(values[30],'%Y%m%d%H%M%S').replace(tzinfo=ZONE)
            result[symbol]={'symbol':symbol,'name':values[1],'code':values[2],
                'price':price,'change':round(change,4),'percent':round(change/previous*100,2),
                'high':float(values[33]),'low':float(values[34]),'time':stamp.strftime('%m/%d %H:%M:%S'),
                'date':stamp.strftime('%Y-%m-%d'),'timestamp':int(stamp.timestamp())}
        except (ValueError,IndexError):continue
    return result

def session_label():
    now=datetime.now(ZONE);minute=now.hour*60+now.minute
    if now.weekday()>=5:return '休市'
    if 570<=minute<=690 or 780<=minute<=900:return '交易时段'
    if 690<minute<780:return '午间休市'
    return '非交易时段'

class Stocks:
    def __init__(self):self.cache={};self.fetched=0;self.lock=asyncio.Lock();self.config_lock=asyncio.Lock();self.failed=False;self.search_cache={};self.attempts={}
    async def request(self,url):
        async with ClientSession(trust_env=False,timeout=ClientTimeout(total=7),headers={'User-Agent':'Mozilla/5.0','Referer':'https://finance.sina.com.cn/'}) as client:
            async with client.get(url) as response:
                response.raise_for_status();return (await response.read()).decode('gb18030',errors='replace')
    async def quotes(self,symbols):
        async with self.lock:
            if time.time()-self.fetched>5 or any(s not in self.cache for s in symbols):
                try:
                    fresh=parse_quotes(await self.request('https://qt.gtimg.cn/q='+','.join(symbols))) if symbols else {}
                    self.cache.update(fresh);self.fetched=time.time();self.failed=any(s not in fresh for s in symbols)
                except Exception:self.failed=True
            return [dict(self.cache[s],stale=self.failed or time.time()-self.fetched>30) if s in self.cache else {'symbol':s,'code':s[2:],'name':s[2:],'stale':True} for s in symbols]
    async def search(self,key):
        cache=self.search_cache.get(key)
        if cache and time.time()-cache[0]<300:return cache[1]
        from urllib.parse import quote
        text=await self.request('https://suggest3.sinajs.cn/suggest/type=11,12,13,14,15&key='+quote(key)+'&name=suggestdata')
        match=re.search(r'="(.*)"',text);items=[];seen=set()
        for record in (match.group(1).split(';') if match else []):
            fields=record.split(',')
            if len(fields)>4 and PATTERN.fullmatch(fields[3]) and fields[3] not in seen:
                seen.add(fields[3]);items.append({'symbol':fields[3],'code':fields[2],'name':fields[4] or fields[0]})
        if re.fullmatch(r'(?:sh|sz|bj)?\d{6}',key):
            symbol=key if len(key)>6 else ('sh' if key.startswith(('6','5')) else 'bj' if key.startswith(('4','8','9')) else 'sz')+key
            found=await self.quotes([symbol])
            if found and found[0].get('price'):
                items=[{k:found[0][k] for k in ['symbol','code','name']}]+[i for i in items if i['symbol']!=symbol]
        self.search_cache[key]=(time.time(),items[:12]);return items[:12]

def install(app,device_token):
    service=Stocks();config()
    def device(request):return hmac.compare_digest(request.headers.get('Authorization',''),'Bearer '+device_token)
    def manager(request):
        address=request.remote or 'unknown';attempts=service.attempts.setdefault(address,[])
        attempts[:]=[t for t in attempts if time.time()-t<60]
        pin=request.headers.get('X-Stocks-Pin','')
        if len(attempts)>=10:raise web.HTTPTooManyRequests(text='请稍后重试')
        if not hmac.compare_digest(pin,config()['pin']):attempts.append(time.time());raise web.HTTPUnauthorized(text='设置码不正确')
    async def snapshot(request):
        if not device(request):raise web.HTTPUnauthorized()
        conf=config();quotes=await service.quotes(conf['symbols']);state=session_label()
        today=datetime.now(ZONE).strftime('%Y-%m-%d')
        if state=='交易时段' and any(q.get('date')!=today for q in quotes):state='行情待更新'
        return web.json_response({'quotes':quotes,'market':state,'pin':conf['pin'],'settings_url':__import__('os').getenv('PUBLIC_BASE_URL','http://NAS_IP:8767').rstrip('/')+'/stocks','fetched_at':int(service.fetched),'source':'腾讯财经'},headers={'Cache-Control':'no-store'})
    async def page(request):return web.FileResponse(ROOT/'stocks.html',headers={'Cache-Control':'no-store'})
    async def listing(request):
        manager(request);conf=config();return web.json_response({'quotes':await service.quotes(conf['symbols'])})
    async def search(request):
        manager(request);key=request.query.get('q','').strip().lower()
        if not key or len(key)>40:return web.json_response({'items':[]})
        try:items=await service.search(key)
        except Exception:return web.json_response({'error':'搜索服务暂不可用，请重试'},status=502)
        return web.json_response({'items':items})
    async def save(request):
        manager(request);body=await request.json()
        async with service.config_lock:
            conf=config();symbols=conf['symbols'][:];action=body.get('action');symbol=body.get('symbol')
            if action in ('add','remove'):
                if not isinstance(symbol,str) or not PATTERN.fullmatch(symbol):raise web.HTTPBadRequest(text='无效代码')
                if action=='add' and symbol not in symbols:
                    candidate=await service.quotes([symbol])
                    if not candidate or not candidate[0].get('price'):raise web.HTTPBadRequest(text='该股票暂无有效行情，请重试')
                    symbols.append(symbol)
                elif action=='remove':symbols=[s for s in symbols if s!=symbol]
            else:raise web.HTTPBadRequest(text='请刷新页面后再添加或移除')
            if len(symbols)>12:raise web.HTTPBadRequest(text='最多添加12只股票')
            conf['symbols']=symbols;temp=ROOT/'stocks-config.new';temp.write_text(json.dumps(conf));temp.chmod(0o600);temp.replace(ROOT/'stocks-config.json')
        return web.json_response({'ok':True,'quotes':await service.quotes(symbols)})
    app.router.add_get('/stocks',page);app.router.add_get('/stocks/snapshot',snapshot)
    app.router.add_get('/stocks/api/list',listing);app.router.add_get('/stocks/api/search',search);app.router.add_post('/stocks/api/watchlist',save)
