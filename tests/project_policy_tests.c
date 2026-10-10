#include "sb_projects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition, label) do { if (!(condition)) { fprintf(stderr, "FAIL: %s (line %d)\n", label, __LINE__); return 1; } ++checks; } while (0)

static const char fixture[] =
    "{\"format_version\":1,\"projects\":["
    "{\"id\":\"full\",\"token_env\":\"TOKEN_A\",\"data_dir\":\"C:/M1/state-a\","
    "\"workspaces\":[{\"id\":\"workspace\",\"path\":\"C:/M1/work-a\",\"read_only\":false}],"
    "\"extra_directories\":[{\"id\":\"extra\",\"path\":\"C:/M1/extra-a\"}],"
    "\"policy\":{\"approval_mode\":\"never\",\"files\":{\"scope\":\"current_user\",\"current_user_read_only\":false},"
    "\"execution\":{\"enabled\":true,\"default_identity\":\"current_user\"},"
    "\"tools\":[\"host_info\",\"host_capabilities\",\"fs_list\",\"fs_read\",\"fs_search\",\"image_view\",\"fs_write\",\"fs_patch\","
    "\"task_start\",\"task_status\",\"task_output\",\"task_input\",\"task_cancel\"]}},"
    "{\"id\":\"scoped\",\"token_env\":\"TOKEN_B\",\"data_dir\":\"C:/M1/state-b\","
    "\"workspaces\":[{\"id\":\"workspace\",\"path\":\"C:/M1/work-b\",\"read_only\":false}],"
    "\"policy\":{\"approval_mode\":\"workspace_auto\",\"execution\":{\"enabled\":true,\"default_identity\":\"restricted\"},"
    "\"tools\":[\"fs_read\",\"fs_write\",\"task_start\"]}}]}";

