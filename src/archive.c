#include "prm.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "miniz.h"
#include "xz.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <ftw.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct ZSTD_DCtx_s ZSTD_DCtx;
typedef struct { const void *src; size_t size; size_t pos; } ZSTD_inBuffer;
typedef struct { void *dst; size_t size; size_t pos; } ZSTD_outBuffer;
extern ZSTD_DCtx *ZSTD_createDCtx(void);
extern size_t ZSTD_freeDCtx(ZSTD_DCtx *dctx);
extern size_t ZSTD_decompressStream(ZSTD_DCtx *zds, ZSTD_outBuffer *output, ZSTD_inBuffer *input);
extern unsigned ZSTD_isError(size_t code);
extern const char *ZSTD_getErrorName(size_t code);

int mkdir_p(const char *path) {
    char tmp[PATH_MAX_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return prm_err("Failed to create directory %s: %s", tmp, strerror(errno));
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST && !path_is_dir(tmp)) {
        return prm_err("Failed to create directory %s: %s", tmp, strerror(errno));
    }
    return 0;
}

static int mkdir_parent(const char *filepath) {
    char parent[PATH_MAX_LEN];
    snprintf(parent, sizeof(parent), "%s", filepath);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = '\0';
    return mkdir_p(parent);
}

/* =========================================================================
 * HTTPS 1.1 Client (Mbed TLS + POSIX sockets)
 * ========================================================================= */

struct parsed_url {
    bool is_https;
    char host[256];
    char port[16];
    char path[1024];
};

static int parse_url(const char *url, struct parsed_url *out) {
    memset(out, 0, sizeof(*out));
    const char *p = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        out->is_https = true;
        snprintf(out->port, sizeof(out->port), "443");
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        out->is_https = false;
        snprintf(out->port, sizeof(out->port), "80");
        p = url + 7;
    } else {
        return prm_err("Unsupported URL scheme: %s", url);
    }

    const char *slash = strchr(p, '/');
    size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return prm_err("Invalid host in URL: %s", url);
    }

    char hostport[256];
    memcpy(hostport, p, host_len);
    hostport[host_len] = '\0';

    char *colon = strchr(hostport, ':');
    if (colon) {
        *colon = '\0';
        snprintf(out->port, sizeof(out->port), "%s", colon + 1);
    }
    snprintf(out->host, sizeof(out->host), "%s", hostport);
    snprintf(out->path, sizeof(out->path), "%s", slash ? slash : "/");
    return 0;
}

struct http_conn {
    int fd;
    bool is_https;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
};

static int net_send_cb(void *ctx, const unsigned char *buf, size_t len) {
    ssize_t r = write(*(int *)ctx, buf, len);
    if (r < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                   ? MBEDTLS_ERR_SSL_WANT_WRITE
                   : -1;
    }
    return (int)r;
}

static int net_recv_cb(void *ctx, unsigned char *buf, size_t len) {
    ssize_t r = read(*(int *)ctx, buf, len);
    if (r < 0) {
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                   ? MBEDTLS_ERR_SSL_WANT_READ
                   : -1;
    }
    return (int)r;
}

static int load_system_ca_bundle(mbedtls_x509_crt *cacert) {
    static const char *const ca_paths[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/etc/ssl/ca-bundle.pem",
        "/etc/pki/tls/cacert.pem",
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        "/etc/ssl/cert.pem",
    };
    const char *env_ca = getenv("SSL_CERT_FILE");
    if (env_ca && *env_ca && mbedtls_x509_crt_parse_file(cacert, env_ca) >= 0) {
        return 0;
    }
    for (size_t i = 0; i < sizeof(ca_paths) / sizeof(ca_paths[0]); i++) {
        if (path_exists(ca_paths[i]) && mbedtls_x509_crt_parse_file(cacert, ca_paths[i]) >= 0) {
            return 0;
        }
    }
    return -1;
}

static void conn_close(struct http_conn *c) {
    if (c->is_https) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->ctr_drbg);
        mbedtls_entropy_free(&c->entropy);
        mbedtls_x509_crt_free(&c->cacert);
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
}

static int conn_open(struct http_conn *c, const struct parsed_url *u) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->is_https = u->is_https;

    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(u->host, u->port, &hints, &res);
    if (gai != 0) {
        return prm_err("DNS lookup failed for %s: %s", u->host, gai_strerror(gai));
    }

    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        int sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd < 0) continue;
        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0) {
            c->fd = sfd;
            break;
        }
        close(sfd);
    }
    freeaddrinfo(res);

    if (c->fd < 0) {
        return prm_err("Failed to connect to %s:%s", u->host, u->port);
    }
    if (!u->is_https) return 0;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->cacert);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_entropy_init(&c->entropy);

    if (mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)"prm", 3) != 0 ||
        load_system_ca_bundle(&c->cacert) != 0 ||
        mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        conn_close(c);
        return prm_err("Failed to initialize TLS context or CA bundle for %s", u->host);
    }

    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0 ||
        mbedtls_ssl_set_hostname(&c->ssl, u->host) != 0) {
        conn_close(c);
        return prm_err("Failed to setup TLS session for %s", u->host);
    }

    mbedtls_ssl_set_bio(&c->ssl, &c->fd, net_send_cb, net_recv_cb, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            conn_close(c);
            return prm_err("TLS handshake failed for %s (err -0x%04x)", u->host, (unsigned int)(-ret));
        }
    }
    return 0;
}

