"""PCM streaming voice adapter for the round ESP32-S3 Hermes terminal."""
import asyncio,contextlib,hmac,json,logging,os,re,time,uuid,wave,traceback,threading,hashlib
from pathlib import Path
import aiohttp
from aiohttp import web
import numpy as np
import sherpa_onnx
import edge_tts
import imageio_ffmpeg
import yaml
from urllib.parse import urlsplit
from dotenv import dotenv_values

logging.basicConfig(level=logging.INFO,format='%(asctime)s %(levelname)s %(message)s',handlers=[logging.StreamHandler(),logging.FileHandler('/app/voice.log')])
LOG=logging.getLogger('roundclock')
ROOT=Path('/app');MODEL=Path(os.environ['VOICE_MODEL_DIR'])
HERMES_URL=os.getenv('HERMES_BASE_URL','http://hermes-gateway:8642').rstrip('/')
DEVICE_TOKEN=os.environ['ROUND_DEVICE_TOKEN'];ADMIN_TOKEN=os.environ['ROUND_ADMIN_TOKEN']
hermes_env=dotenv_values('/hermes/.env')
test_key_file=ROOT/'deepseek-test.env'
if test_key_file.exists():test_key_file.chmod(0o600)
DEEPSEEK_KEY=dotenv_values(test_key_file).get('DEEPSEEK_API_KEY') or hermes_env.get('DEEPSEEK_API_KEY','')
HERMES_KEY=hermes_env.get('API_SERVER_KEY','')
PROXY=os.getenv('HTTPS_PROXY') or os.getenv('https_proxy') or None
VOICE=os.getenv('TTS_VOICE','zh-CN-XiaoxiaoNeural')
FFMPEG=imageio_ffmpeg.get_ffmpeg_exe()
metrics={};client=None;last_error=''
local_tts=None;local_lock=threading.Lock()

def local_pcm(text):
    global local_tts
    with local_lock:
        if local_tts is None:
            folder=ROOT/'models/vits-icefall-zh-aishell3'
            config=sherpa_onnx.OfflineTtsConfig(model=sherpa_onnx.OfflineTtsModelConfig(
                vits=sherpa_onnx.OfflineTtsVitsModelConfig(model=str(folder/'model.onnx'),lexicon=str(folder/'lexicon.txt'),tokens=str(folder/'tokens.txt')),
                num_threads=2),rule_fsts=','.join(str(folder/name) for name in ['phone.fst','date.fst','number.fst']))
            local_tts=sherpa_onnx.OfflineTts(config)
        audio=local_tts.generate(text,sid=66,speed=1.08)
        samples=np.asarray(audio.samples)
        if not len(samples):raise RuntimeError('Local TTS produced no audio')
        count=round(len(samples)*24000/audio.sample_rate)
        samples=np.interp(np.arange(count)*audio.sample_rate/24000,np.arange(len(samples)),samples)
        return (np.clip(samples,-1,1)*32767).astype('<i2').tobytes()

def settings():
    try:return json.loads((ROOT/'voice-settings.json').read_text())
    except (OSError,ValueError):return {}
histories=ROOT/'conversations';histories.mkdir(exist_ok=True)
LOG.info('Loading compact int8 streaming recognizer')
recognizer=sherpa_onnx.OnlineRecognizer.from_transducer(
    tokens=str(MODEL/'tokens.txt'),encoder=str(MODEL/'encoder-epoch-99-avg-1.int8.onnx'),
    decoder=str(MODEL/'decoder-epoch-99-avg-1.onnx'),joiner=str(MODEL/'joiner-epoch-99-avg-1.int8.onnx'),
    num_threads=2,sample_rate=16000,feature_dim=80,decoding_method='greedy_search',
    enable_endpoint_detection=True,rule1_min_trailing_silence=1.5,
    rule2_min_trailing_silence=.6,rule3_min_utterance_length=25)
LOG.info('Streaming recognizer ready')

