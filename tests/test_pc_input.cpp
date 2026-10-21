// SPDX-License-Identifier: GPL-3.0-or-later
#include "../engine/native/gpu/shim/input/bindings.h"
#include "../engine/native/gpu/shim/input/compat.h"
#include "../engine/native/gpu/shim/input/mouse_camera_step.h"
#include <SDL3/SDL.h>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
extern "C" {std::uint8_t bb_mouse_camera_free_now=0;}
void host_log(const char*,...) {}
bool debug_menu_active(){return false;}
uint32_t menu_confirm_button(){return 0x4000;}
uint32_t menu_back_button(){return 0x2000;}
static void verify_chords() {
    host_bindings_load_defaults();
    bool keys[SDL_SCANCODE_COUNT]={}, held[kBindCount]={};
    auto sample=[&](uint32_t mouse) {host_bindings_held(keys,SDL_SCANCODE_COUNT,mouse,held);};
    assert(host_bindings_parse("strong_attack","V, Left Shift + Mouse1"));
    assert(host_binding_ini_value(kBindStrong)=="V, Left Shift + Mouse1");
    sample(1);assert(held[kBindAttack] && !held[kBindStrong]);
    keys[SDL_SCANCODE_LSHIFT]=true;
    host_bindings_mouse_held_with_keys(keys, SDL_SCANCODE_COUNT, 1, held);
    assert(held[kBindStrong] && !held[kBindAttack]); // The real adapter's mouse path sees keyboard modifiers.
    sample(0);assert(!held[kBindStrong] && !held[kBindAttack]);
    sample(1);assert(held[kBindStrong] && !held[kBindAttack]);
    PadState p;host_bindings_apply(held,held[kBindStrongMod],p);
    assert(p.buttons==0x200 && p.r2==255);
    // Release the mouse first, then the modifier: no latched attack.
    sample(0);assert(!held[kBindStrong] && !held[kBindAttack]);
    keys[SDL_SCANCODE_LSHIFT]=false;sample(0);
    p=PadState{};host_bindings_apply(held,held[kBindStrongMod],p);assert(p.buttons==0);
    // Releasing Shift first stops the chord and restores the plain mouse binding.
    keys[SDL_SCANCODE_LSHIFT]=true;sample(1);assert(held[kBindStrong]);
    keys[SDL_SCANCODE_LSHIFT]=false;sample(1);assert(!held[kBindStrong] && held[kBindAttack]);
    keys[SDL_SCANCODE_RSHIFT]=true;sample(1);assert(!held[kBindStrong] && held[kBindAttack]);
    keys[SDL_SCANCODE_RSHIFT]=false;
    assert(host_bindings_parse("roll","Left Ctrl + Mouse2"));
    assert(host_bindings_parse("blood_vial","Right Shift + Left Ctrl + Mouse2"));
    assert(host_binding_ini_value(kBindBloodVial)=="Right Shift + Left Ctrl + Mouse2");
    keys[SDL_SCANCODE_LCTRL]=true;sample(2);
    assert(held[kBindRoll] && !held[kBindTransform] && !held[kBindBloodVial]);
    keys[SDL_SCANCODE_RSHIFT]=true;sample(2);
    assert(held[kBindBloodVial] && !held[kBindRoll] && !held[kBindTransform]);
    keys[SDL_SCANCODE_LALT]=true;sample(2);assert(held[kBindBloodVial]);
    keys[SDL_SCANCODE_LCTRL]=false;sample(2);assert(!held[kBindBloodVial] && !held[kBindRoll] && held[kBindTransform]);
    std::memset(keys,0,sizeof keys);
    assert(host_bindings_parse("roll","Left Ctrl + W"));
    keys[SDL_SCANCODE_W]=true;sample(0);assert(held[kBindMoveF] && !held[kBindRoll]);
    keys[SDL_SCANCODE_LCTRL]=true;sample(0);assert(!held[kBindMoveF] && held[kBindRoll]);
    keys[SDL_SCANCODE_W]=false;sample(0);assert(!held[kBindMoveF] && !held[kBindRoll]);
    keys[SDL_SCANCODE_W]=true;sample(0);assert(held[kBindRoll]);
    keys[SDL_SCANCODE_LCTRL]=false;sample(0);assert(held[kBindMoveF] && !held[kBindRoll]);
    keys[SDL_SCANCODE_W]=false;sample(0);assert(!held[kBindMoveF]);
    assert(host_bindings_parse("move_forward","Left Shift + W"));
    assert(host_bindings_parse("roll","Space"));
    keys[SDL_SCANCODE_W]=keys[SDL_SCANCODE_LSHIFT]=keys[SDL_SCANCODE_SPACE]=true;sample(0);
    p=PadState{};host_bindings_apply(held,held[kBindStrongMod],p);assert(p.ly==0 && p.buttons==0x2000);
    keys[SDL_SCANCODE_W]=false;sample(0);
    p=PadState{};host_bindings_apply(held,held[kBindStrongMod],p);assert(p.ly==127 && p.buttons==0x2000);
    keys[SDL_SCANCODE_SPACE]=keys[SDL_SCANCODE_LSHIFT]=false;sample(0);
    p=PadState{};host_bindings_apply(held,held[kBindStrongMod],p);assert(p.ly==127 && p.buttons==0);
    assert(host_bindings_parse("walk","Left Ctrl + Mouse4"));
    keys[SDL_SCANCODE_LSHIFT]=keys[SDL_SCANCODE_W]=keys[SDL_SCANCODE_LCTRL]=true;sample(8);
    p=PadState{};host_bindings_apply(held,false,p);assert(p.ly>0 && p.ly<127);
    sample(0);p=PadState{};host_bindings_apply(held,false,p);assert(p.ly==0);
    std::memset(keys,0,sizeof keys);
    assert(host_bindings_parse("use_item","WheelDown"));
    assert(host_bindings_parse("switch_item","Left Ctrl + WheelDown"));
    keys[SDL_SCANCODE_LCTRL]=true;sample(64);assert(held[kBindSwitchItem] && !held[kBindUseItem]);
    sample(0);assert(!held[kBindSwitchItem] && !held[kBindUseItem]);
    assert(host_bindings_parse("strong_attack","Left Ctrl + Keypad +, Right Alt + Mouse5"));
    assert(host_binding_key(kBindStrong)==SDL_SCANCODE_KP_PLUS && host_binding_mouse(kBindStrong)==5);
    assert(host_binding_ini_value(kBindStrong)=="Left Ctrl + Keypad +, Right Alt + Mouse5");
    keys[SDL_SCANCODE_KP_PLUS]=true;sample(0);assert(held[kBindStrong]);
    keys[SDL_SCANCODE_LCTRL]=false;sample(0);assert(!held[kBindStrong]);
    keys[SDL_SCANCODE_RALT]=true;sample(16);assert(held[kBindStrong]);
    char description[192], prompt[192];
    host_binding_describe(kBindStrong,description,sizeof description);
    host_binding_prompt(kBindStrong,prompt,sizeof prompt);
    assert(std::strstr(description,"Left Ctrl + Keypad +") && std::strstr(description,"Right Alt + Button 5"));
    assert(!std::strcmp(prompt,"Left Ctrl + Keypad +"));
    const auto previous=host_binding_ini_value(kBindStrong);
    assert(!host_bindings_parse("strong_attack","W + Mouse1"));
    assert(!host_bindings_parse("strong_attack","Left Ctrl + Left Ctrl + Mouse1"));
    assert(!host_bindings_parse("strong_attack","Left Ctrl + Left Ctrl"));
    assert(!host_bindings_parse("strong_attack","Left Ctrl + Unknown input"));
    assert(host_binding_ini_value(kBindStrong)==previous);
    FILE* file=std::tmpfile();assert(file);host_bindings_save(file);std::rewind(file);
    char ini[8192]={};std::fread(ini,1,sizeof ini-1,file);std::fclose(file);
    assert(std::strstr(ini,"bind.strong_attack=Left Ctrl + Keypad +, Right Alt + Mouse5\n"));
    host_bind_capture_begin(kBindStrong);assert(host_bind_capture_key(SDL_SCANCODE_F));
    assert(host_binding_ini_value(kBindStrong)=="F, Right Alt + Mouse5");
    host_bind_capture_note_held(false);
    host_binding_clear(kBindStrong);assert(host_binding_ini_value(kBindStrong)=="none");
    host_bindings_held(nullptr,0,16,held);assert(!held[kBindStrong]);
    host_bindings_load_defaults();
}
int main(){
    host_bindings_load_defaults();
    assert(host_binding_key(kBindWalk)==SDL_SCANCODE_LALT);
    assert(host_binding_mouse(kBindAttack)==1);
    assert(host_binding_ini_value(kBindAttack)=="Mouse1");
    bool h[kBindCount]={};h[kBindMoveF]=true;
    PadState p;host_bindings_apply(h,false,p);assert(p.lx==127 && p.ly==0);
    h[kBindMoveR]=true;p=PadState{};host_bindings_apply(h,false,p);assert(p.lx==254 && p.ly==0);
    h[kBindWalk]=true;p=PadState{};host_bindings_apply(h,false,p);
    const float diagonal=std::hypot(float(p.lx)-127,float(p.ly)-127);
    h[kBindMoveR]=false;p=PadState{};host_bindings_apply(h,false,p);
    assert(std::abs(diagonal-(127-p.ly))<=1);
    std::memset(h,0,sizeof h);h[kBindAttack]=true;p=PadState{};host_bindings_apply(h,true,p);
    assert(p.buttons==0x200 && p.r2==255);
    assert(host_bindings_parse("attack","E, Mouse2"));
    assert(host_binding_ini_value(kBindAttack)=="E, Mouse2");
    host_bind_capture_begin(kBindAttack);assert(host_bind_capture_key(SDL_SCANCODE_F));
    assert(host_binding_key(kBindAttack)==SDL_SCANCODE_F && host_binding_key(kBindUseItem)==0);
    assert(host_bind_capture_blocking());host_bind_capture_note_held(false);assert(!host_bind_capture_blocking());
    host_bind_capture_begin(kBindAttack);host_bind_capture_key(SDL_SCANCODE_ESCAPE);
    assert(host_binding_key(kBindAttack)==SDL_SCANCODE_F);host_bind_capture_note_held(false);
    host_binding_clear(kBindAttack);assert(host_binding_ini_value(kBindAttack)=="none");
    verify_chords();
    float sensitivity=mouse_camera::degrees_per_count(5);assert(std::abs(sensitivity-.25f)<1e-6);
    uint32_t bits;std::memcpy(&bits,&sensitivity,4);mouse_camera::g_degrees_per_count=bits;
    auto turn=[](int frames){
        float pitch=0,yaw=0;
        for(int i=0;i<frames;i++){
            mouse_camera::add(mouse_camera::g_counts,120.0f/frames,60.0f/frames);
            mouse_camera::begin_update();mouse_camera::turn(0,0,pitch,yaw,-1,1);
        }
        return std::pair{pitch,yaw};
    };
    const auto a=turn(30),b=turn(60),c=turn(90);
    assert(std::abs(a.first-b.first)<1e-5 && std::abs(a.second-c.second)<1e-5);
    float pitch=0,yaw=0;mouse_camera::g_frame={10,10};
    assert(!mouse_camera::turn(1,0,pitch,yaw,-1,1) && pitch==0 && yaw==0);
    mouse_camera::g_flags=mouse_camera::kInvertX|mouse_camera::kInvertY;
    assert(mouse_camera::turn(0,0,pitch,yaw,-1,1) && pitch<0 && yaw<0);
    mouse_camera::g_frame={10000,10000};mouse_camera::turn(0,0,pitch,yaw,-.5,.5);
    assert(pitch==-.5f && yaw>=-3.141593f && yaw<=3.141593f);
    std::puts("PC input: PASS (modifier chords, priority, releases, walk/sprint axes, remapping, capture, triggers, 30/60/90 camera, invert/limits)");
}