static int conn_write_all(struct http_conn *c, const void *buf, size_t len) {
    const unsigned char *p = buf;
    while (len > 0) {
        int w = c->is_https ? mbedtls_ssl_write(&c->ssl, p, len) : (int)write(c->fd, p, len);
        if (c->is_https && (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE)) continue;
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static ssize_t conn_read_some(struct http_conn *c, void *buf, size_t cap) {
    if (c->is_https) {
        int r;
        while ((r = mbedtls_ssl_read(&c->ssl, buf, cap)) < 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            return -1;
        }
        return r;
    }
    ssize_t r;
    while ((r = read(c->fd, buf, cap)) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return r;
}

typedef int (*http_sink_fn)(void *userdata, const unsigned char *data, size_t len, uint64_t content_len);

static int https_request_sink(const char *url, const char *accept_hdr, const char *bearer_token,
                              http_sink_fn sink, void *userdata) {
    char current_url[1024];
    snprintf(current_url, sizeof(current_url), "%s", url);

    for (int redirects = 0; redirects <= 8; redirects++) {
        struct parsed_url u;
        if (parse_url(current_url, &u) != 0) return -1;

        struct http_conn conn;
        if (conn_open(&conn, &u) != 0) return -1;

        char req[2048];
        int req_len;
        if (bearer_token && *bearer_token && redirects == 0) {
            req_len = snprintf(req, sizeof(req),
                               "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: prm/" PRM_VERSION "\r\n"
                               "Accept: %s\r\nAuthorization: Bearer %s\r\nConnection: close\r\n\r\n",
                               u.path, u.host, accept_hdr ? accept_hdr : "*/*", bearer_token);
        } else {
            req_len = snprintf(req, sizeof(req),
                               "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: prm/" PRM_VERSION "\r\n"
                               "Accept: %s\r\nConnection: close\r\n\r\n",
                               u.path, u.host, accept_hdr ? accept_hdr : "*/*");
        }

        if (conn_write_all(&conn, req, (size_t)req_len) != 0) {
            conn_close(&conn);
            return prm_err("Failed to send HTTP request to %s", u.host);
        }

        char hdr_buf[16384];
        size_t hdr_used = 0;
        char *body_start = NULL;
        while (hdr_used + 1 < sizeof(hdr_buf)) {
            ssize_t r = conn_read_some(&conn, hdr_buf + hdr_used, sizeof(hdr_buf) - 1 - hdr_used);
            if (r <= 0) break;
            hdr_used += (size_t)r;
            hdr_buf[hdr_used] = '\0';
            if ((body_start = strstr(hdr_buf, "\r\n\r\n")) != NULL) {
                *body_start = '\0';
                body_start += 4;
                break;
            }
        }

        int status_code = 0;
        if (!body_start || sscanf(hdr_buf, "HTTP/%*d.%*d %d", &status_code) != 1) {
            conn_close(&conn);
            return prm_err("Invalid HTTP response from %s", u.host);
        }

        char location[1024] = {0};
        uint64_t content_len = 0;
        bool is_chunked = false;

        for (char *line = strchr(hdr_buf, '\n'); line; ) {
            line++;
            char *next = strchr(line, '\n');
            if (next) *next = '\0';

            if (strncasecmp(line, "Location:", 9) == 0) {
                char *v = line + 9;
                while (*v && isspace((unsigned char)*v)) v++;
                snprintf(location, sizeof(location), "%s", v);
                size_t l = strlen(location);
                while (l > 0 && isspace((unsigned char)location[l - 1])) location[--l] = '\0';
            } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
                content_len = strtoull(line + 15, NULL, 10);
            } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
                       strcasestr(line + 18, "chunked") != NULL) {
                is_chunked = true;
            }
            line = next;
        }

        if ((status_code == 301 || status_code == 302 || status_code == 303 ||
             status_code == 307 || status_code == 308) && location[0]) {
            conn_close(&conn);
            if (location[0] == '/') {
                snprintf(current_url, sizeof(current_url), "%s://%s%s",
                         u.is_https ? "https" : "http", u.host, location);
            } else {
                snprintf(current_url, sizeof(current_url), "%s", location);
            }
            continue;
        }

        if (status_code < 200 || status_code >= 300) {
            conn_close(&conn);
            return prm_err("HTTP %d for %s", status_code, current_url);
        }

        size_t init_len = hdr_used - (size_t)(body_start - hdr_buf);

        if (!is_chunked) {
            if (init_len > 0 && sink(userdata, (const unsigned char *)body_start, init_len, content_len) != 0) {
                conn_close(&conn);
                return prm_err("Write error while receiving %s", current_url);
            }
            unsigned char buf[65536];
            for (;;) {
                ssize_t r = conn_read_some(&conn, buf, sizeof(buf));
                if (r < 0) {
                    conn_close(&conn);
                    return prm_err("Network read error from %s", u.host);
                }
                if (r == 0) break;
                if (sink(userdata, buf, (size_t)r, content_len) != 0) {
                    conn_close(&conn);
                    return prm_err("Write error while receiving %s", current_url);
                }
            }
        } else {
            size_t cbuf_cap = 65536, cbuf_len = init_len;
            unsigned char *cbuf = malloc(cbuf_cap);
            if (!cbuf) {
                conn_close(&conn);
                return prm_err("Out of memory");
            }
            memcpy(cbuf, body_start, init_len);

            for (;;) {
                char *crlf = NULL;
                while (!(crlf = memmem(cbuf, cbuf_len, "\r\n", 2))) {
                    if (cbuf_len + 16384 > cbuf_cap) {
                        cbuf_cap *= 2;
                        unsigned char *nb = realloc(cbuf, cbuf_cap);
                        if (!nb) { free(cbuf); conn_close(&conn); return prm_err("Out of memory"); }
                        cbuf = nb;
                    }
                    ssize_t r = conn_read_some(&conn, cbuf + cbuf_len, 16384);
                    if (r <= 0) break;
                    cbuf_len += (size_t)r;
                }
                if (!crlf) break;

                *crlf = '\0';
                size_t chunk_sz = (size_t)strtoull((char *)cbuf, NULL, 16);
                size_t line_bytes = (size_t)((unsigned char *)crlf - cbuf) + 2;
                memmove(cbuf, cbuf + line_bytes, cbuf_len - line_bytes);
                cbuf_len -= line_bytes;
                if (chunk_sz == 0) break;

                while (cbuf_len < chunk_sz + 2) {
                    if (cbuf_len + 65536 > cbuf_cap) {
                        cbuf_cap = cbuf_len + chunk_sz + 65536;
                        unsigned char *nb = realloc(cbuf, cbuf_cap);
                        if (!nb) { free(cbuf); conn_close(&conn); return prm_err("Out of memory"); }
                        cbuf = nb;
                    }
                    ssize_t r = conn_read_some(&conn, cbuf + cbuf_len, 65536);
                    if (r <= 0) break;
                    cbuf_len += (size_t)r;
                }

                size_t avail = cbuf_len < chunk_sz ? cbuf_len : chunk_sz;
                if (avail > 0 && sink(userdata, cbuf, avail, 0) != 0) {
                    free(cbuf);
                    conn_close(&conn);
                    return prm_err("Write error while receiving %s", current_url);
                }
                size_t consume = cbuf_len >= chunk_sz + 2 ? chunk_sz + 2 : cbuf_len;
                memmove(cbuf, cbuf + consume, cbuf_len - consume);
                cbuf_len -= consume;
            }
            free(cbuf);
        }

        conn_close(&conn);
        return 0;
    }

    return prm_err("Too many HTTP redirects for %s", url);
}

struct mem_sink {
    char *data;
    size_t len;
    size_t cap;
};

static int mem_sink_cb(void *userdata, const unsigned char *data, size_t len, uint64_t content_len) {
    (void)content_len;
    struct mem_sink *m = userdata;
    if (m->len + len + 1 > m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 16384;
        while (ncap < m->len + len + 1) ncap *= 2;
        char *nb = realloc(m->data, ncap);
        if (!nb) return -1;
        m->data = nb;
        m->cap = ncap;
    }
    memcpy(m->data + m->len, data, len);
    m->len += len;
    m->data[m->len] = '\0';
    return 0;
}

int https_get(const char *url, const char *accept_hdr, const char *bearer_token, char **out_buf, size_t *out_len) {
    struct mem_sink m = {NULL, 0, 0};
    if (https_request_sink(url, accept_hdr, bearer_token, mem_sink_cb, &m) != 0) {
        free(m.data);
        return -1;
    }
    if (!m.data) m.data = calloc(1, 1);
    *out_buf = m.data;
    if (out_len) *out_len = m.len;
    return 0;
}

struct file_sink {
    FILE *fp;
    uint64_t downloaded;
    uint64_t last_report;
    uint64_t total;
};

static int file_sink_cb(void *userdata, const unsigned char *data, size_t len, uint64_t content_len) {
    struct file_sink *fs = userdata;
    if (content_len > 0 && fs->total == 0) fs->total = content_len;
    if (fwrite(data, 1, len, fs->fp) != len) return -1;
    fs->downloaded += len;

    if (fs->total > 0 && fs->downloaded - fs->last_report >= 8ULL * 1024 * 1024) {
        fs->last_report = fs->downloaded;
        double pct = ((double)fs->downloaded / (double)fs->total) * 100.0;
        char d_str[64], t_str[64];
        format_bytes(fs->downloaded, d_str, sizeof(d_str));
        format_bytes(fs->total, t_str, sizeof(t_str));
        fprintf(stderr, "\r  %.1f%% (%s / %s)", pct, d_str, t_str);
        fflush(stderr);
    }
    return 0;
}

int download_file(const char *url, const char *dest, uint64_t expected_size) {
    if (mkdir_parent(dest) != 0) return -1;

    const char *slash = strrchr(dest, '/');
    const char *fname = slash ? slash + 1 : dest;

    struct stat st;
    if (expected_size > 0 && stat(dest, &st) == 0 && (uint64_t)st.st_size == expected_size) {
        char sz_str[64];
        format_bytes(expected_size, sz_str, sizeof(sz_str));
        fprintf(stderr, "Using cached archive: %s (%s)\n", fname, sz_str);
        return 0;
    }

    char tmp_dest[PATH_MAX_LEN];
    snprintf(tmp_dest, sizeof(tmp_dest), "%s.part", dest);
    fprintf(stderr, "Downloading %s ...\n", fname);

    FILE *fp = fopen(tmp_dest, "wb");
    if (!fp) {
        return prm_err("Failed to create %s: %s", tmp_dest, strerror(errno));
    }

    struct file_sink fs = {fp, 0, 0, expected_size};
    int rc = https_request_sink(url, "*/*", NULL, file_sink_cb, &fs);
    fclose(fp);
    if (rc != 0) {
        unlink(tmp_dest);
        return -1;
    }

    if (fs.total > 0) {
        char d_str[64], t_str[64];
        format_bytes(fs.downloaded, d_str, sizeof(d_str));
        format_bytes(fs.total, t_str, sizeof(t_str));
        fprintf(stderr, "\r  100.0%% (%s / %s)\n", d_str, t_str);
    }

    if (rename(tmp_dest, dest) != 0) {
        unlink(tmp_dest);
        return prm_err("Failed to finalize %s: %s", dest, strerror(errno));
    }
    return 0;
}

/* =========================================================================
 * SHA-256 / SHA-512 Checksum Verification
 * ========================================================================= */

int fetch_expected_checksum(const struct prm_asset *cs_asset, char *out_hex, size_t out_cap) {
    char *text = NULL;
    if (https_get(cs_asset->download_url, "*/*", NULL, &text, NULL) != 0) return -1;

    char *p = text;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t i = 0;
    while (p[i] && !isspace((unsigned char)p[i]) && i + 1 < out_cap) {
        out_hex[i] = (char)tolower((unsigned char)p[i]);
        i++;
    }
    out_hex[i] = '\0';
    free(text);

    return i > 0 ? 0 : prm_err("Empty checksum file: %s", cs_asset->name);
}

int compute_file_checksum(const char *archive_path, bool use_sha256, char *out_hex, size_t out_cap) {
    FILE *f = fopen(archive_path, "rb");
    if (!f) {
        return prm_err("Failed to open %s for hashing: %s", archive_path, strerror(errno));
    }

    unsigned char buf[65536], digest[64];
    size_t digest_len = use_sha256 ? 32 : 64;

    if (use_sha256) {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0; ) {
            mbedtls_sha256_update(&ctx, buf, n);
        }
        mbedtls_sha256_finish(&ctx, digest);
        mbedtls_sha256_free(&ctx);
    } else {
        mbedtls_sha512_context ctx;
        mbedtls_sha512_init(&ctx);
        mbedtls_sha512_starts(&ctx, 0);
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0; ) {
            mbedtls_sha512_update(&ctx, buf, n);
        }
        mbedtls_sha512_finish(&ctx, digest);
        mbedtls_sha512_free(&ctx);
    }
    fclose(f);

    if (out_cap < digest_len * 2 + 1) return prm_err("Checksum buffer too small");
    for (size_t i = 0; i < digest_len; i++) {
        snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
    }
    out_hex[digest_len * 2] = '\0';
    return 0;
}

