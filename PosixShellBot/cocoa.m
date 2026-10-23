#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include "native.h"
#include "credentials.h"
#import "cocoa_project.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

static NSString *str(const char *s) { return s ? [NSString stringWithUTF8String:s] ?: @"" : @""; }
static NSTextField *label(NSString *s) {
    NSTextField *v = [NSTextField wrappingLabelWithString:s];
    return v;
}
static NSStackView *stack(BOOL vertical, NSArray<NSView *> *views) {
    NSStackView *s = [NSStackView stackViewWithViews:views];
    s.orientation =
        vertical ? NSUserInterfaceLayoutOrientationVertical : NSUserInterfaceLayoutOrientationHorizontal;
    s.spacing = 8;
    s.alignment = vertical ? NSLayoutAttributeLeading : NSLayoutAttributeCenterY;
    s.distribution = NSStackViewDistributionFill;
    s.detachesHiddenViews = NO;
    return s;
}
static NSScrollView *scrolled(NSView *view) {
    NSScrollView *s = [NSScrollView new];
    s.hasVerticalScroller = YES;
    s.borderType = NSBezelBorder;
    s.documentView = view;
    s.translatesAutoresizingMaskIntoConstraints = NO;
    [s setContentHuggingPriority:1 forOrientation:NSLayoutConstraintOrientationVertical];
    return s;
}
static NSTextView *textView(void) {
    NSTextView *v = [NSTextView new];
    v.editable = NO;
    v.richText = NO;
    v.font = [NSFont systemFontOfSize:13];
    v.textContainerInset = NSMakeSize(10, 10);
    v.autoresizingMask = NSViewWidthSizable;
    v.verticallyResizable = YES;
    v.horizontallyResizable = NO;
    v.textContainer.widthTracksTextView = YES;
    return v;
}
static NSString *pretty(yyjson_val *v, yyjson_val *configuration) {
    char *s = sb_gui_request_text(v, configuration);
    NSString *out = str(s);
    free(s);
    return out;
}

static void putText(NSTextView *view, NSString *text) {
    if (![view.string isEqualToString:text]) {
        NSPoint origin = view.enclosingScrollView.contentView.bounds.origin;
        view.string = text;
        [view scrollPoint:origin];
    }
}

@interface ShellBotDelegate
    : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate>
@property NSWindow *window;
@property NSStatusItem *statusItem;
@property NSTableView *projects, *requests;
@property NSTextView *detail, *requestDetail, *desktopStatus;
@property NSTextView *adminStatus;
@property NSTextField *rootRegistry, *rootState;
@property NSPopUpButton *approvalBackend;
@property NSPopUpButton *serviceKind;
@property NSTextField *serviceName, *serviceDirectory, *serviceKey;
@property NSTextView *serviceStarts, *serviceStatus;
@property NSButton *serviceOAuth;
@property NSTextField *summary, *message, *port, *url, *startupStatus, *ownerEnv, *defaultProject;
@property NSButton *http, *oauth, *persistent, *remoteOwner;
@property NSMutableArray<NSDictionary *> *projectRows, *requestRows;
@property NSString *selected, *requestID, *requestType, *smoke;
@property BOOL busy, rendering, quitting, settingsLoaded, serviceSettingsLoaded;
@property unsigned ticks;
@property dispatch_queue_t queue;
@property NSTimer *timer;
@property sb_native_manager *desktop;
@property yyjson_doc *model;
@end

