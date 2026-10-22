#import "cocoa_project.h"
#import <objc/runtime.h>
#include <stdlib.h>
#include <string.h>
static char editor_key;
static NSString *string(const char *s){return s?[NSString stringWithUTF8String:s]:@"";}
static NSStackView *column(void){NSStackView *v=[NSStackView new];v.orientation=NSUserInterfaceLayoutOrientationVertical;v.alignment=NSLayoutAttributeLeading;v.spacing=10;return v;}
static NSStackView *line(NSArray<NSView *> *views){NSStackView *v=[NSStackView stackViewWithViews:views];v.orientation=NSUserInterfaceLayoutOrientationHorizontal;v.spacing=6;v.alignment=NSLayoutAttributeCenterY;return v;}
@interface SBFormDocument:NSView
@end
@implementation SBFormDocument
- (BOOL)isFlipped{return YES;}
@end
NSScrollView *sb_macos_form_scroll(NSStackView *panel){
    NSScrollView *scroll=[NSScrollView new];scroll.hasVerticalScroller=YES;scroll.drawsBackground=NO;
    NSView *doc=[SBFormDocument new];scroll.documentView=doc;doc.translatesAutoresizingMaskIntoConstraints=NO;panel.translatesAutoresizingMaskIntoConstraints=NO;[doc addSubview:panel];
    [NSLayoutConstraint activateConstraints:@[[doc.widthAnchor constraintEqualToAnchor:scroll.contentView.widthAnchor],
        [panel.leadingAnchor constraintEqualToAnchor:doc.leadingAnchor constant:12],[panel.trailingAnchor constraintEqualToAnchor:doc.trailingAnchor constant:-12],
        [panel.topAnchor constraintEqualToAnchor:doc.topAnchor constant:12],[panel.bottomAnchor constraintEqualToAnchor:doc.bottomAnchor constant:-12]]];
    return scroll;
}
static id at(NSDictionary *root,NSString *path){id value=root;for(NSString *key in [path componentsSeparatedByString:@"."]){if(![value isKindOfClass:[NSDictionary class]])return nil;value=value[key];}return value;}
static void put(NSMutableDictionary *root,NSString *path,id value){NSArray *keys=[path componentsSeparatedByString:@"."];for(NSUInteger i=0;i+1<keys.count;i++){id child=root[keys[i]];if(![child isKindOfClass:[NSMutableDictionary class]]){child=child?[child mutableCopy]:[NSMutableDictionary new];root[keys[i]]=child;}root=child;}root[keys.lastObject]=value;}
@interface SBFRow:NSObject
@property NSStackView *view;
@property NSTextField *identifier,*path;
@property NSButton *readonly;
@property BOOL file,removed;
@end
@implementation SBFRow
- (void)remove:(id)sender{(void)sender;self.removed=YES;self.view.hidden=YES;}
- (void)choose:(id)sender{(void)sender;NSOpenPanel *p=[NSOpenPanel openPanel];p.canChooseDirectories=!self.file;p.canChooseFiles=self.file;p.allowsMultipleSelection=NO;
    [p beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse response){if(response==NSModalResponseOK)self.path.stringValue=p.URL.path;}];}
