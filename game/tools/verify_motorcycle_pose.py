"""Check actual Rocketbox motorcycle hand contacts without loading the city or GPU."""
from pathlib import Path
import argparse
import os
import subprocess

GAME=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--skip-build',action='store_true')
    args=parser.parse_args()
    env={k.upper():v for k,v in os.environ.items()}
    env['PATH']='C:/msys64/ucrt64/bin;'+env.get('PATH','')
    output=GAME/'generated/motorcycle-validation';output.mkdir(parents=True,exist_ok=True)
    temporary=GAME/'generated/tmp';temporary.mkdir(parents=True,exist_ok=True)
    env['TMP']=env['TEMP']=str(temporary)
    if not args.skip_build:
        subprocess.run(['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(GAME/'Play.ps1'),'-BuildOnly'],env=env,check=True)
    subprocess.run(['cmake','--build',str(GAME/'generated/world-windows/build'),
                    '--target','r1_motorcycle_pose_test','--parallel','2'],env=env,check=True)
    target=GAME/'generated/native-tests/r1_motorcycle_pose_test.exe'
    subprocess.run([str(target),str(GAME)],env=env,check=True)

if __name__=='__main__':main()
