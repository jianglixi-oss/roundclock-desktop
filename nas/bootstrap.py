"""Prepare an isolated NAS voice runtime without modifying the Hermes image."""
import os,sys,json,time,threading,subprocess,hashlib,tarfile,urllib.request
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer

ROOT=Path('/app');DEPS=ROOT/'deps';MODELS=ROOT/'models'
state={'ready':False,'stage':'starting'}
def stage(message):
    state.update(stage=message,updated_at=time.time())
    (ROOT/'boot-state.json').write_text(json.dumps(state))
    print(message,flush=True)
class Status(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def do_GET(self):
        raw=json.dumps(state).encode();self.send_response(200)
        self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(raw)));self.end_headers();self.wfile.write(raw)
server=ThreadingHTTPServer(('0.0.0.0',8767),Status)
threading.Thread(target=server.serve_forever,daemon=True).start()
try:
    requirements=ROOT/'requirements.txt';digest=hashlib.sha256(requirements.read_bytes()).hexdigest()
    stamp=ROOT/'deps.ready'
    if not stamp.exists() or stamp.read_text()!=digest:
        stage('Installing isolated streaming ASR / TTS dependencies')
        subprocess.run([sys.executable,'-m','pip','install','--disable-pip-version-check','--target',str(DEPS),'--upgrade','--cache-dir',str(ROOT/'pip-cache'),'-r',str(requirements)],check=True)
        stamp.write_text(digest)
    model_name='sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16'
    model=MODELS/model_name
    needed=['encoder-epoch-99-avg-1.int8.onnx','decoder-epoch-99-avg-1.onnx','joiner-epoch-99-avg-1.int8.onnx','tokens.txt','test_wavs/0.wav']
    if not all((model/name).exists() for name in needed):
        MODELS.mkdir(exist_ok=True)
        archive=MODELS/(model_name+'.tar.bz2')
        if not archive.exists():
            stage('Downloading compact Chinese / English streaming recognition model')
            url='https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models/'+archive.name
            temporary=archive.with_suffix('.download')
            with urllib.request.urlopen(url,timeout=90) as response,temporary.open('wb') as target:
                while chunk:=response.read(1024*1024):target.write(chunk)
            temporary.replace(archive)
        stage('Extracting streaming recognition model')
        with tarfile.open(archive,mode='r|bz2') as source:
            for member in source:
                relative=member.name.removeprefix(model_name+'/')
                if relative in needed and member.isfile():source.extract(member,MODELS,filter='data')
        if not all((model/name).exists() for name in needed):raise RuntimeError('Model archive missing expected files')
    stage('Model ready; waiting for voice bridge application')
    while not (ROOT/'bridge.py').exists():time.sleep(1)
    env=dict(os.environ,PYTHONPATH=str(DEPS),VOICE_MODEL_DIR=str(model),PYTHONUNBUFFERED='1')
    server.shutdown();server.server_close()
    os.execve(sys.executable,[sys.executable,'-u',str(ROOT/'bridge.py')],env)
except Exception as error:
    stage('Startup failed: '+str(error))
    while True:time.sleep(30)