@end
@interface SBProjectEditor:NSWindowController<NSWindowDelegate>
@property(nonatomic,weak) NSWindow *parent;
@property NSMutableDictionary *definition;
@property NSDictionary *configuration;
@property NSString *original,*expected;
@property NSMutableArray<NSControl *> *fields;
@property NSMutableArray<NSButton *> *tools;
@property NSMutableArray<NSMutableArray<SBFRow *> *> *rows;
@property NSArray<NSStackView *> *lists;
@property NSTextField *name;
@property NSButton *automatic;
@property(copy) void(^submit)(NSDictionary *);
@end
@implementation SBProjectEditor
- (void)addRow:(unsigned)type value:(id)value {
    SBFRow *r=[SBFRow new];r.file=type==2;r.path=[NSTextField textFieldWithString:r.file?(value?:@""):(value[@"path"]?:@"")];r.path.placeholderString=@"绝对路径";
    NSMutableArray *views=[NSMutableArray new];
    if(!r.file){r.identifier=[NSTextField textFieldWithString:value[@"id"]?:@""];r.identifier.placeholderString=@"目录标识";[r.identifier.widthAnchor constraintEqualToConstant:110].active=YES;[views addObject:r.identifier];}
    [r.path.widthAnchor constraintGreaterThanOrEqualToConstant:310].active=YES;[views addObject:r.path];
    [views addObject:[NSButton buttonWithTitle:@"选择…" target:r action:@selector(choose:)]];
    if(!r.file){r.readonly=[NSButton checkboxWithTitle:@"只读" target:nil action:nil];r.readonly.state=!value||!value[@"read_only"]||[value[@"read_only"] boolValue]?NSControlStateValueOn:NSControlStateValueOff;[views addObject:r.readonly];}
    [views addObject:[NSButton buttonWithTitle:@"移除" target:r action:@selector(remove:)]];
    r.view=line(views);[self.lists[type] addArrangedSubview:r.view];[self.rows[type] addObject:r];
}
- (void)add:(NSButton *)sender{[self addRow:(unsigned)sender.tag value:nil];}
- (void)cancel:(id)sender{(void)sender;[self.parent endSheet:self.window];[self.window orderOut:nil];objc_setAssociatedObject(self.parent,&editor_key,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);}
- (BOOL)windowShouldClose:(NSWindow *)sender{(void)sender;[self cancel:nil];return NO;}
- (void)save:(id)sender{
    (void)sender;
    for(unsigned i=0;i<sb_form_field_count;i++){
        const sb_form_field *f=&sb_form_fields[i];NSControl *w=self.fields[i];id value=nil;
        if(!strcmp(f->kind,"bool"))value=@(((NSButton *)w).state==NSControlStateValueOn);
        else if(!strcmp(f->kind,"number")){
            NSString *text=w.stringValue;unsigned long long number=0;NSScanner *scan=[NSScanner scannerWithString:text];
            if(![scan scanUnsignedLongLong:&number]||!scan.isAtEnd||number<f->minimum||number>f->maximum){[self error:[NSString stringWithFormat:@"%@：请输入 %u～%u 的整数。",string(f->label),f->minimum,f->maximum]];return;}value=@(number);
        }else if(!strcmp(f->kind,"enum")){NSArray *choices=[string(f->choices) componentsSeparatedByString:@"|"];NSInteger index=((NSPopUpButton *)w).indexOfSelectedItem;value=index>=0&&index<(NSInteger)choices.count?choices[index]:string(f->fallback);}
        else value=w.stringValue;
        put(self.definition,string(f->path),value);
    }
    NSArray *paths=@[@"workspaces",@"extra_directories",@"policy.execution.runtime_files"];
    for(unsigned type=0;type<3;type++){
        NSMutableArray *array=[NSMutableArray new];
        for(SBFRow *r in self.rows[type])if(!r.removed){if(type==2)[array addObject:r.path.stringValue];else[array addObject:@{@"id":r.identifier.stringValue,@"path":r.path.stringValue,@"read_only":@(r.readonly.state==NSControlStateValueOn)}];}
        put(self.definition,paths[type],array);
    }
    NSMutableArray *tools=[NSMutableArray new];for(unsigned i=0;i<SB_PROJECT_TOOL_COUNT;i++)if(self.tools[i].state==NSControlStateValueOn)[tools addObject:string(sb_project_tool_name(i))];put(self.definition,@"policy.tools",tools);
    NSData *bytes=[NSJSONSerialization dataWithJSONObject:self.definition options:0 error:nil];
    yyjson_doc *parsed=yyjson_read(bytes.bytes,bytes.length,0);yyjson_mut_doc *d=yyjson_doc_mut_copy(parsed,NULL);sb_form_normalize(d,yyjson_mut_doc_get_root(d));
    char *definition=yyjson_mut_write(d,0,NULL);NSData *registry=[NSJSONSerialization dataWithJSONObject:self.configuration options:0 error:nil];sb_error e={0};
    char *valid=definition?sb_projects_edit(registry.bytes,registry.length,definition,strlen(definition),[self.definition[@"id"] UTF8String],self.original.length>0,&e):NULL;
    if(!valid){free(definition);yyjson_mut_doc_free(d);yyjson_doc_free(parsed);[self error:string(e.text)];return;}free(valid);
    NSDictionary *normalized=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:definition length:strlen(definition)] options:0 error:nil];free(definition);yyjson_mut_doc_free(d);yyjson_doc_free(parsed);
    self.submit(@{@"action":@"configure_project",@"definition":normalized,@"original_id":self.original,@"expected_registry_sha256":self.expected,@"display_name":self.name.stringValue,@"auto_start":@(self.automatic.state==NSControlStateValueOn)});
    [self cancel:nil];
}
- (void)error:(NSString *)message{NSAlert *a=[NSAlert new];a.messageText=@"请检查项目设置";a.informativeText=message.length?message:@"项目配置无效。";[a beginSheetModalForWindow:self.window completionHandler:nil];}
@end
void sb_macos_project_editor(NSWindow *parent,yyjson_val *project,yyjson_val *configuration,yyjson_val *settings,const char *directory,const char *expected,void(^submit)(NSDictionary *)){
    if(yyjson_obj_get(project,"legacy_config"))return;
    yyjson_mut_doc *d=sb_form_project(project,directory);char *json=d?yyjson_mut_write(d,0,NULL):NULL;
    if(!json){yyjson_mut_doc_free(d);return;}
    SBProjectEditor *ed=[[SBProjectEditor alloc] initWithWindow:[[NSWindow alloc] initWithContentRect:NSMakeRect(0,0,920,720) styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO]];
    ed.parent=parent;ed.window.delegate=ed;ed.window.title=project?@"完整项目设置":@"新建项目";ed.window.minSize=NSMakeSize(900,640);ed.submit=submit;
    ed.definition=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:json length:strlen(json)] options:NSJSONReadingMutableContainers error:nil];free(json);yyjson_mut_doc_free(d);
    char *cfg=yyjson_val_write(configuration,0,NULL);ed.configuration=cfg?[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:cfg length:strlen(cfg)] options:0 error:nil]:@{};free(cfg);
    ed.original=project?string(yyjson_get_str(yyjson_obj_get(project,"id"))):@"";ed.expected=string(expected);ed.fields=[NSMutableArray new];ed.tools=[NSMutableArray new];ed.rows=[NSMutableArray new];
    NSStackView *root=column();root.translatesAutoresizingMaskIntoConstraints=NO;[ed.window.contentView addSubview:root];
    [NSLayoutConstraint activateConstraints:@[[root.leadingAnchor constraintEqualToAnchor:ed.window.contentView.leadingAnchor constant:16],[root.trailingAnchor constraintEqualToAnchor:ed.window.contentView.trailingAnchor constant:-16],[root.topAnchor constraintEqualToAnchor:ed.window.contentView.topAnchor constant:16],[root.bottomAnchor constraintEqualToAnchor:ed.window.contentView.bottomAnchor constant:-16]]];
    NSTextField *help=[NSTextField wrappingLabelWithString:@"文件范围、操作审批、命令身份和管理员许可分别生效。当前账户和管理员命令保留账户权限。保存后手动启动后台。"];[root addArrangedSubview:help];[help.widthAnchor constraintEqualToAnchor:root.widthAnchor].active=YES;
    NSTabView *tabs=[NSTabView new];[root addArrangedSubview:tabs];[tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor].active=YES;
    NSArray *names=@[@"基本",@"目录",@"权限",@"工具",@"桌面",@"配额"];NSMutableArray<NSStackView *> *panels=[NSMutableArray new];
    for(NSString *name in names){NSStackView *panel=column();[panels addObject:panel];NSTabViewItem *tab=[[NSTabViewItem alloc] initWithIdentifier:name];tab.label=name;tab.view=sb_macos_form_scroll(panel);[tabs addTabViewItem:tab];}
    [tabs.heightAnchor constraintGreaterThanOrEqualToConstant:500].active=YES;
    yyjson_val *options=yyjson_obj_get(yyjson_obj_get(settings,"project_options"),ed.original.UTF8String);
    ed.name=[NSTextField textFieldWithString:string(yyjson_get_str(yyjson_obj_get(options,"display_name")))];[ed.name.widthAnchor constraintEqualToConstant:740].active=YES;
    ed.automatic=[NSButton checkboxWithTitle:@"启动管理后台时自动启动此项目" target:nil action:nil];ed.automatic.state=yyjson_get_bool(yyjson_obj_get(options,"auto_start"))?NSControlStateValueOn:NSControlStateValueOff;
    [panels[0] addArrangedSubview:[NSTextField labelWithString:@"显示名称（支持中文）"]];[panels[0] addArrangedSubview:ed.name];[panels[0] addArrangedSubview:ed.automatic];
    for(unsigned i=0;i<sb_form_field_count;i++){
        const sb_form_field *f=&sb_form_fields[i];NSUInteger index=[names indexOfObject:string(f->page)];NSStackView *panel=panels[index];id value=at(ed.definition,string(f->path));NSControl *w=nil;
        if(!strcmp(f->kind,"bool")){NSButton *b=[NSButton checkboxWithTitle:string(f->label) target:nil action:nil];b.state=[value boolValue]?NSControlStateValueOn:NSControlStateValueOff;w=b;}
        else{
            [panel addArrangedSubview:[NSTextField labelWithString:string(f->label)]];
            if(!strcmp(f->kind,"enum")){NSPopUpButton *popup=[NSPopUpButton new];[popup addItemsWithTitles:[string(f->titles) componentsSeparatedByString:@"|"]];NSUInteger k=[[string(f->choices) componentsSeparatedByString:@"|"] indexOfObject:value];[popup selectItemAtIndex:k==NSNotFound?0:(NSInteger)k];w=popup;}
            else{NSTextField *text=[NSTextField textFieldWithString:[value isKindOfClass:[NSString class]]?value:[value description]];[text.widthAnchor constraintEqualToConstant:740].active=YES;
                if(project&&(!strcmp(f->path,"id")||!strcmp(f->path,"data_dir")||!strcmp(f->path,"token_env")))text.editable=NO;w=text;}
        }
        [ed.fields addObject:w];[panel addArrangedSubview:w];
    }
    NSMutableArray *lists=[NSMutableArray new];for(unsigned type=0;type<3;type++){[lists addObject:column()];[ed.rows addObject:[NSMutableArray new]];}ed.lists=lists;
    NSArray *paths=@[@"workspaces",@"extra_directories",@"policy.execution.runtime_files"],*titles=@[@"工作目录",@"额外授权目录",@"受限命令运行文件（最多 16 个，精确文件路径）"];
    for(unsigned type=0;type<3;type++){NSStackView *panel=panels[type==2?2:1];[panel addArrangedSubview:[NSTextField labelWithString:titles[type]]];[panel addArrangedSubview:ed.lists[type]];
        for(id value in at(ed.definition,paths[type]))[ed addRow:type value:value];NSButton *add=[NSButton buttonWithTitle:type==2?@"添加运行文件…":@"添加目录…" target:ed action:@selector(add:)];add.tag=type;[panel addArrangedSubview:add];}
    for(unsigned i=0;i<SB_PROJECT_TOOL_COUNT;i++){NSString *name=string(sb_project_tool_name(i));NSButton *b=[NSButton checkboxWithTitle:[NSString stringWithFormat:@"%@ · %@",string(sb_form_tool_label(i)),name] target:nil action:nil];b.state=[at(ed.definition,@"policy.tools") containsObject:name]?NSControlStateValueOn:NSControlStateValueOff;[ed.tools addObject:b];[panels[3] addArrangedSubview:b];}
    [root addArrangedSubview:line(@[[NSButton buttonWithTitle:@"取消" target:ed action:@selector(cancel:)],[NSButton buttonWithTitle:@"保存项目设置" target:ed action:@selector(save:)]])];
    objc_setAssociatedObject(parent,&editor_key,ed,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[parent beginSheet:ed.window completionHandler:nil];
}
