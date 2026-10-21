#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "sb.h"
#include "sb_projects.h"
#include "../platform/execution.h"
#include "../platform/manager.h"
#include "../platform/startup.h"
#include "../platform/desktop_app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

_Static_assert(__STDC_VERSION__ >= 201710L, "ShellBot requires C17");

static void usage(void) {
    fputs("ShellBot " SB_VERSION " (C17)\n"
          "Usage: WinShellBot [--stdio | --http] [--config FILE | --root ABSOLUTE_DIR]\n"
          "                  [--data-dir ABSOLUTE_DIR] [--port 1024..65535]\n"
          "                  [--public-url DOMAIN_OR_HTTP_ORIGIN]\n"
          "       WinShellBot --generate-token | --version | --help\n"
          "       WinShellBot --validate-projects FILE\n"
            "       WinShellBot --project-settings [--projects REGISTRY]\n"
            "       WinShellBot --project-remove ID --projects REGISTRY --output NEW\n"
          "       WinShellBot --project-create | --project-configure ID --project-config FILE\n"
          "                  [--projects REGISTRY] --output NEW (never overwrites existing files)\n"
          "       WinShellBot --manager [--http] --projects REGISTRY --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --manage | --management-status --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --startup-prepare --mode tray|service --projects REGISTRY\n"
          "                  --manager-data-dir ABSOLUTE_DIR --output NEW_PROFILE [--start-project ID ...]\n"
          "       WinShellBot --startup-inspect | --startup-console | --resident --startup-profile FILE\n"
          "       WinShellBot --logon-enable | --logon-disable | --logon-status --startup-profile FILE\n"
          "       WinShellBot --service-install | --service-uninstall | --service-start | --service-stop\n"
          "                  --startup-profile FILE (install needs an elevated terminal)\n"
          "       WinShellBot --service-status [--startup-profile FILE]\n"
          "       WinShellBot --project-start | --project-stop ID --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --project-pause | --project-resume ID --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --project-status [ID] --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --project-links ID --manager-data-dir ABSOLUTE_DIR (no token values)\n"
          "       WinShellBot --project-request | --project-owner ID --request FILE\n"
          "                  --manager-data-dir ABSOLUTE_DIR (local owner channel only)\n"
          "       WinShellBot --manager-stop --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --desktop-status | --desktop-pause | --desktop-resume\n"
          "         --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --admin-logon-enable | --admin-logon-disable | --admin-logon-status\n"
          "                  --startup-profile FILE (highest-privilege logon task)\n"
          "       WinShellBot --administrator-start | --administrator-status | --administrator-stop\n"
          "                  --manager-data-dir ABSOLUTE_DIR (start explicitly requests UAC)\n"
          "       WinShellBot --project-administrator-allow | --project-administrator-revoke ID\n"
          "                  --manager-data-dir ABSOLUTE_DIR (owner only; current manager lifetime)\n"
          "       WinShellBot --project-administrator-owner ID --request FILE --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --project-administrator-boundary-probe ID --manager-data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --migrate-config OLD --output NEW --project-id ID\n"
          "                  --project-data-dir NEW_ABSOLUTE_DIR (backs up OLD; does not migrate Data)\n"
          "       WinShellBot --execution-status [--config FILE] [--data-dir ABSOLUTE_DIR]\n"
          "       WinShellBot --check-owner-boundary --data-dir ABSOLUTE_DIR\n"
          "       WinShellBot --list-approvals [--data-dir ABSOLUTE_DIR]\n"
          "       WinShellBot --stop [--data-dir ABSOLUTE_DIR] (HTTP host only)\n"
          "       WinShellBot --inspect | --approve | --deny | --revoke ID [--data-dir ABSOLUTE_DIR]\n"
          "Default: stdio; no implicit workspace. --root creates id 'workspace'.\n"
          "HTTP: 127.0.0.1 only, /mcp and /healthz, bearer from SHELLBOT_TOKEN.\n"
          "Manager HTTP: /projects/ID/mcp, each project's token_env; /mcp needs default_project_id.\n"
          "Roots default to read-only. Create-only writes require explicit configuration and local approval.\n"
          "Existing external tunnels are configured separately. No OAuth or tunnel management.\n", stdout);
}

static char *argument(const wchar_t *wide) {
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    char *text = length > 0 ? malloc((size_t)length) : NULL;
    if (text) WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text, length, NULL, NULL);
    return text;
}