int verify_checksum(const char *archive_path, const struct prm_asset *cs_asset, char *out_hex, size_t out_cap) {
    char expected_hex[160], actual_hex[160];
    if (fetch_expected_checksum(cs_asset, expected_hex, sizeof(expected_hex)) != 0) return -1;

    size_t nlen = strlen(cs_asset->name);
    bool use_sha256 = (nlen >= 10 && strcasecmp(cs_asset->name + nlen - 10, ".sha256sum") == 0) ||
                      strlen(expected_hex) == 64;
    if (compute_file_checksum(archive_path, use_sha256, actual_hex, sizeof(actual_hex)) != 0) return -1;

    if (strcasecmp(actual_hex, expected_hex) != 0) {
        unlink(archive_path);
        return prm_err("Checksum mismatch for %s!\n  Expected: %s\n  Actual:   %s",
                       archive_path, expected_hex, actual_hex);
    }

    fprintf(stderr, "Checksum verified (%s).\n", cs_asset->name);
    snprintf(out_hex, out_cap, "%s", expected_hex);
    return 0;
}

/* =========================================================================
 * Streaming Decompressor (Gzip / XZ / Zstd) + POSIX/PAX/GNU Tar Extractor
 * ========================================================================= */

struct decomp_stream {
    enum { COMP_GZIP, COMP_XZ, COMP_ZSTD } fmt;
    FILE *fp;
    unsigned char in_buf[65536];
    size_t in_pos;
    size_t in_len;
    bool eof_in;
    bool stream_done;