@implementation ShellBotDelegate
- (NSButton *)button:(NSString *)title action:(NSString *)action {
    NSButton *b = [NSButton buttonWithTitle:title target:self action:@selector(command:)];
    b.identifier = action;
    return b;
}
- (NSButton *)special:(NSString *)title selector:(SEL)selector {
    return [NSButton buttonWithTitle:title target:self action:selector];
}
- (void)alert:(NSString *)text {
    NSAlert *a = [NSAlert new];
    a.messageText = @"ShellBot";
    a.informativeText = text;
    [a addButtonWithTitle:@"好"];
    [a beginSheetModalForWindow:self.window completionHandler:nil];
}
- (yyjson_val *)value:(const char *)key {
    return yyjson_obj_get(yyjson_doc_get_root(self.model), key);
}
- (yyjson_val *)project {
    size_t i, max;
    yyjson_val *p;
    yyjson_arr_foreach(yyjson_obj_get([self value:"config"], "projects"), i, max,
                       p) if (!strcmp(sb_gui_string(p, "id"), self.selected.UTF8String)) return p;
    return NULL;
}
- (NSTableView *)table {
    NSTableView *t = [NSTableView new];
    NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    column.width = 250;
    [t addTableColumn:column];
    t.headerView = nil;
    t.delegate = self;
    t.dataSource = self;
    t.rowHeight = 58;
    t.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    return t;
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.queue = dispatch_queue_create("org.shellbot.desktop.work", DISPATCH_QUEUE_SERIAL);
    self.selected = @"";
    self.requestID = @"";
    self.requestType = @"";
    self.projectRows = [NSMutableArray new];
    self.requestRows = [NSMutableArray new];
    self.window =
        [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1080, 760)
                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                              NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.window.title = @"ShellBot";
    self.window.minSize = NSMakeSize(900, 650);
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;
    self.window.backgroundColor = [NSColor windowBackgroundColor];
    self.summary = label(@"ShellBot · 本机项目管理");
    self.summary.font = [NSFont boldSystemFontOfSize:18];
    self.message = label(@"");
    NSStackView *toolbar = stack(NO, @[
        [self button:@"启动后台" action:@"start_manager"],
        [self special:@"停止后台" selector:@selector(stopManager:)], [self button:@"刷新" action:@"refresh"],
        [self special:@"取消迁移" selector:@selector(cancelMigration:)],
        [self special:@"退出 ShellBot" selector:@selector(quit:)]
    ]);
    NSTabView *tabs = [NSTabView new];
    tabs.translatesAutoresizingMaskIntoConstraints = NO;
    self.projects = [self table];
    NSScrollView *projectList = scrolled(self.projects);
    [projectList.widthAnchor constraintEqualToConstant:240].active = YES;
    NSStackView *projectEdit = stack(NO, @[
        [self special:@"添加" selector:@selector(add:)], [self special:@"编辑" selector:@selector(edit:)],
        [self special:@"移除" selector:@selector(remove:)]
    ]);
    NSStackView *left = stack(
        YES, @[ projectList, projectEdit, [self special:@"导入现有项目配置…" selector:@selector(import:)] ]);
    NSStackView *actions = stack(NO, @[
        [self button:@"启动项目" action:@"start"],
        [self special:@"停止项目" selector:@selector(stopProject:)],
        [self button:@"暂停新请求" action:@"pause"], [self button:@"恢复请求" action:@"resume"]
    ]);
    self.detail = textView();
    NSScrollView *details = scrolled(self.detail);
    NSStackView *right = stack(YES, @[ actions, [self special:@"迁移项目数据…" selector:@selector(migrateProject:)], details ]);
    NSStackView *projects = stack(NO, @[ left, right ]);
    [left.heightAnchor constraintEqualToAnchor:projects.heightAnchor].active = YES;
    [right.heightAnchor constraintEqualToAnchor:projects.heightAnchor].active = YES;
    [details.widthAnchor constraintEqualToAnchor:right.widthAnchor].active = YES;
    [left.widthAnchor constraintEqualToConstant:240].active = YES;
    NSTabViewItem *tab = [[NSTabViewItem alloc] initWithIdentifier:@"projects"];
    tab.label = @"项目";
    tab.view = projects;
    [tabs addTabViewItem:tab];
    self.desktopStatus = textView();
    NSScrollView *desktopDetail = scrolled(self.desktopStatus);
    NSStackView *desktopActions = stack(NO, @[
        [self special:@"设置桌面权限…" selector:@selector(desktopPermissions:)],
        [self button:@"连接桌面" action:@"desktop-connect"],
        [self button:@"暂停桌面" action:@"desktop-pause"], [self button:@"本机恢复" action:@"desktop-resume"]
    ]);
    NSStackView *desktopPage = stack(YES, @[
        label(@"当前所选项目的远程桌面。设置项目权限后手动连接，并在系统设置中允许屏幕录制、辅助功能和输入监"
              @"控。"),
        desktopActions,
        label(@"人工输入会暂停控制；暂停影响本会话所有 ShellBot 项目。只有本机可恢复，旧观察图会失效。"),
        desktopDetail
    ]);
    [desktopDetail.widthAnchor constraintEqualToAnchor:desktopPage.widthAnchor].active = YES;
    tab = [[NSTabViewItem alloc] initWithIdentifier:@"desktop"];
    tab.label = @"远程桌面";
    tab.view = desktopPage;
    [tabs addTabViewItem:tab];
    self.requests = [self table];
    self.approvalBackend = [NSPopUpButton new];
    [self.approvalBackend addItemsWithTitles:@[@"普通操作审批", @"管理员操作审批（独立审批池）"]];
    self.approvalBackend.target = self; self.approvalBackend.action = @selector(approvalBackendChanged:);
    NSScrollView *requestList = scrolled(self.requests);
    [requestList.widthAnchor constraintEqualToConstant:300].active = YES;
    self.requestDetail = textView();
    NSScrollView *rd = scrolled(self.requestDetail);
    self.persistent = [NSButton checkboxWithTitle:@"持续授权至手动撤销：包括此项目今后允许的操作"
                                           target:nil
                                           action:nil];
    NSStackView *decisions = stack(NO, @[
        [self button:@"查看完整操作" action:@"inspect"], [self button:@"允许" action:@"approve"],
        [self button:@"拒绝" action:@"deny"], [self button:@"撤销" action:@"revoke"],
        [self button:@"删除客户端" action:@"remove_client"]
    ]);
    NSStackView *requestRight = stack(
        YES, @[ self.approvalBackend, rd, self.persistent, label(@"未选中时为限时连接。连接授权不代替每次操作审批。"), decisions ]);
    NSStackView *requests = stack(NO, @[ requestList, requestRight ]);
    [requestList.heightAnchor constraintEqualToAnchor:requests.heightAnchor].active = YES;
    [requestRight.heightAnchor constraintEqualToAnchor:requests.heightAnchor].active = YES;
    [rd.widthAnchor constraintEqualToAnchor:requestRight.widthAnchor].active = YES;
    [requestRight.widthAnchor constraintGreaterThanOrEqualToConstant:600].active = YES;
    tab = [[NSTabViewItem alloc] initWithIdentifier:@"requests"];
    tab.label = @"审批与连接";
    tab.view = requests;
    [tabs addTabViewItem:tab];
    self.rootRegistry = [NSTextField new]; self.rootState = [NSTextField new]; self.adminStatus = textView();
    NSScrollView *adminStatusScroll = scrolled(self.adminStatus);
    [adminStatusScroll.heightAnchor constraintGreaterThanOrEqualToConstant:240].active = YES;
    NSStackView *adminPage = stack(YES, @[
        label(@"先把完整程序、与当前项目配置完全一致的注册表副本和独立状态目录放到 root 拥有的位置，再从该程序打开管理界面。系统授权只在点击启动时请求。"),
        label(@"管理员项目配置副本"), self.rootRegistry, label(@"管理员独立状态目录"), self.rootState,
        [self special:@"保存管理员设置" selector:@selector(adminSave:)],
        stack(NO, @[[self special:@"启动管理员会话…" selector:@selector(adminStart:)],
                    [self special:@"结束管理员会话…" selector:@selector(adminStop:)],
                    [self special:@"取消授权等待" selector:@selector(adminCancel:)]]),
        stack(NO, @[[self special:@"重启所选项目并连接…" selector:@selector(adminConnect:)],
                    [self special:@"撤销所选项目授权…" selector:@selector(adminRevoke:)]]), adminStatusScroll
    ]);
    [self.rootRegistry.widthAnchor constraintEqualToAnchor:adminPage.widthAnchor].active = YES;
    [self.rootState.widthAnchor constraintEqualToAnchor:adminPage.widthAnchor].active = YES;
    [adminStatusScroll.widthAnchor constraintEqualToAnchor:adminPage.widthAnchor].active = YES;
    tab = [[NSTabViewItem alloc] initWithIdentifier:@"administrator"]; tab.label = @"管理员";
    tab.view = sb_macos_form_scroll(adminPage); [tabs addTabViewItem:tab];
    self.serviceKind = [NSPopUpButton new];
    [self.serviceKind addItemsWithTitles:@[@"当前账户服务（登录后运行）", @"系统服务（开机后以当前账户运行）"]];
    self.serviceKind.target = self; self.serviceKind.action = @selector(serviceKindChanged:);
    self.serviceName = [NSTextField new]; self.serviceDirectory = [NSTextField new]; self.serviceKey = [NSTextField new];
    self.serviceStarts = textView(); self.serviceStarts.editable = YES;
    self.serviceOAuth = [NSButton checkboxWithTitle:@"服务启用 OAuth 连接授权" target:nil action:nil];
    self.serviceStatus = textView();
    NSScrollView *serviceStartsScroll = scrolled(self.serviceStarts), *serviceStatusScroll = scrolled(self.serviceStatus);
    [serviceStartsScroll.heightAnchor constraintEqualToConstant:70].active = YES;
    [serviceStatusScroll.heightAnchor constraintGreaterThanOrEqualToConstant:240].active = YES;
    NSStackView *servicePage = stack(YES, @[
        label(@"独立后台服务使用当前窗口已保存的项目配置和后台数据，以当前账户运行。系统服务通过系统管理员授权安装和控制；项目管理员权限仍在“管理员”页单独授权。"),
        label(@"服务类型"), self.serviceKind, label(@"服务名称"), self.serviceName,
        label(@"服务登记目录（与后台数据目录分开）"), self.serviceDirectory,
        label(@"凭据密钥文件（当前账户服务可留空；系统服务必填）"), self.serviceKey,
        label(@"服务启动项目（每行或逗号分隔；留空只启动后台，不使用界面的项目自动启动选项）"), serviceStartsScroll,
        self.serviceOAuth,
        stack(NO, @[[self special:@"保存服务设置" selector:@selector(serviceSave:)],
                    [self special:@"预览安装内容" selector:@selector(servicePlan:)],
                    [self special:@"安装服务…" selector:@selector(serviceInstall:)]]),
        stack(NO, @[[self special:@"检查状态" selector:@selector(serviceRefresh:)],
                    [self special:@"启动服务…" selector:@selector(serviceStart:)],
                    [self special:@"停止服务…" selector:@selector(serviceStop:)],
                    [self special:@"卸载服务…" selector:@selector(serviceUninstall:)],
                    [self special:@"取消授权等待" selector:@selector(adminCancel:)]]),
        label(@"系统服务状态只在点击检查或执行操作时请求授权，下面显示最近一次检查结果。安装仅登记服务，请随后显式启动。停止或卸载会结束服务所属项目和当前任务。"),
        serviceStatusScroll
    ]);
    for (NSView *field in @[self.serviceName, self.serviceDirectory, self.serviceKey, serviceStartsScroll, serviceStatusScroll])
        [field.widthAnchor constraintEqualToAnchor:servicePage.widthAnchor].active = YES;
    tab = [[NSTabViewItem alloc] initWithIdentifier:@"service"]; tab.label = @"服务";
    tab.view = sb_macos_form_scroll(servicePage); [tabs addTabViewItem:tab];
    self.http = [NSButton checkboxWithTitle:@"启用本机 HTTP 网关（仅监听回环地址）" target:nil action:nil];
    self.oauth = [NSButton checkboxWithTitle:@"启用 OAuth 连接授权" target:nil action:nil];
    self.remoteOwner=[NSButton checkboxWithTitle:@"启用远程所有者操作审批（独立凭据）" target:nil action:nil];
    self.ownerEnv=[NSTextField textFieldWithString:@""];self.defaultProject=[NSTextField textFieldWithString:@""];
    self.port = [NSTextField textFieldWithString:@"8765"];
    self.url = [NSTextField textFieldWithString:@""];
    self.url.placeholderString = @"https://shellbot.example.com";
    self.startupStatus=label(@"登录自启：正在检查");
    NSStackView *settings = stack(YES, @[
        self.http, label(@"本机端口"), self.port, label(@"公网 HTTPS 地址（隧道由你配置）"), self.url,
        self.oauth, self.remoteOwner, label(@"所有者凭据引用（留空由程序管理）"), self.ownerEnv,
        label(@"兼容 /mcp 的默认项目标识（可留空）"), self.defaultProject,
        [self button:@"复制独立审批凭据" action:@"copy_owner_token"],
        label(@"凭据由系统钥匙串保护。保存前请停止后台；项目自动启动选项在项目设置中选择。"),
        [self special:@"保存连接设置" selector:@selector(saveSettings:)],
        self.startupStatus,
        label(@"登录自启只打开管理界面；独立后台服务在“服务”页管理。"),
        [self special:@"启用登录自启" selector:@selector(enableStartup:)],
        [self special:@"取消登录自启" selector:@selector(disableStartup:)],
        [self special:@"迁移应用数据…" selector:@selector(migrateApplication:)],
        [self special:@"取消未完成的迁移…" selector:@selector(recoverMigration:)]
    ]);
    [self.port.widthAnchor constraintEqualToConstant:180].active = YES;
    [self.url.widthAnchor constraintEqualToConstant:640].active = YES;
    tab = [[NSTabViewItem alloc] initWithIdentifier:@"settings"];
    tab.label = @"连接设置";
    NSScrollView *settingsScroll=sb_macos_form_scroll(settings);
    tab.view = settingsScroll;
    [tabs addTabViewItem:tab];
    NSStackView *root = stack(
        YES, @[ self.summary, toolbar, tabs, label(@"关闭窗口后可从菜单栏图标重新打开。"), self.message ]);
    root.translatesAutoresizingMaskIntoConstraints = NO;
    [self.window.contentView addSubview:root];
    [NSLayoutConstraint activateConstraints:@[
        [root.leadingAnchor constraintEqualToAnchor:self.window.contentView.leadingAnchor constant:18],
        [root.trailingAnchor constraintEqualToAnchor:self.window.contentView.trailingAnchor constant:-18],
        [root.topAnchor constraintEqualToAnchor:self.window.contentView.topAnchor constant:16],
        [root.bottomAnchor constraintEqualToAnchor:self.window.contentView.bottomAnchor constant:-16],
        [tabs.widthAnchor constraintEqualToAnchor:root.widthAnchor]
    ]];
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSSquareStatusItemLength];
    NSString *icon = [[NSBundle mainBundle] pathForResource:@"shellbot-icon-small" ofType:@"png"];
    NSImage *image = icon ? [[NSImage alloc] initWithContentsOfFile:icon]
                          : [NSImage imageWithSystemSymbolName:@"terminal"
                                      accessibilityDescription:@"ShellBot"];
    image.size = NSMakeSize(20, 20);
    self.statusItem.button.image = image;
    self.statusItem.button.toolTip = @"ShellBot";
    NSMenu *menu = [NSMenu new];
    NSMenuItem *open = [[NSMenuItem alloc] initWithTitle:@"打开 ShellBot"
                                                  action:@selector(show:)
                                           keyEquivalent:@""];
    open.target = self;
    [menu addItem:open];
    for (NSString *action in @[ @"desktop-pause", @"desktop-resume" ]) {
        NSMenuItem *item = [[NSMenuItem alloc]
            initWithTitle:[action isEqual:@"desktop-pause"] ? @"暂停桌面控制" : @"本机恢复桌面控制"
                   action:@selector(desktopMenu:)
            keyEquivalent:@""];
        item.representedObject = action;
        item.target = self;
        [menu addItem:item];
    }
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *quit = [[NSMenuItem alloc] initWithTitle:@"退出 ShellBot"
                                                  action:@selector(quit:)
                                           keyEquivalent:@"q"];
    quit.target = self;
    [menu addItem:quit];
    self.statusItem.menu = menu;
    NSMenu *main = [NSMenu new], *appMenu = [NSMenu new];
    NSMenuItem *app = [NSMenuItem new];
    app.submenu = appMenu;
    [main addItem:app];
    NSMenuItem *show = [[NSMenuItem alloc] initWithTitle:@"打开 ShellBot"
                                                  action:@selector(show:)
                                           keyEquivalent:@"0"];
    show.target = self;
    [appMenu addItem:show];
    NSMenuItem *quitMenu = [[NSMenuItem alloc] initWithTitle:@"退出 ShellBot"
                                                      action:@selector(quit:)
                                               keyEquivalent:@"q"];
    quitMenu.target = self;
    [appMenu addItem:quitMenu];
    NSMenuItem *editItem = [NSMenuItem new];
    editItem.title = @"编辑";
    NSMenu *editMenu = [[NSMenu alloc] initWithTitle:@"编辑"];
    editItem.submenu = editMenu;
    [main addItem:editItem];
    NSArray *editTitles = @[ @"剪切", @"复制", @"粘贴", @"全选" ], *editKeys = @[ @"x", @"c", @"v", @"a" ],
            *editSelectors = @[ @"cut:", @"copy:", @"paste:", @"selectAll:" ];
    for (unsigned i = 0; i < 4; i++) {
        NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:editTitles[i]
                                                      action:NSSelectorFromString(editSelectors[i])
                                               keyEquivalent:editKeys[i]];
        [editMenu addItem:item];
    }
    NSApp.mainMenu = main;
    [self.window center];
    [self show:nil];
