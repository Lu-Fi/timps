# Real certificates (ACME) + HTTP->HTTPS redirect — cost assessment

Date: 2026-08-29. Assessment only, nothing implemented. Follow-up to today's
self-signed-cert usability findings (iOS Safari multi-origin trust pain, see
PREVIEW_REALTIME_IDEAS_2026-08-29.md) and MOTORS_WSS_SIZE_REVIEW_2026-08-29.md.

Reference build used for all numbers:
`output/ciao/cinnado_d1_t31l_sc2336_atbm6031-...-192.168.10.33` (T31L, 8 MB NOR),
built 2026-08-29 14:11 UTC.

---

## Q1: Let's Encrypt / ACME on these cameras

### Verdict up front

**Let's Encrypt does not fit this fleet's deployment model, and the binary
that would make it cheapest doesn't matter because the constraint that
actually bites is a different one: the rootfs partition has 12 KB free.**
The honest recommendations, in order:

1. Best fit for THIS fleet: a **private fleet CA** — per-camera certs signed
   at build time by one self-made CA root, CA root installed once per client
   device. Zero new on-camera bytes (certs replace the self-signed ones in
   the same `/etc/ssl` paths), fixes every browser-warning problem including
   iOS Safari multi-origin, works with raw-IP URLs (IP SANs — `S02ssl`
   already puts IP SANs in its self-signed certs). We already have per-IP
   build overlays (`user/<profile>/<ip>/overlay`), which is exactly the
   plumbing per-camera cert injection needs. Downside: every viewing device
   must import the CA root once, and *we* are the CA (key hygiene matters).
2. If a real public cert is truly wanted: obtain it **centrally, not
   on-camera** — one DNS-01 wildcard cert (e.g. `*.cam.example.com`) on an
   always-on LAN box (the docker host at 192.168.178.17 qualifies), pushed
   to cameras over SSH + `S60uhttpd restart`. On-camera cost: ~6 KB of cert
   files in the data overlay, no new binaries at all.
3. On-camera ACME client: feasible in principle **only via uacme** (~35 KB,
   reuses libcurl+mbedTLS already on the target) — but see the flash math
   below, and it still requires the user to own a domain + a DNS-01-capable
   DNS provider, so it serves a minority of sophisticated users.

### Feasibility: why LE structurally doesn't fit

- Public CAs **cannot issue for private IPs or mDNS names at all**. No cert
  for `192.168.10.33`, none for `cam-vorne.local`, ever. (LE's 2025 IP-cert
  offering covers *public* IPs only.) Today the fleet is addressed by raw
  RFC1918 IP — a real cert forces a naming migration: every camera needs a
  public DNS record (`cam-vorne.cam.example.com` → A 192.168.10.33 is legal;
  DNS-01 never contacts the host) and every client must switch to browsing
  by that name. That migration, not any binary, is the real adoption cost.
- **HTTP-01 is out**: it needs LE to reach port 80 on the *validated name*,
  i.e. per-camera public exposure (port-forward per camera or a public
  reverse proxy). Nobody should port-forward IP cameras; disqualified.
- **DNS-01 is the only viable challenge**: requires owning a domain and API
  credentials for a supported DNS provider, stored on whatever runs the
  client. Credentials on 10+ cameras is strictly worse than credentials on
  one central box — another argument for option 2.

### Client footprint comparison (MIPS32/uclibc), for the record

What the target already ships (so "new dependency" is measured honestly):
`curl` binary 195 KB + `libcurl.so` 581 KB (mbedTLS-backed,
`BR2_PACKAGE_THINGINO_LIBCURL_CURL=y`), mbedTLS 3.6.6 shared libs
(libmbedcrypto 556 KB / libmbedtls 301 KB / libmbedx509 71 KB),
`ca-certificates.crt` bundle, busybox ash (no bash), **no OpenSSL anywhere**.

| Client | Own size | NEW deps on this target | Realistic added flash |
|---|---|---|---|
| acme.sh | 277 KB single POSIX-sh script (measured, master) + per-provider dnsapi script | **openssl CLI + libcrypto/libssl** — hard requirement (`openssl genrsa/dgst/x509` for JWS); curl/sh already present | ~4 MB uncompressed OpenSSL (~1.5–2 MB in squashfs) + 280 KB script |
| dehydrated | ~90 KB script | **bash** (buildroot Config.in selects it; ~1 MB) **+ openssl CLI/libs** as above | worst of both worlds; strictly dominated by acme.sh here |
| lego | 17.5 MB *gzipped tarball* for linux_mips_softfloat v5.4.0 (binary inside is several times larger) | none (static Go) | not even wrong — see flash math |
| **uacme** | ~35 KB installed (OpenWrt figure); plain C, ~same order for our build | **none** — buildroot's own `package/uacme` builds `--with-mbedtls` when `BR2_PACKAGE_MBEDTLS=y` (true for us) and links libcurl (present) | **~35–60 KB** |
| custom mbedTLS ACME client | est. 1–2 k lines C | none | ~30–50 KB — i.e. uacme is that client, already written and packaged in our buildroot tree |