    tinfl_decompressor tinfl;
    unsigned char gz_dict[TINFL_LZ_DICT_SIZE];
    size_t gz_dict_ofs;
    size_t gz_avail;
    size_t gz_read_ofs;
    bool gz_hdr_parsed;

    struct xz_dec *xz;
    struct xz_buf xzb;

    ZSTD_DCtx *zstd;
};

static int gzip_skip_header(struct decomp_stream *ds) {
    while (ds->in_len - ds->in_pos < 10 && !ds->eof_in) {
        memmove(ds->in_buf, ds->in_buf + ds->in_pos, ds->in_len - ds->in_pos);
        ds->in_len -= ds->in_pos;
        ds->in_pos = 0;
        size_t r = fread(ds->in_buf + ds->in_len, 1, sizeof(ds->in_buf) - ds->in_len, ds->fp);
        if (r == 0) ds->eof_in = true;
        ds->in_len += r;
    }
    if (ds->in_len - ds->in_pos < 10) return -1;
    const unsigned char *p = ds->in_buf + ds->in_pos;
    if (p[0] != 0x1f || p[1] != 0x8b || p[2] != 8) return -1;
    unsigned char flg = p[3];
    ds->in_pos += 10;

    if (flg & 4) {
        if (ds->in_len - ds->in_pos < 2) return -1;
        size_t xlen = (size_t)ds->in_buf[ds->in_pos] | ((size_t)ds->in_buf[ds->in_pos + 1] << 8);
        ds->in_pos += 2 + xlen;
    }
    if (flg & 8) {
        while (ds->in_pos < ds->in_len && ds->in_buf[ds->in_pos++] != 0) {}
    }
    if (flg & 16) {
        while (ds->in_pos < ds->in_len && ds->in_buf[ds->in_pos++] != 0) {}
    }
    if (flg & 2) ds->in_pos += 2;
    ds->gz_hdr_parsed = true;
    return 0;
}

