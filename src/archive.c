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
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Forward declarations from single-file zstddeclib.c */
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
typedef struct {
    const void *src;
    size_t size;
    size_t pos;
} ZSTD_inBuffer;
typedef struct {
    void *dst;
    size_t size;
    size_t pos;
} ZSTD_outBuffer;
extern ZSTD_DCtx *ZSTD_createDCtx(void);
extern size_t ZSTD_freeDCtx(ZSTD_DCtx *dctx);
extern size_t ZSTD_decompressStream(ZSTD_DCtx *zds, ZSTD_outBuffer *output, ZSTD_inBuffer *input);
extern unsigned ZSTD_isError(size_t code);
extern const char *ZSTD_getErrorName(size_t code);

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

int mkdir_p(const char *path, char *err, size_t err_cap) {
    char tmp[PATH_MAX_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len == 0) return 0;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                snprintf(err, err_cap, "Failed to create directory %s: %s", tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        struct stat st;
        if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
            snprintf(err, err_cap, "Failed to create directory %s: %s", tmp, strerror(errno));
            return -1;
        }
    }
    return 0;
}

static int mkdir_parent(const char *filepath, char *err, size_t err_cap) {
    char parent[PATH_MAX_LEN];
    snprintf(parent, sizeof(parent), "%s", filepath);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent) return 0;
    *slash = '\0';
    return mkdir_p(parent, err, err_cap);
}

/* =========================================================================
 * HTTPS 1.1 Client (Mbed TLS + POSIX sockets)
 * ========================================================================= */

typedef struct {
    bool is_https;
    char host[256];
    char port[16];
    char path[1024];
} ParsedUrl;

static int parse_url(const char *url, ParsedUrl *out, char *err, size_t err_cap) {
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
        snprintf(err, err_cap, "Unsupported URL scheme: %s", url);
        return -1;
    }

    const char *slash = strchr(p, '/');
    size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        snprintf(err, err_cap, "Invalid host in URL: %s", url);
        return -1;
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

typedef struct {
    int fd;
    bool is_https;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cacert;
} HttpConn;

static int net_send_cb(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t r = write(fd, buf, len);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return -1;
    }
    return (int)r;
}

static int net_recv_cb(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t r = read(fd, buf, len);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return -1;
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
        if (file_exists(ca_paths[i]) && mbedtls_x509_crt_parse_file(cacert, ca_paths[i]) >= 0) {
            return 0;
        }
    }
    return -1;
}

static void conn_close(HttpConn *c) {
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

static int conn_open(HttpConn *c, const ParsedUrl *u, char *err, size_t err_cap) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->is_https = u->is_https;

    struct addrinfo hints, *res = NULL, *rp = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(u->host, u->port, &hints, &res);
    if (gai != 0) {
        snprintf(err, err_cap, "DNS lookup failed for %s: %s", u->host, gai_strerror(gai));
        return -1;
    }

    for (rp = res; rp != NULL; rp = rp->ai_next) {
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
        snprintf(err, err_cap, "Failed to connect to %s:%s", u->host, u->port);
        return -1;
    }

    if (!u->is_https) return 0;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->cacert);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_entropy_init(&c->entropy);

    const char *pers = "prm_tls";
    if (mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func, &c->entropy,
                              (const unsigned char *)pers, strlen(pers)) != 0) {
        snprintf(err, err_cap, "Failed to initialize TLS RNG");
        conn_close(c);
        return -1;
    }

    if (load_system_ca_bundle(&c->cacert) != 0) {
        snprintf(err, err_cap, "No system CA certificate bundle found in /etc/ssl or /etc/pki");
        conn_close(c);
        return -1;
    }

    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        snprintf(err, err_cap, "Failed to configure TLS");
        conn_close(c);
        return -1;
    }

    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->cacert, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0 ||
        mbedtls_ssl_set_hostname(&c->ssl, u->host) != 0) {
        snprintf(err, err_cap, "Failed to setup TLS session for %s", u->host);
        conn_close(c);
        return -1;
    }

    mbedtls_ssl_set_bio(&c->ssl, &c->fd, net_send_cb, net_recv_cb, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            snprintf(err, err_cap, "TLS handshake failed for %s (err -0x%04x)", u->host, (unsigned int)(-ret));
            conn_close(c);
            return -1;
        }
    }
    return 0;
}

static int conn_write_all(HttpConn *c, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t rem = len;
    while (rem > 0) {
        int w;
        if (c->is_https) {
            w = mbedtls_ssl_write(&c->ssl, p, rem);
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        } else {
            w = (int)write(c->fd, p, rem);
        }
        if (w <= 0) return -1;
        p += w;
        rem -= (size_t)w;
    }
    return 0;
}

