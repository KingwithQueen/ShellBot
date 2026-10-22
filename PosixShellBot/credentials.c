#include "credentials.h"
#include <arpa/inet.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonHMAC.h>
#else
#include <openssl/evp.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#endif
#define SEALED_LIMIT (1024u * 1024u)
#define HEADER 60u
struct sb_secrets {
    sb_root *root;
    unsigned char key[64];
    char context[65];
    const char *provider;
    bool failed;
};
void sb_posix_wipe(void *bytes, size_t length) {
    volatile unsigned char *at = bytes;
    while (length--)
        *at++ = 0;
}
bool sb_secret_hmac(const unsigned char *key, size_t key_length, const void *bytes, size_t length,
                    unsigned char out[32]) {
#ifdef __APPLE__
    CCHmac(kCCHmacAlgSHA256, key, key_length, bytes, length, out);
    return true;
#else
    EVP_MAC *algorithm = EVP_MAC_fetch(NULL, "HMAC", NULL);
    EVP_MAC_CTX *ctx = algorithm ? EVP_MAC_CTX_new(algorithm) : NULL;
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
                           OSSL_PARAM_construct_end()};
    size_t n = 0;
    bool ok = ctx && EVP_MAC_init(ctx, key, key_length, params) == 1 &&
              EVP_MAC_update(ctx, bytes, length) == 1 && EVP_MAC_final(ctx, out, &n, 32) == 1 && n == 32;
    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(algorithm);
    return ok;
#endif
}
static bool cipher(bool encrypt, const unsigned char key[32], const unsigned char iv[16],
                   const unsigned char *input, size_t n, unsigned char *out, size_t *written) {
#ifdef __APPLE__
    return CCCrypt(encrypt ? kCCEncrypt : kCCDecrypt, kCCAlgorithmAES, kCCOptionPKCS7Padding, key, 32, iv,
                   input, n, out, n + 16, written) == kCCSuccess;
#else
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int at = 0, tail = 0;
    bool ok = ctx && EVP_CipherInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv, encrypt ? 1 : 0) == 1 &&
              EVP_CipherUpdate(ctx, out, &at, input, (int)n) == 1 &&
              EVP_CipherFinal_ex(ctx, out + at, &tail) == 1;
    EVP_CIPHER_CTX_free(ctx);
    *written = ok ? (size_t)(at + tail) : 0;
    return ok;
