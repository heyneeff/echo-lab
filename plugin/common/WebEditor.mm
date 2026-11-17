#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "WebEditor.h"

// Each plugin bundle compiles its own copy; Objective-C classes share one global namespace per
// process, so the class names carry a per-target prefix (LAB_OBJC_PREFIX, set in CMake).
#ifndef LAB_OBJC_PREFIX
#define LAB_OBJC_PREFIX Lab
#endif
#define LAB_CAT2(a, b) a##b
#define LAB_CAT(a, b) LAB_CAT2(a, b)
#define LabBridge LAB_CAT(LAB_OBJC_PREFIX, Bridge)
#define LabView LAB_CAT(LAB_OBJC_PREFIX, View)

using namespace Steinberg;

// Receives postMessage() calls from the page and forwards parameter edits to the plugin.
@interface LabBridge : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) echolab::WebEditor* owner;
@end

@implementation LabBridge
- (void)userContentController:(WKUserContentController*)ucc didReceiveScriptMessage:(WKScriptMessage*)msg {
    if (!self.owner || ![msg.body isKindOfClass:[NSDictionary class]]) return;
    NSDictionary* d = (NSDictionary*)msg.body;
    NSString* type = d[@"type"];
    if ([type isEqualToString:@"log"]) { NSLog(@"[face] %@", d[@"msg"]); return; }
    if ([type isEqualToString:@"param"] && [d[@"name"] isKindOfClass:[NSString class]] && [d[@"value"] isKindOfClass:[NSNumber class]])
        self.owner->onMessage([d[@"name"] UTF8String], [d[@"value"] doubleValue]);
}
@end

// Owns the web view and the ~30 Hz timer that pushes live state into the page.
@interface LabView : NSObject
@property(nonatomic, strong) WKWebView* web;
@property(nonatomic, strong) LabBridge* bridge;
@property(nonatomic, strong) NSTimer* timer;
@property(nonatomic, assign) BOOL busy;
@end

@implementation LabView
@end

namespace echolab {

WebEditor::WebEditor(Hooks h, int width, int height) : CPluginView(nullptr), hooks(std::move(h)) {
    ViewRect r(0, 0, width, height);
    setRect(r);
}

WebEditor::~WebEditor() { removed(); }

tresult PLUGIN_API WebEditor::isPlatformTypeSupported(FIDString type) {
    return strcmp(type, kPlatformTypeNSView) == 0 ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API WebEditor::attached(void* parent, FIDString type) {
    if (strcmp(type, kPlatformTypeNSView) != 0 || !parent) return kResultFalse;
    @autoreleasepool {
        LabView* lv = [LabView new];
        lv.bridge = [LabBridge new];
        lv.bridge.owner = this;

        WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
        WKUserContentController* ucc = [WKUserContentController new];
        [ucc addScriptMessageHandler:lv.bridge name:@"lab"];
        NSString* boot = [NSString stringWithFormat:@"window.__UNIT = '%s'; window.addEventListener('error', e => window.webkit.messageHandlers.lab.postMessage({type:'log', msg: String(e.message) + ' @' + e.lineno + ':' + e.colno}));", hooks.unit.c_str()];
        [ucc addUserScript:[[WKUserScript alloc] initWithSource:boot injectionTime:WKUserScriptInjectionTimeAtDocumentStart forMainFrameOnly:YES]];
        cfg.userContentController = ucc;

        NSView* host = (__bridge NSView*)parent;
        NSRect frame = NSMakeRect(0, 0, rect.getWidth(), rect.getHeight());
        lv.web = [[WKWebView alloc] initWithFrame:frame configuration:cfg];
        [lv.web setValue:@NO forKey:@"drawsBackground"];
        lv.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        [host addSubview:lv.web];

        // the page ships inside the plugin bundle: Contents/Resources/faces.html
        NSBundle* bundle = [NSBundle bundleForClass:[LabBridge class]];
        NSURL* page = [bundle URLForResource:@"faces" withExtension:@"html"];
        if (page) [lv.web loadFileURL:page allowingReadAccessToURL:[page URLByDeletingLastPathComponent]];
        else [lv.web loadHTMLString:@"<body style='background:#0b0b0d;color:#ff8a1f;font:14px monospace;padding:40px'>faces.html missing from the plugin bundle</body>" baseURL:nil];

        __weak LabView* weakLV = lv;
        WebEditor* self_ = this;
        lv.timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 40.0 repeats:YES block:^(NSTimer*) {
            LabView* v = weakLV;
            if (!v || v.busy) return;
            std::string json = self_->feed();
            NSString* js = [NSString stringWithFormat:@"window.__host && window.__host(%s)", json.c_str()];
            v.busy = YES;
            [v.web evaluateJavaScript:js completionHandler:^(id, NSError*) { weakLV.busy = NO; }];
        }];
        [[NSRunLoop currentRunLoop] addTimer:lv.timer forMode:NSRunLoopCommonModes];
        // test hook: LAB_SNAPSHOT=/path.png writes what the editor shows after 4 s (used outside Live)
        if (const char* snap = getenv("LAB_SNAPSHOT")) {
            NSString* path = [NSString stringWithUTF8String:snap];
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 4 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                LabView* v = weakLV; if (!v) return;
                [v.web takeSnapshotWithConfiguration:nil completionHandler:^(NSImage* img, NSError*) {
                    if (!img) return;
                    CGImageRef cg = [img CGImageForProposedRect:nil context:nil hints:nil];
                    NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithCGImage:cg];
                    [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:path atomically:YES];
                }];
            });
        }
        impl = (__bridge_retained void*)lv;
    }
    return CPluginView::attached(parent, type);
}

tresult PLUGIN_API WebEditor::removed() {
    if (impl) {
        @autoreleasepool {
            LabView* lv = (__bridge_transfer LabView*)impl;
            impl = nullptr;
            [lv.timer invalidate];
            lv.bridge.owner = nullptr;
            [lv.web.configuration.userContentController removeScriptMessageHandlerForName:@"lab"];
            [lv.web removeFromSuperview];
        }
    }
    return CPluginView::removed();
}

}  // namespace echolab
