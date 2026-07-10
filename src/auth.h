/* auth.h - HTTP Basic + RTSP Digest authentication (no OpenSSL) */
#ifndef MS_AUTH_H
#define MS_AUTH_H

#define AUTH_REALM "timps"

/* HTTP Basic: 'value' is the text after "Authorization: ". Returns 1 if valid. */
int  auth_http_basic(const char *value, const char *user, const char *pass);

/* RTSP Digest: validate an "Authorization: Digest ..." value against method+creds.
 * Returns 1 if valid. */
int  auth_rtsp_digest(const char *method, const char *value,
                      const char *user, const char *pass);

/* generate a fresh opaque nonce (32 hex chars + NUL) */
void auth_make_nonce(char out[33]);

#endif
