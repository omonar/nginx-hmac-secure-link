
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_string.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>

#define NGX_DEFAULT_HASH_FUNCTION  "sha256"

typedef struct {
    ngx_http_complex_value_t  *hmac_variable;
    ngx_http_complex_value_t  *hmac_message;
    ngx_http_complex_value_t  *hmac_secret;
    ngx_str_t                  hmac_algorithm;
} ngx_http_secure_link_conf_t;


typedef struct {
    ngx_str_t                  expires;
} ngx_http_secure_link_ctx_t;

static ngx_int_t ngx_http_secure_link_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_secure_link_expires_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_secure_link_token_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_secure_link_timestamp_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static void *ngx_http_secure_link_create_conf(ngx_conf_t *cf);
static char *ngx_http_secure_link_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static ngx_int_t ngx_http_secure_link_add_variables(ngx_conf_t *cf);
void ngx_secure_link_encode_base64url(ngx_str_t *dst, ngx_str_t *src);


static ngx_command_t  ngx_http_hmac_secure_link_commands[] = {

    { ngx_string("secure_link"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_secure_link_conf_t, hmac_variable),
      NULL },

    { ngx_string("secure_link_hmac_message"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_secure_link_conf_t, hmac_message),
      NULL },

    { ngx_string("secure_link_hmac_secret"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_http_set_complex_value_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_secure_link_conf_t, hmac_secret),
      NULL },

    { ngx_string("secure_link_hmac_algorithm"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_secure_link_conf_t, hmac_algorithm),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_secure_link_module_ctx = {
    ngx_http_secure_link_add_variables,    /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_secure_link_create_conf,      /* create location configuration */
    ngx_http_secure_link_merge_conf        /* merge location configuration */
};


ngx_module_t  ngx_http_hmac_secure_link_module = {
    NGX_MODULE_V1,
    &ngx_http_secure_link_module_ctx,      /* module context */
    ngx_http_hmac_secure_link_commands,    /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_http_variable_t ngx_http_secure_link_vars[] = {
    { ngx_string("secure_link"), NULL,
      ngx_http_secure_link_variable, 0, NGX_HTTP_VAR_CHANGEABLE, 0 },

    { ngx_string("secure_link_expires"), NULL,
      ngx_http_secure_link_expires_variable, 0, NGX_HTTP_VAR_CHANGEABLE, 0 },

    { ngx_string("secure_link_token"), NULL,
      ngx_http_secure_link_token_variable, 0, NGX_HTTP_VAR_CHANGEABLE, 0 },

    { ngx_string("secure_link_timestamp"), NULL,
      ngx_http_secure_link_timestamp_variable, 0, NGX_HTTP_VAR_CHANGEABLE, 0 },

    { ngx_null_string, NULL, NULL, 0, 0, 0}
};


static ngx_int_t
ngx_http_secure_link_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    u_char                       *p, *last;
    ngx_str_t                     value, token, hash, key, timestamp, expires;
    time_t                        timestamp_t, expires_t, gmtoff;
    ngx_http_secure_link_ctx_t   *ctx;
    ngx_http_secure_link_conf_t  *conf;
    u_char                        hash_buf[EVP_MAX_MD_SIZE], hmac_buf[EVP_MAX_MD_SIZE], temp[NGX_INT64_LEN];
    const EVP_MD                 *evp_md;
    u_int                         hmac_len;
    ngx_int_t                     year, month, mday, hour, min, sec, gmtoff_hour, gmtoff_min, nid, index;
    char                          gmtoff_sign;
    uint64_t                      time;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_hmac_secure_link_module);

    if (conf->hmac_variable == NULL || conf->hmac_message == NULL || conf->hmac_secret == NULL) {
        goto not_found;
    }

    nid = OBJ_txt2nid((const char*) conf->hmac_algorithm.data);

    evp_md = EVP_get_digestbynid(nid);
    if (evp_md == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "Unknown cryptographic hash function \"%s\"", conf->hmac_algorithm.data);

        return NGX_ERROR;
    }

    if (ngx_http_complex_value(r, conf->hmac_variable, &value) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "secure link variable: \"%V\"", &value);

    timestamp_t = 0;
    expires_t = 0;

    token.data = value.data;
    token.len = value.len;

    timestamp.data = NULL;
    timestamp.len = 0;

    expires.data = NULL;
    expires.len = 0;

    last = value.data + value.len;

    p = ngx_strlchr(value.data, last, ',');
    if (p) {
        /* Secure token */
        token.len = p++ - value.data;

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "secure link token: \"%V\"", &token);

        /* Timestamp */
        timestamp.data = p;
        timestamp.len = last - timestamp.data;

        /* Expiration period */
        p = ngx_strlchr(p, last, ',');
        if (p) {
            timestamp.len = p++ - timestamp.data;

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "secure link timestamp: \"%V\"", &timestamp);
 
            expires.data = p;
            expires.len = last - expires.data;

            ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                           "secure link expires: \"%V\"", &expires);
        }

        /* Parse timestamp in ISO8601 format */
        if (sscanf((char *)timestamp.data, "%d-%d-%dT%d:%d:%d%c%d:%d",
                               &year, &month, &mday, &hour, &min, &sec,
                               &gmtoff_sign, &gmtoff_hour, &gmtoff_min) == 9) {

            /* Put February last because it has leap day */
            month -= 2;
            if (month <= 0) {
                month += 12;
                year -= 1;
            }

            /* Gauss' formula for Gregorian days since March 1, 1 BC */
            /* Taken from ngx_http_parse_time.c */
            timestamp_t = (time_t) (
                     /* days in years including leap years since March 1, 1 BC */
                     365 * year + year / 4 - year / 100 + year / 400
                     /* days before the month */
                     + 367 * month / 12 - 30
                     /* days before the day */
                     + mday - 1
                     /*
                      * 719527 days were between March 1, 1 BC and March 1, 1970,
                      * 31 and 28 days were in January and February 1970
                      */
                     - 719527 + 31 + 28) * 86400 + hour * 3600 + min * 60 + sec;

            /* Determine the time offset with respect to GMT */
            gmtoff = 3600 * gmtoff_hour + 60 * gmtoff_min;

            if (gmtoff_sign == '+') {
                timestamp_t -= gmtoff;
            }

            if (gmtoff_sign == '-') {
                timestamp_t += gmtoff;
            }
        }
        else {
            /* Parse timestamp in base64 encoded Unix epoch */
            value.data = temp;

            if (ngx_decode_base64url(&value, &timestamp) != NGX_OK) {
                goto not_found;
            }

            time = 0;

            for(index=value.len-1; index>=0; index--) {
                time = time * 256 + value.data[index];
            }

            /* Drop microseconds */
            timestamp_t = time / 1e6;
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "secure link timestamp: \"%T\"", timestamp_t);

        if (timestamp_t <= 0) {
            goto not_found;
        }

        /* Parse expiration period in seconds */
        if (expires.len > 0) {
            expires_t = ngx_atotm(expires.data, expires.len);
        }

        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "secure link expires: \"%T\"", expires_t);

        if (expires_t < 0) {
            goto not_found;
        }

        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_secure_link_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_hmac_secure_link_module);

        ctx->expires.len = expires.len;
        ctx->expires.data = expires.data;
    }

    hash.data = hash_buf;
    hash.len  = (u_int) EVP_MD_size(evp_md);

    if (token.len > ngx_base64_encoded_length(hash.len)+2) {
        goto not_found;
    }

    if (ngx_decode_base64url(&hash, &token) != NGX_OK) {
        goto not_found;
    }

    if (hash.len != (u_int) EVP_MD_size(evp_md)) {
        goto not_found;
    }

    if (ngx_http_complex_value(r, conf->hmac_message, &value) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "secure link message: \"%V\"", &value);

    if (ngx_http_complex_value(r, conf->hmac_secret, &key) != NGX_OK) {
        return NGX_ERROR;
    }

    HMAC(evp_md, key.data, key.len, value.data, value.len, hmac_buf, &hmac_len);

    if (CRYPTO_memcmp(hash_buf, hmac_buf, EVP_MD_size(evp_md)) != 0) {
        goto not_found;
    }

    v->data = (u_char *) ((expires_t && timestamp_t + expires_t < ngx_time()) ? "0" : "1");
    v->len = 1;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;

