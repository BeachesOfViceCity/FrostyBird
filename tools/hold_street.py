"""Observer-side recognition for an opt-in pause on an actual submitted city frame."""
from contextlib import redirect_stdout
import io
from pathlib import Path

import numpy as np
from PIL import Image,ImageOps

from compare_street import compare
from street_timeline import StreetTimeline


def appearance(image):
    im=image.convert("RGB")
    a=np.asarray(im.resize((256,144),Image.Resampling.BOX))
    rows=np.flatnonzero(a.max(axis=(1,2))>6)
    if len(rows)>8:
        im=im.crop((0,im.height*int(rows[0])//144,im.width,(im.height*(int(rows[-1])+1)+143)//144))
    a=np.asarray(ImageOps.fit(im,(160,66),Image.Resampling.BOX),dtype=float)/255
    lum=a.mean(axis=-1);lum-=lum.mean()
    return a,lum/(np.sqrt((lum*lum).sum())+1e-9)


class StreetHold:
    def __init__(self,reference):
        self.reference=Path(reference)
        with Image.open(reference) as im:self.a,self.lum=appearance(im)
        self.done=False;self.attempts=0

    def consider(self,run,frame):
        if self.done:return
        stem=run/f"street-window-{frame}";s=StreetTimeline()
        for l in stem.with_suffix(".timeline").read_text(errors="replace").splitlines():s.update(l)
        if not s.downtown:return
        self.attempts+=1
        with Image.open(run/f"frame-{frame:06}.ppm") as im:a,l=appearance(im)
        mae=float(np.abs(self.a-a).mean());corr=float((self.lum*l).sum())
        with (run/"street-scores.tsv").open("a") as f:f.write(f"{frame}\t{s.section}\t{corr:.6f}\t{mae:.6f}\n")
        if corr>=0.90 and mae<=0.095:
            output=run/"held-street"
            with redirect_stdout(io.StringIO()):m=compare(run,frame,self.reference,output)
            if m["exact_rgb_pixels"]==m["visible_pixels"]:
                (run/f"window-pair-{frame}.hold").touch()
                (output/"present.txt").write_text(f"{frame}\n")
                self.done=True
                print(f"Street hold: verified shipping frame {frame}, correlation {corr:.3f}, thumbnail difference {mae:.3f}",flush=True)
        if not self.done and self.attempts<75:(run/"window-pair.request").touch()
