/// @file main_ios.mm
/// @brief elm327gui on iOS: the app, drawn with Dear ImGui on Metal.
///
/// UIKit runs the app with a scene (ElmSceneDelegate), whose window shows ElmViewController: an
/// MTKView that draws the same App as main.cpp does on the desktop, at 30 frames per second while the
/// worker is busy or monitoring, or a touch, a key or a result of the worker has just come, and four
/// per second otherwise. The Worker talks to the adapter on its thread and reaches Bluetooth Low
/// Energy adapters through Core Bluetooth (see ios_bridge.hpp), and Wi-Fi adapters itself.
///
/// The window keeps clear of the safe area's insets (the notch, the home indicator), a text field
/// brings up the keyboard, and the screen stays on while the app is connected. The definitions of
/// the Features tab and the backups are in the app's Documents, which the Files app shows.

#import <MetalKit/MetalKit.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

#include <atomic>
#include <cstdint>
#include <string_view>

#include "gui/app.hpp"
#include "gui/features.hpp"
#include "gui/ios_bridge.hpp"
#include "gui/worker.hpp"
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "imgui_internal.h"  // ImGuiViewportP: the work area, which keeps clear of the insets.

namespace {

using elm327::gui::App;
using elm327::gui::Worker;

/// In points: a little larger than on the desktop, for fingers.
constexpr float kFontSize{16.0F};

/// What ImGui's sizes grow by for touch, in points: the area around a widget that a finger may hit,
/// and the width of the scroll bars.
constexpr float kTouchPadding{4.0F};
constexpr float kScrollbarSize{20.0F};

/// Frames per second while something happens, and otherwise.
constexpr NSInteger kActiveFramesPerSecond{30};
constexpr NSInteger kIdleFramesPerSecond{4};

/// Frames drawn at the active rate after an event: ImGui takes one input event per frame.
constexpr std::uint32_t kFramesAfterEvent{8U};

/// The worker and the app embed large buffers: static storage, created on first use.
Worker& worker() noexcept {
    static Worker instance{};
    return instance;
}

App& app() noexcept {
    static App instance{worker()};
    return instance;
}

/// Frames still to draw at the active rate; set by events on any thread.
std::atomic<std::uint32_t> g_frames_left{0U};

/// Called by the worker's thread when it has published something.
void wake_up(void* /*context*/) noexcept {
    g_frames_left.store(kFramesAfterEvent, std::memory_order_relaxed);
}

/// Gives the Features tab its definitions: the simulator's, built in, then features.ini in the app's
/// Documents.
void load_definitions() noexcept {
    static elm327::gui::Definitions definitions{};
    elm327::gui::Path documents{};
    if (is_ok(elm327::gui::ios::documents_directory(documents))) {
        static_cast<void>(elm327::gui::set_config_directory(documents.view()));
    }
    elm327::gui::load_definitions(nullptr, definitions);
    app().set_features(definitions.features, definitions.source.view(), definitions.problem.view(),
                       definitions.directory.view());
}

}  // namespace

/// The view: Metal, and the on-screen keyboard, whose text goes to ImGui.
@interface ElmView : MTKView <UIKeyInput>
@end

@implementation ElmView

- (BOOL)canBecomeFirstResponder {
    return YES;
}

- (BOOL)hasText {
    return NO;
}

- (void)insertText:(NSString*)text {
    ImGuiIO& io{ImGui::GetIO()};
    if ([text isEqualToString:@"\n"]) {
        io.AddKeyEvent(ImGuiKey_Enter, true);  // Return sends what was typed, as on the desktop.
        io.AddKeyEvent(ImGuiKey_Enter, false);
    } else {
        io.AddInputCharactersUTF8(text.UTF8String);
    }
    g_frames_left.store(kFramesAfterEvent, std::memory_order_relaxed);
}

- (void)deleteBackward {
    ImGuiIO& io{ImGui::GetIO()};
    io.AddKeyEvent(ImGuiKey_Backspace, true);
    io.AddKeyEvent(ImGuiKey_Backspace, false);
    g_frames_left.store(kFramesAfterEvent, std::memory_order_relaxed);
}