def authenticated(request,token):
    return hmac.compare_digest(request.headers.get('Authorization',''),'Bearer '+token)

def load_history(device):
    try:
        value=json.loads((histories/(device+'.json')).read_text())
        return value[-16:] if isinstance(value,list) else []
    except (OSError,ValueError):return []

def save_history(device,messages):
    path=histories/(device+'.json');temporary=path.with_suffix('.new')
    temporary.write_text(json.dumps(messages[-16:],ensure_ascii=False));temporary.replace(path)

async def hermes_deltas(device,text,selection=None):
    messages=[{'role':'system','content':'这是桌面语音终端对话。请优先用简洁、自然的中文回答，适合直接朗读；用户明确要求详细内容时再展开。'}]+load_history(device)+[{'role':'user','content':text}]
    headers={'Authorization':'Bearer '+HERMES_KEY,'Idempotency-Key':str(uuid.uuid4()),'X-Hermes-Session-Key':'roundclock-'+device}
    body={'model':'hermes-agent','messages':messages,'stream':True,'model_options':{'reasoning':{'enabled':True,'effort':'low'}}}
    selected=selection or settings().get('model')
    if selected=='deepseek':
        body.update(model='deepseek-flash',provider='deepseek',model_options={'reasoning':{'enabled':False}})
    complete=[]
    async with client.post(HERMES_URL+'/v1/chat/completions',headers=headers,json=body,timeout=aiohttp.ClientTimeout(total=240,sock_read=90)) as response:
        if response.status!=200:
            detail=(await response.text())[:300]
            LOG.error('Hermes HTTP %s: %s',response.status,detail)
            raise RuntimeError('Hermes 服务暂时无法响应（HTTP '+str(response.status)+'）')
        async for raw in response.content:
            line=raw.decode('utf-8',errors='replace').strip()
            if not line.startswith('data:'):continue
            data=line[5:].strip()
            if data=='[DONE]':break
            try:event=json.loads(data)
            except ValueError:continue
            choices=event.get('choices') or []
            if not choices:continue
            choice=choices[0]
            if choice.get('finish_reason') in ('error','cancelled'):
                detail=json.dumps(event,ensure_ascii=False)
                for key in [HERMES_KEY,DEEPSEEK_KEY,hermes_env.get('DEEPSEEK_API_KEY','')]:
                    if key:detail=detail.replace(key,'[redacted]')
                LOG.error('Hermes incomplete: %s',detail[:2000])
                raise RuntimeError('Hermes 本次处理未完成')
            delta=(choice.get('delta') or {}).get('content','')
            if isinstance(delta,str) and delta:
                complete.append(delta);yield delta
    answer=''.join(complete).strip()
    if answer:save_history(device,messages[1:]+[{'role':'assistant','content':answer}])

def clean_speech(text):
    text=re.sub(r'```.*?```','代码内容已显示在屏幕上。',text,flags=re.S)
    text=re.sub(r'\[([^\]]+)\]\([^)]*\)',r'\1',text)
    return re.sub(r'[*#`_]+','',text).strip()

