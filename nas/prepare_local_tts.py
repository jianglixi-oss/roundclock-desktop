"""Prepare a small local Chinese TTS candidate for measured latency comparison."""
import json,tarfile,urllib.request,time
from pathlib import Path
root=Path('/app/models');name='vits-icefall-zh-aishell3';model=root/name
wanted={'model.onnx','lexicon.txt','tokens.txt','phone.fst','date.fst','number.fst'}
state=Path('/app/local-tts-status.json')
def status(stage):state.write_text(json.dumps({'stage':stage,'updated_at':time.time()}));print(stage,flush=True)
try:
    root.mkdir(exist_ok=True);archive=root/(name+'.tar.bz2')
    if not archive.exists():
        status('downloading');temporary=archive.with_suffix('.download')
        with urllib.request.urlopen('https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/'+archive.name,timeout=90) as response,temporary.open('wb') as target:
            while chunk:=response.read(1024*1024):target.write(chunk)
        temporary.replace(archive)
    status('extracting')
    with tarfile.open(archive,'r|bz2') as source:
        for member in source:
            if member.name.removeprefix(name+'/') in wanted and member.isfile():source.extract(member,root,filter='data')
    if not all((model/file).exists() for file in ['model.onnx','lexicon.txt','tokens.txt']):raise RuntimeError('Incomplete local TTS model')
    status('ready')
except Exception as error:
    status('failed: '+str(error));raise