#endif
}
static bool regular(int fd, struct stat *st, sb_error *e) {
    if (fstat(fd, st) || !S_ISREG(st->st_mode) || st->st_nlink != 1 || st->st_uid != geteuid() ||
        (st->st_mode & 077) || st->st_size < 0) {
        sb_fail(e, "Credential file must be a private, single-link regular file owned by this account.");
        return false;
    }
    return true;
}
static char *read_at(sb_root *root, const char *name, size_t *n, bool *missing, sb_error *e) {
    *missing = false;
    *n = 0;
    int fd = openat(root->fd, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) {
            *missing = true;
            return NULL;
        }
        sb_posix_error(e, "Open private credential file");
        return NULL;
    }
    struct stat before, after;
    char *bytes = NULL;
    if (regular(fd, &before, e) && before.st_size > 0 && before.st_size <= SEALED_LIMIT + 256) {
        *n = (size_t)before.st_size;
        bytes = malloc(*n + 1);
        size_t at = 0;
        while (bytes && at < *n) {
            ssize_t got = read(fd, bytes + at, *n - at);
            if (got < 0 && errno == EINTR)
                continue;
            if (got <= 0)
                break;
            at += (size_t)got;
        }
        if (!bytes || at != *n || !regular(fd, &after, e) || before.st_dev != after.st_dev ||
            before.st_ino != after.st_ino || before.st_size != after.st_size ||
            before.st_mtime != after.st_mtime) {
            free(bytes);
            bytes = NULL;
        } else
            bytes[*n] = 0;
    }
    close(fd);
    if (!bytes && !*e->text)
        sb_fail(e, "Credential file is truncated, oversized or changed while reading; existing files were "
                   "retained.");
    return bytes;
}
static bool create_at(sb_root *root, const char *name, const void *bytes, size_t n, sb_error *e) {
    int fd = openat(root->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        sb_posix_error(e, "Create private credential file");
        return false;
    }
    bool ok = sb_posix_write(fd, bytes, n, e) && !fsync(fd);
    close(fd);
    if (!ok && !*e->text)
        sb_posix_error(e, "Flush credential file");
    return ok;
}
static bool nonce(char out[33], sb_error *e) {
    unsigned char b[16];
    if (!sb_platform_random(b, sizeof(b), e))
        return false;
    for (unsigned i = 0; i < 16; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
    return true;
}
static bool replace(sb_secrets *s, const char *name, const void *bytes, size_t n, sb_error *e) {
    char id[33], temporary[128], backup[128], previous[128];
    if (!nonce(id, e))
        return false;
    snprintf(temporary, sizeof(temporary), "%s.pending-%s", name, id);
    snprintf(backup, sizeof(backup), "%s.backup-pending-%s", name, id);
    snprintf(previous, sizeof(previous), "%s.previous", name);
    /* Backup ciphertext only. Never copy decrypted data into a recovery file. */
    bool missing;
    size_t old_length;
    char *old = read_at(s->root, name, &old_length, &missing, e);
    if (!old && !missing)
        return false;
    bool ok = (!old || create_at(s->root, backup, old, old_length, e)) &&
              create_at(s->root, temporary, bytes, n, e) && !fsync(s->root->fd);
    if (ok && old) {
        struct stat st;
        if (!fstatat(s->root->fd, previous, &st, AT_SYMLINK_NOFOLLOW)) {
            if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() || (st.st_mode & 077))
                ok = false;
        } else if (errno != ENOENT)
            ok = false;
        if (ok)
            ok = !renameat(s->root->fd, backup, s->root->fd, previous) && !fsync(s->root->fd);
    }
    free(old);
    if (ok) {
        struct stat st;
        if (!fstatat(s->root->fd, name, &st, AT_SYMLINK_NOFOLLOW)) {
            if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() || (st.st_mode & 077))
                ok = false;
        } else if (errno != ENOENT)
            ok = false;
    }
    if (ok)
        ok = !renameat(s->root->fd, temporary, s->root->fd, name) && !fsync(s->root->fd);
    if (!ok && !*e->text)
        sb_fail(e, "Credential replacement could not be flushed; old ciphertext, backups and pending file "
                   "were retained.");
    return ok;
}
static bool allowed(const char *name) {
    return name && (!strcmp(name, "credentials.sealed") || !strcmp(name, "oauth.sealed") || !strcmp(name, "owner.sealed"));
}
static bool related_files(sb_root *root, const char *name) {
    int fd = openat(root->fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    DIR *d = fd < 0 ? NULL : fdopendir(fd);
    if (!d) {
        if (fd >= 0)
            close(fd);
        return true;
    }
    bool found = false;
    struct dirent *entry;
    size_t n = strlen(name);
    while ((entry = readdir(d)))
        if (!strncmp(entry->d_name, name, n) && (entry->d_name[n] == '.' || !entry->d_name[n])) {
            found = true;
            break;
        }
    closedir(d);
    return found;
}
static bool context(sb_secrets *s, const char *name, unsigned char out[32], sb_error *e) {
    char text[160], hex[65];
    snprintf(text, sizeof(text), "ShellBot sealed v1:%s:%s", s->context, name);
    if (!sb_platform_sha256(text, strlen(text), hex, e))
        return false;
    for (unsigned i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1)
            return false;
        out[i] = (unsigned char)v;
    }
    return true;
}
sb_secrets *sb_secrets_open(sb_lease *lease, const sb_secret_options *options, sb_error *e) {
    if (!lease || !options || (options->key_file && options->keychain)) {
        sb_fail(e, "Select one credential key provider.");
        return NULL;
    }
    sb_secrets *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->root = sb_platform_root(lease->root->path, e);
    if (!s->root || !sb_posix_private(s->root, e))
        goto fail;
    char identity[SB_PATH_CAP + 80];
    snprintf(identity, sizeof(identity), "ShellBot credential namespace v1:%llu:%s",
             (unsigned long long)geteuid(), s->root->path);
    if (!sb_platform_sha256(identity, strlen(identity), s->context, e))
        goto fail;
    if (options->key_file) {
        char *leaf = NULL;
        sb_root *r = sb_platform_account_parent(options->key_file, &leaf, e);
        if (!r)
            goto fail;
        if (!sb_posix_private(r, e) || sb_platform_roots_overlap(r, s->root)) {
            sb_platform_root_close(r);
            free(leaf);
            sb_fail(e,
                    "The headless key file requires a separate private 0700 directory outside manager Data.");
            goto fail;
        }
        bool missing;
        size_t n;
        char *key = read_at(r, leaf, &n, &missing, e);
        sb_platform_root_close(r);
        free(leaf);
        if (!key || n != 64) {
            if (key) {
                sb_posix_wipe(key, n);
                free(key);
            }
            sb_fail(e, "Headless key file is missing or invalid; create it explicitly. No fallback key was "
                       "generated.");
            goto fail;
        }
        memcpy(s->key, key, 64);
        sb_posix_wipe(key, n);
        free(key);
        s->provider = "explicit-private-key-file";
    } else {
        bool existing =
            related_files(s->root, "credentials.sealed") || related_files(s->root, "oauth.sealed") || related_files(s->root, "owner.sealed");
        if (!sb_secret_native_key(s->context, options->keychain, !existing, s->key, e))
            goto fail;
#ifdef __APPLE__
        s->provider = "macOS-Keychain";
#else
        s->provider = "Linux-Secret-Service";
#endif
    }
    return s;
fail:
    sb_secrets_close(s);
    return NULL;
}
void sb_secrets_close(sb_secrets *s) {
    if (s) {
        sb_platform_root_close(s->root);
        sb_posix_wipe(s, sizeof(*s));
        free(s);
    }
}
const char *sb_secrets_provider(sb_secrets *s) { return s ? s->provider : "environment-only"; }
bool sb_secrets_relocate(sb_lease *source, sb_lease *target, const sb_secret_options *options,
    sb_secrets **old_secrets, sb_secrets **new_secrets, sb_error *e) {
    *old_secrets = NULL; *new_secrets = NULL;
    if (sb_platform_roots_overlap(source->root,target->root)) { sb_fail(e,"Credential relocation requires independent source and target directories."); return false; }
    sb_secrets *old = sb_secrets_open(source, options, e);
    if (!old) return false;
    /* These are verified copies in the new, unpublished private destination.
       Recovery ciphertext stays intact in the original source namespace. */
    const char *names[] = {"credentials.sealed", "owner.sealed", "oauth.sealed"};
    char *plain[3] = {0}; size_t length[3] = {0}; bool ok = true;
    for (unsigned i = 0; i < 3 && ok; i++) {
        bool missing;
        plain[i] = sb_secret_read(old, names[i], &length[i], &missing, e);
        ok = plain[i] || missing;
    }
    char *copies[256] = {0}; size_t count = 0;
    int listing = ok ? openat(target->root->fd, ".", O_RDONLY|O_DIRECTORY|O_CLOEXEC) : -1;
    DIR *d = listing >= 0 ? fdopendir(listing) : NULL;
    if (!d) { if (listing >= 0) close(listing); ok = false; }
    while (ok) {
        errno = 0; struct dirent *entry = readdir(d);
        if (!entry) { if (errno) ok = false; break; }
        bool sealed = false;
        for (unsigned i = 0; i < 3; i++) {
            size_t n = strlen(names[i]);
            sealed |= !strncmp(entry->d_name,names[i],n) && (!entry->d_name[n] || entry->d_name[n]=='.');
        }
        if (!sealed) continue;
        struct stat st;
        ok = count < 256 && !fstatat(target->root->fd,entry->d_name,&st,AT_SYMLINK_NOFOLLOW) &&
            S_ISREG(st.st_mode) && st.st_nlink==1 && st.st_uid==geteuid() && !(st.st_mode&077);
        if (ok) { copies[count] = sb_strdup(entry->d_name); ok = copies[count] != NULL; if (ok) count++; }
    }
    if (d) closedir(d);
    for (size_t i=0;i<count;i++) { if (ok && unlinkat(target->root->fd,copies[i],0)) ok=false; free(copies[i]); }
    if (ok && fsync(target->root->fd)) ok=false;
    sb_secrets *next = ok ? sb_secrets_open(target, options, e) : NULL;
    ok = next != NULL;
    for (unsigned i = 0; i < 3 && ok; i++) if (plain[i])
        ok = sb_secret_write(next, names[i], plain[i], length[i], e);
    for (unsigned i = 0; i < 3; i++) if (plain[i]) { sb_posix_wipe(plain[i], length[i]); free(plain[i]); }
    if (!ok) { sb_secrets_close(old); sb_secrets_close(next); return false; }
    *old_secrets = old; *new_secrets = next; return true;
}
char *sb_secret_read(sb_secrets *s, const char *name, size_t *length, bool *missing, sb_error *e) {
    *length = 0;
    *missing = false;
    if (!s || s->failed || !allowed(name)) {
        sb_fail(e, "Encrypted credential store is unavailable.");
        return NULL;
    }
    size_t n;
    char *raw = read_at(s->root, name, &n, missing, e);
    if (!raw) {
        if (*missing && related_files(s->root, name)) {
            *missing = false;
            s->failed = true;
            sb_fail(e, "Current encrypted store is missing but recovery files exist; explicit recovery is "
                       "required. No empty store was created.");
        }
        return NULL;
    }
    unsigned char expected[32], mac[32];
    char *plain = NULL;
    bool ok = n >= HEADER + 16 + 32 && !memcmp(raw, "SBSEAL1\0", 8) && context(s, name, expected, e) &&
              !memcmp(raw + 28, expected, 32);
    uint32_t network = 0;
    if (ok)
        memcpy(&network, raw + 24, 4);
    size_t encrypted = ntohl(network);
    ok = ok && encrypted && encrypted % 16 == 0 && encrypted <= SEALED_LIMIT + 16 &&
         n == HEADER + encrypted + 32 && sb_secret_hmac(s->key + 32, 32, raw, n - 32, mac);
    unsigned difference = 0;
    if (ok)
        for (unsigned i = 0; i < 32; i++)
            difference |= mac[i] ^ (unsigned char)raw[n - 32 + i];
    ok = ok && !difference;
    if (ok) {
        plain = malloc(encrypted + 16 + 1);
        ok = plain &&
             cipher(false, s->key, (unsigned char *)raw + 8, (unsigned char *)raw + HEADER, encrypted,
                    (unsigned char *)plain, length) &&
             *length <= SEALED_LIMIT;
        if (ok)
            plain[*length] = 0;
    }
    sb_posix_wipe(mac, sizeof(mac));
    free(raw);
    if (!ok) {
        if (plain) {
            sb_posix_wipe(plain, encrypted + 16);
            free(plain);
        }
        s->failed = true;
        sb_fail(e, "Credential authentication/decryption failed; existing ciphertext was retained. No empty "
                   "store was created.");
        return NULL;
    }
    return plain;
}
bool sb_secret_write(sb_secrets *s, const char *name, const char *bytes, size_t n, sb_error *e) {
    if (!s || s->failed || !allowed(name) || !n || n > SEALED_LIMIT) {
        sb_fail(e, "Encrypted credential store is unavailable or full.");
        return false;
    }
    bool missing;
    size_t prior_length;
    char *prior = sb_secret_read(s, name, &prior_length, &missing, e);
    if (!prior && !missing)
        return false;
    if (prior) {
        sb_posix_wipe(prior, prior_length);
        free(prior);
    }
    unsigned char *sealed = malloc(HEADER + n + 16 + 32);
    if (!sealed)
        return false;
    memcpy(sealed, "SBSEAL1\0", 8);
    size_t encrypted = 0;
    bool ok = sb_platform_random(sealed + 8, 16, e) && context(s, name, sealed + 28, e) &&
              cipher(true, s->key, sealed + 8, (const unsigned char *)bytes, n, sealed + HEADER, &encrypted);
    uint32_t network = htonl((uint32_t)encrypted);
    memcpy(sealed + 24, &network, 4);
    if (ok)
        ok = sb_secret_hmac(s->key + 32, 32, sealed, HEADER + encrypted, sealed + HEADER + encrypted) &&
             replace(s, name, sealed, HEADER + encrypted + 32, e);
    sb_posix_wipe(sealed, HEADER + n + 16 + 32);
    free(sealed);
    if (!ok) {
        s->failed = true;
        if (!*e->text)
            sb_fail(e, "Encrypted credential storage failed; new authentication changes are blocked.");
    }
    return ok;
}
static yyjson_doc *catalog(sb_secrets *s, sb_error *e) {
    bool missing;
    size_t n;
    char *bytes = sb_secret_read(s, "credentials.sealed", &n, &missing, e);
    if (!bytes && !missing)
        return NULL;
    yyjson_doc *doc =
        missing ? yyjson_read("{\"version\":1,\"projects\":[]}", 27, 0) : yyjson_read(bytes, n, 0);
    if (bytes) {
        sb_posix_wipe(bytes, n);
        free(bytes);
    }
    yyjson_val *v = yyjson_doc_get_root(doc), *projects = yyjson_obj_get(v, "projects");
    bool ok = doc && yyjson_obj_size(v) == 2 && yyjson_get_uint(yyjson_obj_get(v, "version")) == 1 &&
              yyjson_is_arr(projects) && yyjson_arr_size(projects) <= 16;
    for (size_t i = 0; ok && i < yyjson_arr_size(projects); i++) {
        yyjson_val *p = yyjson_arr_get(projects, i);
        const char *id = sb_string(yyjson_obj_get(p, "id")), *env = sb_string(yyjson_obj_get(p, "token_env")),
                   *token = sb_string(yyjson_obj_get(p, "token"));
        ok = yyjson_obj_size(p) == 3 && sb_identifier(id) && env && *env && strlen(env) < 64 && token &&
             strlen(token) >= 32 && strlen(token) <= 128 &&
             strspn(token, "0123456789abcdefABCDEF") == strlen(token);
        for (size_t j = 0; ok && j < i; j++)
            if (!strcmp(id, sb_string(yyjson_obj_get(yyjson_arr_get(projects, j), "id"))))
                ok = false;
    }
    if (!ok) {
        yyjson_doc_free(doc);
        sb_fail(e, "Encrypted project credential catalog is invalid; retained unchanged.");
        return NULL;
    }
    return doc;
}
char *sb_owner_bearer(sb_secrets *s, const char *env, bool create, sb_error *e) {
    const char *external = env && *env ? getenv(env) : NULL;
    char *saved = NULL;
    size_t n = 0;
    bool missing = true;
    if (s) saved = sb_secret_read(s, "owner.sealed", &n, &missing, e);
    if (!saved && !missing) return NULL;
    if (saved && (n != 65 || saved[64] || strspn(saved, "0123456789abcdef") != 64)) {
        sb_posix_wipe(saved, n); free(saved);
        sb_fail(e, "Stored owner credential is corrupt."); return NULL;
    }
    if (external && saved && strcmp(external, saved)) {
        sb_posix_wipe(saved, n); free(saved);
        sb_fail(e, "Owner environment and encrypted credential conflict."); return NULL;
    }
    if (external) {
        size_t size = strlen(external);
        bool valid = size >= 32 && size <= 256;
        for (size_t i = 0; valid && i < size; i++) valid = (unsigned char)external[i] > 32 && (unsigned char)external[i] < 127;
        if (!valid) { if(saved)sb_posix_wipe(saved,strlen(saved));free(saved); sb_fail(e, "Owner bearer requires 32..256 visible ASCII bytes."); return NULL; }
        if (!saved) saved = sb_strdup(external);
    }
    if (!saved && create && s) {
        unsigned char random[32]; char token[65];
        if (!sb_platform_random(random, sizeof(random), e)) return NULL;
        for (unsigned i = 0; i < 32; i++) snprintf(token + i * 2, 3, "%02x", random[i]);
        if (sb_secret_write(s, "owner.sealed", token, sizeof(token), e)) saved = sb_strdup(token);
        sb_posix_wipe(random, sizeof(random)); sb_posix_wipe(token, sizeof(token));
    }
    if (!saved) { sb_fail(e, "Independent owner credential is missing."); return NULL; }
    if(s){
        yyjson_doc *projects=catalog(s,e);bool independent=projects!=NULL;
        yyjson_val *entries=yyjson_obj_get(yyjson_doc_get_root(projects),"projects");
        for(size_t i=0;independent&&i<yyjson_arr_size(entries);i++){
            const char *token=sb_string(yyjson_obj_get(yyjson_arr_get(entries,i),"token"));
            independent=strcmp(saved,token)!=0;
        }
        yyjson_doc_free(projects);
        if(!independent){sb_posix_wipe(saved,strlen(saved));free(saved);if(!*e->text)sb_fail(e,"Owner bearer must be independent of every stored project credential, including disabled projects.");return NULL;}
    }
    size_t saved_length=strlen(saved);
    char *bearer = malloc(saved_length + 8);
    if (bearer) { memcpy(bearer,"Bearer ",7);memcpy(bearer+7,saved,saved_length+1); }
    sb_posix_wipe(saved, strlen(saved)); free(saved);
    return bearer;
}
char *sb_credential_bearer(sb_secrets *s, const char *id, const char *env, sb_error *e) {
    if (!s)
        return sb_posix_bearer(env, e);
    yyjson_doc *doc = catalog(s, e);
    if (!doc)
        return NULL;
    yyjson_val *projects = yyjson_obj_get(yyjson_doc_get_root(doc), "projects");
    char *bearer = NULL;
    bool found = false;
    for (size_t i = 0; i < yyjson_arr_size(projects); i++) {
        yyjson_val *p = yyjson_arr_get(projects, i);
        if (strcmp(id, sb_string(yyjson_obj_get(p, "id"))))
            continue;
        found = true;
        const char *token = sb_string(yyjson_obj_get(p, "token")), *override = getenv(env);
        if (strcmp(env, sb_string(yyjson_obj_get(p, "token_env"))) || (override && strcmp(override, token))) {
            sb_fail(e, "Stored project credential conflicts with token_env; resolve the owner configuration "
                       "explicitly.");
            break;
        }
        size_t n = strlen(token) + 8;
        bearer = malloc(n);
        if (bearer)
            snprintf(bearer, n, "Bearer %s", token);
        break;
    }
    yyjson_doc_free(doc);
    return found ? bearer : sb_posix_bearer(env, e);
}
bool sb_credential_option(const char *s) {
    return !strcmp(s, "--credential-key-create") || !strcmp(s, "--credential-set") ||
           !strcmp(s, "--credential-generate") || !strcmp(s, "--credential-status") ||
           !strcmp(s, "--credential-remove") || !strcmp(s, "--credential-export");
}
int sb_credential_cli(int argc, char **argv) {
    const char *command = NULL, *id = NULL, *data = NULL, *key_create = NULL, *env = NULL, *output = NULL;
    sb_secret_options options = {0};
    sb_error e = {0};
    int result = 2;
    sb_lease *lease = NULL;
    sb_secrets *secrets = NULL;
    yyjson_doc *old = NULL;
    yyjson_mut_doc *d = NULL;
    char *json = NULL;
    char token[130] = {0};
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (sb_credential_option(s)) {
            if (command)
                goto bad;
            command = s;
            if (!strcmp(s, "--credential-status"))
                continue;
            if (i + 1 >= argc)
                goto bad;
            if (!strcmp(s, "--credential-key-create"))
                key_create = argv[++i];
            else
                id = argv[++i];
        } else {
            const char **slot = !strcmp(s, "--manager-data-dir")      ? &data
                                : !strcmp(s, "--credential-key-file") ? &options.key_file
                                : !strcmp(s, "--credential-keychain") ? &options.keychain
                                : !strcmp(s, "--token-env")           ? &env
                                : !strcmp(s, "--output")              ? &output
                                                                      : NULL;
            if (!slot || *slot || i + 1 >= argc)
                goto bad;
            *slot = argv[++i];
        }
    }
    if (key_create) {
        if (data || id || env || output || options.key_file || options.keychain)
            goto bad;
        char *leaf = NULL;
        sb_root *r = sb_platform_account_parent(key_create, &leaf, &e);
        unsigned char key[64];
        bool ok = r && sb_posix_private(r, &e) && sb_platform_random(key, sizeof(key), &e) &&
                  create_at(r, leaf, key, sizeof(key), &e) && !fsync(r->fd);
        sb_posix_wipe(key, sizeof(key));
        free(leaf);
        sb_platform_root_close(r);
        if (ok) {
            puts("{\"created\":true,\"key_material_printed\":false}");
            result = 0;
        }
        goto done;
    }
    if (!command || !data || (id && !sb_identifier(id)))
        goto bad;
    lease = sb_platform_lease(data, &e);
    if (!lease)
        goto done;
    secrets = sb_secrets_open(lease, &options, &e);
    if (!secrets)
        goto done;
    old = catalog(secrets, &e);
    if (!old)
        goto done;
    d = yyjson_mut_doc_new(NULL);
    if (!d)
        goto done;
    yyjson_mut_val *root = yyjson_val_mut_copy(d, yyjson_doc_get_root(old)),
                   *projects = yyjson_mut_obj_get(root, "projects");
    yyjson_mut_doc_set_root(d, root);
    size_t index = 0;
    yyjson_mut_val *entry = NULL;
    for (; index < yyjson_mut_arr_size(projects); index++) {
        yyjson_mut_val *v = yyjson_mut_arr_get(projects, index);
        if (id && !strcmp(yyjson_mut_get_str(yyjson_mut_obj_get(v, "id")), id)) {
            entry = v;
            break;
        }
    }
    if (!strcmp(command, "--credential-status")) {
        if (env || output || id)
            goto bad;
        yyjson_mut_val *list = yyjson_mut_arr(d);
        for (size_t i = 0; i < yyjson_mut_arr_size(projects); i++) {
            yyjson_mut_val *v = yyjson_mut_arr_get(projects, i), *summary = yyjson_mut_obj(d);
            yyjson_mut_obj_add_str(d, summary, "id", yyjson_mut_get_str(yyjson_mut_obj_get(v, "id")));
            yyjson_mut_obj_add_str(d, summary, "token_env",
                                   yyjson_mut_get_str(yyjson_mut_obj_get(v, "token_env")));
            yyjson_mut_arr_add_val(list, summary);
        }
        yyjson_mut_val *summary = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, summary, "provider", sb_secrets_provider(secrets));
        yyjson_mut_obj_add_val(d, summary, "projects", list);
        json = yyjson_mut_val_write(summary, 0, NULL);
        if (json) {
            puts(json);
            result = 0;
        }
        goto done;
    }
    if (!strcmp(command, "--credential-export")) {
        if (!entry || !output || env)
            goto bad;
        char *leaf = NULL;
        sb_root *r = sb_platform_account_parent(output, &leaf, &e);
        const char *value = yyjson_mut_get_str(yyjson_mut_obj_get(entry, "token"));
        bool ok = r && create_at(r, leaf, value, strlen(value), &e) && !fsync(r->fd);
        free(leaf);
        sb_platform_root_close(r);
        if (ok) {
            puts("{\"exported\":true}");
            result = 0;
        }
        goto done;
    }
    if (output)
        goto bad;
    if (!strcmp(command, "--credential-remove")) {
        if (!entry || env)
            goto bad;
        yyjson_mut_arr_remove(projects, index);
    } else {
        if (!env || !*env || strlen(env) >= 64 ||
            strspn(env, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != strlen(env))
            goto bad;
        if (!strcmp(command, "--credential-set")) {
            if (!fgets(token, sizeof(token), stdin))
                goto bad;
            size_t n = strcspn(token, "\r\n");
            token[n] = 0;
            if (n < 32 || n > 128 || strspn(token, "0123456789abcdefABCDEF") != n)
                goto bad;
        } else {
            unsigned char b[32];
            if (!sb_platform_random(b, sizeof(b), &e))
                goto done;
            for (unsigned i = 0; i < 32; i++)
                snprintf(token + 2 * i, 3, "%02x", b[i]);
            sb_posix_wipe(b, sizeof(b));
        }
        yyjson_mut_val *v = yyjson_mut_obj(d);
        yyjson_mut_obj_add_str(d, v, "id", id);
        yyjson_mut_obj_add_str(d, v, "token_env", env);
        yyjson_mut_obj_add_str(d, v, "token", token);
        if (entry)
            yyjson_mut_arr_replace(projects, index, v);
        else if (yyjson_mut_arr_size(projects) < 16)
            yyjson_mut_arr_add_val(projects, v);
        else
            goto bad;
    }
    json = yyjson_mut_write(d, 0, NULL);
    if (json && sb_secret_write(secrets, "credentials.sealed", json, strlen(json), &e)) {
        puts("{\"saved\":true,\"secret_printed\":false}");
        result = 0;
    }
    goto done;
bad:
    sb_fail(&e, "Invalid credential command or arguments. Secrets are read from stdin or generated "
                "internally, never from command-line arguments.");
done:
    if (result && *e.text)
        fprintf(stderr, "ShellBot: %s\n", e.text);
    if (json) {
        sb_posix_wipe(json, strlen(json));
        free(json);
    }
    sb_posix_wipe(token, sizeof(token));
    yyjson_mut_doc_free(d);
    yyjson_doc_free(old);
    sb_secrets_close(secrets);
    sb_platform_lease_close(lease);
    return result;
}
