"""Original CC0 looping emergency sirens and helicopter rotor, no recordings."""
from pathlib import Path
import math
import struct
import wave


def main():
    root=Path(__file__).resolve().parents[2]/'assets/audio/police'
    root.mkdir(parents=True,exist_ok=True)
    rate=22050
    for name,seconds in [('siren_eu',2),('siren_us',4),('rotor',2)]:
        phase=0.;samples=[]
        for i in range(rate*seconds):
            t=i/rate
            if name=='rotor':
                value=(math.sin(2*math.pi*62*t)+.45*math.sin(2*math.pi*124*t))*(.45+.55*math.sin(2*math.pi*16*t)**4)
            else:
                hz=(435 if t%1<.5 else 580) if name=='siren_eu' else 650+280*math.sin(2*math.pi*t/4)
                phase+=2*math.pi*hz/rate
                value=.6*math.sin(phase)+.12*math.sin(phase*3)
            samples.append(struct.pack('<h',int(max(-1,min(1,value*.5))*32767)))
        with wave.open(str(root/(name+'.wav')),'wb') as output:
            output.setparams((1,2,rate,0,'NONE','not compressed'))
            output.writeframes(b''.join(samples))


if __name__=='__main__':main()
