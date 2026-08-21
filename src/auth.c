#include "auth.h"
#include "md5.h"
#include "util.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <pthread.h>

int auth_http_basic(const char *value, const char *user, const char *pass)
{
    if (!value) return 0;
    while (*value==' ') value++;
    if (strncasecmp(value,"Basic ",6)!=0) return 0;
    const char *tok=value+6; while(*tok==' ')tok++;
    char creds[160]; snprintf(creds,sizeof creds,"%s:%s",user,pass);
    char expect[256]; ms_base64(expect,(const uint8_t*)creds,(int)strlen(creds));
    /* compare up to whitespace/end of provided token, constant-time (no
     * early exit on a CONTENT mismatch, like auth_token_eq below) so a
     * timing side-channel cannot be used to recover the expected base64
     * blob byte-by-byte. el/tl are both attacker-observable lengths, not
     * secret, so branching on the length compare itself is fine - and it
     * keeps the loop below (and the tok[el] access after it) in-bounds: it
     * only runs, and only reads tok[el], when tok is at least el bytes. */
    size_t el=strlen(expect);
    size_t tl=strlen(tok);
    size_t n=(tl<el)?tl:el;
    unsigned char diff=(tl<el)?1:0;
    for (size_t i=0;i<n;i++)
        diff |= (unsigned char)tok[i] ^ (unsigned char)expect[i];
    if (diff) return 0;
    char t=tok[el];
    return (t==0||t=='\r'||t=='\n'||t==' ');
}

/* extract key="value" (or key=value) from a digest header into out.
 * The key must start at a parameter boundary (start of string or after
 * space/comma/tab) and be followed by '=': plain strstr would let "nonce"
 * match inside "cnonce=..." when a client orders cnonce first (parameter
 * order is not fixed by the RFC), silently hashing the wrong value. */
static int field(const char *hdr, const char *key, char *out, int outsz)
{
    size_t kl = strlen(key);
    const char *p = hdr;
    for (;;) {
        p = strstr(p, key);
        if (!p) return 0;
        if ((p==hdr || p[-1]==' ' || p[-1]==',' || p[-1]=='\t') && p[kl]=='=')
            break;
        p += kl;
    }
    p += kl + 1;
    if (*p=='"'){ p++; const char *e=strchr(p,'"'); if(!e)return 0;
        int n=(int)(e-p); if(n>=outsz)n=outsz-1; memcpy(out,p,n); out[n]=0; return 1; }
    const char *e=p; while(*e&&*e!=','&&*e!=' '&&*e!='\r'&&*e!='\n')e++;
    int n=(int)(e-p); if(n>=outsz)n=outsz-1; memcpy(out,p,n); out[n]=0; return 1;
}