async def pcm_speech(text,emit,engine=None):
    text=clean_speech(text)
    if not text or not re.search(r'[\w\u4e00-\u9fff]',text):return
    engine=engine or settings().get('tts','edge')
    if engine=='local' and re.search(r'[\u4e00-\u9fff]',text):
        try:
            data=await asyncio.to_thread(local_pcm,text)
        except Exception:
            LOG.exception('Local TTS unavailable; using online fallback')
        else:
            for offset in range(0,len(data),3840):await emit(data[offset:offset+3840])
            return
    process=await asyncio.create_subprocess_exec(FFMPEG,'-hide_banner','-loglevel','error',
        '-probesize','1024','-analyzeduration','0','-f','mp3','-i','pipe:0',
        '-f','s16le','-acodec','pcm_s16le','-ac','1','-ar','24000','pipe:1',
        stdin=asyncio.subprocess.PIPE,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
    async def feed():
        try:
            prefs=settings()
            communicate=edge_tts.Communicate(text,prefs.get('voice',VOICE),rate=prefs.get('rate','+8%'),pitch=prefs.get('pitch','+0Hz'),proxy=PROXY)
            async for chunk in communicate.stream():
                if chunk['type']=='audio':
                    process.stdin.write(chunk['data']);await process.stdin.drain()
        finally:
            process.stdin.close()
            with contextlib.suppress(Exception):await process.stdin.wait_closed()
    writer=asyncio.create_task(feed());errors=asyncio.create_task(process.stderr.read())
    try:
        async with asyncio.timeout(40):
            while chunk:=await process.stdout.read(3840):
                await emit(chunk)
            await writer
            code=await process.wait()
            if code:raise RuntimeError('语音音频转换失败')
    finally:
        writer.cancel()
        with contextlib.suppress(asyncio.CancelledError,Exception):await writer
        if process.returncode is None:
            process.kill();await process.wait()
        detail=await errors
        if detail and process.returncode:LOG.warning('Audio decoder: %s',detail.decode(errors='replace')[:200])

class Connection:
    def __init__(self,ws):
        self.ws=ws;self.device='roundclock';self.stream=None;self.samples=0;self.partial=''
        self.turn=None;self.sent_audio=False;self.last_partial=0
    async def event(self,kind,**fields):
        await self.ws.send_json({'type':kind,**fields},dumps=lambda value:json.dumps(value,ensure_ascii=False))
    async def cancel(self):
        if self.turn:
            self.turn.cancel()
            with contextlib.suppress(asyncio.CancelledError,Exception):await self.turn
            self.turn=None
        self.stream=None
    async def start(self):
        await self.cancel();self.stream=recognizer.create_stream();self.samples=0;self.partial=''
        await self.event('listening')
    def decode(self,samples):
        recognizer.accept_waveform(self.stream,16000,samples) if hasattr(recognizer,'accept_waveform') else self.stream.accept_waveform(16000,samples)
        while recognizer.is_ready(self.stream):recognizer.decode_stream(self.stream)
        return recognizer.get_result(self.stream)
    async def audio(self,data):
        if self.stream is None:return
        if len(data)%2 or len(data)>16384:raise ValueError('PCM frame invalid')
        samples=np.frombuffer(data,dtype='<i2').astype(np.float32)/32768.0
        self.samples+=len(samples)
        result=self.decode(samples)
        now=time.monotonic()
        if result and result!=self.partial and now-self.last_partial>.12:
            self.partial=result;self.last_partial=now;await self.event('transcript',text=result,final=False)
        if self.samples>=16000*25 or (result and recognizer.is_endpoint(self.stream)):
            await self.finish()
    async def finish(self):
        if self.stream is None:return
        self.decode(np.zeros(6400,dtype=np.float32));self.stream.input_finished()
        while recognizer.is_ready(self.stream):recognizer.decode_stream(self.stream)
        text=recognizer.get_result(self.stream).strip();self.stream=None
        await self.event('recording_end')
        if not text:
            await self.event('no_speech');return
        await self.event('transcript',text=text,final=True)
        self.turn=asyncio.create_task(self.respond(text))
    async def respond(self,text):
        global last_error
        start=time.monotonic();first_text=None;first_audio=None;spoken=False;reply='';pending=''
        sentences=asyncio.Queue(maxsize=8);queued_sentences=0
        async def emit(data):
            nonlocal first_audio,spoken
            if not spoken:
                spoken=True;first_audio=time.monotonic()-start
                await self.event('audio_start',sample_rate=24000,channels=1,format='pcm_s16le')
            await self.ws.send_bytes(data)
        async def speak_worker():
            prepared=asyncio.Queue(maxsize=2);slots=asyncio.Semaphore(2);producers=set()
            async def produce(sentence,audio):
                try:
                    emitted=False
                    async def segment_emit(data):
                        nonlocal emitted
                        emitted=True;await audio.put(data)
                    async with slots:
                        if not spoken:await self.event('synthesizing')
                        for attempt in range(2):
                            try:await pcm_speech(sentence,segment_emit);break
                            except Exception:
                                if emitted or attempt:raise
                                await asyncio.sleep(.15)
                    await audio.put(None)
                except asyncio.CancelledError:raise
                except Exception as error:await audio.put(error)
            async def prefetch():
                while True:
                    sentence=await sentences.get()
                    if sentence is None:await prepared.put(None);return
                    audio=asyncio.Queue(maxsize=16)
                    task=asyncio.create_task(produce(sentence,audio));producers.add(task)
                    await prepared.put((audio,task))
            scheduler=asyncio.create_task(prefetch())
            try:
                while True:
                    item=await prepared.get()
                    if item is None:return
                    audio,task=item
                    while True:
                        data=await audio.get()
                        if data is None:break
                        if isinstance(data,Exception):raise data
                        await emit(data)
                    await task;producers.discard(task)
            finally:
                scheduler.cancel()
                for task in producers:task.cancel()
                await asyncio.gather(scheduler,*producers,return_exceptions=True)
        worker=asyncio.create_task(speak_worker())
        await self.event('thinking')
        try:
            async for delta in hermes_deltas(self.device,text):
                if first_text is None:first_text=time.monotonic()-start
                reply+=delta;pending+=delta
                await self.event('text_delta',text=delta)
                while pending:
                    match=re.search(r'[。！？!?；;\n]',pending)
                    if not match and not queued_sentences:
                        comma=re.search(r'[，,：:]',pending)
                        if comma and comma.end()>=8:match=comma
                    limit=16 if not queued_sentences else 32
                    cut=match.end() if match else (limit if len(pending)>=limit else 0)
                    if not cut:break
                    sentence=pending[:cut];pending=pending[cut:]
                    if sentence.strip():await sentences.put(sentence);queued_sentences+=1
                if worker.done():await worker
            if pending.strip():await sentences.put(pending)
            await sentences.put(None);await worker
            await self.event('reply',text=reply)
            await self.event('audio_end')
            metrics.update(last_first_text_ms=round((first_text or 0)*1000),
                last_first_audio_ms=round((first_audio or 0)*1000),last_turn_ms=round((time.monotonic()-start)*1000))
            LOG.info('Voice timing: %s',metrics)
        except asyncio.CancelledError:raise
        except Exception:
            last_error=traceback.format_exc().replace(HERMES_KEY,'[redacted]') if HERMES_KEY else traceback.format_exc()
            LOG.exception('Voice turn failed')
            await self.event('error',message='对话服务暂时不可用，请重试')
        finally:
            worker.cancel()
            with contextlib.suppress(asyncio.CancelledError,Exception):await worker

async def websocket(request):
    if not authenticated(request,DEVICE_TOKEN):raise web.HTTPUnauthorized()
    ws=web.WebSocketResponse(heartbeat=20,max_msg_size=20000);await ws.prepare(request)
    connection=Connection(ws)
    await connection.event('ready',sample_rate=16000,tts_sample_rate=24000)
    try:
        async for message in ws:
            if message.type==aiohttp.WSMsgType.BINARY:await connection.audio(message.data)
            elif message.type==aiohttp.WSMsgType.TEXT:
                data=json.loads(message.data);kind=data.get('type')
                if kind=='hello':
                    device=str(data.get('device_id','roundclock'))
                    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}',device):raise ValueError('Invalid device id')
                    connection.device=device
                    await connection.event('ready',sample_rate=16000,tts_sample_rate=24000)
                elif kind=='start':await connection.start()
                elif kind=='end':await connection.finish()
                elif kind=='cancel':await connection.cancel();await connection.event('cancelled')
            elif message.type in (aiohttp.WSMsgType.ERROR,aiohttp.WSMsgType.CLOSE):break
    except Exception:LOG.exception('Voice connection ended with error')
    finally:await connection.cancel()
    return ws