#ifdef SHELLBOT_TESTS
    if (self.smoke)
        [[NSString stringWithFormat:@"%ld", (long)self.window.windowNumber]
            writeToFile:[self.smoke stringByAppendingString:@".window"]
             atomically:YES
               encoding:NSUTF8StringEncoding
                  error:nil];
#endif
    [self submit:@"refresh"];
    self.timer = [NSTimer scheduledTimerWithTimeInterval:2
                                                  target:self
                                                selector:@selector(tick:)
                                                userInfo:nil
                                                 repeats:YES];
}
- (void)show:(id)sender {
    (void)sender;
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (BOOL)windowShouldClose:(NSWindow *)sender {
    [sender orderOut:nil];
    return NO;
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)flag {
    (void)sender;
    (void)flag;
    [self show:nil];
    return YES;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    (void)sender;
    return NO;
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender {
    (void)sender;
    if (self.quitting)
        return NSTerminateNow;
    [self quit:nil];
    return NSTerminateCancel;
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table {
    return table == self.projects ? self.projectRows.count : self.requestRows.count;
}
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    (void)column;
    NSArray *rows = table == self.projects ? self.projectRows : self.requestRows;
    NSTextField *l = label(rows[row][@"title"]);
    l.lineBreakMode = NSLineBreakByTruncatingTail;
    return l;
}
- (yyjson_val *)currentRequest {
    const char *type = self.requestType.UTF8String;
    yyjson_val *array = !strcmp(type, "operation") ? yyjson_obj_get([self value:"approvals"], "approvals")
                                                   : yyjson_obj_get([self value:"oauth"], type);
    size_t i, max;
    yyjson_val *v;
    yyjson_arr_foreach(array, i, max,
                       v) if (!strcmp(sb_gui_string(v, !strcmp(type, "operation") ? "approval_id" : "id"),
                                      self.requestID.UTF8String)) return v;
    return NULL;
}
- (void)tableViewSelectionDidChange:(NSNotification *)n {
    if (self.rendering)
        return;
    NSTableView *t = n.object;
    NSInteger row = t.selectedRow;
    if (row < 0)
        return;
    if (t == self.projects) {
        if (self.busy)
            return;
        self.selected = self.projectRows[row][@"id"];
        [self submit:@"refresh"];
    } else {
        NSDictionary *r = self.requestRows[row];
        self.requestID = r[@"id"];
        self.requestType = r[@"type"];
        self.persistent.state = NSControlStateValueOff;
        self.requestDetail.string = pretty([self currentRequest], [self value:"config"]);
    }
}
- (void)render {
    self.rendering = YES;
    self.selected = str(sb_gui_string(yyjson_doc_get_root(self.model), "selected"));
    BOOL active = sb_gui_bool(yyjson_doc_get_root(self.model), "online");
    self.summary.stringValue = active ? @"后台运行中 · 项目独立运行" : @"后台已停止 · 启动后台后可启动项目";
    self.message.stringValue = str(sb_gui_string(yyjson_doc_get_root(self.model), "message"));
    self.startupStatus.stringValue= *sb_gui_string([self value:"startup"],"error")?@"登录自启：配置需要检查":
        sb_gui_bool([self value:"startup"],"installed")?@"登录自启：已启用":@"登录自启：未启用";
    char *desktopText = sb_gui_desktop_text([self value:"desktop"]);
    putText(self.desktopStatus, str(desktopText));
    free(desktopText);
    char *adminText = sb_gui_admin_text([self value:"administrator"], self.selected.UTF8String,
        sb_gui_bool(yyjson_doc_get_root(self.model), "administrator_connected"),
        sb_gui_bool(yyjson_doc_get_root(self.model), "administrator_closing"));
    putText(self.adminStatus, str(adminText)); free(adminText);
    char *serviceText = sb_gui_service_text([self value:"service"], [self value:"service_plan"],
        !strcmp(sb_gui_string([self value:"service_settings"], "kind"), "system-service"),
        sb_gui_string(yyjson_doc_get_root(self.model), "service_error"));
    putText(self.serviceStatus, str(serviceText)); free(serviceText);
    [self.approvalBackend selectItemAtIndex:!strcmp(sb_gui_string(yyjson_doc_get_root(self.model), "approval_backend"), "administrator") ? 1 : 0];
    if (!self.serviceSettingsLoaded) {
        yyjson_val *service = [self value:"service_settings"], *item;
        [self.serviceKind selectItemAtIndex:!strcmp(sb_gui_string(service, "kind"), "system-service") ? 1 : 0];
        self.serviceName.stringValue = str(sb_gui_string(service, "name"));
        self.serviceDirectory.stringValue = str(sb_gui_string(service, "directory"));
        self.serviceKey.stringValue = str(sb_gui_string(service, "key"));
        self.serviceOAuth.state = sb_gui_bool(service, "oauth") ? NSControlStateValueOn : NSControlStateValueOff;
        NSMutableArray<NSString *> *starts = [NSMutableArray new];
        size_t si, sm;
        yyjson_arr_foreach(yyjson_obj_get(service, "starts"), si, sm, item)
            if (yyjson_is_str(item)) [starts addObject:str(yyjson_get_str(item))];
        self.serviceStarts.string = [starts componentsJoinedByString:@"\n"];
        self.serviceSettingsLoaded = YES;
    }
    if (!self.settingsLoaded) {
        yyjson_val *gateway = yyjson_obj_get([self value:"config"], "gateway"), *s = [self value:"settings"];
        self.port.stringValue = [NSString
            stringWithFormat:@"%llu", (unsigned long long)yyjson_get_uint(yyjson_obj_get(gateway, "port"))];
        self.url.stringValue = str(sb_gui_string(gateway, "public_url"));
        self.http.state = sb_gui_bool(s, "http");
        self.oauth.state = sb_gui_bool(s, "oauth");
        self.remoteOwner.state=*sb_gui_string(gateway,"owner_token_env")?NSControlStateValueOn:NSControlStateValueOff;
        self.ownerEnv.stringValue=str(sb_gui_string(gateway,"owner_token_env"));self.defaultProject.stringValue=str(sb_gui_string(gateway,"default_project_id"));
        self.settingsLoaded = YES;
        yyjson_val *admin = yyjson_obj_get(s, "administrator");
        self.rootRegistry.stringValue = str(sb_gui_string(admin, "registry"));
        self.rootState.stringValue = str(sb_gui_string(admin, "state"));
    }
    [self.projectRows removeAllObjects];
    size_t i, max;
    yyjson_val *p;
    NSInteger selected = -1;
    yyjson_arr_foreach(yyjson_obj_get([self value:"config"], "projects"), i, max, p) {
        NSString *identifier = str(sb_gui_string(p, "id"));
        if (!self.selected.length)
            self.selected = identifier;
        NSString *state = @"已停止";
        size_t j, jmax;
        yyjson_val *v;
        yyjson_arr_foreach(yyjson_obj_get([self value:"status"], "projects"), j, jmax,
                           v) if (!strcmp(sb_gui_string(v, "project_id"), identifier.UTF8String)) state =
            sb_gui_bool(v, "paused")    ? @"已暂停"
            : sb_gui_bool(v, "running") ? @"运行中"
                                        : @"已停止";
        if ([identifier isEqual:self.selected])
            selected = self.projectRows.count;
        [self.projectRows addObject:@{
            @"id" : identifier,
            @"title" : [NSString stringWithFormat:@"%@\n%@ · %@", *sb_gui_string(yyjson_obj_get(yyjson_obj_get([self value:"settings"],"project_options"),identifier.UTF8String),"display_name")?str(sb_gui_string(yyjson_obj_get(yyjson_obj_get([self value:"settings"],"project_options"),identifier.UTF8String),"display_name")):identifier, identifier, state]
        }];
    }
    [self.projects reloadData];
    if (selected >= 0)
        [self.projects selectRowIndexes:[NSIndexSet indexSetWithIndex:selected] byExtendingSelection:NO];
    yyjson_val *cfg = [self project];
    char *description =
        sb_gui_project_text(cfg, yyjson_obj_get([self value:"config"], "gateway"), [self value:"status"]);
    putText(self.detail, str(description));
    free(description);
    [self.requestRows removeAllObjects];
    const char *types[] = {"operation", "pending", "connections", "clients"};
    NSArray *titles = @[ @"操作审批", @"连接申请", @"已授权连接", @"客户端" ];
    selected = -1;
    for (unsigned t = 0; t < 4; t++) {
        yyjson_val *a = t ? yyjson_obj_get([self value:"oauth"], types[t])
                          : yyjson_obj_get([self value:"approvals"], "approvals");
        yyjson_arr_foreach(a, i, max, p) {
            NSString *identifier = str(sb_gui_string(p, t ? "id" : "approval_id")), *type = str(types[t]);
            if ([identifier isEqual:self.requestID] && [type isEqual:self.requestType])
                selected = self.requestRows.count;
            [self.requestRows addObject:@{
                @"id" : identifier,
                @"type" : type,
                @"title" : [NSString stringWithFormat:@"%@ · %@\n%@ · %@", titles[t],
                                                      str(sb_gui_string(p, t ? "client_name" : "tool")),
                                                      str(sb_gui_string(p, t ? "project" : "target")),
                                                      str(sb_gui_string(p, "status"))]
            }];
        }
    }
    [self.requests reloadData];
    if (selected >= 0)
        [self.requests selectRowIndexes:[NSIndexSet indexSetWithIndex:selected] byExtendingSelection:NO];
    yyjson_val *inspected = [self value:"inspected"];
    putText(self.requestDetail,
            pretty([self.requestType isEqual:@"operation"] &&
                           [self.requestID isEqual:str(sb_gui_string(inspected, "approval_id"))]
                       ? inspected
                       : [self currentRequest],
                   [self value:"config"]));
    self.rendering = NO;
}
- (void)send:(NSDictionary *)request {
    if (self.busy)
        return;
    NSMutableDictionary *q = [request mutableCopy];
    if (![q[@"project"] isKindOfClass:[NSString class]]) q[@"project"] = self.selected ?: @"";
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:q options:0 error:nil];
    NSString *json = [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
    self.busy = YES;
    self.serviceKind.enabled = NO;
    dispatch_async(self.queue, ^{
      char *reply = sb_desktop_work(self.desktop, json.UTF8String);
      dispatch_async(dispatch_get_main_queue(), ^{
        self.busy = NO;
        self.serviceKind.enabled = YES;
        yyjson_doc_free(self.model);
        self.model = reply ? yyjson_read(reply, strlen(reply), 0) : NULL;
        if(reply)sb_posix_wipe(reply,strlen(reply));
        free(reply);
        const char *secret=sb_gui_string(yyjson_doc_get_root(self.model),"clipboard");
        if(*secret){NSPasteboard *pasteboard=[NSPasteboard generalPasteboard];[pasteboard clearContents];[pasteboard setString:str(secret) forType:NSPasteboardTypeString];sb_posix_wipe((void *)secret,strlen(secret));}
        [self render];
        if (self.quitting) {
            if (sb_gui_bool(yyjson_doc_get_root(self.model), "online") &&
                sb_gui_bool(yyjson_doc_get_root(self.model), "owned")) {
                self.quitting = NO;
                [self alert:@"后台尚未停止，请稍后重试退出。"];
            } else
                [NSApp terminate:nil];
        }
      });
    });
}
- (void)submit:(NSString *)action {
    [self send:@{@"action" : action}];
}
- (void)approvalBackendChanged:(id)sender {
    (void)sender;
    if (self.rendering) return;
    self.requestID = @""; self.requestType = @"";
    [self send:@{@"action":@"approval_backend", @"backend":self.approvalBackend.indexOfSelectedItem == 1 ? @"administrator" : @"ordinary"}];
}
- (void)adminSave:(id)sender {
    (void)sender;
    [self send:@{@"action":@"admin_settings", @"root_registry":self.rootRegistry.stringValue, @"root_state":self.rootState.stringValue}];
}
- (void)adminStart:(id)sender {
    (void)sender;
    NSString *registry = [self.rootRegistry.stringValue copy], *state = [self.rootState.stringValue copy];
    [self confirm:@"将按当前已保存的项目配置打开系统管理员授权。只有明确允许管理员操作的项目获得本次会话；启动后可重启所选项目以连接。ShellBot 不读取或保存密码。"
        action:^{ [self send:@{@"action":@"admin_start", @"root_registry":registry, @"root_state":state}]; }];
}
- (void)adminStop:(id)sender {
    (void)sender;
    [self confirm:@"结束本窗口的管理员会话？所有所属管理员任务会结束，未提交上传会丢弃。普通项目可继续运行。" action:^{[self submit:@"admin_stop"];}];
}
- (void)adminCancel:(id)sender { (void)sender; sb_desktop_cancel(self.desktop); }
- (void)adminConnect:(id)sender {
    (void)sender; NSString *project = [self.selected copy];
    [self confirm:[NSString stringWithFormat:@"重启项目 %@ 并连接管理员代理？该项目的当前任务会结束；其他项目保留。",project]
        action:^{[self send:@{@"action":@"admin_connect_project",@"project":project}];}];
}
- (void)adminRevoke:(id)sender {
    (void)sender; NSString *project = [self.selected copy];
    [self confirm:[NSString stringWithFormat:@"撤销项目 %@ 的本次管理员授权？所属管理员任务会结束，其他项目保留。重新允许需要结束会话后再次授权。",project]
        action:^{[self send:@{@"action":@"admin_revoke",@"project":project}];}];
}
- (NSMutableDictionary *)serviceRequest:(NSString *)action {
    NSMutableArray<NSString *> *starts = [NSMutableArray new];
    for (NSString *part in [self.serviceStarts.string componentsSeparatedByCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@",\r\n"]]) {
        NSString *identifier = [part stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (identifier.length) [starts addObject:identifier];
    }
    return [@{@"action":action, @"kind":self.serviceKind.indexOfSelectedItem == 1 ? @"system-service" : @"user-service",
        @"name":self.serviceName.stringValue, @"directory":self.serviceDirectory.stringValue,
        @"key":self.serviceKey.stringValue, @"oauth":@(self.serviceOAuth.state == NSControlStateValueOn),
        @"starts":starts} mutableCopy];
}
- (void)serviceKindChanged:(id)sender {
    (void)sender;
    if (self.rendering) return;
    if (sb_gui_bool([self value:"service"], "registered")) {
        [self.serviceKind selectItemAtIndex:!strcmp(sb_gui_string([self value:"service_settings"], "kind"), "system-service") ? 1 : 0];
        [self alert:@"请先卸载已登记的服务，再更换服务类型或登记目录。"]; return;
    }
    const char *directory = sb_gui_string([self value:"service_default_directories"], self.serviceKind.indexOfSelectedItem == 1 ? "system-service" : "user-service");
    if (*directory) self.serviceDirectory.stringValue = str(directory);
}
- (void)serviceSave:(id)sender { (void)sender; [self send:[self serviceRequest:@"service_settings"]]; }
- (void)servicePlan:(id)sender { (void)sender; [self send:[self serviceRequest:@"service_plan"]]; }
- (void)serviceAction:(NSString *)action {
    if (self.busy) return;
    NSMutableDictionary *request = [self serviceRequest:action];
    yyjson_val *service = [self value:"service"], *plan = [self value:"service_plan"];
    BOOL install = [action isEqual:@"service_install"], system = [request[@"kind"] isEqual:@"system-service"];
    if (install) {
        NSString *sha = str(sb_gui_string(yyjson_doc_get_root(self.model), "service_plan_sha256"));
        if (!sha.length) { [self alert:@"请先点击“预览安装内容”，核对完整参数、启动项目和原生服务文件后再安装。"]; return; }
        request[@"expected_plan_sha256"] = sha;
    } else {
        const char *unit = sb_gui_string(service, "unit_id"), *sha = sb_gui_string(service, "content_sha256");
        if (*unit) request[@"expected_unit_id"] = str(unit);
        if (*sha) request[@"expected_content_sha256"] = str(sha);
    }
    NSString *unit = str(sb_gui_string(install ? plan : service, "unit_id"));
    NSString *path = str(sb_gui_string(install ? plan : service, "native_path"));
    NSString *operation = install ? @"安装" : [action isEqual:@"service_start"] ? @"启动" :
        [action isEqual:@"service_stop"] ? @"停止" : [action isEqual:@"service_uninstall"] ? @"卸载" : @"检查";
    NSString *effect = install ? @"安装仅登记服务，不立即启动；服务将使用预览中的项目清单。" :
        [action isEqual:@"service_start"] ? @"将启动服务及其登记的项目；已经运行的服务可能重启并结束当前任务。" :
        [action isEqual:@"service_stop"] || [action isEqual:@"service_uninstall"] ?
            @"所属项目和当前任务会结束，未提交上传会丢弃。数据和历史记录保留。" : @"读取系统服务最近状态。";
    NSString *message = [NSString stringWithFormat:@"%@服务？\n名称：%@\n服务标识：%@\n原生文件：%@\n\n%@%@", operation,
        request[@"name"], unit.length ? unit : @"尚未检查", path.length ? path : @"尚未检查", effect,
        system ? @"\n将打开系统管理员授权；后台仍以当前账户运行。ShellBot 不读取或保存密码。" : @""];
    NSDictionary *frozen = [request copy];
    [self confirm:message action:^{ [self send:frozen]; }];
}
- (void)serviceInstall:(id)sender { (void)sender; [self serviceAction:@"service_install"]; }
- (void)serviceStart:(id)sender { (void)sender; [self serviceAction:@"service_start"]; }
- (void)serviceStop:(id)sender { (void)sender; [self serviceAction:@"service_stop"]; }
- (void)serviceUninstall:(id)sender { (void)sender; [self serviceAction:@"service_uninstall"]; }
- (void)serviceRefresh:(id)sender {
    (void)sender;
    if (self.serviceKind.indexOfSelectedItem == 1) [self serviceAction:@"service_refresh"];
    else {
        NSMutableDictionary *request = [self serviceRequest:@"service_refresh"];
        const char *unit = sb_gui_string([self value:"service"], "unit_id"), *sha = sb_gui_string([self value:"service"], "content_sha256");
        if (*unit) request[@"expected_unit_id"] = str(unit);
        if (*sha) request[@"expected_content_sha256"] = str(sha);
        [self send:request];
    }
}
- (void)desktopMenu:(NSMenuItem *)item {
    [self submit:item.representedObject];
}
- (void)desktopPermissions:(id)sender {
    (void)sender;
    if (self.busy)
        return;
    NSAlert *dialog = [NSAlert new];
    dialog.messageText = @"所选项目的桌面权限";
    dialog.informativeText = @"先停止后台再保存。重新启动项目后手动连接桌面；系统权限仍需单独允许。";
    const char *keys[] = {"enabled", "control", "clipboard_read", "clipboard_write"};
    NSArray *titles = @[
        @"允许屏幕查看（整个共享屏幕）", @"允许键盘和鼠标控制", @"允许读取剪贴板文本", @"允许写入剪贴板文本"
    ];
    NSMutableArray<NSButton *> *checks = [NSMutableArray new];
    yyjson_val *policy = yyjson_obj_get(yyjson_obj_get([self project], "policy"), "desktop");
    for (unsigned i = 0; i < 4; i++) {
        NSButton *b = [NSButton checkboxWithTitle:titles[i] target:nil action:nil];
        b.state = sb_gui_bool(policy, keys[i]);
        [checks addObject:b];
    }
    NSStackView *view = stack(YES, checks);
    view.frame = NSMakeRect(0, 0, 420, 130);
    dialog.accessoryView = view;
    [dialog addButtonWithTitle:@"保存"];
    [dialog addButtonWithTitle:@"取消"];
    [dialog beginSheetModalForWindow:self.window
                   completionHandler:^(NSModalResponse response) {
                     if (response != NSAlertFirstButtonReturn)
                         return;
                     [self send:@{
                         @"action" : @"desktop_permissions",
                         @"enabled" : @(checks[0].state == NSControlStateValueOn),
                         @"control" : @(checks[1].state == NSControlStateValueOn),
                         @"clipboard_read" : @(checks[2].state == NSControlStateValueOn),
                         @"clipboard_write" : @(checks[3].state == NSControlStateValueOn)
                     }];
                   }];
}
- (void)command:(NSButton *)sender {
    NSString *a = sender.identifier;
    if ([@[ @"inspect", @"approve", @"deny", @"revoke", @"remove_client" ] containsObject:a]) {
        if (!self.requestID.length)
            return;
        if (![self.requestType isEqual:@"operation"]) {
            if ([a isEqual:@"inspect"]) {
                self.requestDetail.string = pretty([self currentRequest], [self value:"config"]);
                return;
            }
            if ([self.requestType isEqual:@"pending"] && ![@[ @"approve", @"deny" ] containsObject:a])
                return;
            if ([self.requestType isEqual:@"connections"] && ![a isEqual:@"revoke"])
                return;
            if ([self.requestType isEqual:@"clients"] && ![a isEqual:@"remove_client"])
                return;
            a = [a isEqual:@"remove_client"] ? @"oauth-remove-client" : [@"oauth-" stringByAppendingString:a];
        }
        [self send:@{
            @"action" : a,
            @"id" : self.requestID,
            @"persistent" : @(self.persistent.state == NSControlStateValueOn)
        }];
        self.persistent.state = NSControlStateValueOff;
    } else
        [self submit:a];
}
- (void)confirm:(NSString *)message action:(void (^)(void))action {
    if (self.busy) {
        [self alert:@"当前操作完成后可以继续。"];
        return;
    }
    NSAlert *a = [NSAlert new];
    a.messageText = message;
    [a addButtonWithTitle:@"继续"];
    [a addButtonWithTitle:@"取消"];
    [a beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse response) {
                if (response == NSAlertFirstButtonReturn)
                    action();
              }];
}
- (void)stopManager:(id)sender {
    (void)sender;
    [self confirm:@"停止后台会结束所有项目和运行中的命令。"
           action:^{
             [self submit:@"stop_manager"];
           }];
}
- (void)stopProject:(id)sender {
    (void)sender;
    [self confirm:@"停止项目会结束该项目运行中的命令。"
           action:^{
             [self submit:@"stop_project"];
           }];
}
- (void)quit:(id)sender {
    (void)sender;
    [self show:nil];
    [self confirm:@"退出 ShellBot？本次启动的后台、项目和命令将结束；连接到既有后台时会保留后台。"
           action:^{
             self.quitting = YES;
             [self submit:@"exit"];
           }];
}
- (void)add:(id)sender {
    (void)sender;
    [self editor:NO];
}
- (void)edit:(id)sender {
    (void)sender;
    [self editor:YES];
}
- (void)editor:(BOOL)editing {
    if (self.busy) return;
    if (sb_gui_bool(yyjson_doc_get_root(self.model), "online")) { [self alert:@"请先停止后台，再保存项目设置。"]; return; }
    sb_macos_project_editor(self.window, editing?[self project]:NULL, [self value:"config"], [self value:"settings"], sb_gui_string(yyjson_doc_get_root(self.model),"directory"), sb_gui_string(yyjson_doc_get_root(self.model),"registry_sha256"), ^(NSDictionary *q){[self send:q];});
}
- (void)chooseFolder:(NSButton *)sender {
    NSTextField *path = objc_getAssociatedObject(sender, @selector(chooseFolder:));
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = YES;
    p.canChooseFiles = NO;
    p.allowsMultipleSelection = NO;
    [p beginWithCompletionHandler:^(NSModalResponse r) {
      if (r == NSModalResponseOK)
          path.stringValue = p.URL.path;
    }];
}
- (void)remove:(id)sender {
    (void)sender;
    NSString *identifier = self.selected;
    [self confirm:[NSString
                      stringWithFormat:@"移除项目 %@？请先停止后台；项目文件、记录和凭据会保留。", identifier]
           action:^{
             [self send:@{@"action" : @"remove", @"id" : identifier}];
             self.selected = @"";
           }];
}
- (void)import:(id)sender {
    (void)sender;
    NSOpenPanel *p = [NSOpenPanel openPanel];
    p.canChooseDirectories = NO;
    p.canChooseFiles = YES;
    p.allowsMultipleSelection = NO;
    p.message = @"导入项目配置的副本，原文件保留。请先停止后台。";
    [p beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse response) {
                if (response == NSModalResponseOK)
                    [self send:@{@"action" : @"import", @"path" : p.URL.path}];
              }];
}
- (void)saveSettings:(id)sender {
    (void)sender;
    [self send:@{
        @"action" : @"settings",
        @"port" : @(self.port.integerValue),
        @"url" : self.url.stringValue,
        @"http" : @(self.http.state == NSControlStateValueOn),
        @"oauth" : @(self.oauth.state == NSControlStateValueOn),
        @"remote_owner":@(self.remoteOwner.state==NSControlStateValueOn),@"owner_env":self.ownerEnv.stringValue,@"default_project_id":self.defaultProject.stringValue
    }];
}
- (void)enableStartup:(id)sender { (void)sender;[self send:@{@"action":@"startup_install"}]; }
- (void)disableStartup:(id)sender { (void)sender;[self send:@{@"action":@"startup_uninstall"}]; }
- (void)cancelMigration:(id)sender {(void)sender;sb_desktop_cancel(self.desktop);}
- (void)recoverMigration:(id)sender{
    (void)sender;if(self.busy)return;
    NSString *identifier=str(sb_gui_string([self value:"migration_pending"],"migration_id"));
    [self confirm:@"取消尚未发布的迁移并恢复原数据位置？原数据和目标准备文件都保留。已发布的迁移不会撤销。" action:^{[self send:@{@"action":@"recover_migration",@"migration_id":identifier}];}];
}
- (void)migrateApplication:(id)sender {(void)sender;[self migrate:YES];}
- (void)migrateProject:(id)sender {(void)sender;[self migrate:NO];}
- (void)migrate:(BOOL)application {
    if(self.busy)return;
    if(sb_gui_bool(yyjson_doc_get_root(self.model),"online")){[self alert:@"请先停止后台。迁移应用数据还需取消登录自启；独立后台服务请先卸载，完成后重新登记。"];return;}
    NSOpenPanel *p=[NSOpenPanel openPanel];p.canChooseFiles=NO;p.canChooseDirectories=YES;p.canCreateDirectories=YES;p.allowsMultipleSelection=NO;
    p.message=@"选择空的私有目标目录。逐文件校验后切换，原目录保留。";
    [p beginSheetModalForWindow:self.window completionHandler:^(NSModalResponse response){if(response==NSModalResponseOK)[self send:@{@"action":application?@"migrate_application":@"migrate_project",@"path":p.URL.path}];}];
}
- (void)tick:(NSTimer *)timer {
    (void)timer;
    if (sb_desktop_raise(self.desktop))
        [self show:nil];
    if (sb_posix_signal && !self.busy) {
        self.quitting = YES;
        [self submit:@"exit"];
        return;
    }
#ifdef SHELLBOT_TESTS
    if (self.smoke && !self.busy && ++self.ticks == 4) {
        NSView *content = self.window.contentView;
        NSBitmapImageRep *image = [content bitmapImageRepForCachingDisplayInRect:content.bounds];
        [content cacheDisplayInRect:content.bounds toBitmapImageRep:image];
        NSBitmapImageRep *opaque = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
                                                                           pixelsWide:image.pixelsWide
                                                                           pixelsHigh:image.pixelsHigh
                                                                        bitsPerSample:8
                                                                      samplesPerPixel:4
                                                                             hasAlpha:YES
                                                                             isPlanar:NO
                                                                       colorSpaceName:NSDeviceRGBColorSpace
                                                                          bytesPerRow:0
                                                                         bitsPerPixel:0];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:[NSGraphicsContext graphicsContextWithBitmapImageRep:opaque]];
        [[NSColor windowBackgroundColor] setFill];
        NSRectFill(NSMakeRect(0, 0, image.pixelsWide, image.pixelsHigh));
        [image drawInRect:NSMakeRect(0, 0, image.pixelsWide, image.pixelsHigh)];
        [NSGraphicsContext restoreGraphicsState];
        image = opaque;
        [[image representationUsingType:NSBitmapImageFileTypePNG
                             properties:@{}] writeToFile:[self.smoke stringByAppendingString:@".png"]
                                              atomically:YES];
        [self.window performClose:nil];
        BOOL hidden = !self.window.isVisible;
        [self.statusItem.menu performActionForItemAtIndex:0];
        NSDictionary *report = @{
            @"status" : @"passed",
            @"native" : @"Cocoa",
            @"window" : @(self.window.isVisible),
            @"hide_restore" : @(hidden && self.window.isVisible),
            @"tray_registered" : @(self.statusItem.button != nil && self.statusItem.isVisible)
        };
        [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted
                                           error:nil] writeToFile:self.smoke atomically:YES];
        [self.timer invalidate];
        self.quitting = YES;
        [self submit:@"exit"];
        return;
    }
