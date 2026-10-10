/* test_http_host.c - host-only test for the DNS-rebinding Host policy and
 * the Origin/Host comparison (src/auth.c). Build/run: make test-http-host */
#include <stdio.h>
#include <string.h>
#include "auth.h"
#include "util.h"

static int failures, checks;

#define CK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int h(const char *host, const char *hn, const char *allowed)
{
    char req[512];
    if (host) snprintf(req, sizeof req, "GET /control HTTP/1.1\r\nHost: %s\r\nAccept: */*\r\n\r\n", host);
    else      snprintf(req, sizeof req, "GET /control HTTP/1.0\r\nAccept: */*\r\n\r\n");
    return auth_host_ok(req, hn, allowed);
}

static void t_literals(void)
{
    CK(h(NULL, "garage", ""), "no Host (HTTP/1.0)");
    CK(h("", "garage", ""), "empty Host");
    CK(h("192.168.178.21", "garage", ""), "IPv4");
    CK(h("192.168.178.21:8880", "garage", ""), "IPv4:port");
    CK(h("10.0.0.1.", "garage", ""), "IPv4 trailing dot");
    CK(h("[fe80::1]:8880", "garage", ""), "IPv6 literal");
    CK(h("[::1]", "garage", ""), "IPv6 loopback");
    CK(!h("[fe80::1:8880", "garage", ""), "unterminated IPv6");
    CK(!h("[cam.local]", "garage", ""), "bracketed name");
    CK(!h("192.168.178.21.nip.io", "garage", ""), "rebinding-style name with an IP prefix");
    CK(!h("3232235777", "garage", ""), "numeric non-dotted is a name to inet_pton");
    CK(!h(":8880", "garage", ""), "port only");
    CK(h("localhost:8880", "garage", ""), "localhost");
    CK(h("LOCALHOST", "garage", ""), "localhost case");
}

static void t_names(void)
{
    CK(h("garage", "garage", ""), "hostname");
    CK(h("Garage:8880", "garage", ""), "hostname case + port");
    CK(h("garage.local", "garage", ""), "mDNS");
    CK(h("garage.fritz.box.", "garage", ""), "router DNS, trailing dot");
    CK(!h("garagex.local", "garage", ""), "longer first label");
    CK(!h("gar", "garage", ""), "shorter");
    CK(!h("attacker.example", "garage", ""), "foreign name");
    CK(!h("attacker.example:8880", "garage", ""), "foreign name:port");
    CK(!h("x.garage.local", "garage", ""), "hostname not as first label");
    CK(!h("garage", "", ""), "unknown hostname matches nothing");
    CK(!h("garage", NULL, NULL), "NULL hostname/allowed");
}

static void t_allowed(void)
{
    const char *al = "cam.example.net, .proxy.lan\t other";
    CK(h("cam.example.net", "garage", al), "exact entry");
    CK(h("CAM.example.net:443", "garage", al), "exact entry, case, port");
    CK(!h("x.cam.example.net", "garage", al), "exact entry is not a suffix");
    CK(h("a.proxy.lan", "garage", al), "suffix entry");
    CK(h("proxy.lan", "garage", al), "suffix entry, bare domain");
    CK(!h("xproxy.lan", "garage", al), "suffix needs the dot");
    CK(h("other", "garage", al), "tab/space separated");
    CK(!h("othe", "garage", al), "prefix of an entry");
    CK(!h("attacker.example", "garage", al), "not listed");
    CK(h("attacker.example", "garage", "*"), "* = any");
    CK(h("attacker.example", "garage", "a.b,*"), "* later in the list");
    CK(!h("attacker.example", "garage", ",,  ,"), "only separators");
    CK(!h("lan", "garage", "."), "a lone dot matches nothing");
}

static void t_header_scope(void)
{
    const char *r1 = "GET / HTTP/1.1\r\nX-Host: garage\r\nHost: evil.example\r\n\r\n";
    CK(!auth_host_ok(r1, "garage", ""), "X-Host does not count as Host");
    const char *r2 = "GET /?Host:%20garage HTTP/1.1\r\nhost: evil.example\r\n\r\n";
    CK(!auth_host_ok(r2, "garage", ""), "request line is not a header, lower-case name");
    const char *r3 = "POST /control HTTP/1.1\r\nHost: evil.example\r\n\r\nHost: garage";
    CK(!auth_host_ok(r3, "garage", ""), "a body line is not a header");
}

static void t_origin(void)
{
    CK(auth_origin_ok("GET / HTTP/1.1\r\nHost: 10.0.0.2:8880\r\nOrigin: http://10.0.0.2\r\n\r\n"),
       "WebUI :80 -> :8880 same host");
    CK(!auth_origin_ok("OPTIONS / HTTP/1.1\r\nHost: 10.0.0.2:8880\r\nOrigin: https://attacker.example\r\n\r\n"),
       "public page -> LAN IP");
    CK(!auth_origin_ok("GET / HTTP/1.1\r\nHost: 10.0.0.2\r\nOrigin: null\r\n\r\n"), "null origin");
}

static int qv(const char *path, const char *name, const char *want)
{
    const char *v = ms_qparam(path, name);
    if (!want) return v == NULL;
    if (!v) return 0;
    size_t n = strcspn(v, "&");
    return n == strlen(want) && !strncmp(v, want, n);
}

static void t_qparam(void)
{
    CK(qv("/snapshot.jpg?chn=1", "chn", "1"), "first parameter");
    CK(qv("/control?token=abc&chn=2", "chn", "2"), "after &");
    CK(qv("/control?xtoken=abc", "token", NULL), "not inside another name");
    CK(qv("/snapshot.jpg?foo=chn=9", "chn", NULL), "not inside a value");
    CK(qv("/control?fields=1", "fields", "1"), "flag");
    CK(qv("/control?fields", "fields", NULL), "name without =");
    CK(qv("/chn=1", "chn", NULL), "path is not the query");
    CK(qv("/control?a=1&token=", "token", ""), "empty value");
    CK(qv("/x?a=1&&b=2", "b", "2"), "empty member");
    CK(qv("/webrtc/whep?chn=x&token=t", "token", "t"), "last parameter");
}

int main(void)
{
    t_qparam();
    t_literals(); t_names(); t_allowed(); t_header_scope(); t_origin();
    printf("http host policy: %d/%d passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
