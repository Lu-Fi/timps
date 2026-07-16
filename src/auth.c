#include "auth.h"
#include "md5.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>

int auth_http_basic(const char *value, const char *user, const char *pass)
{
    if (!value) return 0;
    while (*value==' ') value++;
    if (strncasecmp(value,"Basic ",6)!=0) return 0;
    const char *tok=value+6; while(*tok==' ')tok++;
    char creds[160]; snprintf(creds,sizeof creds,"%s:%s",user,pass);
    char expect[256]; ms_base64(expect,(const uint8_t*)creds,(int)strlen(creds));
    /* compare up to whitespace/end of provided token */
    size_t el=strlen(expect);
    if (strncmp(tok,expect,el)!=0) return 0;
    char t=tok[el];
    return (t==0||t=='\r'||t=='\n'||t==' ');
}

/* extract key="value" (or key=value) from a digest header into out */
static int field(const char *hdr, const char *key, char *out, int outsz)
{
    char pat[32]; snprintf(pat,sizeof pat,"%s=",key);
    const char *p=strstr(hdr,pat);
    if (!p) return 0;
    p+=strlen(pat);
    if (*p=='"'){ p++; const char *e=strchr(p,'"'); if(!e)return 0;
        int n=(int)(e-p); if(n>=outsz)n=outsz-1; memcpy(out,p,n); out[n]=0; return 1; }
    const char *e=p; while(*e&&*e!=','&&*e!=' '&&*e!='\r'&&*e!='\n')e++;
    int n=(int)(e-p); if(n>=outsz)n=outsz-1; memcpy(out,p,n); out[n]=0; return 1;
}

int auth_rtsp_digest(const char *method, const char *value,
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
    return strcasecmp(resp,expect)==0;
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
