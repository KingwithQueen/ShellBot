typedef struct {
    GtkWidget *row, *id, *path, *readonly;
    bool file;
} form_row;
typedef struct {
    GtkWidget *dialog, *fields[32], *tools[SB_PROJECT_TOOL_COUNT], *lists[3], *name, *automatic;
    GPtrArray *rows[3];
    yyjson_doc *definition;
    char original[64], expected[65];
} project_editor;
static void form_free(gpointer data) {
    project_editor *ed=data;
    yyjson_doc_free(ed->definition);
    for(unsigned i=0;i<3;i++)g_ptr_array_unref(ed->rows[i]);
    g_free(ed);
}
static void runtime_chosen(GObject *source,GAsyncResult *result,gpointer data){
    GFile *file=gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source),result,NULL);
    if(file){char *path=g_file_get_path(file);if(path)gtk_editable_set_text(GTK_EDITABLE(data),path);g_free(path);g_object_unref(file);}
    g_object_unref(data);
}
static void runtime_choose(GtkButton *b,gpointer data){
    (void)b;GtkFileDialog *d=gtk_file_dialog_new();gtk_file_dialog_open(d,GTK_WINDOW(ui.window),NULL,runtime_chosen,g_object_ref(data));g_object_unref(d);
}
static void row_remove(GtkButton *b,gpointer data){(void)b;form_row *r=data;gtk_widget_set_visible(r->row,FALSE);}
static void form_row_add(project_editor *ed,unsigned type,yyjson_val *value){
    form_row *r=g_new0(form_row,1);r->file=type==2;r->row=box(false,6);r->path=gtk_entry_new();gtk_widget_set_hexpand(r->path,TRUE);
    if(!r->file){r->id=gtk_entry_new();gtk_entry_set_placeholder_text(GTK_ENTRY(r->id),"目录标识");gtk_widget_set_size_request(r->id,110,-1);append(r->row,r->id);
        gtk_editable_set_text(GTK_EDITABLE(r->id),sb_gui_string(value,"id"));}
    gtk_editable_set_text(GTK_EDITABLE(r->path),r->file?(yyjson_get_str(value)?yyjson_get_str(value):""):sb_gui_string(value,"path"));
    gtk_entry_set_placeholder_text(GTK_ENTRY(r->path),"绝对路径");append(r->row,r->path);
    GtkWidget *choose=gtk_button_new_with_label("选择…");append(r->row,choose);
    g_signal_connect(choose,"clicked",r->file?G_CALLBACK(runtime_choose):G_CALLBACK(choose_folder),r->path);
    if(!r->file){r->readonly=gtk_check_button_new_with_label("只读");gtk_check_button_set_active(GTK_CHECK_BUTTON(r->readonly),!value||!yyjson_obj_get(value,"read_only")||sb_gui_bool(value,"read_only"));append(r->row,r->readonly);}
    GtkWidget *remove=gtk_button_new_with_label("移除");append(r->row,remove);g_signal_connect(remove,"clicked",G_CALLBACK(row_remove),r);
    append(ed->lists[type],r->row);g_ptr_array_add(ed->rows[type],r);
}
static void row_clicked(GtkButton *b,gpointer data){project_editor *ed=data;form_row_add(ed,GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b),"type")),NULL);}
static void form_response(GtkButton *b,gpointer data){
    project_editor *ed=data;
    if(!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b),"save"))){gtk_window_destroy(GTK_WINDOW(ed->dialog));return;}
    yyjson_mut_doc *d=yyjson_mut_doc_new(NULL);yyjson_mut_val *p=yyjson_val_mut_copy(d,yyjson_doc_get_root(ed->definition));
    for(unsigned i=0;i<sb_form_field_count;i++){
        const sb_form_field *f=&sb_form_fields[i];GtkWidget *w=ed->fields[i];yyjson_mut_val *v=NULL;
        if(!strcmp(f->kind,"bool"))v=yyjson_mut_bool(d,gtk_check_button_get_active(GTK_CHECK_BUTTON(w)));
        else if(!strcmp(f->kind,"number"))v=yyjson_mut_uint(d,(uint64_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(w)));
        else if(!strcmp(f->kind,"enum")){
            gchar **values=g_strsplit(f->choices,"|",-1);unsigned n=gtk_drop_down_get_selected(GTK_DROP_DOWN(w));
            v=yyjson_mut_strcpy(d,n<g_strv_length(values)?values[n]:f->fallback);g_strfreev(values);
        }else v=yyjson_mut_strcpy(d,gtk_editable_get_text(GTK_EDITABLE(w)));
        sb_form_set(d,p,f->path,v);
    }
    const char *paths[]={"workspaces","extra_directories","policy.execution.runtime_files"};
    for(unsigned type=0;type<3;type++){
        yyjson_mut_val *array=yyjson_mut_arr(d);
        for(unsigned i=0;i<ed->rows[type]->len;i++){
            form_row *r=g_ptr_array_index(ed->rows[type],i);if(!gtk_widget_get_visible(r->row))continue;
            const char *path=gtk_editable_get_text(GTK_EDITABLE(r->path));
            yyjson_mut_val *v=type==2?yyjson_mut_strcpy(d,path):yyjson_mut_obj(d);
            if(type!=2){yyjson_mut_obj_add_strcpy(d,v,"id",gtk_editable_get_text(GTK_EDITABLE(r->id)));yyjson_mut_obj_add_strcpy(d,v,"path",path);yyjson_mut_obj_add_bool(d,v,"read_only",gtk_check_button_get_active(GTK_CHECK_BUTTON(r->readonly)));}
            yyjson_mut_arr_add_val(array,v);
        }
        sb_form_set(d,p,paths[type],array);
    }
    yyjson_mut_val *tools=yyjson_mut_arr(d);for(unsigned i=0;i<SB_PROJECT_TOOL_COUNT;i++)if(gtk_check_button_get_active(GTK_CHECK_BUTTON(ed->tools[i])))yyjson_mut_arr_add_str(d,tools,sb_project_tool_name(i));
    sb_form_set(d,p,"policy.tools",tools);sb_form_normalize(d,p);
    char *definition=yyjson_mut_val_write(p,0,NULL),*config=yyjson_val_write(model("config"),0,NULL);
    const char *id=yyjson_mut_get_str(yyjson_mut_obj_get(p,"id"));sb_error error={0};
    char *validated=definition&&config?sb_projects_edit(config,strlen(config),definition,strlen(definition),id,*ed->original!=0,&error):NULL;
    free(definition);free(config);
    if(!validated){yyjson_mut_doc_free(d);alert(*error.text?error.text:"请检查项目设置。");return;}free(validated);
    yyjson_mut_val *q=yyjson_mut_obj(d);yyjson_mut_obj_add_str(d,q,"action","configure_project");yyjson_mut_obj_add_val(d,q,"definition",p);
    yyjson_mut_obj_add_strcpy(d,q,"original_id",ed->original);yyjson_mut_obj_add_strcpy(d,q,"expected_registry_sha256",ed->expected);
    yyjson_mut_obj_add_bool(d,q,"auto_start",gtk_check_button_get_active(GTK_CHECK_BUTTON(ed->automatic)));
    yyjson_mut_obj_add_strcpy(d,q,"display_name",gtk_editable_get_text(GTK_EDITABLE(ed->name)));
    send_doc(d,q);gtk_window_destroy(GTK_WINDOW(ed->dialog));
}
static void edit_clicked(GtkButton *button,gpointer editing){
    (void)button;if(ui.busy)return;
    if(sb_gui_bool(yyjson_doc_get_root(ui.model),"online")){alert("请先停止后台，再保存项目设置。");return;}
    project_editor *ed=g_new0(project_editor,1);
    for(unsigned i=0;i<3;i++)ed->rows[i]=g_ptr_array_new_with_free_func(g_free);
    yyjson_val *existing=GPOINTER_TO_INT(editing)?project_config():NULL;
    if(GPOINTER_TO_INT(editing)&&!existing){form_free(ed);return;}
    if(yyjson_obj_get(existing,"legacy_config")){form_free(ed);alert("此项目是 Windows 旧配置封装，请创建原生 POSIX 项目。");return;}
    yyjson_mut_doc *defaults=sb_form_project(existing,sb_gui_string(yyjson_doc_get_root(ui.model),"directory"));
    char *json=yyjson_mut_write(defaults,0,NULL);ed->definition=json?yyjson_read(json,strlen(json),0):NULL;free(json);yyjson_mut_doc_free(defaults);
    if(!ed->definition){form_free(ed);return;}
    yyjson_val *p=yyjson_doc_get_root(ed->definition);snprintf(ed->original,sizeof(ed->original),"%s",existing?ui.project:"");
    snprintf(ed->expected,sizeof(ed->expected),"%s",sb_gui_string(yyjson_doc_get_root(ui.model),"registry_sha256"));
    ed->dialog=gtk_window_new();gtk_window_set_title(GTK_WINDOW(ed->dialog),existing?"完整项目设置":"新建项目");gtk_window_set_default_size(GTK_WINDOW(ed->dialog),900,680);
    gtk_window_set_transient_for(GTK_WINDOW(ed->dialog),GTK_WINDOW(ui.window));gtk_window_set_modal(GTK_WINDOW(ed->dialog),TRUE);
    g_object_set_data_full(G_OBJECT(ed->dialog),"form",ed,form_free);
    GtkWidget *content=box(true,10);gtk_widget_set_margin_start(content,16);gtk_widget_set_margin_end(content,16);gtk_widget_set_margin_top(content,16);gtk_widget_set_margin_bottom(content,16);gtk_window_set_child(GTK_WINDOW(ed->dialog),content);
    append(content,label("文件范围、操作审批、命令身份和管理员许可分别生效。当前账户和管理员命令保留账户权限。保存后手动启动后台。"));
    GtkWidget *tabs=gtk_notebook_new();gtk_widget_set_vexpand(tabs,TRUE);append(content,tabs);
    const char *pages[]={"基本","目录","权限","工具","桌面","配额"};GtkWidget *panels[6];
    for(unsigned i=0;i<6;i++){panels[i]=box(true,10);gtk_notebook_append_page(GTK_NOTEBOOK(tabs),scroll(panels[i]),label(pages[i]));}
    ed->name=gtk_entry_new();ed->automatic=gtk_check_button_new_with_label("启动管理后台时自动启动此项目");
    yyjson_val *options=yyjson_obj_get(yyjson_obj_get(model("settings"),"project_options"),ed->original);
    gtk_editable_set_text(GTK_EDITABLE(ed->name),sb_gui_string(options,"display_name"));gtk_check_button_set_active(GTK_CHECK_BUTTON(ed->automatic),sb_gui_bool(options,"auto_start"));
    append(panels[0],label("显示名称（支持中文）"));append(panels[0],ed->name);append(panels[0],ed->automatic);
    for(unsigned i=0;i<sb_form_field_count;i++){
        const sb_form_field *f=&sb_form_fields[i];yyjson_val *v=sb_form_get(p,f->path);GtkWidget *w=NULL;unsigned page=0;
        while(page<5&&strcmp(pages[page],f->page))page++;
        if(!strcmp(f->kind,"bool")){w=gtk_check_button_new_with_label(f->label);gtk_check_button_set_active(GTK_CHECK_BUTTON(w),yyjson_get_bool(v));}
        else{
            append(panels[page],label(f->label));
            if(!strcmp(f->kind,"number")){w=gtk_spin_button_new_with_range(f->minimum,f->maximum,1);gtk_spin_button_set_value(GTK_SPIN_BUTTON(w),(double)yyjson_get_uint(v));}
            else if(!strcmp(f->kind,"enum")){
                gchar **titles=g_strsplit(f->titles,"|",-1),**values=g_strsplit(f->choices,"|",-1);w=gtk_drop_down_new_from_strings((const char *const *)titles);
                for(unsigned k=0;values[k];k++)if(!strcmp(values[k],yyjson_get_str(v)?yyjson_get_str(v):f->fallback))gtk_drop_down_set_selected(GTK_DROP_DOWN(w),k);
                g_strfreev(titles);g_strfreev(values);
            }else{w=gtk_entry_new();gtk_editable_set_text(GTK_EDITABLE(w),yyjson_get_str(v)?yyjson_get_str(v):"");
                if(existing&&(!strcmp(f->path,"id")||!strcmp(f->path,"token_env")||!strcmp(f->path,"data_dir")))gtk_editable_set_editable(GTK_EDITABLE(w),FALSE);}
        }
        ed->fields[i]=w;append(panels[page],w);
    }
    const char *paths[]={"workspaces","extra_directories","policy.execution.runtime_files"},*titles[]={"工作目录（工作目录自动审批仅用于这些目录）","额外授权目录","受限命令运行文件（最多 16 个，精确文件路径）"};
    for(unsigned type=0;type<3;type++){
        GtkWidget *panel=panels[type==2?2:1];append(panel,label(titles[type]));ed->lists[type]=box(true,6);append(panel,ed->lists[type]);
        size_t i,n;yyjson_val *item;yyjson_arr_foreach(sb_form_get(p,paths[type]),i,n,item)form_row_add(ed,type,item);
        GtkWidget *b=gtk_button_new_with_label(type==2?"添加运行文件…":"添加目录…");g_object_set_data(G_OBJECT(b),"type",GUINT_TO_POINTER(type));g_signal_connect(b,"clicked",G_CALLBACK(row_clicked),ed);append(panel,b);
    }
    append(panels[3],label("只有勾选的工具会向客户端公开；勾选工具不会扩大目录范围或授予执行身份。"));
    for(unsigned i=0;i<SB_PROJECT_TOOL_COUNT;i++){
        char title[160];snprintf(title,sizeof(title),"%s · %s",sb_form_tool_label(i),sb_project_tool_name(i));ed->tools[i]=gtk_check_button_new_with_label(title);
        size_t j,n;yyjson_val *t;yyjson_arr_foreach(sb_form_get(p,"policy.tools"),j,n,t)if(!strcmp(yyjson_get_str(t),sb_project_tool_name(i)))gtk_check_button_set_active(GTK_CHECK_BUTTON(ed->tools[i]),TRUE);
        append(panels[3],ed->tools[i]);
    }
    GtkWidget *buttons=box(false,8),*cancel=gtk_button_new_with_label("取消"),*save=gtk_button_new_with_label("保存项目设置");append(buttons,cancel);append(buttons,save);append(content,buttons);
    g_object_set_data(G_OBJECT(save),"save",GINT_TO_POINTER(1));g_signal_connect(save,"clicked",G_CALLBACK(form_response),ed);g_signal_connect(cancel,"clicked",G_CALLBACK(form_response),ed);gtk_window_present(GTK_WINDOW(ed->dialog));
}
