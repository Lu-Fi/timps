# Audio backchannel — moved

This file is a stub kept so older links still land somewhere.

The audio backchannel is documented in the wiki, on the
[Audio](wiki/Audio.md) page: the shared `IMP_AO` owner and its ownership
arbitration, the ONVIF backchannel itself (SDP advertisement, codecs,
single-talker election, AEC), the browser push-to-talk WebSocket at
`/talk`, and the system-sound play queue. Build flags are listed on
[Building](wiki/Building.md), the `audio.*` keys on
[Configuration Reference](wiki/Configuration-Reference.md).

What used to be here described the pre-2026 architecture, in which timps
decoded backchannel RTP and piped the PCM to `/bin/iac`, thingino's
`ingenic-audiodaemon` client, and never opened `IMP_AO` itself. That is
no longer how it works: `src/rtsp/speaker.c` owns the AO device natively
(via the HAL), there is no external audio daemon in the path, and
`BR2_PACKAGE_TIMPS_BACKCHANNEL` needs no `ingenic-audiodaemon` on the
device. Only a physically wired speaker is still a hard requirement.
