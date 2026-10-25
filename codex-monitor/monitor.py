"""Read-only local Codex telemetry: official quota RPC + local lifecycle events."""
import json,queue,threading,subprocess,shutil,time,sqlite3,sys,urllib.request
from pathlib import Path
ROOT=Path(__file__).resolve().parent
CONFIG=json.loads((ROOT/'config.private.json').read_text(encoding='utf-8'))
HOME=Path.home()/'.codex'
CREATE_NO_WINDOW=getattr(subprocess,'CREATE_NO_WINDOW',0)

class Quota:
    def __init__(self):self.process=None;self.messages=queue.Queue();self.sequence=0
    def close(self):
        if self.process:
            self.process.terminate()
            try:self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:self.process.kill()
            self.process=None
    def connect(self):
        self.close();self.messages=queue.Queue()
        executable=shutil.which('codex') or CONFIG['codex_path']
        self.process=subprocess.Popen([executable,'app-server','--listen','stdio://'],stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,encoding='utf-8',creationflags=CREATE_NO_WINDOW)
        messages=self.messages;process=self.process
        def reader():
            for line in process.stdout:
                try:messages.put(json.loads(line))
                except ValueError:pass
        threading.Thread(target=reader,daemon=True).start()
        self.call('initialize',{'clientInfo':{'name':'roundclock_monitor','version':'0.1'},'capabilities':{}})
        self.process.stdin.write('{"method":"initialized"}\n');self.process.stdin.flush()
    def call(self,method,params):
        self.sequence+=1;identifier=self.sequence
        self.process.stdin.write(json.dumps({'id':identifier,'method':method,'params':params})+'\n');self.process.stdin.flush()
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            item=self.messages.get(timeout=max(.1,deadline-time.monotonic()))
            if item.get('id')==identifier:
                if 'error' in item:raise RuntimeError('Codex quota RPC unavailable')
                return item['result']
        raise TimeoutError('Codex quota RPC')
    def read(self):
        if not self.process or self.process.poll() is not None:self.connect()
        result=self.call('account/rateLimits/read',{})
        return (result.get('rateLimitsByLimitId') or {}).get('codex') or result.get('rateLimits') or {}

class Events:
    def __init__(self):self.files={}
    def read(self):
        db=sqlite3.connect((HOME/'state_5.sqlite').as_uri()+'?mode=ro',uri=True,timeout=2)
        try:rows=db.execute('select id,rollout_path from threads where archived=0 order by updated_at_ms desc limit 12').fetchall()
        finally:db.close()
        states=[]
        for identifier,name in rows:
            path=Path(name)
            if not path.is_file():continue
            stat=path.stat();entry=self.files.setdefault(identifier,{'offset':0,'state':'idle','at':0,'waiting':False})
            if stat.st_size<entry['offset']:entry['offset']=0
            with path.open('rb') as source:
                if entry['offset']==0 and stat.st_size>3000000:
                    source.seek(stat.st_size-3000000);source.readline()
                else:source.seek(entry['offset'])
                while True:
                    before=source.tell();line=source.readline()
                    if not line:break
                    if not line.endswith(b'\n'):source.seek(before);break
                    try:data=json.loads(line)
                    except ValueError:continue
                    if data.get('type')!='event_msg':continue
                    from datetime import datetime
                    at=int(datetime.fromisoformat(data['timestamp'].replace('Z','+00:00')).timestamp())
                    payload=data.get('payload') or {};kind=payload.get('type');state=None
                    if kind=='task_started':state='working';entry['waiting']=False
                    elif kind=='task_complete':state='waiting' if entry['waiting'] else 'completed'
                    elif kind in ('turn_aborted','error'):state='error'
                    elif kind=='item_completed':
                        item=payload.get('item') or {};item_type=item.get('type','')
                        if item_type=='UserMessage':entry['waiting']=False;state='working'
                        elif item_type=='AgentMessage':state='completed' if item.get('phase')=='final' else 'working'
                        elif item_type in ('CommandExecution','Reasoning','McpToolCall','FileChange','Extension'):state='working'
                        if 'request_user_input' in str(item.get('tool','')):
                            entry['waiting']=True;state='waiting'
                    if state:
                        entry['state']=state;entry['at']=at
                entry['offset']=source.tell()
            if time.time()-entry['at']>900 and entry['state']=='working':entry['state']='unknown'
            states.append(entry)
        busy=[s for s in states if s['state'] in ('working','waiting')]
        chosen=max(busy or states,key=lambda s:s['at'],default={'state':'idle','at':int(time.time())})
        return {'state':chosen['state'],'state_at':chosen['at'],'active':len(busy)}

def main():
    quota=Quota();events=Events();windows={};usage_at=0;next_quota=0;last_error=0
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        while True:
            now=time.time()
            if now>=next_quota:
                try:
                    result=quota.read();windows={}
                    for name in ['primary','secondary']:
                        value=result.get(name)
                        windows[name]=None if value is None else {'used':value.get('usedPercent'),'minutes':value.get('windowDurationMins',0),'reset':value.get('resetsAt',0)}
                    usage_at=int(now);next_quota=now+120
                except Exception:
                    quota.close();next_quota=now+30
            try:
                data={**events.read(),**windows,'usage_at':usage_at}
                request=urllib.request.Request(CONFIG['url'],data=json.dumps(data).encode(),headers={
                    'Authorization':'Bearer '+CONFIG['ingest_token'],'Content-Type':'application/json'})
                with opener.open(request,timeout=5) as response:response.read()
                (ROOT/'health.json').write_text(json.dumps({'updated_at':int(now),'state':data['state'],'usage_at':usage_at}),encoding='utf-8')
            except Exception:
                if now-last_error>60:print('Codex display sync temporarily unavailable',flush=True);last_error=now
            if '--once' in sys.argv:return
            time.sleep(3)
    finally:quota.close()

if __name__=='__main__':
    if '--once' not in sys.argv:
        import msvcrt
        singleton=(ROOT/'monitor.lock').open('a+b');singleton.seek(0);singleton.write(b'0');singleton.flush();singleton.seek(0)
        try:msvcrt.locking(singleton.fileno(),msvcrt.LK_NBLCK,1)
        except OSError:sys.exit(0)
    main()