#endif
    if (!self.busy && !self.quitting)
        [self submit:@"refresh"];
}
@end

int sb_native_main(int argc, char **argv) {
    @autoreleasepool {
        const char *base = NULL, *key = NULL;
        NSString *smoke = nil;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--data-dir") && i + 1 < argc)
                base = argv[++i];
            else if (!strcmp(argv[i], "--credential-key-file") && i + 1 < argc)
                key = argv[++i];
#ifdef SHELLBOT_TESTS
            else if (!strcmp(argv[i], "--ui-smoke") && i + 1 < argc)
                smoke = str(argv[++i]);
#endif
            else if (strncmp(argv[i], "-psn_", 5)) {
                fprintf(stderr, "Invalid desktop arguments.\n");
                return 2;
            }
        }
        NSString *defaultBase = nil;
        if (!base) {
            NSString *support =
                [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"];
            defaultBase = [support stringByAppendingPathComponent:@"ShellBot"];
            base = defaultBase.fileSystemRepresentation;
        }
        sb_error e = {0};
        sb_native_manager *desktop = sb_desktop_open(base, key, &e);
        if (!desktop) {
            fprintf(stderr, "ShellBot: %s\n", e.text);
            return 2;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        ShellBotDelegate *delegate = [ShellBotDelegate new];
        delegate.desktop = desktop;
        delegate.smoke = smoke;
        NSApp.delegate = delegate;
        [NSApp run];
        [delegate.timer invalidate];
        yyjson_doc_free(delegate.model);
        sb_desktop_close(desktop);
        return 0;
    }
}