static ssize_t decomp_read(struct decomp_stream *ds, unsigned char *out, size_t out_cap) {
    if (ds->stream_done) return 0;

    if (ds->fmt == COMP_GZIP) {
        if (!ds->gz_hdr_parsed && gzip_skip_header(ds) != 0) {
            return prm_err("Invalid gzip header");
        }
        while (ds->gz_avail == 0) {
            if (ds->in_pos == ds->in_len && !ds->eof_in) {
                ds->in_pos = 0;
                ds->in_len = fread(ds->in_buf, 1, sizeof(ds->in_buf), ds->fp);
                if (ds->in_len == 0) ds->eof_in = true;
            }
            size_t in_bytes = ds->in_len - ds->in_pos;
            size_t out_bytes = TINFL_LZ_DICT_SIZE - ds->gz_dict_ofs;
            mz_uint32 flags = (!ds->eof_in || in_bytes > 0) ? TINFL_FLAG_HAS_MORE_INPUT : 0;

            tinfl_status status = tinfl_decompress(&ds->tinfl, ds->in_buf + ds->in_pos, &in_bytes,
                                                   ds->gz_dict, ds->gz_dict + ds->gz_dict_ofs,
                                                   &out_bytes, flags);
            ds->in_pos += in_bytes;
            if (out_bytes > 0) {
                ds->gz_read_ofs = ds->gz_dict_ofs;
                ds->gz_avail = out_bytes;
                ds->gz_dict_ofs = (ds->gz_dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
            }
            if (status == TINFL_STATUS_DONE || (in_bytes == 0 && out_bytes == 0 && ds->eof_in)) {
                if (ds->gz_avail == 0) ds->stream_done = true;
                break;
            }
            if (status < 0) return prm_err("Gzip decompression failed (%d)", (int)status);
        }
        size_t n = ds->gz_avail < out_cap ? ds->gz_avail : out_cap;
        memcpy(out, ds->gz_dict + ds->gz_read_ofs, n);
        ds->gz_read_ofs += n;
        ds->gz_avail -= n;
        return (ssize_t)n;
    }

    if (ds->fmt == COMP_XZ) {
        ds->xzb.out = out;
        ds->xzb.out_pos = 0;
        ds->xzb.out_size = out_cap;
        while (ds->xzb.out_pos == 0) {
            if (ds->xzb.in_pos == ds->xzb.in_size && !ds->eof_in) {
                ds->xzb.in_pos = 0;
                ds->xzb.in_size = fread(ds->in_buf, 1, sizeof(ds->in_buf), ds->fp);
                ds->xzb.in = ds->in_buf;
                if (ds->xzb.in_size == 0) ds->eof_in = true;
            }
            enum xz_ret ret = xz_dec_run(ds->xz, &ds->xzb);
            if (ret == XZ_STREAM_END || (ds->xzb.out_pos == 0 && ds->eof_in && ds->xzb.in_pos == ds->xzb.in_size)) {
                ds->stream_done = true;
                break;
            }
            if (ret != XZ_OK) return prm_err("XZ decompression failed (%d)", (int)ret);
        }
        return (ssize_t)ds->xzb.out_pos;
    }

    ZSTD_outBuffer zob = {out, out_cap, 0};
    while (zob.pos == 0) {
        if (ds->in_pos == ds->in_len && !ds->eof_in) {
            ds->in_pos = 0;
            ds->in_len = fread(ds->in_buf, 1, sizeof(ds->in_buf), ds->fp);
            if (ds->in_len == 0) ds->eof_in = true;
        }
        if (ds->in_pos == ds->in_len && ds->eof_in) {
            ds->stream_done = true;
            break;
        }
        ZSTD_inBuffer zib = {ds->in_buf, ds->in_len, ds->in_pos};
        size_t ret = ZSTD_decompressStream(ds->zstd, &zob, &zib);
        ds->in_pos = zib.pos;
        if (ZSTD_isError(ret)) return prm_err("Zstd decompression failed: %s", ZSTD_getErrorName(ret));
    }
    return (ssize_t)zob.pos;
}

static int decomp_read_exact(struct decomp_stream *ds, unsigned char *buf, size_t n, size_t *out_got) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = decomp_read(ds, buf + total, n - total);
        if (r < 0) return -1;
        if (r == 0) break;
        total += (size_t)r;
    }
    *out_got = total;
    return 0;
}

