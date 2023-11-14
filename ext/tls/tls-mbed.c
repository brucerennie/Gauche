/*
 * tls-mbed.c - tls layer using mbedTLS
 *
 *   Copyright (c) 2018 YOKOTA Hiroshi
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *
 *   3. Neither the name of the authors nor the names of its contributors
 *      may be used to endorse or promote products derived from this
 *      software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "gauche-tls.h"
#include <gauche/extend.h>

#if defined(GAUCHE_USE_MBEDTLS)

#include <mbedtls/version.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>

/* Define this to enable mbedtls debugging */
//#define MBEDTLS_DEBUG 1
#undef MBEDTLS_DEBUG

SCM_CLASS_DECL(Scm_MbedTLSClass);

#ifdef HAVE_WINCRYPT_H
#include <wincrypt.h>
#endif

/*
 * Class
 */

static ScmObj mbed_allocate(ScmClass *klass, ScmObj initargs);
static void mbedtls_print(ScmObj, ScmPort*, ScmWriteContext*);
#if MBEDTLS_DEBUG
static void mbed_debug(void*, int, const char*, int, const char *);
#endif

/* NB: We avoid referring Scm_TLSClass statically, since it is in another
   DSO module and some OS doesn't resolve inter-DSO static data.  We
   set the CPA field in init routine. */
SCM_DEFINE_BUILTIN_CLASS(Scm_MbedTLSClass, mbedtls_print, NULL,
                         NULL, mbed_allocate, NULL);

static ScmObj k_server_name;

enum MbedState {
    UNCONNECTED,
    CONNECTED,
    BOUND,
    CLOSED
};

/*
 * Instance
 */
typedef struct ScmMbedTLSRec {
    ScmTLS common;
    enum MbedState state;
    mbedtls_ssl_context ctx;
    mbedtls_net_context conn;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_pk_context pk;
    ScmString *server_name;
} ScmMbedTLS;

/*
 * 'system ca-bundle support
 */
#include "load_system_cert.c"

#ifdef HAVE_WINCRYPT_H
static int mem_loader(ScmTLS *t, BYTE *pbCertEncoded, DWORD cbCertEncoded)
{
    return mbedtls_x509_crt_parse_der(&((ScmMbedTLS*)t)->ca,
                                      pbCertEncoded,
                                      cbCertEncoded);
}

static ScmObj load_system_cert(ScmMbedTLS *t)
{
    return system_cert_loader((ScmTLS*)t, mem_loader);
}
#else
static int file_loader(ScmTLS *t, const char *path)
{
    return mbedtls_x509_crt_parse_file(&((ScmMbedTLS*)t)->ca, path);
}

static ScmObj load_system_cert(ScmMbedTLS *t SCM_UNUSED) {
    return system_cert_loader((ScmTLS*)t, file_loader);
}
#endif

/* fmt must contain one %s and one %d. */
static void mbed_error(const char *fmt, int errcode)
{
    const int bufsiz = 4096;
    char buf[bufsiz];
    mbedtls_strerror(errcode, buf, bufsiz);
    Scm_Error(fmt, buf, errcode);
}

static void mbed_context_check(ScmMbedTLS* t SCM_UNUSED,
                               const char* op SCM_UNUSED)
{
    /* nothing to do (for now) */
}

static void mbed_close_check(ScmMbedTLS* t, const char *op)
{
    if (t->state != CONNECTED) {
        Scm_Error("attempt to %s unconnected or closed TLS: %S", op, t);
    }
}