async def health(request):
    return web.json_response({'ready':True,'version':'0.2','asr':'streaming-zipformer-small-int8','tts':settings().get('tts','edge'),'voice':VOICE,'sample_rate':16000,**metrics})

async def sample_audio(request):
    if not authenticated(request,ADMIN_TOKEN):raise web.HTTPUnauthorized()
    return web.FileResponse(MODEL/'test_wavs/0.wav')

async def errors(request):
    if not authenticated(request,ADMIN_TOKEN):raise web.HTTPUnauthorized()
    tail=(ROOT/'voice.log').read_text(errors='replace')[-6000:]
    for key in [HERMES_KEY,hermes_env.get('DEEPSEEK_API_KEY','')]:
        if key:tail=tail.replace(key,'[redacted]')
    tail=re.sub(r'sk-[A-Za-z0-9_-]+','[redacted]',tail)
    return web.json_response({'last_error':last_error,'log_tail':tail})

async def info(request):
    if not authenticated(request,ADMIN_TOKEN):raise web.HTTPUnauthorized()
    try:config=yaml.safe_load(Path('/hermes/config.yaml').read_text()) or {}
    except Exception:config={}
    model=config.get('model',{})
    if not isinstance(model,dict):model={'default':model}
    safe={k:model.get(k) for k in ['default','provider','reasoning_effort','max_tokens']}
    for key in ['base_url','api_base']:
        if model.get(key):safe[key+'_host']=urlsplit(str(model[key])).hostname
    agent=config.get('agent',{}) or {}
    return web.json_response({'model':safe,'agent':{k:agent.get(k) for k in ['reasoning_effort','max_turns','max_tokens']},
        'api_base_host':urlsplit(hermes_env.get('OPENAI_BASE_URL','')).hostname,
        'deepseek_key_available':bool(hermes_env.get('DEEPSEEK_API_KEY') or os.getenv('DEEPSEEK_API_KEY')),
        'local_tts_ready':(ROOT/'models/vits-icefall-zh-aishell3/model.onnx').exists(),
        'supplied_key_matches_hermes':hmac.compare_digest(DEEPSEEK_KEY,hermes_env.get('DEEPSEEK_API_KEY','')),
        'voice_settings':settings()})

