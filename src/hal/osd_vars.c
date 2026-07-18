#include "osd_vars.h"
#include "../hub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static double g_fps = 0.0;
void osd_vars_set_fps(double fps){ g_fps = fps; }

static int64_t mono_us(void);   /* defined below get_uptime(); used by the
                                  * ~1s-TTL caches added to the /proc /sys
                                  * readers in this file (L6) */

/* first non-loopback interface name + IPv4 into ip[] (>=INET_ADDRSTRLEN) */
static int primary_iface(char *ifname, int ifnsz, char *ip, int ipsz)
{
    struct ifaddrs *ifa, *p; int ok=0;
    if (getifaddrs(&ifa)!=0) return -1;
    for (p=ifa; p; p=p->ifa_next){
        if (!p->ifa_addr || p->ifa_addr->sa_family!=AF_INET) continue;
        if (p->ifa_flags & 0x8 /*IFF_LOOPBACK*/) continue;
        struct sockaddr_in *sin=(struct sockaddr_in*)p->ifa_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip, ipsz);
        snprintf(ifname, ifnsz, "%s", p->ifa_name);
        ok=1; break;
    }
    freeifaddrs(ifa);
    return ok?0:-1;
}

/* MAC doesn't change at runtime, but keep the same ~1s-TTL cache pattern as
 * the other /proc//sys readers below rather than special-casing it. */
static void get_mac(const char *ifname, char *out, int outsz)
{
    static char cached[32]="00:00:00:00:00:00"; static int64_t last_us=0;
    int64_t now=mono_us();
    if (last_us==0 || now-last_us >= 1000000){
        char path[128];
        snprintf(path,sizeof path,"/sys/class/net/%s/address", ifname);
        FILE *f=fopen(path,"r");
        if (f){
            char buf[32];
            if (fgets(buf,sizeof buf,f)){
                char *nl=strchr(buf,'\n'); if(nl)*nl=0;
                snprintf(cached,sizeof cached,"%s",buf);
            }
            fclose(f);
        }
        last_us=now;
    }
    snprintf(out,outsz,"%s",cached);
}

/* resolve() used to fopen() these /proc files fresh for every placeholder in
 * every OSD text item, every ~1s tick (osd_thread) - across multiple items/
 * streams that's several redundant fopen+read+fclose per second for a value
 * that's already refreshed at most once a second. Cache like get_net_tx. */
static void get_uptime(char *out, int outsz)
{
    static char cached[24]="0:00:00"; static int64_t last_us=0;
    int64_t now=mono_us();
    if (last_us==0 || now-last_us >= 1000000){
        FILE *f=fopen("/proc/uptime","r");
        double up=0;
        if (f){ if(fscanf(f,"%lf",&up)!=1) up=0; fclose(f); }
        int s=(int)up;
        snprintf(cached,sizeof cached,"%d:%02d:%02d", s/86400, (s%86400)/3600, (s%3600)/60); /* d:hh:mm */
        last_us=now;
    }
    snprintf(out,outsz,"%s",cached);
}

static int64_t mono_us(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (int64_t)ts.tv_sec*1000000 + ts.tv_nsec/1000;
}

/* network TX throughput on the primary interface, auto kbit/s <-> Mbit/s.
 * Re-samples at most ~once/second and caches the formatted string, so being
 * called several times per OSD refresh (multiple streams) stays accurate. */
static void get_net_tx(const char *ifname, char *out, int outsz)
{
    static unsigned long long last_tx=0; static int64_t last_us=0;
    static char cached[24]="0 kbit/s";
    int64_t now=mono_us();
    if (last_us==0 || now-last_us >= 900000){
        char path[128]; snprintf(path,sizeof path,"/sys/class/net/%s/statistics/tx_bytes",ifname);
        unsigned long long tx=0; FILE *f=fopen(path,"r");
        if (f){ if(fscanf(f,"%llu",&tx)!=1) tx=last_tx; fclose(f); }
        if (last_us){
            double dt=(now-last_us)/1000000.0;
            double kbps = dt>0 ? (double)(tx-last_tx)*8.0/1000.0/dt : 0.0;
            if (kbps >= 1000.0) snprintf(cached,sizeof cached,"%.1f Mbit/s", kbps/1000.0);
            else                snprintf(cached,sizeof cached,"%.0f kbit/s", kbps);
        }
        last_tx=tx; last_us=now;
    }
    snprintf(out,outsz,"%s",cached);
}

static void get_cpu(char *out, int outsz)   /* 1-min load average */
{
    static char cached[16]="0.00"; static int64_t last_us=0;
    int64_t now=mono_us();
    if (last_us==0 || now-last_us >= 1000000){
        FILE *f=fopen("/proc/loadavg","r"); double la=0;
        if (f){ if(fscanf(f,"%lf",&la)!=1) la=0; fclose(f); }
        snprintf(cached,sizeof cached,"%.2f",la);
        last_us=now;
    }
    snprintf(out,outsz,"%s",cached);
}