static ssize_t conn_read_some(HttpConn *c, void *buf, size_t cap) {
    if (c->is_https) {
        int r;
        while ((r = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, cap)) < 0) {
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            return -1;
        }
        return (ssize_t)r;
    }
    ssize_t r;
    while ((r = read(c->fd, buf, cap)) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return r;
}

typedef int (*HttpBodySinkFn)(void *userdata, const unsigned char *data, size_t len, uint64_t content_len);

static int https_request_sink(
    const char *url,
    const char *accept_hdr,
    const char *bearer_token,
    HttpBodySinkFn sink,
    void *userdata,
    char *err,
    size_t err_cap)
{
    char current_url[1024];
    snprintf(current_url, sizeof(current_url), "%s", url);

    for (int redirects = 0; redirects <= 8; redirects++) {
        ParsedUrl u;
        if (parse_url(current_url, &u, err, err_cap) != 0) return -1;

        HttpConn conn;
        if (conn_open(&conn, &u, err, err_cap) != 0) return -1;

        char req[2048];
        int req_len;
        /* Only send GITHUB_TOKEN to api.github.com, not redirected S3/CDN hosts */
        if (bearer_token && *bearer_token && redirects == 0) {
            req_len = snprintf(req, sizeof(req),
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "User-Agent: prm/" PRM_VERSION "\r\n"
                               "Accept: %s\r\n"
                               "Authorization: Bearer %s\r\n"
                               "Connection: close\r\n\r\n",
                               u.path, u.host, accept_hdr ? accept_hdr : "*/*", bearer_token);
        } else {
            req_len = snprintf(req, sizeof(req),
                               "GET %s HTTP/1.1\r\n"
                               "Host: %s\r\n"
                               "User-Agent: prm/" PRM_VERSION "\r\n"
                               "Accept: %s\r\n"
                               "Connection: close\r\n\r\n",
                               u.path, u.host, accept_hdr ? accept_hdr : "*/*");
        }

        if (conn_write_all(&conn, req, (size_t)req_len) != 0) {
            conn_close(&conn);
            snprintf(err, err_cap, "Failed to send HTTP request to %s", u.host);
            return -1;
        }

        /* Read response headers */
        char hdr_buf[16384];
        size_t hdr_used = 0;
        char *body_start = NULL;
        while (hdr_used + 1 < sizeof(hdr_buf)) {
            ssize_t r = conn_read_some(&conn, hdr_buf + hdr_used, sizeof(hdr_buf) - 1 - hdr_used);
            if (r <= 0) break;
            hdr_used += (size_t)r;
            hdr_buf[hdr_used] = '\0';
            body_start = strstr(hdr_buf, "\r\n\r\n");
            if (body_start) {
                *body_start = '\0';
                body_start += 4;
                break;
            }
        }

        if (!body_start) {
            conn_close(&conn);
            snprintf(err, err_cap, "Invalid HTTP response headers from %s", u.host);
            return -1;
        }

        int status_code = 0;
        if (sscanf(hdr_buf, "HTTP/%*d.%*d %d", &status_code) != 1) {
            conn_close(&conn);
            snprintf(err, err_cap, "Malformed HTTP status line from %s", u.host);
            return -1;
        }

        char location[1024] = {0};
        uint64_t content_len = 0;
        bool is_chunked = false;

        char *line = strchr(hdr_buf, '\n');
        while (line) {
            line++;
            char *next_line = strchr(line, '\n');
            if (next_line) *next_line = '\0';

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
            line = next_line;
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
            snprintf(err, err_cap, "HTTP %d for %s", status_code, current_url);
            return -1;
        }

        size_t initial_body_len = hdr_used - (size_t)(body_start - hdr_buf);

        if (!is_chunked) {
            if (initial_body_len > 0) {
                if (sink(userdata, (const unsigned char *)body_start, initial_body_len, content_len) != 0) {
                    conn_close(&conn);
                    snprintf(err, err_cap, "Write error while receiving %s", current_url);
                    return -1;
                }
            }
            unsigned char buf[65536];
            for (;;) {
                ssize_t r = conn_read_some(&conn, buf, sizeof(buf));
                if (r < 0) {
                    conn_close(&conn);
                    snprintf(err, err_cap, "Network read error from %s", u.host);
                    return -1;
                }
                if (r == 0) break;
                if (sink(userdata, buf, (size_t)r, content_len) != 0) {
                    conn_close(&conn);
                    snprintf(err, err_cap, "Write error while receiving %s", current_url);
                    return -1;
                }
            }
        } else {
            /* Decode HTTP chunked transfer-encoding */
            size_t cbuf_cap = 65536;
            unsigned char *cbuf = malloc(cbuf_cap);
            if (!cbuf) {
                conn_close(&conn);
                snprintf(err, err_cap, "Out of memory");
                return -1;
            }
            size_t cbuf_len = initial_body_len;
            memcpy(cbuf, body_start, initial_body_len);

            bool done_chunks = false;
            while (!done_chunks) {
                /* Ensure we have a full chunk size line ending in \r\n */
                char *crlf = NULL;
                while (!(crlf = memmem(cbuf, cbuf_len, "\r\n", 2))) {
                    if (cbuf_len + 16384 > cbuf_cap) {
                        cbuf_cap *= 2;
                        unsigned char *nb = realloc(cbuf, cbuf_cap);
                        if (!nb) {
                            free(cbuf);
                            conn_close(&conn);
                            return -1;
                        }
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

                if (chunk_sz == 0) {
                    done_chunks = true;
                    break;
                }

                while (cbuf_len < chunk_sz + 2) {
                    if (cbuf_len + 65536 > cbuf_cap) {
                        cbuf_cap = (cbuf_len + chunk_sz + 65536);
                        unsigned char *nb = realloc(cbuf, cbuf_cap);
                        if (!nb) {
                            free(cbuf);
                            conn_close(&conn);
                            return -1;
                        }
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
                    return -1;
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

    snprintf(err, err_cap, "Too many HTTP redirects for %s", url);
    return -1;
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} MemSink;

static int mem_sink_cb(void *userdata, const unsigned char *data, size_t len, uint64_t content_len) {
    (void)content_len;
    MemSink *m = (MemSink *)userdata;
    if (m->len + len + 1 > m->cap) {
        size_t ncap = m->cap == 0 ? 16384 : m->cap * 2;
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

int https_get_Buffer(const char *url, const char *accept_hdr, const char *bearer_token, char **out_buf, size_t *out_len, char *err, size_t err_cap) {
    MemSink m = {NULL, 0, 0};
    if (https_request_sink(url, accept_hdr, bearer_token, mem_sink_cb, &m, err, err_cap) != 0) {
        free(m.data);
        return -1;
    }
    if (!m.data) {
        m.data = calloc(1, 1);
    }
    *out_buf = m.data;
    if (out_len) *out_len = m.len;
    return 0;
}

typedef struct {
    FILE *fp;
    uint64_t downloaded;
    uint64_t last_report;
    uint64_t expected_total;
} FileSink;

static int file_sink_cb(void *userdata, const unsigned char *data, size_t len, uint64_t content_len) {
    FileSink *fs = (FileSink *)userdata;
    if (content_len > 0 && fs->expected_total == 0) {
        fs->expected_total = content_len;
    }
    if (fwrite(data, 1, len, fs->fp) != len) {
        return -1;
    }
    fs->downloaded += len;
    if (fs->expected_total > 0 && fs->downloaded - fs->last_report >= 8ULL * 1024 * 1024) {
        fs->last_report = fs->downloaded;
        double pct = ((double)fs->downloaded / (double)fs->expected_total) * 100.0;
        char d_str[64], t_str[64];
        format_bytes(fs->downloaded, d_str, sizeof(d_str));
        format_bytes(fs->expected_total, t_str, sizeof(t_str));
        fprintf(stderr, "\r  %.1f%% (%s / %s)", pct, d_str, t_str);
        fflush(stderr);
    }
    return 0;
}

int download_file(const char *url, const char *dest, uint64_t expected_size, char *err, size_t err_cap) {
    if (mkdir_parent(dest, err, err_cap) != 0) return -1;

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
        snprintf(err, err_cap, "Failed to create %s: %s", tmp_dest, strerror(errno));
        return -1;
    }

    FileSink fs = {fp, 0, 0, expected_size};
    int rc = https_request_sink(url, "*/*", NULL, file_sink_cb, &fs, err, err_cap);
    fclose(fp);
    if (rc != 0) {
        unlink(tmp_dest);
        return -1;
    }

    if (fs.expected_total > 0) {
        char d_str[64], t_str[64];
        format_bytes(fs.downloaded, d_str, sizeof(d_str));
        format_bytes(fs.expected_total, t_str, sizeof(t_str));
        fprintf(stderr, "\r  100.0%% (%s / %s)\n", d_str, t_str);
    }

    if (rename(tmp_dest, dest) != 0) {
        unlink(tmp_dest);
        snprintf(err, err_cap, "Failed to finalize %s: %s", dest, strerror(errno));
        return -1;
    }
    return 0;
}

/* =========================================================================
 * SHA-256 / SHA-512 Checksum Verification
 * ========================================================================= */

int fetch_expected_checksum(const ReleaseAsset *checksum_asset, char *out_hex, size_t out_cap, char *err, size_t err_cap) {
    char *text = NULL;
    size_t len = 0;
    if (https_get_Buffer(checksum_asset->download_url, "*/*", NULL, &text, &len, err, err_cap) != 0) {
        return -1;
    }
    char *p = text;
    while (*p && isspace((unsigned char)*p)) p++;
    size_t i = 0;
    while (p[i] && !isspace((unsigned char)p[i]) && i + 1 < out_cap) {
        out_hex[i] = (char)tolower((unsigned char)p[i]);
        i++;
    }
    out_hex[i] = '\0';
    free(text);

    if (i == 0) {
        snprintf(err, err_cap, "Empty checksum file: %s", checksum_asset->name);
        return -1;
    }
    return 0;
}

int compute_file_checksum(const char *archive_path, bool use_sha256, char *out_hex, size_t out_cap, char *err, size_t err_cap) {
    FILE *f = fopen(archive_path, "rb");
    if (!f) {
        snprintf(err, err_cap, "Failed to open %s for hashing: %s", archive_path, strerror(errno));
        return -1;
    }

    unsigned char buf[65536];
    unsigned char digest[64];
    size_t digest_len = use_sha256 ? 32 : 64;

    if (use_sha256) {
        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            mbedtls_sha256_update(&ctx, buf, n);
        }
        mbedtls_sha256_finish(&ctx, digest);
        mbedtls_sha256_free(&ctx);
    } else {
        mbedtls_sha512_context ctx;
        mbedtls_sha512_init(&ctx);
        mbedtls_sha512_starts(&ctx, 0);
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            mbedtls_sha512_update(&ctx, buf, n);
        }
        mbedtls_sha512_finish(&ctx, digest);
        mbedtls_sha512_free(&ctx);
    }
    fclose(f);

    if (out_cap < digest_len * 2 + 1) {
        snprintf(err, err_cap, "Checksum buffer too small");
        return -1;
    }
    for (size_t i = 0; i < digest_len; i++) {
        snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
    }
    out_hex[digest_len * 2] = '\0';
    return 0;
}

int verify_checksum(const char *archive_path, const ReleaseAsset *checksum_asset, char *out_hex, size_t out_cap, char *err, size_t err_cap) {
    char expected_hex[160];
    if (fetch_expected_checksum(checksum_asset, expected_hex, sizeof(expected_hex), err, err_cap) != 0) {
        return -1;
    }
    size_t nlen = strlen(checksum_asset->name);
    bool use_sha256 = (nlen >= 10 && strcasecmp(checksum_asset->name + nlen - 10, ".sha256sum") == 0) ||
                      strlen(expected_hex) == 64;
    char actual_hex[160];
    if (compute_file_checksum(archive_path, use_sha256, actual_hex, sizeof(actual_hex), err, err_cap) != 0) {
        return -1;
    }

    if (strcasecmp(actual_hex, expected_hex) != 0) {
        unlink(archive_path);
        snprintf(err, err_cap,
                 "Checksum mismatch for %s!\n  Expected: %s\n  Actual:   %s",
                 archive_path, expected_hex, actual_hex);
        return -1;
    }

    fprintf(stderr, "Checksum verified (%s).\n", checksum_asset->name);
    snprintf(out_hex, out_cap, "%s", expected_hex);
    return 0;
}

/* =========================================================================
 * Streaming Decompressor (Gzip / XZ / Zstd) + POSIX/PAX/GNU Tar Extractor
 * ========================================================================= */

typedef enum {
    COMP_GZIP,
    COMP_XZ,
    COMP_ZSTD
} CompFormat;

typedef struct {
    CompFormat fmt;
    FILE *fp;
    unsigned char in_buf[65536];
    size_t in_pos;
    size_t in_len;
    bool eof_in;
    bool stream_done;

    /* GZIP (miniz tinfl) */
    tinfl_decompressor tinfl;
    unsigned char gz_dict[TINFL_LZ_DICT_SIZE];
    size_t gz_dict_ofs;
    size_t gz_avail;
    size_t gz_read_ofs;
    bool gz_hdr_parsed;

    /* XZ (xz-embedded) */
    struct xz_dec *xz;
    struct xz_buf xzb;

    /* ZSTD (zstddeclib) */
    ZSTD_DCtx *zstd;
} DecompStream;

static int gzip_skip_header(DecompStream *ds) {
    /* Read at least 10 bytes for gzip header */
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

    if (flg & 4) { /* FEXTRA */
        if (ds->in_len - ds->in_pos < 2) return -1;
        size_t xlen = (size_t)ds->in_buf[ds->in_pos] | ((size_t)ds->in_buf[ds->in_pos + 1] << 8);
        ds->in_pos += 2 + xlen;
    }
    if (flg & 8) { /* FNAME */
        while (ds->in_pos < ds->in_len && ds->in_buf[ds->in_pos++] != 0) {}
    }
    if (flg & 16) { /* FCOMMENT */
        while (ds->in_pos < ds->in_len && ds->in_buf[ds->in_pos++] != 0) {}
    }
    if (flg & 2) { /* FHCRC */
        ds->in_pos += 2;
    }
    ds->gz_hdr_parsed = true;
    return 0;
}

static ssize_t decomp_read(DecompStream *ds, unsigned char *out, size_t out_cap, char *err, size_t err_cap) {
    if (ds->stream_done) return 0;

    if (ds->fmt == COMP_GZIP) {
        if (!ds->gz_hdr_parsed && gzip_skip_header(ds) != 0) {
            snprintf(err, err_cap, "Invalid gzip header");
            return -1;
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

            tinfl_status status = tinfl_decompress(
                &ds->tinfl,
                ds->in_buf + ds->in_pos,
                &in_bytes,
                ds->gz_dict,
                ds->gz_dict + ds->gz_dict_ofs,
                &out_bytes,
                flags);

            ds->in_pos += in_bytes;
            if (out_bytes > 0) {
                ds->gz_read_ofs = ds->gz_dict_ofs;
                ds->gz_avail = out_bytes;
                ds->gz_dict_ofs = (ds->gz_dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
            }
            if (status == TINFL_STATUS_DONE) {
                if (ds->gz_avail == 0) ds->stream_done = true;
                break;
            }
            if (status < 0) {
                snprintf(err, err_cap, "Gzip decompression failed (status %d)", (int)status);
                return -1;
            }
            if (in_bytes == 0 && out_bytes == 0 && ds->eof_in) {
                ds->stream_done = true;
                break;
            }
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
            if (ret == XZ_STREAM_END) {
                ds->stream_done = true;
                break;
            }
            if (ret != XZ_OK) {
                snprintf(err, err_cap, "XZ decompression failed (code %d)", (int)ret);
                return -1;
            }
            if (ds->xzb.out_pos == 0 && ds->eof_in && ds->xzb.in_pos == ds->xzb.in_size) {
                ds->stream_done = true;
                break;
            }
        }
        return (ssize_t)ds->xzb.out_pos;
    }

    if (ds->fmt == COMP_ZSTD) {
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
            if (ZSTD_isError(ret)) {
                snprintf(err, err_cap, "Zstd decompression failed: %s", ZSTD_getErrorName(ret));
                return -1;
            }
        }
        return (ssize_t)zob.pos;
    }

    return -1;
}

static int decomp_read_exact(DecompStream *ds, unsigned char *buf, size_t n, size_t *out_got, char *err, size_t err_cap) {
    size_t total = 0;
    while (total < n) {
        ssize_t r = decomp_read(ds, buf + total, n - total, err, err_cap);
        if (r < 0) return -1;
        if (r == 0) break;
        total += (size_t)r;
    }
    *out_got = total;
    return 0;
}

static int decomp_skip_exact(DecompStream *ds, uint64_t n, char *err, size_t err_cap) {
    unsigned char discard[16384];
    while (n > 0) {
        size_t chunk = n < sizeof(discard) ? (size_t)n : sizeof(discard);
        size_t got = 0;
        if (decomp_read_exact(ds, discard, chunk, &got, err, err_cap) != 0) return -1;
        if (got == 0) break;
        n -= got;
    }
    return 0;
}

static uint64_t parse_tar_octal(const char *field, size_t len) {
    /* Support base-256 (GNU binary size extension) if high bit is set */
    if ((unsigned char)field[0] & 0x80) {
        uint64_t v = 0;
        for (size_t i = 1; i < len; i++) {
            v = (v << 8) | (unsigned char)field[i];
        }
        return v;
    }
    uint64_t v = 0;
    for (size_t i = 0; i < len && field[i]; i++) {
        if (field[i] >= '0' && field[i] <= '7') {
            v = (v << 3) | (uint64_t)(field[i] - '0');
        }
    }
    return v;
}

static void parse_pax_records(const char *buf, size_t len, char *pax_path, size_t path_cap,
                              char *pax_link, size_t link_cap, uint64_t *pax_size, bool *has_pax_size) {
    size_t pos = 0;
    while (pos < len) {
        char *sp = memchr(buf + pos, ' ', len - pos);
        if (!sp) break;
        size_t rec_len = (size_t)strtoull(buf + pos, NULL, 10);
        if (rec_len == 0 || pos + rec_len > len) break;

        const char *kv = sp + 1;
        size_t kv_len = (pos + rec_len) - (size_t)(kv - buf);
        if (kv_len > 0 && kv[kv_len - 1] == '\n') kv_len--;

        if (kv_len > 5 && strncmp(kv, "path=", 5) == 0) {
            size_t vlen = kv_len - 5;
            if (vlen >= path_cap) vlen = path_cap - 1;
            memcpy(pax_path, kv + 5, vlen);
            pax_path[vlen] = '\0';
        } else if (kv_len > 9 && strncmp(kv, "linkpath=", 9) == 0) {
            size_t vlen = kv_len - 9;
            if (vlen >= link_cap) vlen = link_cap - 1;
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
    const char *s = p;
    while (*s) {
        if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || s[2] == '\0')) {
            return false;
        }
        while (*s && *s != '/') s++;
        while (*s == '/') s++;
    }
    return true;
}

static int unpack_tar_stream(DecompStream *ds, const char *staging_dir, char *err, size_t err_cap) {
    char next_path[PATH_MAX_LEN] = {0};
    char next_link[PATH_MAX_LEN] = {0};
    uint64_t next_size = 0;
    bool has_next_size = false;

    unsigned char blk[512];
    for (;;) {
        size_t got = 0;
        if (decomp_read_exact(ds, blk, 512, &got, err, err_cap) != 0) return -1;
        if (got == 0) break;
        if (got < 512) {
            snprintf(err, err_cap, "Truncated tar header");
            return -1;
        }

        bool all_zero = true;
        for (size_t i = 0; i < 512; i++) {
            if (blk[i] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) continue;

        char name[101] = {0};
        char linkname[101] = {0};
        char prefix[156] = {0};
        memcpy(name, blk, 100);
        memcpy(linkname, blk + 157, 100);
        if (memcmp(blk + 257, "ustar", 5) == 0) {
            memcpy(prefix, blk + 345, 155);
        }

        mode_t mode = (mode_t)parse_tar_octal((const char *)blk + 100, 8);
        uint64_t size = has_next_size ? next_size : parse_tar_octal((const char *)blk + 124, 12);
        has_next_size = false;
        char typeflag = (char)blk[156];
        if (typeflag == '\0') typeflag = '0';

        /* PAX extended header ('x' or 'g') or GNU LongName ('L') / LongLink ('K') */
        if (typeflag == 'x' || typeflag == 'g' || typeflag == 'L' || typeflag == 'K') {
            uint64_t padded = (size + 511ULL) & ~511ULL;
            char *meta = calloc(1, (size_t)padded + 1);
            if (!meta) return -1;
            size_t mgot = 0;
            if (decomp_read_exact(ds, (unsigned char *)meta, (size_t)padded, &mgot, err, err_cap) != 0) {
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

        char relpath[PATH_MAX_LEN];
        if (next_path[0]) {
            snprintf(relpath, sizeof(relpath), "%s", next_path);
            next_path[0] = '\0';
        } else if (prefix[0]) {
            snprintf(relpath, sizeof(relpath), "%s/%s", prefix, name);
        } else {
            snprintf(relpath, sizeof(relpath), "%s", name);
        }

        char target_link[PATH_MAX_LEN];
        if (next_link[0]) {
            snprintf(target_link, sizeof(target_link), "%s", next_link);
            next_link[0] = '\0';
        } else {
            snprintf(target_link, sizeof(target_link), "%s", linkname);
        }

        /* Strip leading "./" */
        char *clean_rel = relpath;
        while (clean_rel[0] == '.' && clean_rel[1] == '/') clean_rel += 2;

        if (*clean_rel == '\0' || strcmp(clean_rel, ".") == 0) {
            uint64_t padded = (size + 511ULL) & ~511ULL;
            if (decomp_skip_exact(ds, padded, err, err_cap) != 0) return -1;
            continue;
        }

        if (!is_safe_relpath(clean_rel)) {
            snprintf(err, err_cap, "Refusing unsafe tar entry path: %s", clean_rel);
            return -1;
        }

        char dest_path[PATH_MAX_LEN];
        snprintf(dest_path, sizeof(dest_path), "%s/%s", staging_dir, clean_rel);

        if (typeflag == '5' || clean_rel[strlen(clean_rel) - 1] == '/') {
            if (mkdir_p(dest_path, err, err_cap) != 0) return -1;
            chmod(dest_path, (mode & 0777) | 0700);
            uint64_t padded = (size + 511ULL) & ~511ULL;
            if (decomp_skip_exact(ds, padded, err, err_cap) != 0) return -1;
        } else if (typeflag == '2') { /* Symlink */
            if (mkdir_parent(dest_path, err, err_cap) != 0) return -1;
            unlink(dest_path);
            if (symlink(target_link, dest_path) != 0) {
                snprintf(err, err_cap, "Failed to create symlink %s -> %s: %s",
                         dest_path, target_link, strerror(errno));
                return -1;
            }
            uint64_t padded = (size + 511ULL) & ~511ULL;
            if (decomp_skip_exact(ds, padded, err, err_cap) != 0) return -1;
        } else if (typeflag == '1') { /* Hardlink */
            if (mkdir_parent(dest_path, err, err_cap) != 0) return -1;
            char *clean_link = target_link;
            while (clean_link[0] == '.' && clean_link[1] == '/') clean_link += 2;
            if (!is_safe_relpath(clean_link)) {
                snprintf(err, err_cap, "Unsafe hardlink target: %s", clean_link);
                return -1;
            }
            char link_src[PATH_MAX_LEN];
            snprintf(link_src, sizeof(link_src), "%s/%s", staging_dir, clean_link);
            unlink(dest_path);
            if (link(link_src, dest_path) != 0) {
                snprintf(err, err_cap, "Failed to create hardlink %s -> %s: %s",
                         dest_path, link_src, strerror(errno));
                return -1;
            }
            uint64_t padded = (size + 511ULL) & ~511ULL;
            if (decomp_skip_exact(ds, padded, err, err_cap) != 0) return -1;
        } else if (typeflag == '0' || typeflag == '7') { /* Regular file */
            if (mkdir_parent(dest_path, err, err_cap) != 0) return -1;
            unlink(dest_path);
            FILE *out_fp = fopen(dest_path, "wb");
            if (!out_fp) {
                snprintf(err, err_cap, "Failed to create %s: %s", dest_path, strerror(errno));
                return -1;
            }
            uint64_t rem = size;
            unsigned char fbuf[65536];
            while (rem > 0) {
                size_t chunk = rem < sizeof(fbuf) ? (size_t)rem : sizeof(fbuf);
                size_t fgot = 0;
                if (decomp_read_exact(ds, fbuf, chunk, &fgot, err, err_cap) != 0 || fgot != chunk) {
                    fclose(out_fp);
                    snprintf(err, err_cap, "Unexpected EOF unpacking %s", clean_rel);
                    return -1;
                }
                if (fwrite(fbuf, 1, fgot, out_fp) != fgot) {
                    fclose(out_fp);
                    snprintf(err, err_cap, "Failed writing %s: %s", dest_path, strerror(errno));
                    return -1;
                }
                rem -= fgot;
            }
            fclose(out_fp);
            chmod(dest_path, (mode & 0777) | 0600);

            uint64_t pad = (512ULL - (size & 511ULL)) & 511ULL;
            if (pad > 0 && decomp_skip_exact(ds, pad, err, err_cap) != 0) return -1;
        } else {
            uint64_t padded = (size + 511ULL) & ~511ULL;
            if (decomp_skip_exact(ds, padded, err, err_cap) != 0) return -1;
        }
    }
    return 0;
}

int extract_tar_archive(const char *archive_path, const char *target_parent_dir, char *out_extracted_dir, size_t out_cap, char *err, size_t err_cap) {
    if (mkdir_p(target_parent_dir, err, err_cap) != 0) return -1;

    char staging_dir[PATH_MAX_LEN];
    snprintf(staging_dir, sizeof(staging_dir), "%s/.prm-extract-%d", target_parent_dir, (int)getpid());
    remove_dir_force(staging_dir, err, err_cap);
    if (mkdir_p(staging_dir, err, err_cap) != 0) return -1;

    const char *slash = strrchr(archive_path, '/');
    const char *fname = slash ? slash + 1 : archive_path;
    fprintf(stderr, "Extracting %s ...\n", fname);

    size_t flen = strlen(fname);
    DecompStream *ds = calloc(1, sizeof(DecompStream));
    if (!ds) {
        snprintf(err, err_cap, "Out of memory");
        return -1;
    }

    if ((flen >= 7 && strcasecmp(fname + flen - 7, ".tar.gz") == 0) ||
        (flen >= 4 && strcasecmp(fname + flen - 4, ".tgz") == 0)) {
        ds->fmt = COMP_GZIP;
        tinfl_init(&ds->tinfl);
    } else if (flen >= 7 && strcasecmp(fname + flen - 7, ".tar.xz") == 0) {
        ds->fmt = COMP_XZ;
        xz_crc32_init();
        xz_crc64_init();
        ds->xz = xz_dec_init(XZ_DYNALLOC, 1 << 26);
        if (!ds->xz) {
            free(ds);
            snprintf(err, err_cap, "Failed to initialize XZ decoder");
            return -1;
        }
    } else if (flen >= 8 && strcasecmp(fname + flen - 8, ".tar.zst") == 0) {
        ds->fmt = COMP_ZSTD;
        ds->zstd = ZSTD_createDCtx();
        if (!ds->zstd) {
            free(ds);
            snprintf(err, err_cap, "Failed to initialize Zstd decoder");
            return -1;
        }
    } else {
        free(ds);
        remove_dir_force(staging_dir, err, err_cap);
        snprintf(err, err_cap, "Unsupported archive format: %s", fname);
        return -1;
    }

    ds->fp = fopen(archive_path, "rb");
    if (!ds->fp) {
        if (ds->xz) xz_dec_end(ds->xz);
        if (ds->zstd) ZSTD_freeDCtx(ds->zstd);
        free(ds);
        remove_dir_force(staging_dir, err, err_cap);
        snprintf(err, err_cap, "Failed to open archive %s: %s", archive_path, strerror(errno));
        return -1;
    }

    int rc = unpack_tar_stream(ds, staging_dir, err, err_cap);
    fclose(ds->fp);
    if (ds->xz) xz_dec_end(ds->xz);
    if (ds->zstd) ZSTD_freeDCtx(ds->zstd);
    free(ds);

    if (rc != 0) {
        char dummy[256];
        remove_dir_force(staging_dir, dummy, sizeof(dummy));
        return -1;
    }

    /* Inspect staging_dir for single top-level directory */
    DIR *d = opendir(staging_dir);
    if (!d) {
        snprintf(err, err_cap, "Failed to read staging dir %s", staging_dir);
        return -1;
    }
    char single_name[256] = {0};
    size_t entry_count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        entry_count++;
        if (entry_count == 1) {
            snprintf(single_name, sizeof(single_name), "%s", ent->d_name);
        }
    }
    closedir(d);

    char top_candidate[PATH_MAX_LEN];
    snprintf(top_candidate, sizeof(top_candidate), "%s/%s", staging_dir, single_name);
    struct stat st;
    if (entry_count == 1 && stat(top_candidate, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out_extracted_dir, out_cap, "%s/%s", target_parent_dir, single_name);
        if (file_exists(out_extracted_dir)) {
            if (remove_dir_force(out_extracted_dir, err, err_cap) != 0) return -1;
        }
        if (rename(top_candidate, out_extracted_dir) != 0) {
            snprintf(err, err_cap, "Failed to move extracted folder to %s: %s",
                     out_extracted_dir, strerror(errno));
            return -1;
        }
        char dummy[256];
        remove_dir_force(staging_dir, dummy, sizeof(dummy));
    } else {
        char stem[256];
        archive_stem(fname, stem, sizeof(stem));
        snprintf(out_extracted_dir, out_cap, "%s/%s", target_parent_dir, stem);
        if (file_exists(out_extracted_dir)) {
            if (remove_dir_force(out_extracted_dir, err, err_cap) != 0) return -1;
        }
        if (rename(staging_dir, out_extracted_dir) != 0) {
            snprintf(err, err_cap, "Failed to move staging dir to %s: %s",
                     out_extracted_dir, strerror(errno));
            return -1;
        }
    }

    return 0;
}

/* =========================================================================
 * Force Directory Removal (handles read-only 0555 files in GE-Proton)
 * ========================================================================= */

static void make_writable_recursive(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0 || S_ISLNK(st.st_mode)) return;

    if (S_ISDIR(st.st_mode)) {
        chmod(path, 0755);
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *ent;
        char child[PATH_MAX_LEN];
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            make_writable_recursive(child);
        }
        closedir(d);
    } else {
        chmod(path, 0644);
    }
}

static int rm_rf_node(const char *path, char *err, size_t err_cap) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return 0;
        snprintf(err, err_cap, "Failed to stat %s: %s", path, strerror(errno));
        return -1;
    }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) {
            snprintf(err, err_cap, "Failed to open directory %s: %s", path, strerror(errno));
            return -1;
        }
        struct dirent *ent;
        char child[PATH_MAX_LEN];
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            if (rm_rf_node(child, err, err_cap) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        if (rmdir(path) != 0) {
            snprintf(err, err_cap, "Failed to remove directory %s: %s", path, strerror(errno));
            return -1;
        }
    } else {
        if (unlink(path) != 0) {
            snprintf(err, err_cap, "Failed to unlink %s: %s", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

int remove_dir_force(const char *path, char *err, size_t err_cap) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    make_writable_recursive(path);
    return rm_rf_node(path, err, err_cap);
}
