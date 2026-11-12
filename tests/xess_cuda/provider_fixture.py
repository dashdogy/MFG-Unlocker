import argparse, hashlib, os, pathlib, shutil, subprocess
p=argparse.ArgumentParser();p.add_argument('--exe',required=True);p.add_argument('--dll',required=True);p.add_argument('--host',required=True);p.add_argument('--runtime',required=True);p.add_argument('--output',required=True);a=p.parse_args()
runtime=pathlib.Path(a.runtime)
assert hashlib.sha256((runtime/'libxess_fg.dll').read_bytes()).hexdigest()=='ec5e0c65e075570c6ede72618bb666d0be0c2e10b2ea9762c0fe8cb8e375ab27'
outputs=[]
for mode in ('off','on'):
    folder=pathlib.Path(a.output)/mode;folder.mkdir(parents=True,exist_ok=True)
    dll=folder/'MFGUnlocker.dll';host=folder/'OptiScaler.dll';exe=folder/pathlib.Path(a.exe).name
    shutil.copy2(a.dll,dll);shutil.copy2(a.host,host);shutil.copy2(a.exe,exe)
    for name in ('libxess_fg.dll','libxell.dll'):shutil.copy2(runtime/name,folder/name)
    # Product logs follow the executable directory, so each mode needs its own executable copy.
    result=subprocess.run([str(exe.resolve()),str(dll.resolve()),str(host.resolve()),str(folder.resolve()),mode],cwd=folder,env=os.environ.copy(),stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
    (folder/'fixture.log').write_text(result.stdout,encoding='utf-8');print(result.stdout)
    assert result.returncode==0,f'{mode} provider fixture failed: {result.returncode}'
    outputs.append((folder/'frame-hashes.txt').read_bytes())
    logs='\n'.join(path.read_text(encoding='utf-8-sig',errors='replace') for path in folder.glob('*.log'))
    assert 'provider_prepare' in logs and '"ported":true' in logs,'real ported provider preparation was not logged'
assert outputs[0]==outputs[1],'on/off generated output hashes differ'
print('D3D12_PROVIDER_ON_OFF_EQUIVALENCE_PASS outputs',len(outputs[0].splitlines()))