static ScmObj mbed_connect_common(ScmMbedTLS *t)
{
    if (mbedtls_ssl_config_defaults(&t->conf,
                                    MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
        Scm_SysError("mbedtls_ssl_config_defaults() failed");
    }
    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->ctr_drbg);

    ScmObj ca_bundle_path = SCM_UNDEFINED;
    SCM_BIND_PROC(ca_bundle_path, "tls-ca-bundle-path",
                  SCM_FIND_MODULE("rfc.tls", 0));
    ScmObj s_ca_file = Scm_ApplyRec0(ca_bundle_path);
    if (SCM_FALSEP(s_ca_file)) {
        Scm_Error("mbedTLS: tls-ca-bundle-path isn't set. It is required to"
                  " validate server certs.");
    } else if(Scm_EqP(s_ca_file, SCM_INTERN(GAUCHE_CA_SYSTEM))) {
        if(SCM_FALSEP(load_system_cert(t))) {
            Scm_Error("Can't load certificates from system certificate store");
        }
    } else if(SCM_STRINGP(s_ca_file)) {
        const char *ca_file = Scm_GetStringConst(SCM_STRING(s_ca_file));
        if (mbedtls_x509_crt_parse_file(&t->ca, ca_file) != 0) {
            Scm_SysError("mbedtls_x509_crt_parse_file() failed: file=%S", s_ca_file);
        }
    } else {
        Scm_Error("Parameter tls-ca-bundle-path must have a string value "
                  "or 'system, but got: %S", s_ca_file);
    }

    mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
    mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);

    if (mbedtls_ssl_setup(&t->ctx, &t->conf) != 0) {
        Scm_SysError("mbedtls_ssl_setup() failed");
    }

    const char* hostname = t->server_name ? Scm_GetStringConst(t->server_name) : NULL;
    if(mbedtls_ssl_set_hostname(&t->ctx, hostname) != 0) {
        Scm_SysError("mbedtls_ssl_set_hostname() failed");
    }

    mbedtls_ssl_set_bio(&t->ctx, &t->conn, mbedtls_net_send, mbedtls_net_recv, NULL);

    int r = mbedtls_ssl_handshake(&t->ctx);
    if (r != 0) {
        mbed_error("TLS handshake failed: %s (%d)", r);
    }
    t->state = CONNECTED;
    return SCM_OBJ(t);
}

static ScmObj mbed_connect(ScmTLS *tls, const char *host, const char *port,
                           int proto)
{
    ScmMbedTLS* t = (ScmMbedTLS*)tls;

    mbed_context_check(t, "connect");
    const char* pers = "Gauche";
    if(mbedtls_ctr_drbg_seed(&t->ctr_drbg, mbedtls_entropy_func, &t->entropy,
                             (const unsigned char *)pers, strlen(pers)) != 0) {
        Scm_SysError("mbedtls_ctr_drbg_seed() failed");
    }

    int mbedtls_proto = MBEDTLS_NET_PROTO_TCP;
    if (proto == SCM_TLS_PROTO_UDP) {
        mbedtls_proto = MBEDTLS_NET_PROTO_UDP;
    }

    int r = mbedtls_net_connect(&t->conn, host, port, mbedtls_proto);
    if (r != 0) {
        mbed_error("mbedtls_net_connect() failed: %s (%d)", r);
    }
    return mbed_connect_common(t);
}

static ScmObj mbed_bind(ScmTLS *tls,
                        const char *ip,
                        const char *port, /* numeric or service name */
                        int proto)
{
    SCM_ASSERT(SCM_XTYPEP(tls, &Scm_MbedTLSClass));
    ScmMbedTLS *t = (ScmMbedTLS*)tls;
    if (t->state != UNCONNECTED) {
        Scm_Error("TLS already bound or connected: %S", SCM_OBJ(tls));
    }

    int mbedtls_proto = MBEDTLS_NET_PROTO_TCP;
    if (proto == SCM_TLS_PROTO_UDP) {
        mbedtls_proto = MBEDTLS_NET_PROTO_UDP;
    }

    int r = mbedtls_net_bind(&t->conn, ip, port, mbedtls_proto);
    if (r != 0) {
        mbed_error("mbedtls_net_bind() failed: %s (%d)", r);
    }

    const char* pers = "Gauche";
    r = mbedtls_ctr_drbg_seed(&t->ctr_drbg, mbedtls_entropy_func,
                              &t->entropy,
                              (const unsigned char *)pers, strlen(pers));
    if (r != 0) {
        mbed_error("mbedtls_ctr_drbg_seed() failed: %s (%d)", r);
    }

    r = mbedtls_ssl_config_defaults(&t->conf,
                                    MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) {
        mbed_error("mbedtls_ssl_config_defaults() failed: %s (%d)", r);
    }

    mbedtls_ssl_conf_rng(&t->conf, mbedtls_ctr_drbg_random, &t->ctr_drbg);

    r = mbedtls_ssl_conf_own_cert(&t->conf, &t->ca, &t->pk);
    if (r != 0) {
        mbed_error("mbedtls_ssl_confown_cert() failed: %s (%d)", r);
    }

    t->state = BOUND;
    return SCM_OBJ(t);
}