static int project_config_command(int argc, char **argv) {
    sb_error error = {0};
    sb_project_registry registry = {0};
    char *bytes = NULL, *json = NULL, *backup = NULL;
    int status = 2;
    if (!strcmp(argv[1], "--validate-projects")) {
        if (argc != 3 || !*argv[2]) goto invalid;
        if (!sb_projects_load(&registry, argv[2], &error)) goto done;
        json = sb_projects_summary(&registry);
        if (!json) { sb_fail(&error, "Cannot serialize project configuration status."); goto done; }
        puts(json); status = 0; goto done;
    }
    const char *source = NULL, *output = NULL, *id = NULL, *data = NULL;
    for (int i = 1; i < argc; ++i) {
        const char **slot = !strcmp(argv[i], "--migrate-config") ? &source : !strcmp(argv[i], "--output") ? &output :
            !strcmp(argv[i], "--project-id") ? &id : !strcmp(argv[i], "--project-data-dir") ? &data : NULL;
        if (!slot || *slot || i + 1 >= argc || !*argv[i + 1]) goto invalid;
        *slot = argv[++i];
    }
    if (!source || !output || !id || !data || !sb_project_path_valid(output)) goto invalid;
    size_t length = 0;
    bytes = sb_platform_load_config(source, &length, &error);
    if (!bytes) goto done;
    json = sb_projects_migrate(bytes, length, id, data, &error);
    if (!json) goto done;
    size_t capacity = strlen(output) + sizeof(".legacy-backup.json");
    backup = malloc(capacity);
    if (!backup) { sb_fail(&error, "Cannot allocate configuration backup path."); goto done; }
    snprintf(backup, capacity, "%s.legacy-backup.json", output);
    if (!sb_platform_export_config(output, backup, json, strlen(json), bytes, length, &error)) goto done;
    puts("{\"migrated\":true,\"legacy_backup_saved\":true,\"runtime_available\":true,\"runtime_started\":false,\"data_migrated\":false}");
    status = 0;
    goto done;
invalid:
    sb_fail(&error, "Invalid project configuration command. Use --help.");
done:
    if (status) fprintf(stderr, "ShellBot: %s\n", error.text);
    sb_projects_free(&registry); free(bytes); free(json); free(backup);
    return status;
}