int auth_rtsp_digest(const char *method, const char *req_uri, const char *value,
                     const char *user, const char *pass,
                     const char *server_nonce)
{
    if (!value) return 0;
    while (*value==' ') value++;
    if (strncasecmp(value,"Digest ",7)!=0) return 0;
    const char *d=value+7;
    char u[64],realm[64],nonce[64],uri[256],resp[64];
    if (!field(d,"username",u,sizeof u)) return 0;
    if (!field(d,"realm",realm,sizeof realm)) return 0;
    if (!field(d,"nonce",nonce,sizeof nonce)) return 0;
    if (!field(d,"uri",uri,sizeof uri)) return 0;
    if (!field(d,"response",resp,sizeof resp)) return 0;
    if (strcmp(u,user)!=0) return 0;
    /* the realm is a hash input (HA1), so it is the server's value to state,
     * not the client's to choose. This is not a bypass while HA1 is derived
     * from the cleartext password per request - a forged realm changes both
     * sides of the comparison alike, so the client still needs the password -
     * but a client echoing a realm we never issued has no business
     * authenticating, and every legitimate client returns exactly what the
     * 401 offered. realm is public request data, so plain strcmp. */
    if (strcmp(realm, AUTH_REALM)!=0) return 0;
    /* the client's digest "uri" must be the URI actually requested (the
     * request-line target), else HA2 is computed over an attacker-chosen
     * string and a response sniffed for one URI replays against another.
     * The uri is public request data, so a plain strcmp is fine timing-wise.
     * NULL req_uri disables the check (kept for API flexibility only). */
    if (req_uri && strcmp(uri, req_uri)!=0) return 0;
    /* the client's nonce must be one THIS server actually issued (via a
     * prior 401 on this session) - otherwise the digest response is fully
     * reproducible offline from a single sniffed Authorization header and
     * replayable forever against any connection, defeating the one thing
     * digest auth buys over Basic. An empty server_nonce (none issued yet
     * this session) never matches, so a forged first-request Authorization
     * header is rejected too. */
    if (!server_nonce || !server_nonce[0] || strcmp(nonce,server_nonce)!=0) return 0;

    char buf[512], ha1[33], ha2[33], expect[33];
    snprintf(buf,sizeof buf,"%s:%s:%s",user,realm,pass);        md5_hex(buf,ha1);
    snprintf(buf,sizeof buf,"%s:%s",method,uri);                md5_hex(buf,ha2);
    snprintf(buf,sizeof buf,"%s:%s:%s",ha1,nonce,ha2);          md5_hex(buf,expect);
    /* both sides are hex-digest strings (case-insensitive per RFC 2617), so
     * lowercase-normalize into fixed buffers and use the same constant-time
     * equal-length compare as auth_token_eq instead of strcasecmp, which
     * short-circuits on the first mismatching byte and leaks timing info
     * about how many leading hex digits of the secret-derived digest the
     * client guessed correctly. */
    char rlow[64], elow[64]; size_t i;
    for (i=0; resp[i] && i+1<sizeof rlow; i++) rlow[i]=(char)tolower((unsigned char)resp[i]);
    rlow[i]=0;
    for (i=0; expect[i] && i+1<sizeof elow; i++) elow[i]=(char)tolower((unsigned char)expect[i]);
    elow[i]=0;
    return auth_token_eq(rlow, elow);
}

int auth_http_digest(const char *method, const char *req_uri, const char *value,
                     const char *user, const char *pass,
                     char *nonce_out, int nonce_cap,
                     char *nc_out, int nc_cap)
{
    if (nonce_cap > 0) nonce_out[0] = 0;
    if (nc_cap > 0) nc_out[0] = 0;
    if (!value) return 0;
    while (*value==' ') value++;
    if (strncasecmp(value,"Digest ",7)!=0) return 0;
    const char *d=value+7;
    char u[64],realm[64],nonce[64],uri[256],resp[64],qop[16],nc[16],cnonce[128];
    if (!field(d,"username",u,sizeof u)) return 0;
    if (!field(d,"realm",realm,sizeof realm)) return 0;
    if (!field(d,"nonce",nonce,sizeof nonce)) return 0;
    if (!field(d,"uri",uri,sizeof uri)) return 0;
    if (!field(d,"response",resp,sizeof resp)) return 0;
    /* the client's digest "uri" must be the request-target actually being
     * authorized (the HTTP request line's Request-URI), else HA2 is computed
     * over an attacker-chosen string and a response sniffed for one URI (e.g.
     * /snapshot.jpg) replays against another (e.g. /control) inside the nonce
     * window. The uri is public request data, so a plain strcmp is fine
     * timing-wise. NULL req_uri disables the check (API flexibility only). */
    if (req_uri && strcmp(uri, req_uri)!=0) return 0;
    int has_qop = field(d,"qop",qop,sizeof qop);
    if (has_qop) {
        /* we only ever offer qop="auth" (never auth-int); nc + cnonce are
         * mandatory companions of qop per RFC 7616 3.4 */
        if (strcasecmp(qop,"auth")!=0) return 0;
        if (!field(d,"nc",nc,sizeof nc)) return 0;
        if (!field(d,"cnonce",cnonce,sizeof cnonce)) return 0;
    }
    if (strcmp(u,user)!=0) return 0;
    /* bind the realm to the one we advertise, as in auth_rtsp_digest above */
    if (strcmp(realm, AUTH_REALM)!=0) return 0;

    char buf[768], ha1[33], ha2[33], expect[33];
    snprintf(buf,sizeof buf,"%s:%s:%s",user,realm,pass);        md5_hex(buf,ha1);
    snprintf(buf,sizeof buf,"%s:%s",method,uri);                md5_hex(buf,ha2);
    if (has_qop)
        snprintf(buf,sizeof buf,"%s:%s:%s:%s:auth:%s",ha1,nonce,nc,cnonce,ha2);
    else                                            /* legacy RFC 2069 form */
        snprintf(buf,sizeof buf,"%s:%s:%s",ha1,nonce,ha2);
    md5_hex(buf,expect);
    /* same constant-time hex-digest comparison as auth_rtsp_digest above
     * (no strcasecmp early-exit timing leak on the secret-derived digest) */
    char rlow[64], elow[64]; size_t i;
    for (i=0; resp[i] && i+1<sizeof rlow; i++) rlow[i]=(char)tolower((unsigned char)resp[i]);
    rlow[i]=0;
    for (i=0; expect[i] && i+1<sizeof elow; i++) elow[i]=(char)tolower((unsigned char)expect[i]);
    elow[i]=0;
    if (!auth_token_eq(rlow, elow)) return 0;
    /* hand the client-supplied nonce (+nc when qop) to the caller, which
     * must still verify the nonce is one IT recently issued - without that
     * a sniffed Authorization header verifies forever (see auth_rtsp_digest) */
    snprintf(nonce_out,(size_t)nonce_cap,"%s",nonce);
    if (has_qop) snprintf(nc_out,(size_t)nc_cap,"%s",nc);
    return 1;
}