// Commands and hexadecimal bytes: no corrections, no capitals.
- (UITextAutocorrectionType)autocorrectionType {
    return UITextAutocorrectionTypeNo;
}

- (UITextAutocapitalizationType)autocapitalizationType {
    return UITextAutocapitalizationTypeNone;
}

- (UIKeyboardType)keyboardType {
    return UIKeyboardTypeASCIICapable;
}

@end

/// Draws the App in the view, and takes the touches.
@interface ElmViewController : UIViewController <MTKViewDelegate>
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, strong) id<MTLCommandQueue> commandQueue;
@property(nonatomic, strong) ElmView* elmView;
@property(nonatomic) BOOL keyboard;
@property(nonatomic) BOOL screenOn;
@property(nonatomic) CFTimeInterval lastFrame;
@end

@implementation ElmViewController

- (void)loadView {
    self.device = MTLCreateSystemDefaultDevice();
    self.commandQueue = [self.device newCommandQueue];
    ElmView* const view{[[ElmView alloc] initWithFrame:UIScreen.mainScreen.bounds device:self.device]};
    view.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
    view.clearColor = MTLClearColorMake(0.08, 0.08, 0.09, 1.0);
    view.preferredFramesPerSecond = kActiveFramesPerSecond;
    view.multipleTouchEnabled = NO;
    view.delegate = self;
    self.elmView = view;
    self.view = view;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    IMGUI_CHECKVERSION();
    static_cast<void>(ImGui::CreateContext());
    ImGuiIO& io{ImGui::GetIO()};
    io.IniFilename = nullptr;  // Nothing to remember between runs.
    ImGui::StyleColorsDark();
    ImGuiStyle& style{ImGui::GetStyle()};
    style.FrameRounding = 3.0F;
    style.TouchExtraPadding = ImVec2{kTouchPadding, kTouchPadding};
    style.ScrollbarSize = kScrollbarSize;
    style.FontSizeBase = kFontSize;
    static_cast<void>(ImGui_ImplMetal_Init(self.device));

    load_definitions();
    worker().set_link_opener(&elm327::gui::ios::open_link, nullptr);
    if (is_ok(worker().start(&wake_up, nullptr))) {
        // Most adapters for an iPhone are on Wi-Fi; the list next to the device field finds the
        // Bluetooth ones.
        elm327::gui::StartOptions options{};
        static_cast<void>(options.settings.device.assign(elm327::kDefaultWifiDevice));
        app().start(options);
    }
    g_frames_left.store(kFramesAfterEvent, std::memory_order_relaxed);
}

