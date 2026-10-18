"""Deterministic native vehicle-radio integration test, using a local HTTP tone.

python game/tools/verify_radio.py [--skip-build]
No public broadcaster or catalogue is required for this regression test.
"""
from pathlib import Path
import argparse
import http.server
import math
import os
import struct
import subprocess
import threading
import wave

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
                    '--target','r1_vehicle_radio_test','--parallel','2'],env=env,check=True)
    target=GAME/'generated/native-tests/r1_vehicle_radio_test.exe'
    with wave.open(str(output/'radio-tone.wav'),'wb') as sound:
        sound.setnchannels(1);sound.setsampwidth(2);sound.setframerate(44100)
        sound.writeframes(b''.join(struct.pack('<h',int(math.sin(i*math.tau*220/44100)*500)) for i in range(44100*4)))

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self,*a,**kw):super().__init__(*a,directory=str(output),**kw)
        def log_message(self,*args):pass

    server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Handler)
    worker=threading.Thread(target=server.serve_forever,daemon=True);worker.start()
    try:
        subprocess.run([str(target),str(output/'radio-fixture'),f'http://127.0.0.1:{server.server_port}/radio-tone.wav'],env=env,check=True,timeout=80)
    finally:
        server.shutdown();server.server_close();worker.join()


if __name__=='__main__':main()