static int run(int argc, char **argv) {
    sb_error error = {0};
    if (argc==2 && !strcmp(argv[1],"--desktop")) return sb_desktop_main(NULL);
    if (argc==4 && !strcmp(argv[1],"--desktop") && !strcmp(argv[2],"--app-data-dir")) return sb_desktop_main(argv[3]);
    if(argc>=2 && sb_startup_option(argv[1]))return sb_startup_cli(argc,argv);
    if (argc >= 2 && sb_manager_option(argv[1])) return sb_manager_cli(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--help")) { usage(); return 0; }
    if (argc == 2 && !strcmp(argv[1], "--version")) { puts("ShellBot " SB_VERSION " (C17)"); return 0; }
    if (argc >= 2 && (!strcmp(argv[1], "--validate-projects") || !strcmp(argv[1], "--migrate-config")))
        return project_config_command(argc, argv);
    if (argc == 2 && !strcmp(argv[1], "--generate-token")) {
        unsigned char bytes[32];
        if (!sb_platform_random(bytes, sizeof(bytes), &error)) { fprintf(stderr, "%s\n", error.text); return 1; }
        for (size_t i = 0; i < sizeof(bytes); ++i) printf("%02x", bytes[i]);
        putchar('\n'); SecureZeroMemory(bytes, sizeof(bytes)); return 0;
    }
    bool http = false, transport_set = false, execution_status = false, boundary_probe = false;
    const char *config_path = NULL, *root_path = NULL, *data_dir = NULL, *port = NULL, *public_url = NULL;
    const char *owner_command = NULL, *approval_id = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--execution-status") || !strcmp(argv[i], "--check-owner-boundary")) {
            if (execution_status || boundary_probe) goto bad_args;
            execution_status = !strcmp(argv[i], "--execution-status"); boundary_probe = !execution_status;
        } else if (!strcmp(argv[i], "--stdio") || !strcmp(argv[i], "--http")) {
            if (transport_set) goto bad_args;
            http = !strcmp(argv[i], "--http"); transport_set = true;
        } else if (!strcmp(argv[i], "--list-approvals") || !strcmp(argv[i], "--stop")) {
            if (owner_command) goto bad_args;
            owner_command = !strcmp(argv[i], "--stop") ? "stop" : "list";
        } else if (!strcmp(argv[i], "--inspect") || !strcmp(argv[i], "--approve") ||
                   !strcmp(argv[i], "--deny") || !strcmp(argv[i], "--revoke")) {
            if (owner_command || i + 1 >= argc) goto bad_args;
            owner_command = argv[i] + 2; approval_id = argv[++i];
            if (strlen(approval_id) != 32 || strspn(approval_id, "0123456789abcdef") != 32) goto bad_args;
        } else {
            const char **slot = !strcmp(argv[i], "--config") ? &config_path : !strcmp(argv[i], "--root") ? &root_path :
                !strcmp(argv[i], "--data-dir") ? &data_dir : !strcmp(argv[i], "--port") ? &port :
                !strcmp(argv[i], "--public-url") ? &public_url : NULL;
            if (!slot || *slot || i + 1 >= argc || !*argv[i + 1]) goto bad_args;
            *slot = argv[++i];
        }
    }
    if (config_path && root_path) goto bad_args;
    if (owner_command && (config_path || root_path || port || public_url || transport_set)) goto bad_args;
    if ((execution_status || boundary_probe) && (owner_command || transport_set || root_path || port || public_url)) goto bad_args;
    if (boundary_probe) {
        if (!data_dir || config_path) goto bad_args;
        return sb_win_owner_boundary_probe(data_dir);
    }
    sb_host host = {0};
    sb_config_defaults(&host.config);
    host.started_ms = sb_platform_millis();
    host.http_mode = http;
    if (config_path && !sb_config_load(&host.config, config_path, &error)) goto failed;
    if (root_path && !sb_config_root(&host.config, "workspace", root_path, &error)) goto failed;
    if (public_url && !sb_config_public_url(&host.config, public_url, &error)) goto failed;
    if (data_dir) {
        free(host.config.data_dir); host.config.data_dir = sb_strdup(data_dir);
        if (!host.config.data_dir) { sb_fail(&error, "Out of memory."); goto failed; }
    }
    if (port) {
        char *end = NULL;
        unsigned long value = strtoul(port, &end, 10);
        if (*port < '0' || *port > '9' || *end || value < 1024 || value > 65535) {
            sb_fail(&error, "Invalid HTTP port."); goto failed;
        }
        host.config.port = (unsigned)value;
    }
    if (!host.config.data_dir) host.config.data_dir = sb_platform_default_data_dir(&error);
    if (!host.config.data_dir) goto failed;
    if (!sb_platform_prepare_runtime(host.config.data_dir,&error)) goto failed;
    if (owner_command) {
        int result = sb_owner_command(host.config.data_dir, owner_command, approval_id, &error);
        if (result && *error.text) fprintf(stderr, "ShellBot: %s\n", error.text);
        sb_config_free(&host.config); return result;
    }
    host.execution = sb_execution_new(&host.config, &error);
    if (!host.execution) goto failed;
    if (execution_status) {
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (!doc) { sb_fail(&error, "Cannot allocate execution status."); goto failed; }
        yyjson_mut_doc_set_root(doc, sb_execution_status(host.execution, doc));
        char *json = yyjson_mut_write(doc, 0, NULL);
        yyjson_mut_doc_free(doc);
        if (!json) { sb_fail(&error, "Cannot serialize execution status."); goto failed; }
        puts(json); free(json);
        sb_execution_free(host.execution); sb_config_free(&host.config); return 0;
    }
    host.lease = sb_platform_lease(host.config.data_dir, &error);
    if (!host.lease) goto failed;
    host.audit = sb_audit_new(host.lease, host.config.audit_max_bytes, &error);
    if (!host.audit) goto failed;
    host.policy = sb_policy_new(host.config.approval_ttl_seconds, host.audit, &error);
    if (!host.policy) goto failed;
    host.owner = sb_owner_start(&host, &error);
    if (!host.owner) goto failed;
    int result = http ? sb_http_run(&host, &error) : sb_stdio_run(&host);
    if (result && *error.text) fprintf(stderr, "ShellBot: %s\n", error.text);
    sb_owner_stop(host.owner); sb_policy_free(host.policy); sb_execution_free(host.execution); sb_audit_free(host.audit); sb_platform_lease_close(host.lease);
    sb_config_free(&host.config);
    return result;
failed:
    fprintf(stderr, "ShellBot: %s\n", error.text);
    sb_owner_stop(host.owner); sb_policy_free(host.policy); sb_execution_free(host.execution); sb_audit_free(host.audit); sb_platform_lease_close(host.lease);
    sb_config_free(&host.config);
    return 2;
bad_args:
    fputs("ShellBot: Invalid or duplicate arguments. Use --help.\n", stderr);
    return 2;
}

int wmain(int argc, wchar_t **wide_argv) {
    if (argc >= 2 && !wcscmp(wide_argv[1], L"--project-worker")) return sb_project_worker(argc, wide_argv);
    if (argc >= 2 && !wcscmp(wide_argv[1], L"--execution-agent")) return sb_win_execution_agent(argc, wide_argv);
    if (argc >= 2 && !wcscmp(wide_argv[1], L"--shared-administrator-agent")) return sb_win_execution_agent(argc, wide_argv);
    char **argv = calloc((size_t)argc, sizeof(*argv));
    if (!argv) return 2;
    for (int i = 0; i < argc; ++i) {
        argv[i] = argument(wide_argv[i]);
        if (!argv[i]) { for (int j = 0; j < i; ++j) free(argv[j]); free(argv); return 2; }
    }
    int result = run(argc, argv);
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    return result;
}
