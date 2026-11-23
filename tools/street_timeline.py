"""Track each Electra session across overlaps between the chase's movie clips."""
import re

class StreetTimeline:
    def __init__(self):
        self.section=None
        self.open={}
    def update(self,line):
        section=re.search(r"Logging Custom Event ChaseSection-([\w-]+)",line)
        if section:self.section=section[1]
        m=re.search(r"LogElectraPlayer: \[([0-9a-fA-F]+)\]\[([0-9a-fA-F]+)\] (.*)",line)
        if not m:return
        key=(m[1],m[2])
        if "IMediaPlayer::Open(" in m[3]:self.open[key]=[line.strip(),False]
        elif "IMediaPlayer::Close()" in m[3]:self.open.pop(key,None)
        elif key in self.open:
            if "Playback started" in m[3]:self.open[key][1]=True
            if "Playback stopped." in m[3]:self.open[key][1]=False
    @property
    def downtown(self):
        return self.section in ("CityIntro","CityStreets") and not self.open