not_found:

    v->not_found = 1;

    return NGX_OK;
}

static ngx_int_t
ngx_http_secure_link_token_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_str_t                     value, key, hmac, token;
    u_int                         hmac_len;
    ngx_http_secure_link_conf_t  *conf;
    u_char                        hmac_buf[EVP_MAX_MD_SIZE];
    const EVP_MD                 *evp_md;
    u_char                       *p;
    int                           nid;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_hmac_secure_link_module);

    if (conf->hmac_message == NULL || conf->hmac_secret == NULL) {
        goto not_found;
    }

    p = ngx_pnalloc(r->pool, ngx_base64_encoded_length(EVP_MAX_MD_SIZE));
    if (p == NULL) {
        return NGX_ERROR;
    }

    if (ngx_http_complex_value(r, conf->hmac_message, &value) != NGX_OK) {
        return NGX_ERROR;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "secure link string to sign: \"%V\"", &value);

    if (ngx_http_complex_value(r, conf->hmac_secret, &key) != NGX_OK) {
        return NGX_ERROR;
    }

    nid = OBJ_txt2nid((const char*) conf->hmac_algorithm.data);

    evp_md = EVP_get_digestbynid(nid);
    if (evp_md == NULL) {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "Unknown cryptographic hash function \"%s\"", conf->hmac_algorithm.data);

        return NGX_ERROR;
    }

    HMAC(evp_md, key.data, key.len, value.data, value.len, hmac_buf, &hmac_len);

    hmac.data = hmac_buf;
    hmac.len = hmac_len;

    token.data = p;

    ngx_secure_link_encode_base64url(&token, &hmac);

    v->data = token.data;
    v->len = token.len;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;