def asr_benchmark():
    with wave.open(str(MODEL/'test_wavs/0.wav'),'rb') as wav:
        rate=wav.getframerate();samples=np.frombuffer(wav.readframes(wav.getnframes()),dtype='<i2').astype(np.float32)/32768
    stream=recognizer.create_stream();start=time.monotonic()
    for offset in range(0,len(samples),1280):
        stream.accept_waveform(rate,samples[offset:offset+1280])
        while recognizer.is_ready(stream):recognizer.decode_stream(stream)
    stream.accept_waveform(rate,np.zeros(int(rate*.4),dtype=np.float32));stream.input_finished()
    while recognizer.is_ready(stream):recognizer.decode_stream(stream)
    elapsed=time.monotonic()-start
    return {'text':recognizer.get_result(stream),'audio_seconds':len(samples)/rate,'decode_seconds':elapsed,'rtf':elapsed/(len(samples)/rate)}

async def admin_test(request):
    if not authenticated(request,ADMIN_TOKEN):raise web.HTTPUnauthorized()
    mode=request.match_info['mode']
    if mode=='deepseek-direct':
        start=time.monotonic();first=None;answer=[]
        body={'model':'deepseek-flash','messages':[{'role':'user','content':'请只回复：语音连接成功。'}],
            'stream':True,'thinking':{'type':'disabled'},'max_tokens':64}
        async with client.post('https://api.deepseek.com/chat/completions',headers={'Authorization':'Bearer '+DEEPSEEK_KEY},json=body,timeout=aiohttp.ClientTimeout(total=60)) as response:
            if response.status!=200:return web.json_response({'status':response.status,'error':(await response.text()).replace(DEEPSEEK_KEY,'[redacted]')[:300]})
            async for raw in response.content:
                line=raw.decode().strip()
                if not line.startswith('data:') or line[5:].strip()=='[DONE]':continue
                item=json.loads(line[5:]);choices=item.get('choices') or []
                delta=(choices[0].get('delta') or {}).get('content') if choices else None
                if delta:
                    if first is None:first=time.monotonic()-start
                    answer.append(delta)
        return web.json_response({'model':'deepseek-flash','reply':''.join(answer),'first_text_ms':round((first or 0)*1000),'total_ms':round((time.monotonic()-start)*1000)})
    if mode=='asr':return web.json_response(await asyncio.to_thread(asr_benchmark))
    if mode in ('tts','tts-local','tts-edge'):
        start=time.monotonic();first=None;count=0
        async def sink(data):
            nonlocal first,count
            if first is None:first=time.monotonic()-start
            count+=len(data)
        await pcm_speech('你好，语音连接测试成功。',sink,engine={'tts-local':'local','tts-edge':'edge'}.get(mode))
        return web.json_response({'first_pcm_ms':round((first or 0)*1000),'total_ms':round((time.monotonic()-start)*1000),'pcm_bytes':count})
    if mode in ('hermes','hermes-deepseek'):
        start=time.monotonic();first=None;reply=[]
        async for part in hermes_deltas('diagnostic-'+mode,'请只回复：语音连接成功。',selection='deepseek' if mode=='hermes-deepseek' else 'default'):
            if first is None:first=time.monotonic()-start
            reply.append(part)
        return web.json_response({'reply':''.join(reply),'first_text_ms':round((first or 0)*1000),'total_ms':round((time.monotonic()-start)*1000)})
    raise web.HTTPNotFound()

