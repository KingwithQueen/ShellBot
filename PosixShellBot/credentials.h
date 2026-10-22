#ifndef SB_POSIX_CREDENTIALS_H
#define SB_POSIX_CREDENTIALS_H
#include "posix.h"
typedef struct sb_secrets sb_secrets;
typedef struct {
    const char *key_file, *keychain;
} sb_secret_options;
void sb_posix_wipe(void *bytes, size_t length);
bool sb_secret_hmac(const unsigned char *key, size_t key_length, const void *bytes, size_t length,
                    unsigned char out[32]);
sb_secrets *sb_secrets_open(sb_lease *lease, const sb_secret_options *options, sb_error *error);
void sb_secrets_close(sb_secrets *secrets);
char *sb_secret_read(sb_secrets *secrets, const char *name, size_t *length, bool *missing, sb_error *error);
bool sb_secret_write(sb_secrets *secrets, const char *name, const char *bytes, size_t length,
                     sb_error *error);
bool sb_secret_native_key(const char *namespace_id, const char *keychain, bool create, unsigned char key[64],
                          sb_error *error);
const char *sb_secrets_provider(sb_secrets *secrets);
char *sb_credential_bearer(sb_secrets *secrets, const char *project, const char *environment,
                           sb_error *error);
bool sb_credential_option(const char *option);
int sb_credential_cli(int argc, char **argv);
#endif