not_found:

    v->not_found = 1;

    return NGX_OK;
}

static ngx_int_t
ngx_http_secure_link_expires_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    ngx_http_secure_link_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_hmac_secure_link_module);

    if (ctx) {
        v->len = ctx->expires.len;
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
        v->data = ctx->expires.data;

    } else {
        v->not_found = 1;
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_secure_link_timestamp_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data)
{
    u_char         *p, temp[NGX_INT64_LEN];
    uint64_t        time;
    ngx_uint_t      index;
    ngx_str_t       value, timestamp;
    struct timeval  tv;

    p = ngx_pnalloc(r->pool, ngx_base64_encoded_length(sizeof(uint64_t)));
    if (p == NULL) {
        return NGX_ERROR;
    }

    ngx_gettimeofday(&tv);

    time = (uint64_t) tv.tv_sec * 1e6 + tv.tv_usec;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "secure link timestamp: \"%uL\"", time);

    /* Timestamp in little-endian format */
    index = 0;
    while (time != 0) {
         temp[index] = time % 256;
         time = time / 256;
         ++index;
    }

    value.data = temp;
    value.len = index;

    timestamp.data = p;

    ngx_secure_link_encode_base64url(&timestamp, &value);

    v->data = timestamp.data;
    v->len = timestamp.len;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}

static void *
ngx_http_secure_link_create_conf(ngx_conf_t *cf)
{
    ngx_http_secure_link_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_secure_link_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->hmac_variable = NULL;
     *     conf->hmac_message = NULL;
     *     conf->hmac_secret = NULL;
     *     conf->hmac_algorithm = {0,NULL};
     */

    return conf;
}


static char *
ngx_http_secure_link_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_secure_link_conf_t *prev = parent;
    ngx_http_secure_link_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->hmac_algorithm, prev->hmac_algorithm, NGX_DEFAULT_HASH_FUNCTION);

    if (conf->hmac_variable == NULL) {
        conf->hmac_variable = prev->hmac_variable;
    }

    if (conf->hmac_message == NULL) {
        conf->hmac_message = prev->hmac_message;
    }

    if (conf->hmac_secret == NULL) {
        conf->hmac_secret = prev->hmac_secret;
    }

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_secure_link_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_secure_link_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}

/* A copy of ngx_encode_base64url from ngx_string.c included in Nginx version 1.5.x */
void
ngx_secure_link_encode_base64url(ngx_str_t *dst, ngx_str_t *src)
{
    static u_char   basis64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    u_char         *d, *s;
    size_t          len;

    len = src->len;
    s = src->data;
    d = dst->data;

    while (len > 2) {
        *d++ = basis64[(s[0] >> 2) & 0x3f];
        *d++ = basis64[((s[0] & 3) << 4) | (s[1] >> 4)];
        *d++ = basis64[((s[1] & 0x0f) << 2) | (s[2] >> 6)];
        *d++ = basis64[s[2] & 0x3f];

        s += 3;
        len -= 3;
    }

    if (len) {
        *d++ = basis64[(s[0] >> 2) & 0x3f];

        if (len == 1) {
            *d++ = basis64[(s[0] & 3) << 4];
        } else {
            *d++ = basis64[((s[0] & 3) << 4) | (s[1] >> 4)];
            *d++ = basis64[(s[1] & 0x0f) << 2];
        }
    }

    dst->len = d - dst->data;
}
