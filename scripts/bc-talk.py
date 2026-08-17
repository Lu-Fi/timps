#!/usr/bin/env python3
"""
bc-talk.py - send audio into a timps ONVIF backchannel (client -> camera speaker).

Requests the dynamic backchannel sink pad on rtspsrc and links a tone (or mic)
-> G.711 mu-law -> RTP PCMU to it. Prints the negotiated pad caps so you can
see exactly what the camera's SDP produced.

Deps (Ubuntu):
  sudo apt install python3-gi gstreamer1.0-plugins-base gstreamer1.0-plugins-good

Usage:
  python3 scripts/bc-talk.py                      # 440 Hz test tone
  python3 scripts/bc-talk.py --mic                # your microphone
  python3 scripts/bc-talk.py --url rtsp://user:pass@ip:554/ch0
"""
import sys, gi, argparse
gi.require_version("Gst", "1.0")
from gi.repository import Gst, GLib

ap = argparse.ArgumentParser()
ap.add_argument("--url", default="rtsp://thingino:thingino@192.168.1.100:554/ch0")
ap.add_argument("--mic", action="store_true", help="use microphone instead of a test tone")
args = ap.parse_args()

Gst.init(None)
pipe = Gst.Pipeline.new("bc")

src = Gst.ElementFactory.make("rtspsrc", "src")
src.set_property("location", args.url)
src.set_property("backchannel", 1)     # 1 = ONVIF backchannel
src.set_property("latency", 100)
pipe.add(src)

# send chain: source -> convert -> resample -> 8k/mono -> mulaw -> RTP PCMU
names = (["autoaudiosrc"] if args.mic else ["audiotestsrc"]) + \
        ["audioconvert", "audioresample", "capsfilter", "mulawenc", "rtppcmupay"]
e = {}
for n in names:
    el = Gst.ElementFactory.make(n, None)
    if not el:
        sys.exit(f"missing GStreamer element: {n} (install gstreamer1.0-plugins-good/base)")
    e[n] = el
    pipe.add(el)
if not args.mic:
    e["audiotestsrc"].set_property("wave", 0)      # sine
    e["audiotestsrc"].set_property("is-live", True)
e["capsfilter"].set_property("caps", Gst.Caps.from_string("audio/x-raw,rate=8000,channels=1"))
e["rtppcmupay"].set_property("pt", 0)
for a, b in zip(names, names[1:]):
    e[a].link(e[b])

state = {"linked": False}
def link_backchannel(_=None):
    if state["linked"]:
        return False
    req = getattr(src, "request_pad_simple", None) or src.get_request_pad
    sinkpad = req("sink_%u")
    if not sinkpad:
        print("... backchannel sink pad not available yet")
        return True   # retry
    caps = sinkpad.query_caps(None)
    print("backchannel pad:", sinkpad.get_name(), "| pad caps:", caps.to_string())
    r = e["rtppcmupay"].get_static_pad("src").link(sinkpad)
    print("link result:", r.value_name)
    state["linked"] = (r == Gst.PadLinkReturn.OK)
    if state["linked"]:
        print(">>> sending audio to the backchannel - watch 'logread -f | grep bc' on the camera")
    return not state["linked"]

src.connect("pad-added", lambda el, pad: print("recv pad:", pad.get_name()))

bus = pipe.get_bus(); bus.add_signal_watch()
def on_msg(_b, m):
    if m.type == Gst.MessageType.ERROR:
        err, dbg = m.parse_error(); print("ERROR:", err.message, "|", dbg); loop.quit()
    elif m.type == Gst.MessageType.STATE_CHANGED and m.src == pipe:
        _o, new, _p = m.parse_state_changed()
        if new == Gst.State.PLAYING:
            link_backchannel()
bus.connect("message", on_msg)

pipe.set_state(Gst.State.PLAYING)
GLib.timeout_add(1000, link_backchannel)   # retry until the pad shows up
loop = GLib.MainLoop()
print("Ctrl-C to stop.")
try:
    loop.run()
except KeyboardInterrupt:
    pass
pipe.set_state(Gst.State.NULL)
