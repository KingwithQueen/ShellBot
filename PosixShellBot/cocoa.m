#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#include "native.h"
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
@property NSTextField *summary, *message, *port, *url, *startupStatus;
@property NSButton *http, *oauth, *persistent;
@property NSMutableArray<NSDictionary *> *projectRows, *requestRows;
@property NSString *selected, *requestID, *requestType, *smoke;
@property BOOL busy, rendering, quitting, settingsLoaded;
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
    NSStackView *right = stack(YES, @[ actions, details ]);
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
        YES, @[ rd, self.persistent, label(@"未选中时为限时连接。连接授权不代替每次操作审批。"), decisions ]);
    NSStackView *requests = stack(NO, @[ requestList, requestRight ]);
    [requestList.heightAnchor constraintEqualToAnchor:requests.heightAnchor].active = YES;
    [requestRight.heightAnchor constraintEqualToAnchor:requests.heightAnchor].active = YES;
    [rd.widthAnchor constraintEqualToAnchor:requestRight.widthAnchor].active = YES;
    [requestRight.widthAnchor constraintGreaterThanOrEqualToConstant:600].active = YES;
    tab = [[NSTabViewItem alloc] initWithIdentifier:@"requests"];
    tab.label = @"审批与连接";
    tab.view = requests;
    [tabs addTabViewItem:tab];
    self.http = [NSButton checkboxWithTitle:@"启用本机 HTTP 网关（仅监听回环地址）" target:nil action:nil];
    self.oauth = [NSButton checkboxWithTitle:@"启用 OAuth 连接授权" target:nil action:nil];
    self.port = [NSTextField textFieldWithString:@"8765"];
    self.url = [NSTextField textFieldWithString:@""];
    self.url.placeholderString = @"https://shellbot.example.com";
    self.startupStatus=label(@"登录自启：正在检查");
    NSStackView *settings = stack(YES, @[
        self.http, label(@"本机端口"), self.port, label(@"公网 HTTPS 地址（隧道由你配置）"), self.url,
        self.oauth, label(@"凭据由系统钥匙串保护。保存前请停止后台；修改后手动启动后台和项目。"),
        [self special:@"保存连接设置" selector:@selector(saveSettings:)],
        self.startupStatus,
        label(@"登录自启只打开管理界面；独立后台服务请按 POSIX_STARTUP.md 安装。"),
        [self special:@"启用登录自启" selector:@selector(enableStartup:)],
        [self special:@"取消登录自启" selector:@selector(disableStartup:)]
    ]);
    [self.port.widthAnchor constraintEqualToConstant:180].active = YES;
    [self.url.widthAnchor constraintEqualToConstant:640].active = YES;
    tab = [[NSTabViewItem alloc] initWithIdentifier:@"settings"];
    tab.label = @"连接设置";
    tab.view = settings;
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
    if (!self.settingsLoaded) {
        yyjson_val *gateway = yyjson_obj_get([self value:"config"], "gateway"), *s = [self value:"settings"];
        self.port.stringValue = [NSString
            stringWithFormat:@"%llu", (unsigned long long)yyjson_get_uint(yyjson_obj_get(gateway, "port"))];
        self.url.stringValue = str(sb_gui_string(gateway, "public_url"));
        self.http.state = sb_gui_bool(s, "http");
        self.oauth.state = sb_gui_bool(s, "oauth");
        self.settingsLoaded = YES;
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
            @"title" : [NSString stringWithFormat:@"%@\n%@", identifier, state]
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
    q[@"project"] = self.selected ?: @"";
    NSData *bytes = [NSJSONSerialization dataWithJSONObject:q options:0 error:nil];
    NSString *json = [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding];
    self.busy = YES;
    dispatch_async(self.queue, ^{
      char *reply = sb_desktop_work(self.desktop, json.UTF8String);
      dispatch_async(dispatch_get_main_queue(), ^{
        self.busy = NO;
        yyjson_doc_free(self.model);
        self.model = reply ? yyjson_read(reply, strlen(reply), 0) : NULL;
        free(reply);
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
    if (self.busy)
        return;
    if (sb_gui_bool(yyjson_doc_get_root(self.model), "online")) {
        [self alert:@"请先停止后台，再修改项目。"];
        return;
    }
    NSAlert *a = [NSAlert new];
    a.messageText = editing ? @"编辑项目" : @"添加项目";
    a.informativeText = @"普通命令以当前账户运行，可访问该账户的文件。连接授权与每次操作审批独立，不获得管理"
                        @"员权限。编辑会使用所选权限方案。";
    NSTextField *identifier = [NSTextField textFieldWithString:editing ? self.selected : @""],
                *path = [NSTextField textFieldWithString:@""];
    identifier.enabled = !editing;
    NSPopUpButton *access = [NSPopUpButton new];
    [access addItemsWithTitles:@[
        @"只读所选目录", @"可修改所选目录（每次审批）", @"当前账户文件与普通命令（每次审批）"
    ]];
    if (editing) {
        yyjson_val *p = [self project], *w = yyjson_arr_get(yyjson_obj_get(p, "workspaces"), 0);
        path.stringValue = str(sb_gui_string(w, "path"));
        [access
            selectItemAtIndex:sb_gui_bool(yyjson_obj_get(yyjson_obj_get(p, "policy"), "execution"), "enabled")
                                  ? 2
                              : sb_gui_bool(w, "read_only") ? 0
                                                            : 1];
    }
    NSButton *choose = [NSButton buttonWithTitle:@"选择目录…" target:nil action:nil];
    choose.target = self;
    choose.action = @selector(chooseFolder:);
    objc_setAssociatedObject(choose, @selector(chooseFolder:), path, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSStackView *form = stack(
        YES,
        @[ label(@"项目标识"), identifier, label(@"工作目录"), path, choose, label(@"访问范围"), access ]);
    form.frame = NSMakeRect(0, 0, 600, 230);
    [identifier.widthAnchor constraintEqualToConstant:580].active = YES;
    [path.widthAnchor constraintEqualToConstant:580].active = YES;
    a.accessoryView = form;
    [a addButtonWithTitle:@"保存"];
    [a addButtonWithTitle:@"取消"];
    [a beginSheetModalForWindow:self.window
              completionHandler:^(NSModalResponse response) {
                if (response == NSAlertFirstButtonReturn)
                    [self send:@{
                        @"action" : editing ? @"edit" : @"add",
                        @"id" : identifier.stringValue,
                        @"path" : path.stringValue,
                        @"access" : @(access.indexOfSelectedItem)
                    }];
              }];
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
        @"oauth" : @(self.oauth.state == NSControlStateValueOn)
    }];
}
- (void)enableStartup:(id)sender { (void)sender;[self send:@{@"action":@"startup_install"}]; }
- (void)disableStartup:(id)sender { (void)sender;[self send:@{@"action":@"startup_uninstall"}]; }
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