- (void)drawInMTKView:(MTKView*)view {
    ImGuiIO& io{ImGui::GetIO()};
    // Without a platform backend the time between frames is the app's to tell.
    const CFTimeInterval now{CACurrentMediaTime()};
    io.DeltaTime = ((self.lastFrame > 0.0) && (now > self.lastFrame)) ? static_cast<float>(now - self.lastFrame)
                                                                       : (1.0F / static_cast<float>(kActiveFramesPerSecond));
    self.lastFrame = now;
    io.DisplaySize = ImVec2{static_cast<float>(view.bounds.size.width), static_cast<float>(view.bounds.size.height)};
    const float scale{static_cast<float>(view.contentScaleFactor)};
    io.DisplayFramebufferScale = ImVec2{scale, scale};

    // The keyboard comes up for a text field.
    if ((io.WantTextInput ? YES : NO) != self.keyboard) {
        self.keyboard = io.WantTextInput ? YES : NO;
        if (self.keyboard) {
            static_cast<void>([view becomeFirstResponder]);
        } else {
            static_cast<void>([view resignFirstResponder]);
        }
    }

    id<MTLCommandBuffer> const commandBuffer{[self.commandQueue commandBuffer]};
    MTLRenderPassDescriptor* const pass{view.currentRenderPassDescriptor};
    if (pass == nil) {
        [commandBuffer commit];
        return;
    }
    ImGui_ImplMetal_NewFrame(pass);
    ImGui::NewFrame();
    // From the next frame on, App draws in the work area, clear of the notch and the home indicator.
    const UIEdgeInsets insets{view.safeAreaInsets};
    ImGuiViewportP* const viewport{static_cast<ImGuiViewportP*>(ImGui::GetMainViewport())};
    viewport->BuildWorkInsetMin.x += static_cast<float>(insets.left);
    viewport->BuildWorkInsetMin.y += static_cast<float>(insets.top);
    viewport->BuildWorkInsetMax.x += static_cast<float>(insets.right);
    viewport->BuildWorkInsetMax.y += static_cast<float>(insets.bottom);
    app().draw();
    ImGui::Render();
    id<MTLRenderCommandEncoder> const encoder{[commandBuffer renderCommandEncoderWithDescriptor:pass]};
    ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), commandBuffer, encoder);
    [encoder endEncoding];
    [commandBuffer presentDrawable:view.currentDrawable];
    [commandBuffer commit];

    // The screen stays on while connected, e.g. for the dashboard of widget mode.
    const BOOL connected{worker().state().connected ? YES : NO};
    if (connected != self.screenOn) {
        self.screenOn = connected;
        UIApplication.sharedApplication.idleTimerDisabled = connected;
    }
    const std::uint32_t left{g_frames_left.load(std::memory_order_relaxed)};
    if (left > 0U) {
        g_frames_left.store(left - 1U, std::memory_order_relaxed);
    }
    const BOOL lively{(app().active() || (left > 0U)) ? YES : NO};
    view.preferredFramesPerSecond = lively ? kActiveFramesPerSecond : kIdleFramesPerSecond;
}

- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size {
    static_cast<void>(view);
    static_cast<void>(size);
    g_frames_left.store(kFramesAfterEvent, std::memory_order_relaxed);
}

/// One finger is the mouse: its position, and the button held while it touches.
- (void)takeTouches:(UIEvent*)event {
    UITouch* const touch{event.allTouches.anyObject};
    const CGPoint location{[touch locationInView:self.view]};
    ImGuiIO& io{ImGui::GetIO()};
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(static_cast<float>(location.x), static_cast<float>(location.y));
    bool touching{false};
    for (UITouch* each in event.allTouches) {
        touching = touching || ((each.phase != UITouchPhaseEnded) && (each.phase != UITouchPhaseCancelled));
    }
    io.AddMouseButtonEvent(0, touching);
    g_frames_left.store(kFramesAfterEvent, std::memory_order_relaxed);
    self.elmView.preferredFramesPerSecond = kActiveFramesPerSecond;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    static_cast<void>(touches);
    [self takeTouches:event];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    static_cast<void>(touches);
    [self takeTouches:event];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    static_cast<void>(touches);
    [self takeTouches:event];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    static_cast<void>(touches);
    [self takeTouches:event];
}

@end

/// The scene: one window with the view controller.
@interface ElmSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation ElmSceneDelegate

- (void)scene:(UIScene*)scene
    willConnectToSession:(UISceneSession*)session
                 options:(UISceneConnectionOptions*)options {
    static_cast<void>(session);
    static_cast<void>(options);
    id const window_scene{scene};  // The scene of an app with one window is a UIWindowScene.
    self.window = [[UIWindow alloc] initWithWindowScene:window_scene];
    self.window.rootViewController = [[ElmViewController alloc] init];
    [self.window makeKeyAndVisible];
}

@end

/// The application: the scene comes from Info.plist (UIApplicationSceneManifest).
@interface ElmAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation ElmAppDelegate

- (BOOL)application:(UIApplication*)application didFinishLaunchingWithOptions:(NSDictionary*)options {
    static_cast<void>(application);
    static_cast<void>(options);
    return YES;
}

- (void)applicationWillTerminate:(UIApplication*)application {
    static_cast<void>(application);
    worker().stop();  // Closes the connection.
}

@end

int main(int argc, char* argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([ElmAppDelegate class]));
    }
}