static int decomp_skip_exact(struct decomp_stream *ds, uint64_t n) {
    unsigned char discard[16384];
    while (n > 0) {
        size_t chunk = n < sizeof(discard) ? (size_t)n : sizeof(discard), got = 0;
        if (decomp_read_exact(ds, discard, chunk, &got) != 0) return -1;
        if (got == 0) break;
        n -= got;
    }
    return 0;
}

static uint64_t parse_tar_octal(const char *field, size_t len) {
    if ((unsigned char)field[0] & 0x80) {
        uint64_t v = 0;
        for (size_t i = 1; i < len; i++) v = (v << 8) | (unsigned char)field[i];
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len && field[i]; i++) {
        if (field[i] >= '0' && field[i] <= '7') v = (v << 3) | (uint64_t)(field[i] - '0');
    }
    return v;
}

static void parse_pax_records(const char *buf, size_t len, char *pax_path, size_t path_cap,
                              char *pax_link, size_t link_cap, uint64_t *pax_size, bool *has_pax_size) {
    for (size_t pos = 0; pos < len; ) {
        const char *sp = memchr(buf + pos, ' ', len - pos);
        if (!sp) break;
        size_t rec_len = (size_t)strtoull(buf + pos, NULL, 10);
        if (rec_len == 0 || pos + rec_len > len) break;

        const char *kv = sp + 1;
        size_t kv_len = (pos + rec_len) - (size_t)(kv - buf);
        if (kv_len > 0 && kv[kv_len - 1] == '\n') kv_len--;

        if (kv_len > 5 && strncmp(kv, "path=", 5) == 0) {
            size_t vlen = kv_len - 5 < path_cap - 1 ? kv_len - 5 : path_cap - 1;
            memcpy(pax_path, kv + 5, vlen);
            pax_path[vlen] = '\0';
        } else if (kv_len > 9 && strncmp(kv, "linkpath=", 9) == 0) {
            size_t vlen = kv_len - 9 < link_cap - 1 ? kv_len - 9 : link_cap - 1;
            memcpy(pax_link, kv + 9, vlen);
            pax_link[vlen] = '\0';
        } else if (kv_len > 5 && strncmp(kv, "size=", 5) == 0) {
            *pax_size = strtoull(kv + 5, NULL, 10);
            *has_pax_size = true;
        }
        pos += rec_len;
    }
}

static bool is_safe_relpath(const char *p) {
    if (!p || !*p || p[0] == '/') return false;
    for (const char *s = p; *s; ) {
        if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == '\0')) return false;
        while (*s && *s != '/') s++;
        while (*s == '/') s++;
    }
    return true;
}

