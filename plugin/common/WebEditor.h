#pragma once
// WebEditor: the plugin's face is ui/prototype/faces.html, shown in a native WKWebView.
//   page -> plugin : window.webkit.messageHandlers.lab.postMessage({type:"param", name, value})
//   plugin -> page : window.__host(json) about 30 times a second (parameters + live meters)
#include <functional>
#include <string>

#include "public.sdk/source/common/pluginview.h"

namespace echolab {

class WebEditor : public Steinberg::CPluginView {
public:
    struct Hooks {
        std::string unit;                                              // "echo" | "drive"
        std::function<std::string()> feed;                             // JSON for window.__host
        std::function<void(const std::string& name, double plain)> setParam;
    };
    WebEditor(Hooks hooks, int width, int height);
    ~WebEditor() override;

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API removed() SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API canResize() SMTG_OVERRIDE { return Steinberg::kResultFalse; }

    void onMessage(const std::string& name, double value) { if (hooks.setParam) hooks.setParam(name, value); }
    std::string feed() { return hooks.feed ? hooks.feed() : "{}"; }

private:
    Hooks hooks;
    void* impl = nullptr;  // Objective-C side (web view, timer, message handler)
};

}  // namespace echolab
