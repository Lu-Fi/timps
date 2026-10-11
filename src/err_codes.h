#ifndef MS_ERR_CODES_H
#define MS_ERR_CODES_H
/* Numeric /control error codes: the POST reply's "errors" object and GET's
 * video.N.rotation_state. A code never changes meaning; a new code bumps
 * ERR_V (caps.err_v) so a client can tell a code it has no text for.
 * Reserved: 3xx FrontCrop, 4xx ROI, 5xx sensor. Table: docs/ai/error-codes.md */
#define ERR_V 1

enum {
    ERR_OK              = 0,
    ERR_VALUE_INVALID   = 101,
    ERR_KEY_UNKNOWN     = 102,
    ERR_KEY_FILE_ONLY   = 103,
    ERR_KEY_NOHW        = 104,
    ERR_ROT_UNSUPPORTED = 201,
    ERR_ROT_MAIN        = 202,
    ERR_ROT_ALIGN       = 203,
    ERR_ROT_SIZE        = 204,
    ERR_ROT_FPS         = 205,
    ERR_ROT_LIB         = 206,
    ERR_ROT_REFUSED     = 207,
    ERR_ROT_SMALL       = 208,
};

#endif
