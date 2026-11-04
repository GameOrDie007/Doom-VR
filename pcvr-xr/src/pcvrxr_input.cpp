// pcvr-xr - controllers: actions, bindings for five profiles, and haptics.
//
// Copyright (c) 2026 Game Or Die
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The layer binds every controller people actually own to one vocabulary. What
// any of it means stays in the port.

#include "internal.h"

namespace pcvrxr {
namespace {

const char* const kHandPath[2] = { "/user/hand/left", "/user/hand/right" };

// One row of a binding table: which action, and the path on this controller it
// comes from, per hand. A null path means this controller has no such input -
// a Vive wand has no A button - and that row is simply skipped for that hand.
struct Binding {
    XrAction Session::Impl::*action;
    const char* left;
    const char* right;
};

} // namespace

// ---------------------------------------------------------------------------
// Creating the actions
// ---------------------------------------------------------------------------

bool Session::Impl::CreateInput() {
    XrActionSetCreateInfo asci{ XR_TYPE_ACTION_SET_CREATE_INFO };
    strcpy(asci.actionSetName, "gameplay");
    strcpy(asci.localizedActionSetName, "Gameplay");
    asci.priority = 0;
    if (!Check(xrCreateActionSet(instance, &asci, &actionSet), "xrCreateActionSet")) {
        return false;
    }

    xrStringToPath(instance, kHandPath[0], &handPath[0]);
    xrStringToPath(instance, kHandPath[1], &handPath[1]);

    struct Decl {
        XrAction Session::Impl::*slot;
        XrActionType type;
        const char* name;
        const char* localized;   // must be unique: a duplicate silently kills
    };                           // the second action and the button never works
    const Decl decls[] = {
        { &Session::Impl::aimPose,         XR_ACTION_TYPE_POSE_INPUT,    "aim",              "Aim" },
        { &Session::Impl::gripPose,        XR_ACTION_TYPE_POSE_INPUT,    "grip",             "Grip" },
        { &Session::Impl::triggerValue,    XR_ACTION_TYPE_FLOAT_INPUT,   "trigger",          "Trigger" },
        { &Session::Impl::squeezeValue,    XR_ACTION_TYPE_FLOAT_INPUT,   "squeeze",          "Squeeze" },
        { &Session::Impl::thumbstickXY,    XR_ACTION_TYPE_VECTOR2F_INPUT,"thumbstick",       "Thumbstick" },
        { &Session::Impl::triggerClick,    XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_click",    "Trigger Click" },
        { &Session::Impl::squeezeClick,    XR_ACTION_TYPE_BOOLEAN_INPUT, "squeeze_click",    "Squeeze Click" },
        { &Session::Impl::thumbstickClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click", "Thumbstick Click" },
        { &Session::Impl::primaryClick,    XR_ACTION_TYPE_BOOLEAN_INPUT, "primary",          "Primary Button" },
        { &Session::Impl::secondaryClick,  XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary",        "Secondary Button" },
        { &Session::Impl::menuClick,       XR_ACTION_TYPE_BOOLEAN_INPUT, "menu",             "Menu Button" },
        { &Session::Impl::hapticOut,       XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic",        "Haptic Feedback" },
        // The Steam Frame's buttons one by one (bound on its own profile only).
        { &Session::Impl::padA,            XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_a",            "Pad A" },
        { &Session::Impl::padB,            XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_b",            "Pad B" },
        { &Session::Impl::padX,            XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_x",            "Pad X" },
        { &Session::Impl::padY,            XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_y",            "Pad Y" },
        { &Session::Impl::dpadUp,          XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_up",          "D-Pad Up" },
        { &Session::Impl::dpadDown,        XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_down",        "D-Pad Down" },
        { &Session::Impl::dpadLeft,        XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_left",        "D-Pad Left" },
        { &Session::Impl::dpadRight,       XR_ACTION_TYPE_BOOLEAN_INPUT, "dpad_right",       "D-Pad Right" },
        { &Session::Impl::shoulderClick,   XR_ACTION_TYPE_BOOLEAN_INPUT, "shoulder",         "Shoulder Button" },
        { &Session::Impl::viewClick,       XR_ACTION_TYPE_BOOLEAN_INPUT, "view",             "View Button" },
        { &Session::Impl::padMenu,         XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_menu",         "Pad Menu Button" },
    };

    for (const Decl& d : decls) {
        XrActionCreateInfo aci{ XR_TYPE_ACTION_CREATE_INFO };
        aci.actionType = d.type;
        strcpy(aci.actionName, d.name);
        strcpy(aci.localizedActionName, d.localized);
        aci.countSubactionPaths = 2;
        aci.subactionPaths = handPath;
        if (!Check(xrCreateAction(actionSet, &aci, &(this->*d.slot)), "xrCreateAction")) {
            Log("pcvr-xr: the action that failed was '%s'", d.name);
            return false;
        }
    }

    SuggestAllBindings();

    // Action spaces, so the poses can be located in the reference space.
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sci{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.poseInActionSpace.orientation.w = 1.0f;
        sci.subactionPath = handPath[h];
        sci.action = aimPose;
        xrCreateActionSpace(session, &sci, &aimSpace[h]);
        sci.action = gripPose;
        xrCreateActionSpace(session, &sci, &gripSpace[h]);
    }

    XrSessionActionSetsAttachInfo attach{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    attach.countActionSets = 1;
    attach.actionSets = &actionSet;
    if (!Check(xrAttachSessionActionSets(session, &attach), "xrAttachSessionActionSets")) {
        return false;
    }

    inputReady = true;
    Log("pcvr-xr: controllers ready");
    return true;
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------

void Session::Impl::SuggestAllBindings() {
    // Each profile is suggested on its own and reported on its own. Suggesting
    // them together means one bad path fails the lot, and the log then says
    // nothing about which controller was mis-bound - while the symptom is
    // "some buttons do nothing on one headset".
    //
    // Where a controller has no such input the path is null and the row is
    // skipped. Where an action is a different type from the input it is bound
    // to - a boolean action on a trigger's float value - the runtime applies
    // its own threshold, which is better than one picked here.

    struct Profile {
        const char* path;
        const char* name;
        Binding rows[30];
    };

    const Profile profiles[] = {
    {   "/interaction_profiles/khr/simple_controller", "any OpenXR controller", {
        { &Session::Impl::aimPose,      "/user/hand/left/input/aim/pose",    "/user/hand/right/input/aim/pose" },
        { &Session::Impl::gripPose,     "/user/hand/left/input/grip/pose",   "/user/hand/right/input/grip/pose" },
        { &Session::Impl::triggerClick, "/user/hand/left/input/select/click","/user/hand/right/input/select/click" },
        { &Session::Impl::menuClick,    "/user/hand/left/input/menu/click",  "/user/hand/right/input/menu/click" },
        { &Session::Impl::hapticOut,    "/user/hand/left/output/haptic",     "/user/hand/right/output/haptic" },
        { nullptr, nullptr, nullptr } } },

    {   "/interaction_profiles/oculus/touch_controller", "Meta Touch", {
        { &Session::Impl::aimPose,         "/user/hand/left/input/aim/pose",        "/user/hand/right/input/aim/pose" },
        { &Session::Impl::gripPose,        "/user/hand/left/input/grip/pose",       "/user/hand/right/input/grip/pose" },
        { &Session::Impl::triggerValue,    "/user/hand/left/input/trigger/value",   "/user/hand/right/input/trigger/value" },
        { &Session::Impl::triggerClick,    "/user/hand/left/input/trigger/value",   "/user/hand/right/input/trigger/value" },
        { &Session::Impl::squeezeValue,    "/user/hand/left/input/squeeze/value",   "/user/hand/right/input/squeeze/value" },
        { &Session::Impl::squeezeClick,    "/user/hand/left/input/squeeze/value",   "/user/hand/right/input/squeeze/value" },
        { &Session::Impl::thumbstickXY,    "/user/hand/left/input/thumbstick",      "/user/hand/right/input/thumbstick" },
        { &Session::Impl::thumbstickClick, "/user/hand/left/input/thumbstick/click","/user/hand/right/input/thumbstick/click" },
        { &Session::Impl::primaryClick,    "/user/hand/left/input/x/click",         "/user/hand/right/input/a/click" },
        { &Session::Impl::secondaryClick,  "/user/hand/left/input/y/click",         "/user/hand/right/input/b/click" },
        // Only the left controller has a menu button; the right one's
        // equivalent is the system button, which the runtime keeps.
        { &Session::Impl::menuClick,       "/user/hand/left/input/menu/click",      nullptr },
        { &Session::Impl::hapticOut,       "/user/hand/left/output/haptic",         "/user/hand/right/output/haptic" },
        { nullptr, nullptr, nullptr } } },

    {   "/interaction_profiles/valve/index_controller", "Valve Index", {
        { &Session::Impl::aimPose,         "/user/hand/left/input/aim/pose",        "/user/hand/right/input/aim/pose" },
        { &Session::Impl::gripPose,        "/user/hand/left/input/grip/pose",       "/user/hand/right/input/grip/pose" },
        { &Session::Impl::triggerValue,    "/user/hand/left/input/trigger/value",   "/user/hand/right/input/trigger/value" },
        { &Session::Impl::triggerClick,    "/user/hand/left/input/trigger/click",   "/user/hand/right/input/trigger/click" },
        { &Session::Impl::squeezeValue,    "/user/hand/left/input/squeeze/value",   "/user/hand/right/input/squeeze/value" },
        { &Session::Impl::squeezeClick,    "/user/hand/left/input/squeeze/value",   "/user/hand/right/input/squeeze/value" },
        { &Session::Impl::thumbstickXY,    "/user/hand/left/input/thumbstick",      "/user/hand/right/input/thumbstick" },
        { &Session::Impl::thumbstickClick, "/user/hand/left/input/thumbstick/click","/user/hand/right/input/thumbstick/click" },
        { &Session::Impl::primaryClick,    "/user/hand/left/input/a/click",         "/user/hand/right/input/a/click" },
        { &Session::Impl::secondaryClick,  "/user/hand/left/input/b/click",         "/user/hand/right/input/b/click" },
        // An Index controller has no menu button at all - its system button
        // belongs to the runtime. A port that needs one maps it to B itself,
        // which is a game's decision and not the layer's.
        { &Session::Impl::hapticOut,       "/user/hand/left/output/haptic",         "/user/hand/right/output/haptic" },
        { nullptr, nullptr, nullptr } } },

    {   "/interaction_profiles/htc/vive_controller", "HTC Vive wand", {
        { &Session::Impl::aimPose,         "/user/hand/left/input/aim/pose",       "/user/hand/right/input/aim/pose" },
        { &Session::Impl::gripPose,        "/user/hand/left/input/grip/pose",      "/user/hand/right/input/grip/pose" },
        { &Session::Impl::triggerValue,    "/user/hand/left/input/trigger/value",  "/user/hand/right/input/trigger/value" },
        { &Session::Impl::triggerClick,    "/user/hand/left/input/trigger/click",  "/user/hand/right/input/trigger/click" },
        // A wand's grip is a button, not an axis: as a float it reads 0 or 1.
        { &Session::Impl::squeezeValue,    "/user/hand/left/input/squeeze/click",  "/user/hand/right/input/squeeze/click" },
        { &Session::Impl::squeezeClick,    "/user/hand/left/input/squeeze/click",  "/user/hand/right/input/squeeze/click" },
        { &Session::Impl::thumbstickXY,    "/user/hand/left/input/trackpad",       "/user/hand/right/input/trackpad" },
        { &Session::Impl::thumbstickClick, "/user/hand/left/input/trackpad/click", "/user/hand/right/input/trackpad/click" },
        { &Session::Impl::menuClick,       "/user/hand/left/input/menu/click",     "/user/hand/right/input/menu/click" },
        { &Session::Impl::hapticOut,       "/user/hand/left/output/haptic",        "/user/hand/right/output/haptic" },
        { nullptr, nullptr, nullptr } } },

    {   "/interaction_profiles/microsoft/motion_controller", "Windows Mixed Reality", {
        { &Session::Impl::aimPose,         "/user/hand/left/input/aim/pose",        "/user/hand/right/input/aim/pose" },
        { &Session::Impl::gripPose,        "/user/hand/left/input/grip/pose",       "/user/hand/right/input/grip/pose" },
        { &Session::Impl::triggerValue,    "/user/hand/left/input/trigger/value",   "/user/hand/right/input/trigger/value" },
        { &Session::Impl::triggerClick,    "/user/hand/left/input/trigger/value",   "/user/hand/right/input/trigger/value" },
        { &Session::Impl::squeezeValue,    "/user/hand/left/input/squeeze/click",   "/user/hand/right/input/squeeze/click" },
        { &Session::Impl::squeezeClick,    "/user/hand/left/input/squeeze/click",   "/user/hand/right/input/squeeze/click" },
        { &Session::Impl::thumbstickXY,    "/user/hand/left/input/thumbstick",      "/user/hand/right/input/thumbstick" },
        { &Session::Impl::thumbstickClick, "/user/hand/left/input/thumbstick/click","/user/hand/right/input/thumbstick/click" },
        // No A or B; the trackpad click is the nearest thing to a face button.
        { &Session::Impl::primaryClick,    "/user/hand/left/input/trackpad/click",  "/user/hand/right/input/trackpad/click" },
        { &Session::Impl::menuClick,       "/user/hand/left/input/menu/click",      "/user/hand/right/input/menu/click" },
        { &Session::Impl::hapticOut,       "/user/hand/left/output/haptic",         "/user/hand/right/output/haptic" },
        { nullptr, nullptr, nullptr } } },

    // The Steam Frame's own controllers (XR_VALVE_frame_controller_interaction,
    // SteamVR 2.17.10+; asked for only when the runtime lists it). They are a
    // split gamepad: A B X Y on the right, a D-pad on the left, View where
    // Touch has Menu. Laid out exactly as SteamVR itself remaps our Touch
    // bindings onto a Frame - A and the D-pad's bottom are primary, every
    // other top button is secondary - so a Frame player finds the same
    // buttons whichever profile the runtime picks; a port changes it from
    // here, per button, without SteamVR's remap in the way.
    {   "/interaction_profiles/valve/frame_controller_valve", "Steam Frame", {
        { &Session::Impl::aimPose,         "/user/hand/left/input/aim/pose",          "/user/hand/right/input/aim/pose" },
        { &Session::Impl::gripPose,        "/user/hand/left/input/grip/pose",         "/user/hand/right/input/grip/pose" },
        { &Session::Impl::triggerValue,    "/user/hand/left/input/trigger/value",     "/user/hand/right/input/trigger/value" },
        { &Session::Impl::triggerClick,    "/user/hand/left/input/trigger/value",     "/user/hand/right/input/trigger/value" },
        { &Session::Impl::squeezeValue,    "/user/hand/left/input/squeeze/value",     "/user/hand/right/input/squeeze/value" },
        { &Session::Impl::squeezeClick,    "/user/hand/left/input/squeeze/value",     "/user/hand/right/input/squeeze/value" },
        { &Session::Impl::thumbstickXY,    "/user/hand/left/input/thumbstick",        "/user/hand/right/input/thumbstick" },
        { &Session::Impl::thumbstickClick, "/user/hand/left/input/thumbstick/click",  "/user/hand/right/input/thumbstick/click" },
        { &Session::Impl::primaryClick,    "/user/hand/left/input/dpad_down/click",   "/user/hand/right/input/a/click" },
        { &Session::Impl::secondaryClick,  "/user/hand/left/input/dpad_up/click",     "/user/hand/right/input/b/click" },
        { &Session::Impl::secondaryClick,  "/user/hand/left/input/dpad_left/click",   "/user/hand/right/input/x/click" },
        { &Session::Impl::secondaryClick,  "/user/hand/left/input/dpad_right/click",  "/user/hand/right/input/y/click" },
        { &Session::Impl::menuClick,       "/user/hand/left/input/view/click",        "/user/hand/right/input/menu/click" },
        { &Session::Impl::hapticOut,       "/user/hand/left/output/haptic",           "/user/hand/right/output/haptic" },
        // And each button on its own, for a port that lays the Frame out as
        // the gamepad it is (HandState::padLayout).
        { &Session::Impl::padA,            nullptr,                                   "/user/hand/right/input/a/click" },
        { &Session::Impl::padB,            nullptr,                                   "/user/hand/right/input/b/click" },
        { &Session::Impl::padX,            nullptr,                                   "/user/hand/right/input/x/click" },
        { &Session::Impl::padY,            nullptr,                                   "/user/hand/right/input/y/click" },
        { &Session::Impl::dpadUp,          "/user/hand/left/input/dpad_up/click",     nullptr },
        { &Session::Impl::dpadDown,        "/user/hand/left/input/dpad_down/click",   nullptr },
        { &Session::Impl::dpadLeft,        "/user/hand/left/input/dpad_left/click",   nullptr },
        { &Session::Impl::dpadRight,       "/user/hand/left/input/dpad_right/click",  nullptr },
        { &Session::Impl::shoulderClick,   "/user/hand/left/input/shoulder/click",    "/user/hand/right/input/shoulder/click" },
        { &Session::Impl::viewClick,       "/user/hand/left/input/view/click",        nullptr },
        { &Session::Impl::padMenu,         nullptr,                                   "/user/hand/right/input/menu/click" },
        { nullptr, nullptr, nullptr } } },
    };

    int accepted = 0, offered = 0;
    for (const Profile& p : profiles) {
        // A profile from an extension the runtime does not have is not a
        // valid path to it: skip it rather than log a rejection.
        if (strstr(p.path, "frame_controller_valve") && !haveFrameControllers) continue;
        ++offered;
        std::vector<XrActionSuggestedBinding> binds;
        for (const Binding& row : p.rows) {
            if (row.action == nullptr) break;
            const char* paths[2] = { row.left, row.right };
            for (int h = 0; h < 2; ++h) {
                if (paths[h] == nullptr) continue;
                XrPath path = XR_NULL_PATH;
                if (XR_FAILED(xrStringToPath(instance, paths[h], &path))) {
                    Log("pcvr-xr: %s - bad binding path '%s'", p.name, paths[h]);
                    continue;
                }
                XrActionSuggestedBinding b{};
                b.action = this->*row.action;
                b.binding = path;
                binds.push_back(b);
            }
        }

        XrPath profilePath = XR_NULL_PATH;
        xrStringToPath(instance, p.path, &profilePath);
        XrInteractionProfileSuggestedBinding sug{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        sug.interactionProfile = profilePath;
        sug.countSuggestedBindings = (uint32_t)binds.size();
        sug.suggestedBindings = binds.data();

        XrResult r = xrSuggestInteractionProfileBindings(instance, &sug);
        // SteamVR before 2.17.10 called the Frame's shoulder buttons 'bumper';
        // one unknown path fails the whole profile, so try the old name once.
        if (XR_FAILED(r) && strstr(p.path, "frame_controller_valve")) {
            XrPath oldL = XR_NULL_PATH, oldR = XR_NULL_PATH, newL = XR_NULL_PATH, newR = XR_NULL_PATH;
            xrStringToPath(instance, "/user/hand/left/input/shoulder/click", &newL);
            xrStringToPath(instance, "/user/hand/right/input/shoulder/click", &newR);
            xrStringToPath(instance, "/user/hand/left/input/bumper/click", &oldL);
            xrStringToPath(instance, "/user/hand/right/input/bumper/click", &oldR);
            for (XrActionSuggestedBinding& b : binds) {
                if (b.binding == newL) b.binding = oldL;
                else if (b.binding == newR) b.binding = oldR;
            }
            r = xrSuggestInteractionProfileBindings(instance, &sug);
            if (XR_SUCCEEDED(r)) Log("pcvr-xr: %s - 'shoulder' refused, 'bumper' taken (an older SteamVR)", p.name);
        }
        if (XR_SUCCEEDED(r)) {
            accepted++;
            Log("pcvr-xr: bindings for %s accepted (%u)", p.name, (unsigned)binds.size());
        } else {
            char name[XR_MAX_RESULT_STRING_SIZE] = {};
            xrResultToString(instance, r, name);
            Log("pcvr-xr: bindings for %s REJECTED: %s", p.name, name);
        }
    }
    Log("pcvr-xr: %d of %d controller profiles bound", accepted, offered);
}

// ---------------------------------------------------------------------------
// Reading it, once a frame
// ---------------------------------------------------------------------------

namespace {

// The whole reason edges live in the layer: an engine asks "is it down?" from
// deep inside a game loop, once a frame, with nowhere to hang a callback.
void Edge(Button& b, bool nowDown, bool nowTouched) {
    b.pressed = nowDown && !b.down;
    b.released = !nowDown && b.down;
    b.down = nowDown;
    b.touched = nowTouched;
}

bool Boolean(XrSession s, XrAction a, XrPath hand, bool* changed = nullptr) {
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    gi.subactionPath = hand;
    XrActionStateBoolean st{ XR_TYPE_ACTION_STATE_BOOLEAN };
    if (XR_FAILED(xrGetActionStateBoolean(s, &gi, &st)) || !st.isActive) return false;
    if (changed) *changed = st.changedSinceLastSync == XR_TRUE;
    return st.currentState == XR_TRUE;
}

float Float(XrSession s, XrAction a, XrPath hand) {
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    gi.subactionPath = hand;
    XrActionStateFloat st{ XR_TYPE_ACTION_STATE_FLOAT };
    if (XR_FAILED(xrGetActionStateFloat(s, &gi, &st)) || !st.isActive) return 0.0f;
    return st.currentState;
}

Vec2 Vector2(XrSession s, XrAction a, XrPath hand) {
    XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
    gi.action = a;
    gi.subactionPath = hand;
    XrActionStateVector2f st{ XR_TYPE_ACTION_STATE_VECTOR2F };
    Vec2 v;
    if (XR_FAILED(xrGetActionStateVector2f(s, &gi, &st)) || !st.isActive) return v;
    v.x = st.currentState.x;
    v.y = st.currentState.y;
    return v;
}

} // namespace

void Session::Impl::SyncInput(XrTime time) {
    if (!inputReady) return;

    // The fake hands run through exactly the same edge detection and land in
    // exactly the same place, so what a test proves about them is true of a
    // real controller. Everything the runtime would have said is skipped.
    if (fakeOn) {
        for (int h = 0; h < 2; ++h) {
            HandState& out = hands[h];
            const HandState& in = fake[h];
            const bool was[6] = {
                out.triggerClick.down, out.squeezeClick.down, out.thumbstickClick.down,
                out.primary.down, out.secondary.down, out.menu.down
            };
            out.active = in.active;
            out.aim = in.aim;
            out.grip = in.grip;
            out.linearVelocity = in.linearVelocity;
            out.angularVelocity = in.angularVelocity;
            out.velocityValid = in.velocityValid;
            out.trigger = in.trigger;
            out.squeeze = in.squeeze;
            out.thumbstick = in.thumbstick;
            Button* outs[6] = { &out.triggerClick, &out.squeezeClick, &out.thumbstickClick,
                                &out.primary, &out.secondary, &out.menu };
            const Button* ins[6] = { &in.triggerClick, &in.squeezeClick, &in.thumbstickClick,
                                     &in.primary, &in.secondary, &in.menu };
            for (int i = 0; i < 6; ++i) {
                outs[i]->down = was[i];
                Edge(*outs[i], ins[i]->down, ins[i]->touched);
            }
            out.padLayout = in.padLayout;
            Button* po[11] = { &out.padA, &out.padB, &out.padX, &out.padY, &out.dpadUp, &out.dpadDown,
                               &out.dpadLeft, &out.dpadRight, &out.shoulder, &out.view, &out.padMenu };
            const Button* pi[11] = { &in.padA, &in.padB, &in.padX, &in.padY, &in.dpadUp, &in.dpadDown,
                                     &in.dpadLeft, &in.dpadRight, &in.shoulder, &in.view, &in.padMenu };
            for (int i = 0; i < 11; ++i) Edge(*po[i], pi[i]->down, false);
        }
        return;
    }

    XrActiveActionSet active{ actionSet, XR_NULL_PATH };
    XrActionsSyncInfo si{ XR_TYPE_ACTIONS_SYNC_INFO };
    si.countActiveActionSets = 1;
    si.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session, &si))) return;

    for (int h = 0; h < 2; ++h) {
        HandState& s = hands[h];
        const XrPath hp = handPath[h];

        XrActionStateGetInfo gi{ XR_TYPE_ACTION_STATE_GET_INFO };
        gi.action = aimPose;
        gi.subactionPath = hp;
        XrActionStatePose posed{ XR_TYPE_ACTION_STATE_POSE };
        xrGetActionStatePose(session, &gi, &posed);
        s.active = posed.isActive == XR_TRUE;

        if (s.active) {
            // Velocity comes back chained onto the location, from the runtime's
            // own tracking filter. Differencing poses frame to frame instead
            // gives a noisier answer, and a worse one at exactly the moment it
            // matters - the fast part of a swing.
            XrSpaceVelocity vel{ XR_TYPE_SPACE_VELOCITY };
            XrSpaceLocation loc{ XR_TYPE_SPACE_LOCATION };
            loc.next = &vel;
            if (XR_SUCCEEDED(xrLocateSpace(aimSpace[h], refSpace, time, &loc))) {
                s.aim = FromXr(loc.pose, loc.locationFlags);
                s.velocityValid =
                    (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
                if (s.velocityValid) {
                    s.linearVelocity = { vel.linearVelocity.x, vel.linearVelocity.y,
                                         vel.linearVelocity.z };
                }
                if (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
                    s.angularVelocity = { vel.angularVelocity.x, vel.angularVelocity.y,
                                          vel.angularVelocity.z };
                }
            }
            XrSpaceLocation gloc{ XR_TYPE_SPACE_LOCATION };
            if (XR_SUCCEEDED(xrLocateSpace(gripSpace[h], refSpace, time, &gloc))) {
                s.grip = FromXr(gloc.pose, gloc.locationFlags);
            }
        } else {
            // An untracked hand keeps its last pose rather than snapping to the
            // origin, but says so - a controller that has gone to sleep should
            // not put a weapon at the player's feet for a frame.
            s.aim.tracked = false;
            s.grip.tracked = false;
        }

        s.trigger = Float(session, triggerValue, hp);
        s.squeeze = Float(session, squeezeValue, hp);
        s.thumbstick = Vector2(session, thumbstickXY, hp);

        Edge(s.triggerClick, Boolean(session, this->triggerClick, hp), s.trigger > 0.0f);
        Edge(s.squeezeClick, Boolean(session, this->squeezeClick, hp), s.squeeze > 0.0f);
        Edge(s.thumbstickClick, Boolean(session, this->thumbstickClick, hp), false);
        Edge(s.primary, Boolean(session, primaryClick, hp), false);
        Edge(s.secondary, Boolean(session, secondaryClick, hp), false);
        Edge(s.menu, Boolean(session, menuClick, hp), false);

        s.padLayout = profileName[h].find("frame_controller_valve") != std::string::npos;
        Edge(s.padA, Boolean(session, padA, hp), false);
        Edge(s.padB, Boolean(session, padB, hp), false);
        Edge(s.padX, Boolean(session, padX, hp), false);
        Edge(s.padY, Boolean(session, padY, hp), false);
        Edge(s.dpadUp, Boolean(session, dpadUp, hp), false);
        Edge(s.dpadDown, Boolean(session, dpadDown, hp), false);
        Edge(s.dpadLeft, Boolean(session, dpadLeft, hp), false);
        Edge(s.dpadRight, Boolean(session, dpadRight, hp), false);
        Edge(s.shoulder, Boolean(session, shoulderClick, hp), false);
        Edge(s.view, Boolean(session, viewClick, hp), false);
        Edge(s.padMenu, Boolean(session, padMenu, hp), false);
    }
}

void Session::Impl::ReadInteractionProfiles() {
    if (!inputReady) return;
    for (int h = 0; h < 2; ++h) {
        XrInteractionProfileState st{ XR_TYPE_INTERACTION_PROFILE_STATE };
        if (XR_FAILED(xrGetCurrentInteractionProfile(session, handPath[h], &st))) continue;
        const std::string was = profileName[h];
        if (st.interactionProfile == XR_NULL_PATH) {
            profileName[h] = "(none)";
        } else {
            char buf[XR_MAX_PATH_LENGTH] = {};
            uint32_t len = 0;
            if (XR_SUCCEEDED(xrPathToString(instance, st.interactionProfile,
                                            sizeof(buf), &len, buf))) {
                profileName[h] = buf;
            }
        }
        // Worth a line in every log, but only when it CHANGES. The runtime
        // sends this event more than once, and a line that repeats is a line
        // people stop reading - which matters, because when a button does
        // nothing this is the first thing to look at: if the profile is not
        // the one you bound, your bindings were never used.
        if (profileName[h] != was) {
            Log("pcvr-xr: %s hand bound to %s", h == 0 ? "left" : "right",
                profileName[h].c_str());
        }
    }
}

// ---------------------------------------------------------------------------
// The port-facing side
// ---------------------------------------------------------------------------

const HandState& Session::GetHand(Hand hand) const {
    return m->hands[m->PortHand(hand)];
}

const char* Session::InteractionProfile(Hand hand) const {
    return m->profileName[m->PortHand(hand)].c_str();
}

void Session::SetSwapHands(bool on) {
    if (m->swapHands == on) return;
    m->swapHands = on;
    m->Log("pcvr-xr: hands %s", on ? "SWAPPED - the port's right hand is the left controller (left-handed play)" : "as they are");
}

bool Session::SwapHands() const { return m->swapHands; }

void Session::Haptic(Hand hand, float seconds, float amplitude, float frequency) {
    if (!m->inputReady || m->fakeOn) return;

    XrHapticVibration v{ XR_TYPE_HAPTIC_VIBRATION };
    v.duration = (XrDuration)(seconds * 1e9);
    v.frequency = frequency > 0.0f ? frequency : XR_FREQUENCY_UNSPECIFIED;
    v.amplitude = amplitude < 0.0f ? 0.0f : (amplitude > 1.0f ? 1.0f : amplitude);

    XrHapticActionInfo hi{ XR_TYPE_HAPTIC_ACTION_INFO };
    hi.action = m->hapticOut;
    hi.subactionPath = m->handPath[m->PortHand(hand)];
    xrApplyHapticFeedback(m->session, &hi, (const XrHapticBaseHeader*)&v);
}

void Session::SetFakeHands(bool on) {
    if (m->fakeOn == on) return;
    m->fakeOn = on;
    // Leaving made-up state behind would be a debug driver that is not inert
    // when switched off, which is how a test harness ends up holding somebody's
    // trigger down for a whole session.
    if (!on) {
        m->fake[0] = HandState();
        m->fake[1] = HandState();
        m->hands[0] = HandState();
        m->hands[1] = HandState();
    }
    m->Log("pcvr-xr: fake hands %s", on ? "ON - this is not real input" : "off");
}

bool Session::FakeHands() const { return m->fakeOn; }

void Session::SetFakeHand(Hand hand, const HandState& state) {
    m->fake[hand == Hand::Right ? 1 : 0] = state;
}

} // namespace pcvrxr
