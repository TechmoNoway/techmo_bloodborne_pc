// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real SDL event -> pad adapter without loading a game or renderer.
#include "../engine/renderer/shim/input/input.h"
#include "../engine/renderer/shim/input/bindings.h"
#include "../engine/renderer/shim/input/compat.h"
#include "../engine/renderer/shim/bbport_settings.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>

static bool menu;
namespace BbSettings {
Values& Get() {static Values settings;return settings;}
bool ResolutionNeedsRestart() {return false;}
void Save() {}
}
namespace BbOverlay {
bool MenuOpen() {return false;}
bool CapturesInput() {return false;}
}
namespace BbEngine {
bool Prepare(Hook&,std::uint8_t*,std::size_t,std::size_t,std::span<const std::uint8_t>,Callback) {return false;}
void Commit(std::span<Hook>) {}
namespace Options {
bool ClickDecides(std::uint8_t*,int) {return false;}
void ListUpdate(std::uint8_t*,bool) {}
}
}
void mouse_camera_install(ElfImage*) {}
bool mouse_camera_installed() {return false;}
void mouse_camera_publish(int,bool,bool,bool) {}
bool mouse_camera_flick(float&,float&) {return false;}
void menu_pointer_install(ElfImage*) {}
bool menu_pointer_in_menu() {return menu;}
void menu_pointer_click() {}
std::uint32_t menu_pointer_take_press() {return 0;}
std::uint32_t menu_confirm_button() {return 0x4000;}
std::uint32_t menu_back_button() {return 0x2000;}

int main() {
    // The adapter may only read this process's empty Windows device, never a
    // launcher/game configuration in the directory from which the test is run.
    _putenv_s("BB_CONFIG","NUL");
    BbSettings::Get().pc_controls=true;
    assert(BbInput::Install(nullptr,0));
    bool keys[SDL_SCANCODE_COUNT]={};
    auto sample=[&](bool gamepad=false) {
        BbPcPad pad{};pad.lx=pad.ly=pad.rx=pad.ry=128;
        if(gamepad){pad.buttons=0x1000;pad.lx=210;}
        assert(bbgpu_pc_input(&pad,keys,SDL_SCANCODE_COUNT,gamepad));
        return pad;
    };
    auto mouse=[](std::uint32_t type,int button) {
        SDL_Event event{};event.type=type;event.button.button=static_cast<std::uint8_t>(button);
        BbInput::Event(event,false);
    };
    auto focus=[](std::uint32_t type) {SDL_Event event{};event.type=type;BbInput::Event(event,false);};
    assert(host_bindings_parse("strong_attack","V, Left Shift + Mouse1"));
    mouse(SDL_EVENT_MOUSE_BUTTON_DOWN,1);
    auto pad=sample();assert(pad.buttons==0x800 && pad.r2==0);
    keys[SDL_SCANCODE_LSHIFT]=true;
    pad=sample();assert(pad.buttons==0x200 && pad.r2==255);
    mouse(SDL_EVENT_MOUSE_BUTTON_UP,1);
    pad=sample();assert(pad.buttons==0 && pad.r2==0);
    // Same mouse input on a fresh pad sample; releasing Shift changes the
    // chord immediately, without a stale keyboard snapshot or a stuck trigger.
    mouse(SDL_EVENT_MOUSE_BUTTON_DOWN,1);pad=sample();assert(pad.buttons==0x200);
    keys[SDL_SCANCODE_LSHIFT]=false;pad=sample();assert(pad.buttons==0x800 && pad.r2==0);
    mouse(SDL_EVENT_MOUSE_BUTTON_UP,1);pad=sample();assert(pad.buttons==0);
    assert(host_bindings_parse("walk","Left Ctrl + Mouse4"));
    keys[SDL_SCANCODE_W]=keys[SDL_SCANCODE_LCTRL]=true;
    mouse(SDL_EVENT_MOUSE_BUTTON_DOWN,4);pad=sample();assert(pad.ly>0 && pad.ly<127);
    mouse(SDL_EVENT_MOUSE_BUTTON_UP,4);pad=sample();assert(pad.ly==0);
    keys[SDL_SCANCODE_LCTRL]=false;
    keys[SDL_SCANCODE_SPACE]=true;pad=sample();assert(pad.ly==0 && pad.buttons==0x2000);
    keys[SDL_SCANCODE_W]=false;pad=sample();assert(pad.ly==127 && pad.buttons==0x2000);
    keys[SDL_SCANCODE_SPACE]=keys[SDL_SCANCODE_LCTRL]=false;
    // Focus loss clears pending mouse edges and blocks keyboard input.
    keys[SDL_SCANCODE_W]=keys[SDL_SCANCODE_LSHIFT]=true;
    mouse(SDL_EVENT_MOUSE_BUTTON_DOWN,1);focus(SDL_EVENT_WINDOW_FOCUS_LOST);
    pad=sample();assert(pad.buttons==0 && pad.ly==127 && pad.r2==0);
    keys[SDL_SCANCODE_W]=keys[SDL_SCANCODE_LSHIFT]=false;
    focus(SDL_EVENT_WINDOW_FOCUS_GAINED);pad=sample();assert(pad.buttons==0 && pad.ly==127);
    keys[SDL_SCANCODE_LCTRL]=true;
    assert(host_bindings_parse("switch_item","Left Ctrl + WheelDown"));
    SDL_Event wheel{};wheel.type=SDL_EVENT_MOUSE_WHEEL;wheel.wheel.y=-1;BbInput::Event(wheel,false);
    // Ctrl retains its separate default L3 binding; only the wheel action is
    // a pulse, and a modifier release removes L3 on the following sample.
    pad=sample();assert(pad.buttons==0x42);
    pad=sample();assert(pad.buttons==0x2);
    keys[SDL_SCANCODE_LCTRL]=false;
    pad=sample();assert(pad.buttons==0);
    // Menu pointer controls keep their own behavior; combat chords stay idle.
    menu=true;keys[SDL_SCANCODE_LSHIFT]=true;mouse(SDL_EVENT_MOUSE_BUTTON_DOWN,1);
    pad=sample();assert(pad.buttons==0 && pad.r2==0);
    keys[SDL_SCANCODE_RETURN]=true;pad=sample();assert(pad.buttons==0x4000);
    keys[SDL_SCANCODE_RETURN]=keys[SDL_SCANCODE_LSHIFT]=false;
    mouse(SDL_EVENT_MOUSE_BUTTON_UP,1);sample();menu=false;
    pad=sample(true);assert(pad.buttons==0x1000 && pad.lx==210);
    host_bind_capture_begin(kBindAttack);
    keys[SDL_SCANCODE_W]=true;pad=sample();assert(pad.buttons==0 && pad.ly==127);
    host_bind_capture_key(SDL_SCANCODE_ESCAPE);
    keys[SDL_SCANCODE_W]=false;pad=sample();assert(pad.buttons==0 && pad.ly==127);
    std::puts("PC input bridge: PASS (chords, release order, walking, W+Space, focus, wheel, menus, gamepad, capture)");
}
