#include "auth.h"
#include "md5.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <strings.h>

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
                     const char *user, const char *pass)
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

    char buf[512], ha1[33], ha2[33], expect[33];
    snprintf(buf,sizeof buf,"%s:%s:%s",user,realm,pass);        md5_hex(buf,ha1);
    snprintf(buf,sizeof buf,"%s:%s",method,uri);                md5_hex(buf,ha2);
    snprintf(buf,sizeof buf,"%s:%s:%s",ha1,nonce,ha2);          md5_hex(buf,expect);
    return strcasecmp(resp,expect)==0;
}

void auth_make_nonce(char out[33])
{
    static unsigned long ctr=0;
    char seed[64];
    snprintf(seed,sizeof seed,"%ld-%d-%lu",(long)time(NULL),rand(),ctr++);
    md5_hex(seed,out);
}