static void get_mem(char *out, int outsz)   /* free RAM in MB */
{
    static char cached[16]="?"; static int64_t last_us=0;
    int64_t now=mono_us();
    if (last_us==0 || now-last_us >= 1000000){
        FILE *f=fopen("/proc/meminfo","r");
        if (f){
            char line[128]; long avail=-1, freek=-1;
            while (fgets(line,sizeof line,f)){
                if (sscanf(line,"MemAvailable: %ld kB",&avail)==1) break;
                sscanf(line,"MemFree: %ld kB",&freek);
            }
            fclose(f);
            long kb = avail>=0 ? avail : (freek>=0?freek:0);
            snprintf(cached,sizeof cached,"%ldMB", kb/1024);
        }
        last_us=now;
    }
    snprintf(out,outsz,"%s",cached);
}


/* look up 'name' in a key=value file; returns 1 if found */
static int lookup_file(const char *file, const char *name, char *out, int outsz)
{
    if (!file || !file[0]) return 0;
    FILE *f=fopen(file,"r"); if(!f) return 0;
    char line[256]; int found=0; size_t nl=strlen(name);
    while (fgets(line,sizeof line,f)){
        char *s=line; while(*s==' '||*s=='\t')s++;
        if (strncmp(s,name,nl)==0){
            char *p=s+nl; while(*p==' '||*p=='\t')p++;
            if (*p=='='){ p++; while(*p==' '||*p=='\t')p++;
                char *e=p+strlen(p); while(e>p&&(e[-1]=='\n'||e[-1]=='\r'||e[-1]==' '))*--e=0;
                snprintf(out,outsz,"%s",p); found=1; break; }
        }
    }
    fclose(f);
    return found;
}

static void resolve(const char *name, const char *vars_file, char *out, int outsz)
{
    static char ifname[32], ip[64]; static int cached=0;
    if (!cached){ if(primary_iface(ifname,sizeof ifname,ip,sizeof ip)!=0){ strcpy(ifname,"eth0"); strcpy(ip,"0.0.0.0"); } cached=1; }

    if      (!strcmp(name,"hostname")){ char h[128]; if(gethostname(h,sizeof h)!=0)strcpy(h,"camera"); snprintf(out,outsz,"%s",h); }
    else if (!strcmp(name,"ip"))       snprintf(out,outsz,"%s",ip);
    else if (!strcmp(name,"mac"))      get_mac(ifname,out,outsz);
    else if (!strcmp(name,"fps"))      snprintf(out,outsz,"%.1f",g_fps);
    else if (!strcmp(name,"uptime"))   get_uptime(out,outsz);
    else if (!strcmp(name,"net")||!strcmp(name,"tx"))  get_net_tx(ifname,out,outsz);
    else if (!strcmp(name,"cpu"))      get_cpu(out,outsz);
    else if (!strcmp(name,"mem"))      get_mem(out,outsz);
    else if (!strcmp(name,"clients"))  snprintf(out,outsz,"%d",hub_video_subs());
    else if (!lookup_file(vars_file,name,out,outsz)) out[0]=0; /* unknown -> empty */
}

int osd_expand(const char *tmpl, const char *vars_file, char *out, int outsz)
{
    char stage1[512]; int o=0;
    for (const char *p=tmpl; *p && o<(int)sizeof(stage1)-1; ){
        if (*p=='{'){
            const char *e=strchr(p,'}');
            if (e){
                char name[64]; int n=(int)(e-p-1);
                if (n>0 && n<(int)sizeof name){
                    memcpy(name,p+1,n); name[n]=0;
                    char val[128]; resolve(name,vars_file,val,sizeof val);
                    for (const char *q=val; *q && o<(int)sizeof(stage1)-1; ) stage1[o++]=*q++;
                }
                p=e+1; continue;
            }
        }
        stage1[o++]=*p++;
    }
    stage1[o]=0;
    /* stage 2: strftime for time placeholders. L13: the template is partly
     * user-controlled (osd text via /control), so whitelist the '%'
     * conversions we intend to support and neutralize everything else into a
     * literal '%<char>' (no memory issue either way, but unvetted
     * conversions give locale/implementation-dependent surprises). */
    static const char strf_ok[] = "aAbBcCdDeFgGhHIjklmMnpPrRsSTuUVwWxXyYzZ%";
    char stage2[512]; int o2=0;
    for (const char *p=stage1; *p && o2<(int)sizeof(stage2)-3; p++){
        if (*p=='%'){
            if (p[1] && strchr(strf_ok, p[1])){
                stage2[o2++]='%'; stage2[o2++]=*++p;
            } else {
                stage2[o2++]='%'; stage2[o2++]='%';   /* literal '%' */
            }
        } else stage2[o2++]=*p;
    }
    stage2[o2]=0;
    time_t t=time(NULL); struct tm tm; localtime_r(&t,&tm);
    if (strftime(out,outsz,stage2,&tm)==0 && stage2[0]) snprintf(out,outsz,"%s",stage2);
    return 0;
}
