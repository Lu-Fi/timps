/* Host-only unit test for src/clients.c: add, byte counting, rate, JSON,
 * delete and a full table. */
#include "clients.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail, g_n;

static void ok(int cond, const char *what)
{
    g_n++;
    if (!cond) { g_fail++; printf("FAIL %s\n", what); }
}

int main(void)
{
    char buf[CLIENTS_JSON_CAP];
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(5000);
    inet_pton(AF_INET, "192.168.178.17", &a.sin_addr);

    ok(clients_json(buf, sizeof buf) > 0 && !strcmp(buf, "{\"clients\":[]}"), "empty list");

    int id = clients_add(CLI_RTSP_UDP, &a, 1, "Lavf61.7.100 \"quoted\"");
    ok(id >= 0, "add returns an id");
    clients_bytes(-1, 1000);                         /* no-op, must not crash */
    clients_bytes(id, 125000);                       /* 1 Mbit */
    sleep(1);
    ok(clients_json(buf, sizeof buf) > 0, "json fits");
    ok(strstr(buf, "\"ip\":\"192.168.178.17\"") && strstr(buf, "\"port\":5000") &&
       strstr(buf, "\"proto\":\"rtsp/udp\"") && strstr(buf, "\"chn\":1"), "fields");
    ok(strstr(buf, "\"agent\":\"Lavf61.7.100 \\\"quoted\\\"\"") != NULL, "agent escaped");
    char ua[CLIENTS_AGENT_MAX];
    clients_agent_from("DESCRIBE rtsp://x RTSP/1.0\r\nCSeq: 2\r\nuser-agent:  LibVLC/3.0.20\r\n\r\n", ua, sizeof ua);
    ok(!strcmp(ua, "LibVLC/3.0.20"), "agent parsed case-insensitively");
    clients_agent_from("GET / HTTP/1.1\r\nHost: x\r\n\r\n", ua, sizeof ua);
    ok(ua[0] == 0, "no agent -> empty");
    unsigned kbps = 0;
    const char *k = strstr(buf, "\"kbps\":");
    if (k) sscanf(k + 7, "%u", &kbps);
    ok(kbps >= 800 && kbps <= 1000, "rate ~1 Mbit over ~1 s");
    ok(strstr(buf, "\"bytes\":125000,") != NULL, "total bytes");
    ok(strstr(buf, "\"lat_ms\":-1,") != NULL, "no latency yet");
    clients_latency(id, 80000);
    for (int i = 0; i < 40; i++) clients_latency(id, 120000);
    clients_json(buf, sizeof buf);
    const char *l = strstr(buf, "\"lat_ms\":");
    int lat = l ? atoi(l + 9) : -1;
    ok(lat >= 115 && lat <= 120, "latency averages toward 120 ms");

    for (int i = 0; i < 40; i++) clients_bytes(id, 125000000);   /* 5 GB, past the 32-bit carry */
    clients_json(buf, sizeof buf);
    ok(strstr(buf, "\"bytes\":5000125000,") != NULL, "total survives 4 GB");

    int ids[CLIENTS_MAX];
    int got = 0;
    for (int i = 0; i < CLIENTS_MAX; i++) {
        ids[i] = clients_add(CLI_FMP4, &a, 0, NULL);
        if (ids[i] >= 0) got++;
    }
    ok(got == CLIENTS_MAX - 1, "fills up to CLIENTS_MAX");
    ok(clients_add(CLI_SRT, &a, 0, NULL) == -1, "full table returns -1");
    ok(clients_json(buf, 64) == -1, "too small buffer returns -1");

    for (int i = 0; i < CLIENTS_MAX; i++) clients_del(ids[i]);
    clients_del(id);
    clients_del(-1);
    ok(clients_json(buf, sizeof buf) > 0 && !strcmp(buf, "{\"clients\":[]}"), "empty after delete");

    printf("%d/%d checks passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
