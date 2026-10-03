"""Independent production late raw-name regression; no test-hook library needed."""
import argparse,os,re,subprocess,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--lib',required=True);p.add_argument('--logs',type=Path);a=p.parse_args()
root=Path(__file__).resolve().parent
def run(args,env=None):
    result=subprocess.run(args,env=env,capture_output=True,text=True,timeout=30)
    assert result.returncode==0,result.stdout+result.stderr
    return result.stdout+result.stderr
with tempfile.TemporaryDirectory(prefix='peak-ordinary-late-') as directory:
    directory=Path(directory);exe=directory/'application'
    run(['cc','-O2','-UNDEBUG',str(root/'ordinary_late_application.c'),'-ldl','-o',str(exe)])
    env={k:v for k,v in os.environ.items() if not k.startswith(('PEAK_','FLAME_')) and k!='LD_PRELOAD'}
    env.update(LD_PRELOAD=str(Path(a.lib).resolve()),PEAK_TARGET='cblas_sdot',PEAK_HEARTBEAT_INTERVAL='0',PEAK_ENABLE_GLOBAL_HEARTBEAT='false',PEAK_ENABLE_PER_TARGET_HEARTBEAT='false',PEAK_DETACH_BACKEND='signal',OMP_NUM_THREADS='1',OPENBLAS_NUM_THREADS='1')
    for kind,flags in [('dynamic',['-DEXPORTED_SYMBOL']),('hidden',[]),('local',['-DLOCAL_SYMBOL']),('duplicate',['-DLOCAL_SYMBOL']),('stripped',['-DLOCAL_SYMBOL'])]:
        module=directory/(kind+'.so')
        sources=[str(root/'ordinary_late_provider.c')]
        if kind=='duplicate':sources.append(str(root/'ordinary_late_duplicate.c'))
        run(['cc','-O2','-shared','-fPIC',*flags,*sources,'-o',str(module)])
        if kind=='stripped':run(['objcopy','--strip-all',str(module)])
        symbols=run(['readelf','--dyn-syms','-W',str(module)])
        assert ('cblas_sdot' in symbols)==(kind=='dynamic')
        text=run([str(exe),str(module),kind],env)
        if a.logs:
            a.logs.mkdir(parents=True,exist_ok=True);(a.logs/(kind+'.log')).write_text(text)
        assert 'ORDINARY_LATE_ORIGINAL_PASS calls=100' in text
        match=re.search(r'Recorded calls: (\d+)',text);assert match,text
        assert int(match[1])==(0 if kind=='stripped' else 100),text
        print('PASS ordinary late '+kind+' originals=100 recorded='+match[1],flush=True)