static ScmObj mbed_accept(ScmTLS* tls) /* tls must already be bound */
{
    SCM_ASSERT(SCM_XTYPEP(tls, &Scm_MbedTLSClass));
    ScmMbedTLS *servt = (ScmMbedTLS*)tls;
    ScmMbedTLS *t = (ScmMbedTLS*)mbed_allocate(Scm_ClassOf(SCM_OBJ(tls)),
                                               SCM_NIL);
    if (servt->state != BOUND) {
        Scm_Error("TLS is not bound: %S", SCM_OBJ(tls));
    }

    int r = mbedtls_ssl_setup(&t->ctx, &servt->conf);
    if (r != 0) {
        mbed_error("mbedtls_ssl_setup() failed: %s (%d)", r);
    }

    /* TODO: Take client address info and save it to newt. */
    r = mbedtls_net_accept(&servt->conn, &t->conn, NULL, 0, NULL);
    if (r != 0) {
        mbed_error("mbedtls_net_accept() failed: %s (%d)", r);
    }

    mbedtls_ssl_set_bio(&t->ctx, &t->conn,
                        mbedtls_net_send, mbedtls_net_recv, NULL);

    r = mbedtls_ssl_handshake(&t->ctx);
    if (r != 0) {
        mbed_error("TLS handshake failed: %s (%d)", r);
    }
    t->state = CONNECTED;
    return SCM_OBJ(t);
}

static ScmObj mbed_read(ScmTLS* tls)
{
    ScmMbedTLS *t = (ScmMbedTLS*)tls;
    mbed_context_check(t, "read");
    mbed_close_check(t, "read");
    uint8_t buf[1024] = {};
    int r;
    r = mbedtls_ssl_read(&t->ctx, buf, sizeof(buf));

    if (r < 0) { Scm_SysError("mbedtls_ssl_read() failed"); }

    return Scm_MakeString((char *)buf, r, r,
                          SCM_STRING_INCOMPLETE | SCM_STRING_COPYING);
}

static ScmObj mbed_write(ScmTLS* tls, ScmObj msg)
{
    ScmMbedTLS *t = (ScmMbedTLS*)tls;
    mbed_context_check(t, "write");
    mbed_close_check(t, "write");

    ScmSize size;
    const uint8_t* cmsg = Scm_GetBytes(msg, &size);

    if (cmsg == NULL) {
        Scm_TypeError("TLS message", "uniform vector or string", msg);
    }
    int r = mbedtls_ssl_write(&t->ctx, cmsg, size);
    if (r < 0) {
        Scm_SysError("mbedtls_ssl_write() failed");
    }
    return SCM_MAKE_INT(r);
}

static ScmObj mbed_close(ScmTLS *tls)
{
    ScmMbedTLS *t = (ScmMbedTLS*)tls;
    if (t->state == CONNECTED) {
        mbedtls_ssl_close_notify(&t->ctx);
        mbedtls_net_free(&t->conn);
        mbedtls_pk_free(&t->pk);
        t->server_name = NULL;
        t->common.in_port = t->common.out_port = SCM_UNDEFINED;
    }
    t->state = CLOSED;
    return SCM_TRUE;
}

static ScmObj mbed_load_certificate(ScmTLS *tls,
                                    const char *filename)
{
    ScmMbedTLS *t = (ScmMbedTLS*)tls;
    int r = mbedtls_x509_crt_parse_file(&t->ca, filename);
    if (r != 0) {
        const int bufsiz = 4096;
        char buf[bufsiz];
        mbedtls_strerror(r, buf, bufsiz);
        Scm_Error("Couldn't load certificate %s: %s (%d)",
                  filename, buf, r);
    }
    return SCM_OBJ(tls);
}

#if MBEDTLS_VERSION_MAJOR >= 3
static int rng_get(void *prng, unsigned char *output, size_t output_len)
{
    mbedtls_ctr_drbg_context *rng = prng;
    return mbedtls_ctr_drbg_random(rng, output, output_len);
}
#endif /*MBEDTLS_VERSION_MAJOR >= 3*/

static ScmObj mbed_load_private_key(ScmTLS *tls,
                                    const char *filename,
                                    const char *password)
{
    ScmMbedTLS *t = (ScmMbedTLS*)tls;
#if MBEDTLS_VERSION_MAJOR < 3
    int r = mbedtls_pk_parse_keyfile(&t->pk, filename, password);
#else  /*MBEDTLS_VERSION_MAJOR >= 3*/
    mbedtls_ctr_drbg_context rng;
    mbedtls_ctr_drbg_init(&rng);
    int r = mbedtls_pk_parse_keyfile(&t->pk, filename, password,
                                     rng_get, &rng);
#endif /*MBEDTLS_VERSION_MAJOR >= 3*/
    if (r != 0) {
        const int bufsiz = 4096;
        char buf[bufsiz];
        mbedtls_strerror(r, buf, bufsiz);
        Scm_Error("Couldn't load certificate %s: %s (%d)",
                  filename, buf, r);
    }
    return SCM_OBJ(tls);
}

