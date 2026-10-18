"""Capture and exercise real spawned/stolen motorcycles in the native game.

python game/tools/verify_motorcycles.py [--kind moto_city|moto_cross|traffic] [--skip-build]
Uses cached geography offline and the normal avatar, weapon and vehicle APIs.
"""
from pathlib import Path
import argparse
import os
import subprocess

GAME=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind',choices=['moto_city','moto_cross','traffic'])
    parser.add_argument('--skip-build',action='store_true')
    parser.add_argument('--aim',choices=['auto','off','on'],default='auto',help='Inspect both grips or the one-handed pistol pose')
    parser.add_argument('--spawn',nargs=2,type=float,default=[2.352571,48.857268],metavar=('LON','LAT'))
    args=parser.parse_args()
    env={k.upper():v for k,v in os.environ.items()}
    env.update(PATH='C:/msys64/ucrt64/bin;'+env.get('PATH',''),SAIDA_WINDOW_HIDDEN='1',
        SAIDA_WINDOW_SIZE='1280x720',HTTP_PROXY='http://127.0.0.1:9',HTTPS_PROXY='http://127.0.0.1:9')
    output=GAME/'generated/motorcycle-validation';output.mkdir(parents=True,exist_ok=True)
    env['SAIDA_CRASH_DIR']=str(output)
    if not args.skip_build:
        subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(GAME/'Play.ps1'),'-BuildOnly'],env=env,check=True)
    for kind in ([args.kind] if args.kind else ['moto_city','moto_cross','traffic']):
        current=dict(env,R1WORLD_CAPTURE_MOTORCYCLE=kind)
        aim=args.aim=='on' or (args.aim=='auto' and kind!='moto_city')
        if aim:current['R1WORLD_CAPTURE_MOTORCYCLE_AIM']='1'
        else:current.pop('R1WORLD_CAPTURE_MOTORCYCLE_AIM',None)
        name=kind+('-aim' if aim else '')+'-rider'
        screenshot=output/(name+'.png');log=output/(name+'.log')
        screenshot.unlink(missing_ok=True)
        with log.open('w',encoding='utf-8') as stream:
            run=subprocess.run([str(GAME/'generated/world-windows/R1World.exe'),'--project',str(GAME),
                '--smoke','--spawn',str(args.spawn[0]),str(args.spawn[1]),'--screenshot',str(screenshot),
                '--at','1792231200','--after-frames','12'],cwd=GAME,env=current,stdout=stream,stderr=stream,timeout=240)
        print(kind,'exit',run.returncode,'log',log,flush=True)
        if run.returncode!=0:
            print('\n'.join(log.read_text(encoding='utf-8',errors='replace').splitlines()[-20:]),flush=True)
            raise SystemExit(run.returncode)
        if not screenshot.is_file():raise RuntimeError('Capture succeeded without its PNG: '+str(screenshot))


if __name__=='__main__':main()