completion_lock=asyncio.Lock()
async def completion_audio(request):
    if not authenticated(request,DEVICE_TOKEN):raise web.HTTPUnauthorized()
    prefs=settings();identity=json.dumps({k:prefs.get(k) for k in ['voice','rate','pitch']},sort_keys=True)
    path=ROOT/('codex-complete-'+hashlib.sha256(identity.encode()).hexdigest()[:12]+'.pcm')
    async with completion_lock:
        if not path.exists():
            chunks=[]
            async def collect(data):chunks.append(data)
            await pcm_speech('任务已完成。',collect,engine='edge')
            if not chunks:raise web.HTTPServiceUnavailable()
            temporary=path.with_suffix('.new');temporary.write_bytes(b''.join(chunks));temporary.replace(path)
    return web.FileResponse(path,headers={'Content-Type':'application/octet-stream'})

async def setup(app):
    global client
    client=aiohttp.ClientSession(connector=aiohttp.TCPConnector(limit=12),trust_env=False)
    if settings().get('tts')=='local':
        try:await asyncio.to_thread(local_pcm,'你好。')
        except Exception:LOG.exception('Local TTS warmup failed; online fallback remains available')
    yield
    await client.close()

app=web.Application(client_max_size=32768)
from codex_status import install as install_codex_status
install_codex_status(app,DEVICE_TOKEN)
from stocks import install as install_stocks
install_stocks(app,DEVICE_TOKEN)
app.cleanup_ctx.append(setup)
app.router.add_get('/health',health);app.router.add_get('/ws',websocket)
app.router.add_post('/admin/test/{mode}',admin_test)
app.router.add_get('/admin/sample.wav',sample_audio)
app.router.add_get('/admin/errors',errors)
app.router.add_get('/admin/info',info)
app.router.add_get('/codex/completion.pcm',completion_audio)
web.run_app(app,host='0.0.0.0',port=8767,print=LOG.info)