static void mbed_finalize(ScmObj obj, void *data SCM_UNUSED)
{
    ScmTLS *t = (ScmTLS*)obj;
    mbed_close(t);
}

static void mbedtls_print(ScmObj obj, ScmPort* port,
                          ScmWriteContext* ctx SCM_UNUSED)
{
    ScmMbedTLS *t = (ScmMbedTLS*)obj;

    Scm_Printf(port, "#<%A", Scm_ShortClassName(SCM_CLASS_OF(obj)));
    if (t->server_name) {
        Scm_Printf(port, " %S", t->server_name);
    }
    switch (t->state) {
    case UNCONNECTED: Scm_Printf(port, " (unconnected)"); break;
    case CONNECTED:   Scm_Printf(port, " (connected)"); break;
    case BOUND:       Scm_Printf(port, " (bound)"); break;
    case CLOSED:      Scm_Printf(port, " (closed)"); break;
    }
    Scm_Printf(port, ">");
}

#if MBEDTLS_DEBUG
void mbed_debug(void *ctx, int level SCM_UNUSED,
                const char *file, int line, const char *str)
{
    fprintf((FILE*)ctx, "%s:%d: %s", file, line, str);
    fflush((FILE*)ctx);
}
#endif /*MBEDTLS_DEBUG*/

static ScmObj mbed_allocate(ScmClass *klass, ScmObj initargs)
{
    ScmMbedTLS* t = SCM_NEW_INSTANCE(ScmMbedTLS, klass);

    ScmObj server_name = Scm_GetKeyword(k_server_name, initargs, SCM_FALSE);
    if (!SCM_STRINGP(server_name) && !SCM_FALSEP(server_name)) {
        Scm_TypeError("mbed-tls server-name", "string or #f", server_name);
    }

    t->state = UNCONNECTED;
    mbedtls_ctr_drbg_init(&t->ctr_drbg);
    mbedtls_net_init(&t->conn);
    mbedtls_ssl_init(&t->ctx);
    mbedtls_ssl_config_init(&t->conf);
    mbedtls_x509_crt_init(&t->ca);
    mbedtls_pk_init(&t->pk);
    mbedtls_entropy_init(&t->entropy);

#if MBEDTLS_DEBUG
    mbedtls_ssl_conf_dbg(&t->conf, mbed_debug, stderr);
#endif

    t->server_name = SCM_STRING(server_name);
    t->common.in_port = t->common.out_port = SCM_UNDEFINED;

    t->common.connect = mbed_connect;
    t->common.bind = mbed_bind;
    t->common.accept = mbed_accept;
    t->common.read = mbed_read;
    t->common.write = mbed_write;
    t->common.close = mbed_close;
    t->common.loadCertificate = mbed_load_certificate;
    t->common.loadPrivateKey = mbed_load_private_key;
    t->common.finalize = mbed_finalize;
    Scm_RegisterFinalizer(SCM_OBJ(t), mbed_finalize, NULL);
    return SCM_OBJ(t);
}

#endif /* defined(GAUCHE_USE_MBEDTLS) */

/*
 * Initialization
 */

void Scm_Init_rfc__tls__mbed()
{
    ScmModule *mod = SCM_MODULE(SCM_FIND_MODULE("rfc.tls.mbed", 0));
#if defined(GAUCHE_USE_MBEDTLS)
    ScmClass **cpa = SCM_NEW_ARRAY(ScmClass*, 4);
    cpa[0] = (ScmClass*)Scm_GlobalVariableRef(SCM_MODULE(SCM_FIND_MODULE("rfc.tls", 0)),
                                              SCM_SYMBOL(SCM_INTERN("<tls>")),
                                              0);
    cpa[1] = SCM_CLASS_CONNECTION;
    cpa[2] = SCM_CLASS_TOP;
    cpa[3] = NULL;
    Scm_MbedTLSClass.cpa = cpa;
    Scm_InitStaticClass(&Scm_MbedTLSClass, "<mbed-tls>", mod, NULL, 0);
    k_server_name = SCM_MAKE_KEYWORD("server-name");
#else
    /* insert dummy binding */
    SCM_DEFINE(mod, "<mbed-tls>", SCM_FALSE);
#endif /* defined(GAUCHE_USE_MBEDTLS) */
}