static int unpack_tar_stream(struct decomp_stream *ds, const char *staging_dir) {
    char next_path[PATH_MAX_LEN] = {0}, next_link[PATH_MAX_LEN] = {0};
    uint64_t next_size = 0;
    bool has_next_size = false;
    unsigned char blk[512];

    for (;;) {
        size_t got = 0;
        if (decomp_read_exact(ds, blk, 512, &got) != 0) return -1;
        if (got == 0) break;
        if (got < 512) return prm_err("Truncated tar header");

        bool all_zero = true;
        for (size_t i = 0; i < 512; i++) {
            if (blk[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) continue;

        char name[101] = {0}, linkname[101] = {0}, prefix[156] = {0};
        memcpy(name, blk, 100);
        memcpy(linkname, blk + 157, 100);
        if (memcmp(blk + 257, "ustar", 5) == 0) memcpy(prefix, blk + 345, 155);

        mode_t mode = (mode_t)parse_tar_octal((const char *)blk + 100, 8);
        uint64_t size = has_next_size ? next_size : parse_tar_octal((const char *)blk + 124, 12);
        has_next_size = false;
        char typeflag = blk[156] ? (char)blk[156] : '0';

        if (typeflag == 'x' || typeflag == 'g' || typeflag == 'L' || typeflag == 'K') {
            uint64_t padded = (size + 511ULL) & ~511ULL;
            char *meta = calloc(1, (size_t)padded + 1);
            if (!meta) return prm_err("Out of memory");
            size_t mgot = 0;
            if (decomp_read_exact(ds, (unsigned char *)meta, (size_t)padded, &mgot) != 0) {
                free(meta);
                return -1;
            }
            if (typeflag == 'x') {
                parse_pax_records(meta, (size_t)size, next_path, sizeof(next_path),
                                  next_link, sizeof(next_link), &next_size, &has_next_size);
            } else if (typeflag == 'L') {
                snprintf(next_path, sizeof(next_path), "%.*s", (int)size, meta);
            } else if (typeflag == 'K') {
                snprintf(next_link, sizeof(next_link), "%.*s", (int)size, meta);
            }
            free(meta);
            continue;
        }

        char relpath[PATH_MAX_LEN], target_link[PATH_MAX_LEN];
        if (next_path[0]) {
            snprintf(relpath, sizeof(relpath), "%s", next_path);
            next_path[0] = '\0';
        } else if (prefix[0]) {
            snprintf(relpath, sizeof(relpath), "%s/%s", prefix, name);
        } else {
            snprintf(relpath, sizeof(relpath), "%s", name);
        }

        snprintf(target_link, sizeof(target_link), "%s", next_link[0] ? next_link : linkname);
        next_link[0] = '\0';

        char *clean_rel = relpath;
        while (clean_rel[0] == '.' && clean_rel[1] == '/') clean_rel += 2;
        if (*clean_rel == '\0' || strcmp(clean_rel, ".") == 0) {
            if (decomp_skip_exact(ds, (size + 511ULL) & ~511ULL) != 0) return -1;
            continue;
        }
        if (!is_safe_relpath(clean_rel)) {
            return prm_err("Refusing unsafe tar entry path: %s", clean_rel);
        }

        char dest_path[PATH_MAX_LEN];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", staging_dir, clean_rel);

        if (typeflag == '5' || clean_rel[strlen(clean_rel) - 1] == '/') {
            if (mkdir_p(dest_path) != 0) return -1;
            chmod(dest_path, (mode & 0777) | 0700);
            if (decomp_skip_exact(ds, (size + 511ULL) & ~511ULL) != 0) return -1;
        } else if (typeflag == '2') {
            if (mkdir_parent(dest_path) != 0) return -1;
            unlink(dest_path);
            if (symlink(target_link, dest_path) != 0) {
                return prm_err("Failed to create symlink %s -> %s: %s", dest_path, target_link, strerror(errno));
            }
            if (decomp_skip_exact(ds, (size + 511ULL) & ~511ULL) != 0) return -1;
        } else if (typeflag == '1') {
            if (mkdir_parent(dest_path) != 0) return -1;
            char *clean_link = target_link;
            while (clean_link[0] == '.' && clean_link[1] == '/') clean_link += 2;
            if (!is_safe_relpath(clean_link)) return prm_err("Unsafe hardlink target: %s", clean_link);
            char link_src[PATH_MAX_LEN];
            snprintf(link_src, sizeof(link_src), "%s/%s", staging_dir, clean_link);
            unlink(dest_path);
            if (link(link_src, dest_path) != 0) {
                return prm_err("Failed to create hardlink %s -> %s: %s", dest_path, link_src, strerror(errno));
            }
            if (decomp_skip_exact(ds, (size + 511ULL) & ~511ULL) != 0) return -1;
        } else if (typeflag == '0' || typeflag == '7') {
            if (mkdir_parent(dest_path) != 0) return -1;
            unlink(dest_path);
            FILE *out_fp = fopen(dest_path, "wb");
            if (!out_fp) return prm_err("Failed to create %s: %s", dest_path, strerror(errno));

            uint64_t rem = size;
            unsigned char fbuf[65536];
            while (rem > 0) {
                size_t chunk = rem < sizeof(fbuf) ? (size_t)rem : sizeof(fbuf), fgot = 0;
                if (decomp_read_exact(ds, fbuf, chunk, &fgot) != 0 || fgot != chunk) {
                    fclose(out_fp);
                    return prm_err("Unexpected EOF unpacking %s", clean_rel);
                }
                if (fwrite(fbuf, 1, fgot, out_fp) != fgot) {
                    fclose(out_fp);
                    return prm_err("Failed writing %s: %s", dest_path, strerror(errno));
                }
                rem -= fgot;
            }
            fclose(out_fp);
            chmod(dest_path, (mode & 0777) | 0600);

            uint64_t pad = (512ULL - (size & 511ULL)) & 511ULL;
            if (pad > 0 && decomp_skip_exact(ds, pad) != 0) return -1;
        } else {
            if (decomp_skip_exact(ds, (size + 511ULL) & ~511ULL) != 0) return -1;
        }
    }
    return 0;
}

int extract_tar_archive(const char *archive_path, const char *target_parent_dir, char *out_extracted_dir, size_t out_cap) {
    if (mkdir_p(target_parent_dir) != 0) return -1;

    char staging_dir[PATH_MAX_LEN];
    snprintf(staging_dir, sizeof(staging_dir), "%s/.prm-extract-%d", target_parent_dir, (int)getpid());
    remove_dir_force(staging_dir);
    if (mkdir_p(staging_dir) != 0) return -1;

    const char *slash = strrchr(archive_path, '/');
    const char *fname = slash ? slash + 1 : archive_path;
    fprintf(stderr, "Extracting %s ...\n", fname);

    size_t flen = strlen(fname);
    struct decomp_stream *ds = calloc(1, sizeof(*ds));
    if (!ds) return prm_err("Out of memory");

    if ((flen >= 7 && strcasecmp(fname + flen - 7, ".tar.gz") == 0) ||
        (flen >= 4 && strcasecmp(fname + flen - 4, ".tgz") == 0)) {
        ds->fmt = COMP_GZIP;
        tinfl_init(&ds->tinfl);
    } else if (flen >= 7 && strcasecmp(fname + flen - 7, ".tar.xz") == 0) {
        ds->fmt = COMP_XZ;
        xz_crc32_init();
        xz_crc64_init();
        if (!(ds->xz = xz_dec_init(XZ_DYNALLOC, 1 << 26))) {
            free(ds);
            return prm_err("Failed to initialize XZ decoder");
        }
    } else if (flen >= 8 && strcasecmp(fname + flen - 8, ".tar.zst") == 0) {
        ds->fmt = COMP_ZSTD;
        if (!(ds->zstd = ZSTD_createDCtx())) {
            free(ds);
            return prm_err("Failed to initialize Zstd decoder");
        }
    } else {
        free(ds);
        remove_dir_force(staging_dir);
        return prm_err("Unsupported archive format: %s", fname);
    }

    ds->fp = fopen(archive_path, "rb");
    if (!ds->fp) {
        if (ds->xz) xz_dec_end(ds->xz);
        if (ds->zstd) ZSTD_freeDCtx(ds->zstd);
        free(ds);
        remove_dir_force(staging_dir);
        return prm_err("Failed to open archive %s: %s", archive_path, strerror(errno));
    }

    int rc = unpack_tar_stream(ds, staging_dir);
    fclose(ds->fp);
    if (ds->xz) xz_dec_end(ds->xz);
    if (ds->zstd) ZSTD_freeDCtx(ds->zstd);
    free(ds);

    if (rc != 0) {
        remove_dir_force(staging_dir);
        return -1;
    }

    DIR *d = opendir(staging_dir);
    if (!d) return prm_err("Failed to read staging dir %s", staging_dir);

    char single_name[256] = {0};
    size_t entry_count = 0;
    for (struct dirent *ent; (ent = readdir(d)) != NULL; ) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (++entry_count == 1) snprintf(single_name, sizeof(single_name), "%s", ent->d_name);
    }
    closedir(d);

    char top_candidate[PATH_MAX_LEN];
    snprintf(top_candidate, sizeof(top_candidate), "%s/%s", staging_dir, single_name);
    if (entry_count == 1 && path_is_dir(top_candidate)) {
        snprintf(out_extracted_dir, out_cap, "%s/%s", target_parent_dir, single_name);
        if (path_exists(out_extracted_dir) && remove_dir_force(out_extracted_dir) != 0) return -1;
        if (rename(top_candidate, out_extracted_dir) != 0) {
            return prm_err("Failed to move extracted folder to %s: %s", out_extracted_dir, strerror(errno));
        }
        remove_dir_force(staging_dir);
    } else {
        char stem[256];
        archive_stem(fname, stem, sizeof(stem));
        snprintf(out_extracted_dir, out_cap, "%s/%s", target_parent_dir, stem);
        if (path_exists(out_extracted_dir) && remove_dir_force(out_extracted_dir) != 0) return -1;
        if (rename(staging_dir, out_extracted_dir) != 0) {
            return prm_err("Failed to move staging dir to %s: %s", out_extracted_dir, strerror(errno));
        }
    }
    return 0;
}

/* =========================================================================
 * Force Directory Removal via POSIX nftw()
 * ========================================================================= */

static int chmod_writable_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)ftwbuf;
    if (typeflag == FTW_D || typeflag == FTW_DP) {
        chmod(fpath, 0755);
    } else if (typeflag == FTW_F) {
        chmod(fpath, 0644);
    }
    return 0;
}

static int remove_node_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    (void)sb;
    (void)typeflag;
    (void)ftwbuf;
    return remove(fpath);
}

int remove_dir_force(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    nftw(path, chmod_writable_cb, 32, FTW_PHYS);
    if (nftw(path, remove_node_cb, 32, FTW_DEPTH | FTW_PHYS) != 0) {
        return prm_err("Failed to remove directory %s: %s", path, strerror(errno));
    }
    return 0;
}
