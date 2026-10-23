#include "project_form.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define F(p,l,g,k,d,c,t,a,b) {p,l,g,k,d,c,t,a,b}
const sb_form_field sb_form_fields[] = {
 F("id","固定地址标识","基本","text","","","",0,63),
 F("data_dir","项目数据目录（已有数据请用迁移按钮）","基本","directory","","","",0,0),
 F("token_env","凭据引用（已有项目保持不变）","基本","text","","","",0,63),
 F("enabled","启用此项目","基本","bool","true","","",0,0),
 F("default_workspace_id","默认工作目录标识（可留空）","基本","text","","","",0,63),
 F("policy.approval_mode","操作审批方式","权限","enum","always","always|workspace_auto|never","每次操作审批|工作目录文件自动批准|预授权允许的操作",0,0),
 F("policy.files.scope","文件访问范围","权限","enum","configured","configured|current_user","仅已配置目录|当前账户可访问的文件",0,0),
 F("policy.files.current_user_read_only","未列出的账户文件只读（仅账户范围生效）","权限","bool","true","","",0,0),
 F("policy.execution.enabled","允许启动命令任务","权限","bool","false","","",0,0),
 F("policy.execution.default_identity","默认命令身份","权限","enum","restricted","restricted|current_user","受限隔离环境|当前账户",0,0),
 F("policy.execution.administrator_allowed","允许使用本机所有者另行启动的管理员代理","权限","bool","false","","",0,0),
 F("policy.desktop.enabled","允许查看共享屏幕","桌面","bool","false","","",0,0),
 F("policy.desktop.control","允许键盘和鼠标输入","桌面","bool","false","","",0,0),
 F("policy.desktop.clipboard_read","允许读取文本剪贴板","桌面","bool","false","","",0,0),
 F("policy.desktop.clipboard_write","允许写入文本剪贴板","桌面","bool","false","","",0,0),
 F("policy.desktop.lease_seconds","单次桌面控制租约上限（秒）","桌面","number","30","","",1,120),
 F("policy.desktop.emergency_key","紧急停止功能键","桌面","enum","F9","F1|F2|F3|F4|F5|F6|F7|F8|F9|F10|F11","F1|F2|F3|F4|F5|F6|F7|F8|F9|F10|F11",0,0),
 F("approval_ttl_seconds","单次审批有效期（秒）","配额","number","300","","",1,600),
 F("audit_max_bytes","审计文件预算（字节）","配额","number","16777216","","",4096,67108864),
 F("resources.max_active_tasks","同时运行的任务数","配额","number","8","","",1,8),
 F("resources.max_task_records","保留的任务记录数","配额","number","32","","",1,32),
 F("resources.max_output_bytes","每项任务输出预算（字节）","配额","number","262144","","",1024,262144),
 F("resources.max_timeout_ms","任务超时上限（毫秒）","配额","number","3600000","","",100,3600000),
 F("resources.max_approval_records","审批记录数","配额","number","64","","",1,64),
 F("resources.worker_memory_mb","命令内存配额（MiB；后代进程合计采样）","配额","number","512","","",16,4096)
};
const unsigned sb_form_field_count = sizeof(sb_form_fields)/sizeof(sb_form_fields[0]);
yyjson_val *sb_form_get(yyjson_val *root, const char *path) {
    char key[128]; snprintf(key, sizeof(key), "%s", path);
    char *save = NULL;
    for (char *part = strtok_r(key, ".", &save); part; part = strtok_r(NULL, ".", &save)) root = yyjson_obj_get(root, part);
    return root;
}
void sb_form_set(yyjson_mut_doc *d, yyjson_mut_val *root, const char *path, yyjson_mut_val *value) {
    char key[128]; snprintf(key, sizeof(key), "%s", path);
    char *save = NULL, *part = strtok_r(key, ".", &save);
    while (part) {
        char *next = strtok_r(NULL, ".", &save);
        if (!next) { yyjson_mut_obj_remove_key(root, part); yyjson_mut_obj_add(root, yyjson_mut_strcpy(d, part), value); return; }
        yyjson_mut_val *child = yyjson_mut_obj_get(root, part);
        if (!child) { child = yyjson_mut_obj(d); yyjson_mut_obj_add(root, yyjson_mut_strcpy(d, part), child); }
        root = child; part = next;
    }
}
yyjson_mut_doc *sb_form_project(yyjson_val *project, const char *directory) {
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *p = project ? yyjson_val_mut_copy(d, project) : yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, p);
    for (unsigned i = 0; i < sb_form_field_count; i++) {
        const sb_form_field *f = &sb_form_fields[i];
        if (sb_form_get(project, f->path)) continue;
        yyjson_mut_val *v = !strcmp(f->kind, "bool") ? yyjson_mut_bool(d, !strcmp(f->fallback, "true")) :
            !strcmp(f->kind, "number") ? yyjson_mut_uint(d, strtoull(f->fallback, NULL, 10)) : yyjson_mut_str(d, f->fallback);
        sb_form_set(d, p, f->path, v);
    }
    if(!sb_form_get(project,"policy.tools")){
        yyjson_mut_val *tools=yyjson_mut_arr(d);
        for(unsigned i=0;i<6;i++)yyjson_mut_arr_add_str(d,tools,sb_project_tool_name(i));
        sb_form_set(d,p,"policy.tools",tools);
    }
    if (!project) {
        unsigned char b[4]; sb_error e = {0}; char id[32], path[SB_PATH_CAP], env[64];
        if (!sb_platform_random(b, sizeof(b), &e)) { yyjson_mut_doc_free(d); return NULL; }
        snprintf(id, sizeof(id), "project-%02x%02x%02x%02x", b[0],b[1],b[2],b[3]);
        snprintf(path, sizeof(path), "%s/%s", directory, id);
        snprintf(env, sizeof(env), "SHELLBOT_%s", id);
        for (char *s = env; *s; s++) if (*s == '-') *s = '_';
        sb_form_set(d,p,"id",yyjson_mut_strcpy(d,id)); sb_form_set(d,p,"data_dir",yyjson_mut_strcpy(d,path));
        sb_form_set(d,p,"token_env",yyjson_mut_strcpy(d,env));
        sb_form_set(d,p,"workspaces",yyjson_mut_arr(d)); sb_form_set(d,p,"extra_directories",yyjson_mut_arr(d));
        yyjson_mut_val *tools = yyjson_mut_arr(d);
        for (unsigned i=0;i<6;i++) yyjson_mut_arr_add_str(d,tools,sb_project_tool_name(i));
        sb_form_set(d,p,"policy.tools",tools);
    }
    return d;
}
const char *sb_form_tool_label(unsigned i) {
    static const char *labels[] = {"主机信息","主机能力","列举文件","读取文件","搜索文件","查看图片",
        "创建或覆盖文件","修改文件内容","启动命令","任务状态","读取任务输出","写入任务输入","取消任务",
        "审批状态","取消审批","桌面租约","桌面状态","屏幕截图","窗口列表","窗口聚焦","单击","拖动","滚动",
        "输入文字","组合键","读取剪贴板","写入剪贴板","文档信息","读取文档","文档预览","下载产物","上传产物",
        "文件属性","创建目录","复制文件","移动文件","删除文件"};
    return i < sizeof(labels)/sizeof(labels[0]) ? labels[i] : "工具";
}
void sb_form_normalize(yyjson_mut_doc *d, yyjson_mut_val *p) {
    (void)d;
    yyjson_mut_val *policy=yyjson_mut_obj_get(p,"policy"),*files=yyjson_mut_obj_get(policy,"files"),*execution=yyjson_mut_obj_get(policy,"execution");
    const char *scope=yyjson_mut_get_str(yyjson_mut_obj_get(files,"scope"));
    if(!scope||strcmp(scope,"current_user"))yyjson_mut_obj_remove_key(files,"current_user_read_only");
    yyjson_mut_val *runtime=yyjson_mut_obj_get(execution,"runtime_files");
    if(!yyjson_mut_arr_size(runtime))yyjson_mut_obj_remove_key(execution,"runtime_files");
    const char *cwd=yyjson_mut_get_str(yyjson_mut_obj_get(p,"default_workspace_id"));
    if(!cwd||!*cwd)yyjson_mut_obj_remove_key(p,"default_workspace_id");
}