void auth_make_nonce(char out[33])
{
    /* was time(NULL)+rand()+counter - rand() is seeded from time^pid
     * (main.c), so at boot the nonce is only as unpredictable as an
     * attacker's uncertainty about the exact boot time, making it
     * brute-forceable. Reuse auth_gen_token()'s /dev/urandom-backed
     * generator (falls back to the same weak time/pid mix only if
     * /dev/urandom is unavailable). */
    auth_gen_token(out);
}

/* per-boot /control token (see auth.h); "" until main() generates it */
char g_ctl_token[33] = "";

int auth_token_eq(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char d = 0;
    for (size_t i = 0; i < la && i < lb; i++)
        d |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return la == lb && d == 0;
}

void auth_gen_token(char out[33])
{
    uint8_t rnd[16];
    int got = 0, fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        int off = 0;
        while (off < (int)sizeof rnd) {
            ssize_t r = read(fd, rnd + off, sizeof rnd - off);
            if (r <= 0) break;
            off += (int)r;
        }
        close(fd);
        got = (off == (int)sizeof rnd);
    }
    if (got) {
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < 16; i++) {
            out[2*i]   = hx[rnd[i] >> 4];
            out[2*i+1] = hx[rnd[i] & 15];
        }
        out[32] = 0;
        return;
    }
    /* last resort (no /dev/urandom): hash a time/pid mix - far weaker,
     * but never leaves the token empty/predictably constant */
    char seed[96];
    snprintf(seed, sizeof seed, "%ld-%ld-%d-%ld",
             (long)time(NULL), (long)getpid(), rand(), (long)clock());
    md5_hex(seed, out);
}

/* Brute-force visibility (auth.h): silent 401s hide a credential sweep
 * against a root daemon, while a scanner must not be able to flood the
 * 64 KB syslog ring. Failures accumulate across RTSP+HTTP; report from the
 * 3rd one on, at most one line per minute, and forget stray typos after
 * 10 min of quiet (so a slow sweep still reports, two typos never do). */
#define AUTH_FAIL_MIN     3
#define AUTH_FAIL_GAP_S  60
#define AUTH_FAIL_IDLE_S 600
void auth_fail_note(const char *mod, const char *ifc, const char *peer)
{
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    static time_t t_report, t_fail;
    static unsigned n;
    pthread_mutex_lock(&mtx);
    time_t now = time(NULL);
    if (t_fail && now - t_fail > AUTH_FAIL_IDLE_S) n = 0;
    t_fail = now;
    n++;
    if (n >= AUTH_FAIL_MIN && (!t_report || now - t_report >= AUTH_FAIL_GAP_S)) {
        log_printf(LOG_WARN, mod,
                   "%u failed login attempts on %s since the last report "
                   "(last from %s)", n, ifc, peer);
        t_report = now;
        n = 0;
    }
    pthread_mutex_unlock(&mtx);
}
