# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy>=2,<3", "pillow>=11,<13"]
# ///
"""Original seamless stereo surf synthesis and a compact UI font atlas."""
from pathlib import Path
import json, wave
import numpy as np
from PIL import Image, ImageDraw, ImageFont

root=Path(__file__).resolve().parents[1]
out=root/'assets/generated';out.mkdir(parents=True,exist_ok=True)
rate=22050;seconds=28;n=rate*seconds
rng=np.random.default_rng(1099)
t=np.arange(n)/rate;freq=np.fft.rfftfreq(n,1/rate)

def noise(power,low,high):
    spectrum=rng.normal(size=len(freq))+1j*rng.normal(size=len(freq))
    spectrum*=np.maximum(freq,low)**(-power/2)
    spectrum*=np.exp(-(freq/high)**3)*(1-np.exp(-(freq/low)**3))
    spectrum[0]=0
    sound=np.fft.irfft(spectrum,n=n)
    return sound/max(np.std(sound),1e-8)

# Every layer is periodic: no silence or obvious crossfade at the loop seam.
base=noise(1.7,45,1800)*0.10
left=base+noise(1.5,80,2200)*0.065
right=base+noise(1.5,80,2200)*0.065
for onset,width,strength,pan in [(1.5,5.8,.22,-.3),(7.9,6.5,.28,.38),(15.8,7.8,.31,-.22),(24.0,5.5,.24,.1)]:
    age=(t-onset)%seconds
    env=(1-np.exp(-age/.6))*np.exp(-age/(width*.34))
    env[age>width]=0
    # Gentle swell arrives first, then crisp breaking foam and a long retreat.
    rumble=noise(2.1,55,550)
    foam=noise(.7,350,6800)
    wash=noise(1.2,140,4200)
    crest=np.exp(-((age-1.05)/.66)**2)
    surf=strength*(rumble*.35*env+foam*.55*crest+wash*.6*env)
    left+=surf*(.8-pan*.24);right+=np.roll(surf,int(.014*rate))*(.8+pan*.24)
# Distant irregular gull calls, softened and placed across the stereo field.
for start in (5.5,6.35,18.2):
    a=t-start;env=np.exp(-((a-.26)/.23)**4)*(a>=0)*(a<.8)
    f=1380-480*np.clip(a,0,1)+75*np.sin(2*np.pi*9*a)
    phase=np.cumsum(f)*2*np.pi/rate
    call=(np.sin(phase)+.26*np.sin(2*phase))*.012*env
    left+=call*.65;right+=np.roll(call,240)
master=np.stack((left,right),1)
master=np.tanh(master*.8)*.64
# Remove tiny residual DC and enforce sample-continuity at the join.
master-=master.mean(0)
fade=128
for ch in range(2):
    delta=master[-1,ch]-master[0,ch]
    master[-fade:,ch]-=np.linspace(0,delta,fade)
pcm=(master*32767).astype('<i2')
(out/'ambience.pcm').write_bytes(pcm.tobytes())
with wave.open(str(out/'ocean-ambience.wav'),'wb') as f:
    f.setnchannels(2);f.setsampwidth(2);f.setframerate(rate);f.writeframes(pcm.tobytes())
report=dict(duration_seconds=seconds,sample_rate=rate,channels=2,peak_dbfs=float(20*np.log10(np.max(np.abs(master)))),rms_dbfs=float(20*np.log10(np.sqrt(np.mean(master**2)))),seam_delta=[int(v) for v in pcm[-1].astype(int)-pcm[0].astype(int)],clipped_samples=int(np.sum(np.abs(master)>=1)))
(out/'audio-report.json').write_text(json.dumps(report,indent=2))
font_path=Path('/System/Library/Fonts/Avenir Next.ttc')
if not font_path.exists():font_path=Path('/System/Library/Fonts/Supplemental/Arial.ttf')
font=ImageFont.truetype(str(font_path),24)
atlas=Image.new('RGBA',(512,256),(255,255,255,0));draw=ImageDraw.Draw(atlas)
for i in range(96):draw.text(((i%16)*32+2,(i//16)*32),chr(i+32),font=font,fill='white',stroke_width=0)
atlas.save(out/'font.png')
a=np.array(atlas).astype(np.uint16)
argb=((a[:,:,3]>>4)<<12)|0xfff
(out/'font.bin').write_bytes(argb.astype('<u2').tobytes())
print('Original coastal soundscape:',report)