static int migrated_checks(const char *path) {
    sb_project_registry registry = {0};
    sb_error error = {0};
    CHECK(sb_projects_load(&registry, path, &error), "migrated registry loads");
    CHECK(registry.project_count == 1 && registry.projects[0].legacy_compatibility, "explicit legacy project binding");
    sb_project *project = &registry.projects[0];
    sb_request_context context = {.actor = "authenticated-owner", .project = project, .policy_revision = project->policy_revision};
    sb_project_operation operation = {.tool = "fs_read", .directory = &project->directories[0], .identity = SB_ID_CURRENT_USER};
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_ALLOW, "loaded legacy file reads automatic");
    operation.tool = "fs_write";
    CHECK(sb_project_evaluate(&context, &operation).decision == (project->directories[0].read_only ? SB_DECISION_DENY : SB_DECISION_PENDING), "loaded legacy write preserves approval and read-only ceiling");
    operation.tool = "task_start";
    CHECK(sb_project_evaluate(&context, &operation).decision ==
        (project->policy.execution_enabled && project->legacy_profile == SB_EXEC_USER ? SB_DECISION_ALLOW : SB_DECISION_DENY), "loaded legacy account authorization retained without identity fallback");
    operation.identity = SB_ID_ADMINISTRATOR;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "loaded legacy cannot use unbound old administrator agent");
    operation.tool = "fs_read"; operation.directory = NULL; operation.current_user_target = true; operation.identity = SB_ID_CURRENT_USER;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "loaded legacy scope never expands beyond roots");
    sb_projects_free(&registry);
    printf("{\"status\":\"passed\",\"assertions_passed\":%u}\n", checks);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2) return migrated_checks(argv[1]);
    if (argc != 1) return 2;
    sb_error error = {0};
    sb_project_registry registry = {0};
    CHECK(sb_projects_parse(&registry, fixture, strlen(fixture), &error), "two projects parse without opening nonexistent directories");
    CHECK(registry.project_count == 2 && !*registry.default_project_id && registry.port == 8765, "explicit default project and default port");
    sb_project *project = &registry.projects[0], *scoped = &registry.projects[1];
    sb_request_context context = {.actor = "authenticated-owner", .project = project, .policy_revision = project->policy_revision};
    sb_project_operation operation = {.tool = "fs_read", .directory = &project->directories[0], .identity = SB_ID_CURRENT_USER};
    CHECK(sb_project_evaluate(NULL, &operation).decision == SB_DECISION_DENY, "missing authenticated context denied");
    context.actor = NULL;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "missing actor denied");
    context.actor = "authenticated-owner";
    context.policy_revision = 0;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "missing policy revision denied");
    context.policy_revision = 2;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "stale policy denied");
    context.policy_revision = 1;
    operation.requested_project_id = "scoped";
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "client project cannot switch context");
    operation.requested_project_id = "full";
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_ALLOW, "matching project assertion allowed");
    operation.requested_project_id = NULL;
    operation.directory = &scoped->directories[0];
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "same workspace id from another project denied");
    operation.directory = &project->directories[0];
    for (int mode = SB_PROJECT_ALWAYS; mode <= SB_PROJECT_NEVER; ++mode) {
        project->policy.approval_mode = (sb_project_approval)mode;
        for (size_t i = 2; i < 8; ++i) {
            operation.tool = sb_project_tool_name(i);
            operation.directory = &project->directories[0];
            CHECK(sb_project_evaluate(&context, &operation).decision == (mode == SB_PROJECT_ALWAYS ? SB_DECISION_PENDING : SB_DECISION_ALLOW), "workspace file mode includes list search and image");
            operation.directory = &project->directories[1];
            CHECK(sb_project_evaluate(&context, &operation).decision == (i >= 6 ? SB_DECISION_DENY : mode == SB_PROJECT_NEVER ? SB_DECISION_ALLOW : SB_DECISION_PENDING), "extra directories require approval and retain read-only");
            operation.directory = NULL;
            operation.current_user_target = true;
            CHECK(sb_project_evaluate(&context, &operation).decision == (mode == SB_PROJECT_NEVER ? SB_DECISION_ALLOW : SB_DECISION_PENDING), "account file scope approval independent");
            project->policy.file_scope = SB_FILES_CONFIGURED;
            CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "never and pending cannot increase file scope");
            project->policy.file_scope = SB_FILES_CURRENT_USER;
            operation.current_user_target = false;
        }
        operation.tool = "task_start"; operation.directory = &project->directories[0];
        project->directories[1].read_only = false;
        CHECK(sb_project_evaluate(&context, &operation).decision == (mode == SB_PROJECT_NEVER ? SB_DECISION_ALLOW : SB_DECISION_PENDING), "command start approval mode independent");
        operation.tool = "task_input"; operation.owned_task = true;
        CHECK(sb_project_evaluate(&context, &operation).decision == (mode == SB_PROJECT_NEVER ? SB_DECISION_ALLOW : SB_DECISION_PENDING), "stdin and EOF require separate approval");
        operation.owned_task = false;
        CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "stdin refuses unowned task");
        for (size_t i = 9; i < SB_PROJECT_TOOL_COUNT; ++i) {
            if (i == 11) continue;
            operation.tool = sb_project_tool_name(i);
            CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "poll and cancel refuse unowned tasks");
            operation.owned_task = true;
            CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_ALLOW, "poll output cancel do not repeat approval");
            project->enabled = false; project->policy.tools = 0;
            CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_ALLOW, "revocation preserves task result and cancellation");
            project->enabled = true; project->policy.tools = (UINT32_C(1) << SB_PROJECT_TOOL_COUNT) - 1;
            operation.owned_task = false;
        }
        project->directories[1].read_only = true;
    }
    project->policy.approval_mode = SB_PROJECT_NEVER;
    operation.tool = "fs_write"; operation.directory = &project->directories[0];
    project->directories[0].read_only = true;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "never preserves workspace read-only");
    project->directories[0].read_only = false;
    operation.directory = NULL; operation.current_user_target = true;
    project->policy.current_user_read_only = true;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "never preserves account read-only");
    project->policy.current_user_read_only = false;
    operation.directory = &project->directories[0];
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "ambiguous resolver binding denied");
    operation.current_user_target = false;
    project->policy.tools = 0;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "never does not enable tools");
    project->policy.tools = (UINT32_C(1) << SB_PROJECT_TOOL_COUNT) - 1;
    project->enabled = false;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "disabled project refuses new operations");
    project->enabled = true;
    operation.identity = SB_ID_ADMINISTRATOR;
    CHECK(!strcmp(sb_project_evaluate(&context, &operation).reason, "administrator_not_allowed"), "never does not authorize administrator");
    project->policy.administrator_allowed = true;
    CHECK(!strcmp(sb_project_evaluate(&context, &operation).reason, "project_administrator_backend_unavailable"), "dev7 agent cannot impersonate project authorization");
    operation.identity = SB_ID_CURRENT_USER; operation.tool = "task_start";
    CHECK(!strcmp(sb_project_evaluate(&context, &operation).reason, "read_only_command_isolation_unavailable"), "arbitrary commands cannot bypass a read-only grant");
    project->directories[1].read_only = false; project->policy.current_user_read_only = true;
    CHECK(!strcmp(sb_project_evaluate(&context, &operation).reason, "read_only_command_isolation_unavailable"), "arbitrary commands cannot bypass account read-only scope");
    project->policy.current_user_read_only = false;
    project->policy.execution_enabled = false;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "execution switch independent of tools");
    project->policy.execution_enabled = true;
    operation.directory = &project->directories[1];
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "command cwd cannot be extra grant");
    operation.directory = &project->directories[0]; project->policy.file_scope = SB_FILES_CONFIGURED;
    CHECK(!strcmp(sb_project_evaluate(&context, &operation).reason, "directory_isolation_unavailable"), "current account command cannot pretend directory isolation");
    project->policy.default_identity = SB_ID_RESTRICTED;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "ordinary identity cannot replace restricted execution");
    operation.identity = SB_ID_RESTRICTED;
    CHECK(!strcmp(sb_project_evaluate(&context, &operation).reason, "restricted_backend_unverified"), "never cannot enable unfinished sandbox");
    operation.restricted_backend_verified=true;
    CHECK(sb_project_evaluate(&context,&operation).decision==SB_DECISION_ALLOW,"verified restricted backend obeys never within configured scope");
    project->policy.approval_mode=SB_PROJECT_WORKSPACE_AUTO;
    CHECK(sb_project_evaluate(&context,&operation).decision==SB_DECISION_PENDING,"verified restricted commands still require workspace_auto approval");
    project->directories[0].read_only=true;
    CHECK(sb_project_evaluate(&context,&operation).decision==SB_DECISION_PENDING,"verified restricted token enforces readonly without pretending account isolation");
    project->directories[0].read_only=false;project->policy.approval_mode=SB_PROJECT_NEVER;
    operation.restricted_backend_verified=false;
    /* M5 verifies the backend separately from both project permission and the
       operation approval mode. These checks never launch an elevated process. */
    operation.identity=SB_ID_ADMINISTRATOR; project->policy.administrator_allowed=true;
    CHECK(!strcmp(sb_project_evaluate(&context,&operation).reason,"administrator_incompatible_with_isolation"),"administrator never bypasses directory isolation");
    project->policy.file_scope=SB_FILES_CURRENT_USER; project->policy.default_identity=SB_ID_CURRENT_USER;
    CHECK(!strcmp(sb_project_evaluate(&context,&operation).reason,"project_administrator_backend_unavailable"),"project permit does not manufacture an agent");
    operation.administrator_backend_verified=true;
    CHECK(sb_project_evaluate(&context,&operation).decision==SB_DECISION_ALLOW,"verified shared backend permits explicit administrator task in never");
    project->policy.approval_mode=SB_PROJECT_ALWAYS;
    CHECK(sb_project_evaluate(&context,&operation).decision==SB_DECISION_PENDING,"administrator still requires always approval");
    operation.tool="fs_read";
    CHECK(sb_project_evaluate(&context,&operation).decision==SB_DECISION_PENDING,"administrator file read still requires approval");
    project->policy.approval_mode=SB_PROJECT_WORKSPACE_AUTO;
    CHECK(sb_project_evaluate(&context,&operation).decision==SB_DECISION_ALLOW,"administrator workspace file follows workspace_auto");
    operation.tool="task_start";
    CHECK(sb_project_evaluate(&context,&operation).decision==SB_DECISION_PENDING,"administrator workspace command remains pending");
    project->directories[0].read_only=true;
    CHECK(!strcmp(sb_project_evaluate(&context,&operation).reason,"read_only_command_isolation_unavailable"),"administrator command cannot bypass readonly grant");
    operation.tool="fs_write";
    CHECK(!strcmp(sb_project_evaluate(&context,&operation).reason,"read_only"),"administrator file write respects readonly");
    project->directories[0].read_only=false; project->policy.administrator_allowed=false;
    CHECK(!strcmp(sb_project_evaluate(&context,&operation).reason,"administrator_not_allowed"),"agent existence cannot grant a project permission");
    operation.administrator_backend_verified=false; operation.identity=SB_ID_RESTRICTED; operation.tool="task_start";
    project->policy.file_scope=SB_FILES_CONFIGURED; project->policy.default_identity=SB_ID_RESTRICTED; project->policy.approval_mode=SB_PROJECT_NEVER;
    context.project = scoped;
    operation.directory = &scoped->directories[0];
    CHECK(!strcmp(sb_project_evaluate(&context, &operation).reason, "restricted_backend_unverified"), "configured scoped project backend remains closed");
    context.project = project;
    project->legacy_compatibility = true; project->policy.default_identity = SB_ID_CURRENT_USER;
    project->legacy_profile = SB_EXEC_USER; operation.identity = SB_ID_CURRENT_USER; operation.directory = &project->directories[0];
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_ALLOW, "legacy user commands retain account trust and preauthorization");
    operation.tool = "fs_read";
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_ALLOW, "legacy read remains automatic");
    operation.tool = "fs_write";
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_PENDING, "legacy write remains approved");
    project->policy.default_identity = SB_ID_ADMINISTRATOR; operation.tool = "task_start";
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "legacy administrator cannot silently become ordinary execution");
    operation.identity = SB_ID_ADMINISTRATOR;
    CHECK(sb_project_evaluate(&context, &operation).decision == SB_DECISION_DENY, "legacy administrator still needs future trusted project backend");
    sb_projects_free(&registry);
    CHECK(!registry.project_count, "registry free clears ownership");
    printf("{\"status\":\"passed\",\"assertions_passed\":%u}\n", checks);
    return 0;
}