So the question's hunch is right: on a device without OpenSSL, the "light"
shell clients are the heavy options (their OpenSSL toll is ~30–50x uacme),
and the mbedTLS-native client wins by an order of magnitude. Buildroot
already carries it; enabling would be a Kconfig line plus a renewal hook.

### The constraint that actually decides it: flash

T31L partition map (from the 2026-08-29 build's generated .md):
rootfs partition 4800 KB, rootfs content 4788 KB — **12 KB headroom**.
The whole 8 MB NOR is allocated end-to-end (kernel 1600 KB partition /
1416 KB used is the only other slack). Even uacme's ~40 KB does not fit
without repartitioning or evicting something; OpenSSL or lego are flatly
impossible. A central-renewal design (option 2) or build-time fleet CA
(option 1) sidesteps this entirely — certs live in the 1408 KB jffs2 data
partition, which has room for 6 KB of PEM.

### RAM framing (transient, not resident)

ACME clients are run-and-exit (cron, ~every 60 days actual issuance on LE's
90-day certs; daily no-op check). Peak transient RSS: uacme one process with
already-resident shared libs, ~2–4 MB for a few seconds; acme.sh worse
(ash + repeated curl/openssl forks, ~5–10 MB peak) but equally transient.
This is a fundamentally different cost class from the **~21 KB heap per
connected TLS client, always-on while connected** measured for the motors
WSS listener — renewal RAM is effectively free on a 64 MB box; nobody
should reject ACME on RAM grounds. Flash and deployment model are the real
gates.

### Renewal state & wear

Persistent state: ACME account key ~1–2 KB + cert/chain ~4–6 KB + key ~1 KB
≈ **~10 KB in the data jffs2 partition, rewritten ~6x/year**. Wear is a
non-issue (jffs2 wear-levels a 1408 KB partition; this write rate is
negligible). One cron line + `S60uhttpd restart` (and timpsd/motors reload)
per renewal. Note industry cert lifetimes are shrinking (towards ~47 days
by 2029), which raises renewal frequency but not the conclusion.

---

## Q2: HTTP -> HTTPS redirect

### 1. Is it happening today? **No — the log line is a lie.**

Verified three independent ways:

- `S60uhttpd` `build_uhttpd_args()` passes `-s 443 -C ... -K ... -p 80` and
  never `-q`. The startup message `"HTTP: http://...:80/ (redirects to
  HTTPS)"` (line 146) is aspirational text only.
- Live check against cam 192.168.10.33 port 80: `HTTP/1.1 200 OK` — content
  served in plaintext, no redirect.
- Bonus finding: **`BR2_PACKAGE_THINGINO_UHTTPD_HTTP_REDIRECT` is a dead
  Kconfig symbol.** It exists in `package/thingino-uhttpd/Config.in`
  (default y, set to y in our built .config, even `select`ed by
  thingino-webserver, documented in the package README as working) but is
  referenced by **nothing** — not the .mk, not the init script. Config
  promises it, README claims it, runtime never does it.

### 2. Minimal fix: one flag, zero new code

Upstream OpenWrt uhttpd (our pinned 7b1bec4) has a **native `-q` flag** →
`conf.tls_redirect` → `tls_redirect_check()` in client.c: answers every
plain-HTTP request with **307 + `Location: https://<host>[:port]<path>`**,
preserving Host and URL, before any handler runs. The code is confirmed
compiled into our shipped 59 KB binary (strings show "Temporary Redirect" /
"Location: https://%s%s"). Cost: 0 bytes flash, 0 RAM.

- Zero-build fix, per camera, today: `EXTRA_ARGS="-q"` in
  `/etc/default/uhttpd` (the init script already appends EXTRA_ARGS).
- Proper fix: in `S60uhttpd` `build_uhttpd_args()`, add `-q` inside the
  existing `[ -n "$listen_https" ]` branch — one line — and fix or drop the
  misleading log line. Also either wire up or delete
  `BR2_PACKAGE_THINGINO_UHTTPD_HTTP_REDIRECT` so config stops lying.
- Caveat: `-q` is global to the process, and the portal branch also passes
  an HTTPS listener when certs exist — but captive-portal probes
  (`/generate_204` etc.) are plain HTTP by definition (the script's own
  comment says so). A blanket `-q` in `build_uhttpd_args` would therefore
  break portal detection; the flag must be added only for the normal
  (non-portal) invocation. Still a one-to-three-line change.

### 3. Should we? Honest opinion

With **self-signed certs (today)**: default-on redirect is a UX downgrade —
it force-marches every user into the interstitial cert warning (and into the
iOS Safari multi-origin trust mess documented today) with no fallback, while
the security win is limited because the WebUI login already happens over
whatever the user chose. Keep port 80 usable; make `-q` an easy opt-in
(EXTRA_ARGS already delivers that with zero code).

With a **trusted cert (fleet CA or real cert)**: flip to default-on
immediately — at that point plaintext port 80 is pure downside (credentials
in the clear on the LAN) and the redirect costs nothing. In short: the
redirect decision is downstream of the certificate decision; implement the
one-line plumbing now behind the (currently dead) Kconfig knob, default it
per cert situation.
