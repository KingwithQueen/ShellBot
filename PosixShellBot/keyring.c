#include "credentials.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef __APPLE__
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>
/* Legacy keychains also work in SSH sessions and with explicit owner keychains.
   All calls forbid interaction; a locked/ACL-denied key is never replaced. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
bool sb_secret_native_key(const char *namespace_id, const char *path, bool create, unsigned char key[64],
                          sb_error *e) {
    SecKeychainSetUserInteractionAllowed(false);
    SecKeychainRef chain = NULL;
    OSStatus status = path ? SecKeychainOpen(path, &chain) : SecKeychainCopyDefault(&chain);
    SecKeychainStatus state = 0;
    if (status != errSecSuccess || SecKeychainGetStatus(chain, &state) != errSecSuccess ||
        !(state & kSecUnlockStateStatus)) {
        if (chain)
            CFRelease(chain);
        sb_fail(e, "macOS Keychain is locked or unavailable. Unlock it locally or explicitly select a "
                   "private headless key file; no dialog was opened.");
        return false;
    }
    CFStringRef account = CFStringCreateWithCString(NULL, namespace_id, kCFStringEncodingUTF8);
    const void *chains[] = {chain};
    CFArrayRef list = CFArrayCreate(NULL, chains, 1, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef q =
        CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR("org.shellbot.posix.master.v1"));
    CFDictionarySetValue(q, kSecAttrAccount, account);
    CFDictionarySetValue(q, kSecMatchSearchList, list);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    CFDictionarySetValue(q, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
    CFTypeRef result = NULL;
    status = SecItemCopyMatching(q, &result);
    bool ok = false;
    if (status == errSecSuccess) {
        ok = result && CFGetTypeID(result) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)result) == 64;
        if (ok)
            memcpy(key, CFDataGetBytePtr((CFDataRef)result), 64);
    } else if (status == errSecItemNotFound && create) {
        if (sb_platform_random(key, 64, e)) {
            CFDictionaryRemoveValue(q, kSecReturnData);
            CFDictionaryRemoveValue(q, kSecMatchLimit);
            CFDictionaryRemoveValue(q, kSecMatchSearchList);
            CFDictionarySetValue(q, kSecUseKeychain, chain);
            CFDataRef bytes = CFDataCreate(NULL, key, 64);
            CFDictionarySetValue(q, kSecValueData, bytes);
            CFDictionarySetValue(q, kSecAttrLabel, CFSTR("ShellBot encrypted credential key"));
            status = SecItemAdd(q, NULL);
            CFRelease(bytes);
            ok = status == errSecSuccess;
        }
    }
    if (result)
        CFRelease(result);
    CFRelease(q);
    CFRelease(account);
    CFRelease(list);
    CFRelease(chain);
    if (!ok) {
        sb_posix_wipe(key, 64);
        if (!*e->text)
            snprintf(e->text, sizeof(e->text),
                     "macOS Keychain key is missing, locked or access was denied (%d); existing encrypted "
                     "files were retained.",
                     (int)status);
    }
    return ok;
}
#pragma clang diagnostic pop
#else
#include <libsecret/secret.h>
typedef struct {
    SecretService parent;
} SbHeadlessSecrets;
typedef struct {
    SecretServiceClass parent;
} SbHeadlessSecretsClass;
G_DEFINE_TYPE(SbHeadlessSecrets, sb_headless_secrets, SECRET_TYPE_SERVICE)
static GVariant *deny_prompt_sync(SecretService *service, SecretPrompt *prompt, GCancellable *cancel,
                                  const GVariantType *type, GError **error) {
    (void)service;
    (void)prompt;
    (void)cancel;
    (void)type;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "ShellBot never opens keyring prompts in its headless backend.");
    return NULL;
}
static void deny_prompt_async(SecretService *service, SecretPrompt *prompt, const GVariantType *type,
                              GCancellable *cancel, GAsyncReadyCallback callback, gpointer context) {
    (void)prompt;
    (void)type;
    GTask *task = g_task_new(service, cancel, callback, context);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "ShellBot never opens keyring prompts in its headless backend.");
    g_object_unref(task);
}
static GVariant *deny_prompt_finish(SecretService *service, GAsyncResult *result, GError **error) {
    (void)service;
    return g_task_propagate_pointer(G_TASK(result), error);
}
static void sb_headless_secrets_class_init(SbHeadlessSecretsClass *klass) {
    SecretServiceClass *service = SECRET_SERVICE_CLASS(klass);
    service->prompt_sync = deny_prompt_sync;
    service->prompt_async = deny_prompt_async;
    service->prompt_finish = deny_prompt_finish;
}
static void sb_headless_secrets_init(SbHeadlessSecrets *service) { (void)service; }
static const SecretSchema schema = {"org.shellbot.posix.master.v1",
                                    SECRET_SCHEMA_NONE,
                                    {{"namespace", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}},
                                    0,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL};
bool sb_secret_native_key(const char *namespace_id, const char *keychain, bool create, unsigned char key[64],
                          sb_error *e) {
    if (keychain) {
        sb_fail(e, "--credential-keychain is available only on macOS.");
        return false;
    }
    GError *error = NULL;
    SecretService *service = secret_service_open_sync(sb_headless_secrets_get_type(), NULL,
                                                      SECRET_SERVICE_OPEN_SESSION, NULL, &error);
    SecretCollection *collection =
        service ? secret_collection_for_alias_sync(service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE,
                                                   NULL, &error)
                : NULL;
    bool ok = false;
    if (!collection || secret_collection_get_locked(collection)) {
        sb_fail(e, "Linux Secret Service default collection is locked or unavailable. Unlock it locally or "
                   "explicitly select a private headless key file; no unlock was requested.");
        goto done;
    }
    GHashTable *attrs = secret_attributes_build(&schema, "namespace", namespace_id, NULL);
    GList *items = secret_collection_search_sync(
        collection, &schema, attrs, SECRET_SEARCH_ALL | SECRET_SEARCH_LOAD_SECRETS, NULL, &error);
    if (!error && items && !items->next) {
        SecretValue *value = secret_item_get_secret(SECRET_ITEM(items->data));
        gsize n = 0;
        const char *bytes = value ? secret_value_get(value, &n) : NULL;
        ok = bytes && n == 64;
        if (ok)
            memcpy(key, bytes, 64);
        if (value)
            secret_value_unref(value);
    } else if (!error && !items && create && sb_platform_random(key, 64, e)) {
        SecretValue *value = secret_value_new((const char *)key, 64, "application/octet-stream");
        SecretItem *item =
            secret_item_create_sync(collection, &schema, attrs, "ShellBot encrypted credential key", value,
                                    SECRET_ITEM_CREATE_NONE, NULL, &error);
        ok = item != NULL;
        if (item)
            g_object_unref(item);
        secret_value_unref(value);
    }
    g_list_free_full(items, g_object_unref);
    g_hash_table_unref(attrs);
done:
    if (collection)
        g_object_unref(collection);
    if (service)
        g_object_unref(service);
    if (error)
        g_error_free(error);
    if (!ok) {
        sb_posix_wipe(key, 64);
        if (!*e->text)
            sb_fail(e, "Linux Secret Service key is missing, locked, ambiguous or denied; no replacement was "
                       "generated for existing encrypted files.");
    }
    return ok;
}
#endif
