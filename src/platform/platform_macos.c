/*
 * src/platform/platform_macos.c — macOS platform layer
 *
 * Pure C using the Objective-C runtime. No .m files, no ObjC compiler needed.
 * Links against: -framework Cocoa -framework CoreFoundation
 */

#ifdef __APPLE__

#include "forge/platform.h"
#include <time.h>

/* Objective-C runtime headers (always available on macOS) */
#include <objc/message.h>
#include <objc/runtime.h>

/* CoreFoundation for bundle path */
#include <CoreFoundation/CoreFoundation.h>

/* -------------------------------------------------------------------------- */
/* ObjC type shims                                                              */
/* -------------------------------------------------------------------------- */

/* On arm64, objc_msgSend has different signatures for stret/fpret.
 * On x86_64, we may need objc_msgSend_stret for struct returns.
 * For simplicity we use the standard objc_msgSend and cast. */

typedef id (*msg0_t)(id, SEL);
typedef id (*msg1_t)(id, SEL, id);
typedef id (*msg2_t)(id, SEL, id, id);
typedef id (*msg3_t)(id, SEL, id, id, id);
typedef id (*msg4_t)(id, SEL, id, id, id, id);
typedef id (*msg_int_t)(id, SEL, long);
typedef id (*msg_int2_t)(id, SEL, long, long);
typedef id (*msg_bool_t)(id, SEL, BOOL);
typedef id (*msg_rect_t)(id, SEL, CGRect);
typedef id (*msg_dbl4_t)(id, SEL, double, double, double, double);
typedef id (*msg_dbl2_t)(id, SEL, double, double);
typedef id (*msg_uint_t)(id, SEL, unsigned long long);
typedef id (*msg_str_t)(id, SEL, const char *);
typedef BOOL (*msg_bool_ret_t)(id, SEL);
typedef long (*msg_long_ret_t)(id, SEL);
typedef double (*msg_dbl_ret_t)(id, SEL);

/* We need CGFloat / CGRect for NSRect. Define them ourselves to avoid
 * pulling in the full NSGeometry.h */
#if defined(__LP64__) && __LP64__
typedef double fge_cgfloat_t;
#else
typedef float fge_cgfloat_t;
#endif

typedef struct {
    fge_cgfloat_t x, y;
} fge_cgpoint_t;

typedef struct {
    fge_cgfloat_t width, height;
} fge_cgsize_t;

typedef struct {
    fge_cgpoint_t origin;
    fge_cgsize_t size;
} fge_cgrect_t;

/* NSWindowStyleMask values */
enum {
    NSWindowStyleMaskBorderless             = 0,
    NSWindowStyleMaskTitled                 = 1 << 0,
    NSWindowStyleMaskClosable               = 1 << 1,
    NSWindowStyleMaskMiniaturizable         = 1 << 2,
    NSWindowStyleMaskResizable              = 1 << 3,
};

/* NSBackingStoreType */
enum {
    NSBackingStoreRetained    = 0,
    NSBackingStoreNonretained = 1,
    NSBackingStoreBuffered    = 2,
};

/* NSEventType */
enum {
    NSEventTypeLeftMouseDown      = 1,
    NSEventTypeLeftMouseUp        = 2,
    NSEventTypeRightMouseDown     = 3,
    NSEventTypeRightMouseUp       = 4,
    NSEventTypeMouseMoved         = 5,
    NSEventTypeLeftMouseDragged   = 6,
    NSEventTypeRightMouseDragged  = 7,
    NSEventTypeMouseEntered       = 8,
    NSEventTypeMouseExited        = 9,
    NSEventTypeKeyDown            = 10,
    NSEventTypeKeyUp              = 11,
    NSEventTypeFlagsChanged       = 12,
    NSEventTypeAppKitDefined      = 13,
    NSEventTypeSystemDefined      = 14,
    NSEventTypeApplicationDefined = 15,
    NSEventTypePeriodic           = 16,
    NSEventTypeCursorUpdate       = 17,
    NSEventTypeScrollWheel        = 22,
    NSEventTypeTabletPoint        = 23,
    NSEventTypeTabletProximity    = 24,
    NSEventTypeOtherMouseDown     = 25,
    NSEventTypeOtherMouseUp       = 26,
    NSEventTypeOtherMouseDragged  = 27,
};

/* NSEventMask */
#define NSEventMaskAny ((unsigned long long)~0ULL)

/* NSEvent modifier flags */
enum {
    NSEventModifierFlagCapsLock   = 1 << 16,
    NSEventModifierFlagShift      = 1 << 17,
    NSEventModifierFlagControl    = 1 << 18,
    NSEventModifierFlagOption     = 1 << 19,
    NSEventModifierFlagCommand    = 1 << 20,
};

/* NSApplicationActivationPolicy */
enum {
    NSApplicationActivationPolicyRegular   = 0,
    NSApplicationActivationPolicyAccessory = 1,
    NSApplicationActivationPolicyProhibited = 2,
};

/* -------------------------------------------------------------------------- */
/* macOS backend state                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    id app;
    id window;
    id view;
    id pool;
    int width, height;
    bool fullscreen;
    bool focused;
    bool cursor_visible;
    bool running;
    bool should_close;
    int mouse_x, mouse_y;
    uint32_t modifier_flags;
    fge_input_state_t *input;
} macos_backend_t;

static macos_backend_t *g_macos = NULL;

/* -------------------------------------------------------------------------- */
/* Selector cache                                                               */
/* -------------------------------------------------------------------------- */

static struct {
    SEL alloc;
    SEL init;
    SEL new_;
    SEL release;
    SEL autorelease;
    SEL retain;
    SEL self;
    SEL class_;
    SEL respondsToSelector_;
    /* NSApplication */
    SEL sharedApplication;
    SEL setActivationPolicy_;
    SEL setDelegate_;
    SEL delegate;
    SEL run;
    SEL finishLaunching;
    SEL nextEventMatchingMask_untilDate_inMode_dequeue_;
    SEL sendEvent_;
    SEL terminate_;
    SEL stop_;
    /* NSWindow */
    SEL initWithContentRect_styleMask_backing_defer_;
    SEL setTitle_;
    SEL makeKeyAndOrderFront_;
    SEL contentView;
    SEL setContentView_;
    SEL frame;
    SEL setFrame_display_;
    SEL center;
    SEL close;
    SEL isVisible;
    /* NSView */
    SEL initWithFrame_;
    SEL setNeedsDisplay_;
    SEL bounds;
    SEL window;
    SEL acceptsFirstResponder;
    SEL becomeFirstResponder;
    SEL keyDown_;
    SEL keyUp_;
    SEL flagsChanged_;
    SEL mouseDown_;
    SEL mouseUp_;
    SEL rightMouseDown_;
    SEL rightMouseUp_;
    SEL otherMouseDown_;
    SEL otherMouseUp_;
    SEL mouseMoved_;
    SEL mouseDragged_;
    SEL scrollWheel_;
    /* NSEvent */
    SEL type;
    SEL locationInWindow;
    SEL keyCode;
    SEL modifierFlags;
    SEL deltaX;
    SEL deltaY;
    SEL buttonNumber;
    SEL characters;
    SEL charactersIgnoringModifiers;
    SEL isARepeat;
    /* NSString */
    SEL UTF8String;
    SEL stringWithUTF8String_;
    /* NSDate */
    SEL distantPast;
    /* NSProcessInfo */
    SEL processInfo;
    SEL processName;
} sel_cache;

static void sel_cache_init(void) {
    sel_cache.alloc = sel_registerName("alloc");
    sel_cache.init = sel_registerName("init");
    sel_cache.new_ = sel_registerName("new");
    sel_cache.release = sel_registerName("release");
    sel_cache.autorelease = sel_registerName("autorelease");
    sel_cache.self = sel_registerName("self");
    sel_cache.class_ = sel_registerName("class");
    sel_cache.respondsToSelector_ = sel_registerName("respondsToSelector:");

    sel_cache.sharedApplication = sel_registerName("sharedApplication");
    sel_cache.setActivationPolicy_ = sel_registerName("setActivationPolicy:");
    sel_cache.setDelegate_ = sel_registerName("setDelegate:");
    sel_cache.delegate = sel_registerName("delegate");
    sel_cache.run = sel_registerName("run");
    sel_cache.finishLaunching = sel_registerName("finishLaunching");
    sel_cache.nextEventMatchingMask_untilDate_inMode_dequeue_ = sel_registerName("nextEventMatchingMask:untilDate:inMode:dequeue:");
    sel_cache.sendEvent_ = sel_registerName("sendEvent:");
    sel_cache.terminate_ = sel_registerName("terminate:");
    sel_cache.stop_ = sel_registerName("stop:");

    sel_cache.initWithContentRect_styleMask_backing_defer_ = sel_registerName("initWithContentRect:styleMask:backing:defer:");
    sel_cache.setTitle_ = sel_registerName("setTitle:");
    sel_cache.makeKeyAndOrderFront_ = sel_registerName("makeKeyAndOrderFront:");
    sel_cache.contentView = sel_registerName("contentView");
    sel_cache.setContentView_ = sel_registerName("setContentView:");
    sel_cache.frame = sel_registerName("frame");
    sel_cache.setFrame_display_ = sel_registerName("setFrame:display:");
    sel_cache.center = sel_registerName("center");
    sel_cache.close = sel_registerName("close");
    sel_cache.isVisible = sel_registerName("isVisible");

    sel_cache.initWithFrame_ = sel_registerName("initWithFrame:");
    sel_cache.setNeedsDisplay_ = sel_registerName("setNeedsDisplay:");
    sel_cache.bounds = sel_registerName("bounds");
    sel_cache.window = sel_registerName("window");
    sel_cache.acceptsFirstResponder = sel_registerName("acceptsFirstResponder");
    sel_cache.becomeFirstResponder = sel_registerName("becomeFirstResponder");
    sel_cache.keyDown_ = sel_registerName("keyDown:");
    sel_cache.keyUp_ = sel_registerName("keyUp:");
    sel_cache.flagsChanged_ = sel_registerName("flagsChanged:");
    sel_cache.mouseDown_ = sel_registerName("mouseDown:");
    sel_cache.mouseUp_ = sel_registerName("mouseUp:");
    sel_cache.rightMouseDown_ = sel_registerName("rightMouseDown:");
    sel_cache.rightMouseUp_ = sel_registerName("rightMouseUp:");
    sel_cache.otherMouseDown_ = sel_registerName("otherMouseDown:");
    sel_cache.otherMouseUp_ = sel_registerName("otherMouseUp:");
    sel_cache.mouseMoved_ = sel_registerName("mouseMoved:");
    sel_cache.mouseDragged_ = sel_registerName("mouseDragged:");
    sel_cache.scrollWheel_ = sel_registerName("scrollWheel:");

    sel_cache.type = sel_registerName("type");
    sel_cache.locationInWindow = sel_registerName("locationInWindow");
    sel_cache.keyCode = sel_registerName("keyCode");
    sel_cache.modifierFlags = sel_registerName("modifierFlags");
    sel_cache.deltaX = sel_registerName("deltaX");
    sel_cache.deltaY = sel_registerName("deltaY");
    sel_cache.buttonNumber = sel_registerName("buttonNumber");
    sel_cache.characters = sel_registerName("characters");
    sel_cache.charactersIgnoringModifiers = sel_registerName("charactersIgnoringModifiers");
    sel_cache.isARepeat = sel_registerName("isARepeat");

    sel_cache.UTF8String = sel_registerName("UTF8String");
    sel_cache.stringWithUTF8String_ = sel_registerName("stringWithUTF8String:");

    sel_cache.distantPast = sel_registerName("distantPast");

    sel_cache.processInfo = sel_registerName("processInfo");
    sel_cache.processName = sel_registerName("processName");
}

/* -------------------------------------------------------------------------- */
/* Helper macros for objc_msgSend                                               */
/* -------------------------------------------------------------------------- */

#define M0(obj, sel)          ((msg0_t)(void *)objc_msgSend)(obj, sel)
#define M1(obj, sel, a)       ((msg1_t)(void *)objc_msgSend)(obj, sel, a)
#define M2(obj, sel, a, b)    ((msg2_t)(void *)objc_msgSend)(obj, sel, a, b)
#define M3(obj, sel, a, b, c) ((msg3_t)(void *)objc_msgSend)(obj, sel, a, b, c)
#define Mi(obj, sel, a)       ((msg_int_t)(void *)objc_msgSend)(obj, sel, a)
#define Mi2(obj, sel, a, b)   ((msg_int2_t)(void *)objc_msgSend)(obj, sel, a, b)
#define Mb(obj, sel, a)       ((msg_bool_t)(void *)objc_msgSend)(obj, sel, a)
#define Mstr(obj, sel, a)     ((msg_str_t)(void *)objc_msgSend)(obj, sel, a)
#define MboolRet(obj, sel)    ((msg_bool_ret_t)(void *)objc_msgSend)(obj, sel)
#define MlongRet(obj, sel)    ((msg_long_ret_t)(void *)objc_msgSend)(obj, sel)
#define MdblRet(obj, sel)     ((msg_dbl_ret_t)(void *)objc_msgSend)(obj, sel)

/* For methods taking NSRect (which is a struct), we need objc_msgSend_stret on x86_64.
 * On arm64, objc_msgSend handles struct returns directly. */
#if defined(__arm64__)
#define MRECT(obj, sel, r)    ((msg_rect_t)(void *)objc_msgSend)(obj, sel, r)
#define MRECT4(obj, sel, a, b, c, d) ((msg_dbl4_t)(void *)objc_msgSend)(obj, sel, a, b, c, d)
#else
#define MRECT(obj, sel, r)    ((msg_rect_t)(void *)objc_msgSend_stret)(obj, sel, r)
#define MRECT4(obj, sel, a, b, c, d) ((msg_dbl4_t)(void *)objc_msgSend)(obj, sel, a, b, c, d)
#endif

/* -------------------------------------------------------------------------- */
/* macOS keycode mapping                                                        */
/* -------------------------------------------------------------------------- */


    switch (code) {
    case 0x00: return FGE_KEY_A;
    case 0x01: return FGE_KEY_S;
    case 0x02: return FGE_KEY_D;
    case 0x03: return FGE_KEY_F;
    case 0x04: return FGE_KEY_H;
    case 0x05: return FGE_KEY_G;
    case 0x06: return FGE_KEY_Z;
    case 0x07: return FGE_KEY_X;
    case 0x08: return FGE_KEY_C;
    case 0x09: return FGE_KEY_V;
    case 0x0B: return FGE_KEY_B;
    case 0x0C: return FGE_KEY_Q;
    case 0x0D: return FGE_KEY_W;
    case 0x0E: return FGE_KEY_E;
    case 0x0F: return FGE_KEY_R;
    case 0x10: return FGE_KEY_Y;
    case 0x11: return FGE_KEY_T;
    case 0x12: return FGE_KEY_1;
    case 0x13: return FGE_KEY_2;
    case 0x14: return FGE_KEY_3;
    case 0x15: return FGE_KEY_4;
    case 0x16: return FGE_KEY_6;
    case 0x17: return FGE_KEY_5;
    case 0x18: return FGE_KEY_NONE; /* =/+ */
    case 0x19: return FGE_KEY_9;
    case 0x1A: return FGE_KEY_7;
    case 0x1B: return FGE_KEY_NONE; /* -/_ */
    case 0x1C: return FGE_KEY_8;
    case 0x1D: return FGE_KEY_0;
    case 0x1E: return FGE_KEY_NONE; /* ] */
    case 0x1F: return FGE_KEY_O;
    case 0x20: return FGE_KEY_U;
    case 0x21: return FGE_KEY_NONE; /* [ */
    case 0x22: return FGE_KEY_I;
    case 0x23: return FGE_KEY_P;
    case 0x25: return FGE_KEY_L;
    case 0x26: return FGE_KEY_J;
    case 0x27: return FGE_KEY_NONE; /* ' */
    case 0x28: return FGE_KEY_K;
    case 0x29: return FGE_KEY_NONE; /* ; */
    case 0x2A: return FGE_KEY_NONE; /* \ */
    case 0x2B: return FGE_KEY_NONE; /* , */
    case 0x2C: return FGE_KEY_NONE; /* / */
    case 0x2D: return FGE_KEY_N;
    case 0x2E: return FGE_KEY_M;
    case 0x2F: return FGE_KEY_NONE; /* . */
    case 0x32: return FGE_KEY_NONE; /* ` */
    case 0x24: return FGE_KEY_ENTER;
    case 0x30: return FGE_KEY_TAB;
    case 0x31: return FGE_KEY_SPACE;
    case 0x33: return FGE_KEY_BACKSPACE;
    case 0x35: return FGE_KEY_ESCAPE;
    case 0x37: return FGE_KEY_LMETA; /* Command */
    case 0x38: return FGE_KEY_LSHIFT;
    case 0x39: return FGE_KEY_NONE; /* CapsLock */
    case 0x3A: return FGE_KEY_LALT; /* Option */
    case 0x3B: return FGE_KEY_LCTRL; /* Control */
    case 0x3C: return FGE_KEY_RSHIFT;
    case 0x3D: return FGE_KEY_RALT; /* Right Option */
    case 0x3E: return FGE_KEY_RCTRL; /* Right Control */
    case 0x3F: return FGE_KEY_RMETA; /* Function */
    case 0x40: return FGE_KEY_NONE; /* F17 */
    case 0x41: return FGE_KEY_NONE; /* KP . */
    case 0x43: return FGE_KEY_NONE; /* KP * */
    case 0x45: return FGE_KEY_NONE; /* KP + */
    case 0x47: return FGE_KEY_NONE; /* Clear */
    case 0x48: return FGE_KEY_NONE; /* Volume up */
    case 0x49: return FGE_KEY_NONE; /* Volume down */
    case 0x4A: return FGE_KEY_NONE; /* Mute */
    case 0x4B: return FGE_KEY_NONE; /* KP / */
    case 0x4C: return FGE_KEY_NONE; /* KP Enter */
    case 0x4E: return FGE_KEY_NONE; /* KP - */
    case 0x4F: return FGE_KEY_NONE; /* F18 */
    case 0x50: return FGE_KEY_NONE; /* F19 */
    case 0x51: return FGE_KEY_NONE; /* KP = */
    case 0x52: return FGE_KEY_NONE; /* KP 0 */
    case 0x53: return FGE_KEY_NONE; /* KP 1 */
    case 0x54: return FGE_KEY_NONE; /* KP 2 */
    case 0x55: return FGE_KEY_NONE; /* KP 3 */
    case 0x56: return FGE_KEY_NONE; /* KP 4 */
    case 0x57: return FGE_KEY_NONE; /* KP 5 */
    case 0x58: return FGE_KEY_NONE; /* KP 6 */
    case 0x59: return FGE_KEY_NONE; /* KP 7 */
    case 0x5B: return FGE_KEY_NONE; /* KP 8 */
    case 0x5C: return FGE_KEY_NONE; /* KP 9 */
    case 0x5D: return FGE_KEY_NONE;
    case 0x5E: return FGE_KEY_NONE;
    case 0x5F: return FGE_KEY_NONE;
    case 0x60: return FGE_KEY_F5;
    case 0x61: return FGE_KEY_F6;
    case 0x62: return FGE_KEY_F7;
    case 0x63: return FGE_KEY_F3;
    case 0x64: return FGE_KEY_F8;
    case 0x65: return FGE_KEY_F9;
    case 0x66: return FGE_KEY_NONE;
    case 0x67: return FGE_KEY_F11;
    case 0x68: return FGE_KEY_NONE;
    case 0x69: return FGE_KEY_NONE; /* F13 */
    case 0x6A: return FGE_KEY_NONE; /* F16 */
    case 0x6B: return FGE_KEY_NONE; /* F14 */
    case 0x6D: return FGE_KEY_F10;
    case 0x6E: return FGE_KEY_NONE;
    case 0x6F: return FGE_KEY_F12;
    case 0x71: return FGE_KEY_NONE; /* F15 */
    case 0x72: return FGE_KEY_INSERT;
    case 0x73: return FGE_KEY_HOME;
    case 0x74: return FGE_KEY_PAGE_UP;
    case 0x75: return FGE_KEY_DELETE;
    case 0x76: return FGE_KEY_F4;
    case 0x77: return FGE_KEY_END;
    case 0x78: return FGE_KEY_F2;
    case 0x79: return FGE_KEY_PAGE_DOWN;
    case 0x7A: return FGE_KEY_F1;
    case 0x7B: return FGE_KEY_LEFT;
    case 0x7C: return FGE_KEY_RIGHT;
    case 0x7D: return FGE_KEY_DOWN;
    case 0x7E: return FGE_KEY_UP;
    default:   return FGE_KEY_NONE;
    }
}

/* -------------------------------------------------------------------------- */
/* View subclass creation at runtime                                            */
/* -------------------------------------------------------------------------- */

static void forge_view_mouse_down(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    (void)event;
    m->input->mouse_buttons[FGE_MOUSE_LEFT] = true;
}

static void forge_view_mouse_up(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    (void)event;
    m->input->mouse_buttons[FGE_MOUSE_LEFT] = false;
}

static void forge_view_right_mouse_down(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    (void)event;
    m->input->mouse_buttons[FGE_MOUSE_RIGHT] = true;
}

static void forge_view_right_mouse_up(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    (void)event;
    m->input->mouse_buttons[FGE_MOUSE_RIGHT] = false;
}

static void forge_view_other_mouse_down(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    long btn = MlongRet(event, sel_cache.buttonNumber);
    if (btn >= 0 && btn < 6) m->input->mouse_buttons[(int)btn] = true;
}

static void forge_view_other_mouse_up(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    long btn = MlongRet(event, sel_cache.buttonNumber);
    if (btn >= 0 && btn < 6) m->input->mouse_buttons[(int)btn] = false;
}

static void forge_view_mouse_moved(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    fge_cgpoint_t loc;
    /* locationInWindow returns NSPoint. On x86_64 this is a struct return.
     * We need to handle this carefully. */
#if defined(__arm64__)
    loc = ((fge_cgpoint_t (*)(id, SEL))objc_msgSend)(event, sel_cache.locationInWindow);
#else
    /* On x86_64, objc_msgSend_stret for struct return */
    ((void (*)(fge_cgpoint_t *, id, SEL))objc_msgSend_stret)(&loc, event, sel_cache.locationInWindow);
#endif
    id window = M0(self, sel_cache.window);
    if (window) {
        /* Convert to view coordinates */
        fge_cgrect_t bounds;
#if defined(__arm64__)
        bounds = ((fge_cgrect_t (*)(id, SEL))objc_msgSend)(self, sel_cache.bounds);
#else
        ((void (*)(fge_cgrect_t *, id, SEL))objc_msgSend_stret)(&bounds, self, sel_cache.bounds);
#endif
        (void)bounds;
        /* loc is in window coordinates; flip Y */
        fge_cgrect_t wframe;
#if defined(__arm64__)
        wframe = ((fge_cgrect_t (*)(id, SEL))objc_msgSend)(window, sel_cache.frame);
#else
        ((void (*)(fge_cgrect_t *, id, SEL))objc_msgSend_stret)(&wframe, window, sel_cache.frame);
#endif
        int mx = (int)loc.x;
        int my = (int)(wframe.size.height - loc.y);
        m->input->mouse_delta.x = (float)(mx - m->mouse_x);
        m->input->mouse_delta.y = (float)(my - m->mouse_y);
        m->input->mouse_pos.x = (float)mx;
        m->input->mouse_pos.y = (float)my;
        m->mouse_x = mx;
        m->mouse_y = my;
    }
}

static void forge_view_scroll_wheel(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    double dx = MdblRet(event, sel_cache.deltaX);
    double dy = MdblRet(event, sel_cache.deltaY);
    m->input->mouse_scroll.x += (float)dx;
    m->input->mouse_scroll.y += (float)dy;
}

static BOOL forge_view_accepts_first_responder(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return YES;
}

static BOOL forge_view_become_first_responder(id self, SEL _cmd) {
    (void)self; (void)_cmd;
    return YES;
}

static void forge_view_key_down(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    unsigned short code = (unsigned short)MlongRet(event, sel_cache.keyCode);
    fge_key_t key = macos_keycode_to_fge(code);
    if (key != FGE_KEY_NONE && (size_t)key < FGE_KEY_COUNT) {
        m->input->keys[key] = true;
    }
    /* Also capture text input from characters */
    id chars = M0(event, sel_cache.characters);
    if (chars) {
        const char *utf8 = ((const char *(*)(id, SEL))objc_msgSend)(chars, sel_cache.UTF8String);
        if (utf8) {
            size_t len = strlen(utf8);
            int room = (int)sizeof(m->input->text_input) - m->input->text_input_len - 1;
            if (room > 0 && len > 0) {
                size_t to_copy = len < (size_t)room ? len : (size_t)room;
                memcpy(m->input->text_input + m->input->text_input_len, utf8, to_copy);
                m->input->text_input_len += (int)to_copy;
            }
        }
    }
}

static void forge_view_key_up(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    unsigned short code = (unsigned short)MlongRet(event, sel_cache.keyCode);
    fge_key_t key = macos_keycode_to_fge(code);
    if (key != FGE_KEY_NONE && (size_t)key < FGE_KEY_COUNT) {
        m->input->keys[key] = false;
    }
}

static void forge_view_flags_changed(id self, SEL _cmd, id event) {
    (void)self; (void)_cmd;
    macos_backend_t *m = g_macos;
    if (!m) return;
    uint32_t flags = (uint32_t)MlongRet(event, sel_cache.modifierFlags);
    uint32_t old = m->modifier_flags;
    m->modifier_flags = flags;

    /* Shift */
    bool shift_now = (flags & NSEventModifierFlagShift) != 0;
    bool shift_old = (old & NSEventModifierFlagShift) != 0;
    if (shift_now != shift_old) {
        m->input->keys[FGE_KEY_LSHIFT] = shift_now;
        m->input->keys[FGE_KEY_RSHIFT] = shift_now;
    }
    /* Control */
    bool ctrl_now = (flags & NSEventModifierFlagControl) != 0;
    bool ctrl_old = (old & NSEventModifierFlagControl) != 0;
    if (ctrl_now != ctrl_old) {
        m->input->keys[FGE_KEY_LCTRL] = ctrl_now;
        m->input->keys[FGE_KEY_RCTRL] = ctrl_now;
    }
    /* Option/Alt */
    bool opt_now = (flags & NSEventModifierFlagOption) != 0;
    bool opt_old = (old & NSEventModifierFlagOption) != 0;
    if (opt_now != opt_old) {
        m->input->keys[FGE_KEY_LALT] = opt_now;
        m->input->keys[FGE_KEY_RALT] = opt_now;
    }
    /* Command/Meta */
    bool cmd_now = (flags & NSEventModifierFlagCommand) != 0;
    bool cmd_old = (old & NSEventModifierFlagCommand) != 0;
    if (cmd_now != cmd_old) {
        m->input->keys[FGE_KEY_LMETA] = cmd_now;
        m->input->keys[FGE_KEY_RMETA] = cmd_now;
    }
}

static Class create_forge_view_class(void) {
    Class nsview = objc_getClass("NSView");
    if (!nsview) return NULL;

    Class forge_view = objc_allocateClassPair(nsview, "FORGEView", 0);
    if (!forge_view) return NULL;

    class_addMethod(forge_view, sel_registerName("mouseDown:"),      (IMP)forge_view_mouse_down,          "v@:@");
    class_addMethod(forge_view, sel_registerName("mouseUp:"),        (IMP)forge_view_mouse_up,            "v@:@");
    class_addMethod(forge_view, sel_registerName("rightMouseDown:"), (IMP)forge_view_right_mouse_down,    "v@:@");
    class_addMethod(forge_view, sel_registerName("rightMouseUp:"),   (IMP)forge_view_right_mouse_up,      "v@:@");
    class_addMethod(forge_view, sel_registerName("otherMouseDown:"), (IMP)forge_view_other_mouse_down,    "v@:@");
    class_addMethod(forge_view, sel_registerName("otherMouseUp:"),   (IMP)forge_view_other_mouse_up,      "v@:@");
    class_addMethod(forge_view, sel_registerName("mouseMoved:"),     (IMP)forge_view_mouse_moved,         "v@:@");
    class_addMethod(forge_view, sel_registerName("mouseDragged:"),   (IMP)forge_view_mouse_moved,         "v@:@");
    class_addMethod(forge_view, sel_registerName("rightMouseDragged:"), (IMP)forge_view_mouse_moved,      "v@:@");
    class_addMethod(forge_view, sel_registerName("otherMouseDragged:"), (IMP)forge_view_mouse_moved,      "v@:@");
    class_addMethod(forge_view, sel_registerName("scrollWheel:"),    (IMP)forge_view_scroll_wheel,        "v@:@");
    class_addMethod(forge_view, sel_registerName("acceptsFirstResponder"), (IMP)forge_view_accepts_first_responder, "B@:");
    class_addMethod(forge_view, sel_registerName("becomeFirstResponder"),  (IMP)forge_view_become_first_responder,  "B@:");
    class_addMethod(forge_view, sel_registerName("keyDown:"),        (IMP)forge_view_key_down,            "v@:@");
    class_addMethod(forge_view, sel_registerName("keyUp:"),          (IMP)forge_view_key_up,              "v@:@");
    class_addMethod(forge_view, sel_registerName("flagsChanged:"),   (IMP)forge_view_flags_changed,       "v@:@");

    objc_registerClassPair(forge_view);
    return forge_view;
}

/* -------------------------------------------------------------------------- */
/* App delegate (minimal)                                                       */
/* -------------------------------------------------------------------------- */

static BOOL forge_app_delegate_should_terminate(id self, SEL _cmd, id sender) {
    (void)self; (void)_cmd; (void)sender;
    macos_backend_t *m = g_macos;
    if (m) m->should_close = true;
    return YES;
}

static Class create_forge_app_delegate_class(void) {
    Class nsobject = objc_getClass("NSObject");
    if (!nsobject) return NULL;

    Class delegate = objc_allocateClassPair(nsobject, "FORGEAppDelegate", 0);
    if (!delegate) return NULL;

    class_addMethod(delegate, sel_registerName("applicationShouldTerminateAfterLastWindowClosed:"),
                    (IMP)forge_app_delegate_should_terminate, "B@:@");

    objc_registerClassPair(delegate);
    return delegate;
}

/* -------------------------------------------------------------------------- */
/* NSAutoreleasePool helpers                                                    */
/* -------------------------------------------------------------------------- */

static id autorelease_pool_new(void) {
    Class poolClass = objc_getClass("NSAutoreleasePool");
    if (!poolClass) return NULL;
    return M0(M0(poolClass, sel_cache.alloc), sel_cache.init);
}

static void autorelease_pool_release(id pool) {
    if (pool) M0(pool, sel_cache.release);
}

/* -------------------------------------------------------------------------- */
/* vtable implementations                                                       */
/* -------------------------------------------------------------------------- */

static bool macos_init(fge_platform_t *p, const char *title, int w, int h, bool fullscreen) {
    (void)fullscreen;
    sel_cache_init();

    macos_backend_t *m = FGE_CALLOC(1, sizeof(macos_backend_t));
    if (!m) return false;
    g_macos = m;
    m->width = w;
    m->height = h;
    m->cursor_visible = true;
    m->running = true;
    m->input = &p->input;

    /* Create autorelease pool */
    m->pool = autorelease_pool_new();

    /* Get NSApplication */
    Class appClass = objc_getClass("NSApplication");
    if (!appClass) goto fail;
    m->app = M0(appClass, sel_cache.sharedApplication);
    if (!m->app) goto fail;

    /* Set activation policy */
    Mi(m->app, sel_cache.setActivationPolicy_, NSApplicationActivationPolicyRegular);

    /* Create app delegate */
    Class delegateClass = create_forge_app_delegate_class();
    if (delegateClass) {
        id delegate = M0(M0(delegateClass, sel_cache.alloc), sel_cache.init);
        if (delegate) {
            M1(m->app, sel_cache.setDelegate_, delegate);
        }
    }

    /* Finish launching */
    M0(m->app, sel_cache.finishLaunching);

    /* Create window */
    Class windowClass = objc_getClass("NSWindow");
    if (!windowClass) goto fail;

    id window = M0(windowClass, sel_cache.alloc);
    if (!window) goto fail;

    fge_cgrect_t rect = {{0, 0}, {(fge_cgfloat_t)w, (fge_cgfloat_t)h}};
    unsigned long style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    window = MRECT4(window, sel_cache.initWithContentRect_styleMask_backing_defer_,
                    rect.origin.x, rect.origin.y, rect.size.width, rect.size.height);
    if (!window) goto fail;

    M1(window, sel_cache.setTitle_, Mstr(objc_getClass("NSString"), sel_cache.stringWithUTF8String_, title ? title : "FORGE"));
    M0(window, sel_cache.center);

    /* Create custom view */
    Class viewClass = create_forge_view_class();
    if (!viewClass) goto fail;

    id view = M0(viewClass, sel_cache.alloc);
    if (!view) goto fail;

    fge_cgrect_t view_rect = {{0, 0}, {(fge_cgfloat_t)w, (fge_cgfloat_t)h}};
    view = MRECT(view, sel_cache.initWithFrame_, view_rect);
    if (!view) goto fail;

    M1(window, sel_cache.setContentView_, view);
    M1(view, sel_cache.setNeedsDisplay_, YES);

    /* Make window key and visible */
    M0(window, sel_cache.makeKeyAndOrderFront_);

    m->window = window;
    m->view = view;

    p->native_window = m->window;
    p->native_display = m->app;
    p->width = w;
    p->height = h;
    p->running = true;
    p->focused = true;

    FGE_INFO(FGE_LOG_CAT_PLATFORM, "macOS platform initialized: %dx%d", w, h);
    return true;

fail:
    autorelease_pool_release(m->pool);
    FGE_FREE(m);
    g_macos = NULL;
    return false;
}

static void macos_shutdown(fge_platform_t *p) {
    (void)p;
    macos_backend_t *m = g_macos;
    if (!m) return;
    if (m->window) {
        M0(m->window, sel_cache.close);
    }
    autorelease_pool_release(m->pool);
    FGE_FREE(m);
    g_macos = NULL;
}

static bool macos_poll_event(fge_platform_t *p, fge_event_t *out) {
    (void)p;
    macos_backend_t *m = g_macos;
    if (!m || !m->app) return false;

    /* Process events using the app-level event loop */
    /* NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
     *                                      untilDate:[NSDate distantPast]
     *                                         inMode:NSDefaultRunLoopMode
     *                                        dequeue:YES]; */

    Class dateClass = objc_getClass("NSDate");
    id distantPast = M0(dateClass, sel_cache.distantPast);

    id event;
    /* NSDefaultRunLoopMode = "kCFRunLoopDefaultMode" */
    id mode = Mstr(objc_getClass("NSString"), sel_cache.stringWithUTF8String_, "kCFRunLoopDefaultMode");

    event = ((id (*)(id, SEL, unsigned long long, id, id, BOOL))objc_msgSend)(
        m->app, sel_cache.nextEventMatchingMask_untilDate_inMode_dequeue_,
        NSEventMaskAny, distantPast, mode, YES);

    if (!event) return false;

    /* Send the event to let the view process it */
    M1(m->app, sel_cache.sendEvent_, event);

    /* Now check for close */
    if (m->should_close) {
        memset(out, 0, sizeof(*out));
        out->type = FGE_EVENT_CLOSE;
        m->should_close = false;
        return true;
    }

    return false;
}

static void macos_swap_buffers(fge_platform_t *p) {
    (void)p;
    /* No-op — the renderer handles its own presentation */
}

static void macos_set_title(fge_platform_t *p, const char *title) {
    (void)p;
    macos_backend_t *m = g_macos;
    if (!m || !m->window || !title) return;
    id str = Mstr(objc_getClass("NSString"), sel_cache.stringWithUTF8String_, title);
    M1(m->window, sel_cache.setTitle_, str);
}

static void macos_set_vsync(fge_platform_t *p, bool enabled) {
    (void)p; (void)enabled;
    /* CVDisplayLink or NSOpenGLContext required */
}

static void macos_show_cursor(fge_platform_t *p, bool show) {
    (void)p;
    macos_backend_t *m = g_macos;
    if (!m) return;
    if (m->cursor_visible == show) return;
    m->cursor_visible = show;
    if (show) {
        /* [NSCursor unhide] */
        Class cursorClass = objc_getClass("NSCursor");
        if (cursorClass) {
            SEL unhide = sel_registerName("unhide");
            M0(cursorClass, unhide);
        }
    } else {
        Class cursorClass = objc_getClass("NSCursor");
        if (cursorClass) {
            SEL hide = sel_registerName("hide");
            M0(cursorClass, hide);
        }
    }
}

static void macos_grab_input(fge_platform_t *p, bool grab) {
    (void)p;
    macos_backend_t *m = g_macos;
    if (!m || !m->window) return;
    if (grab) {
        /* CGAssociateMouseAndMouseCursorPosition(false) — requires CoreGraphics */
    }
    (void)grab;
}

static const char *macos_get_clipboard(fge_platform_t *p) {
    (void)p;
    return NULL;
}

static void macos_set_clipboard(fge_platform_t *p, const char *text) {
    (void)p; (void)text;
}

/* -------------------------------------------------------------------------- */
/* Platform API                                                                 */
/* -------------------------------------------------------------------------- */

fge_platform_t *fge_platform_create(const char *title, int w, int h, bool fullscreen) {
    fge_platform_t *p = FGE_CALLOC(1, sizeof(fge_platform_t));
    if (!p) return NULL;

    p->events = FGE_CALLOC(256, sizeof(fge_event_t));
    p->event_capacity = 256;

    p->init = macos_init;
    p->shutdown = macos_shutdown;
    p->poll_event = macos_poll_event;
    p->swap_buffers = macos_swap_buffers;
    p->set_title = macos_set_title;
    p->set_vsync = macos_set_vsync;
    p->show_cursor = macos_show_cursor;
    p->grab_input = macos_grab_input;
    p->get_clipboard = macos_get_clipboard;
    p->set_clipboard = macos_set_clipboard;

    if (p->init(p, title, w, h, fullscreen)) return p;

    FGE_FREE(p->events);
    FGE_FREE(p);
    return NULL;
}

void fge_platform_destroy(fge_platform_t *p) {
    if (!p) return;
    if (p->shutdown) p->shutdown(p);
    FGE_FREE(p->events);
    FGE_FREE(p);
}

bool fge_platform_poll_event(fge_platform_t *p, fge_event_t *out) {
    if (!p || !out) return false;
    if (p->poll_event && p->poll_event(p, out)) return true;
    uint32_t tail = FGE_ATOMIC_LOAD(&p->event_tail);
    uint32_t head = FGE_ATOMIC_LOAD_ACQ(&p->event_head);
    if (tail == head) return false;
    *out = p->events[tail & (p->event_capacity - 1)];
    FGE_ATOMIC_STORE_REL(&p->event_tail, (tail + 1) & (p->event_capacity - 1));
    return true;
}

void fge_platform_push_event(fge_platform_t *p, const fge_event_t *event) {
    if (!p || !event) return;
    uint32_t head = FGE_ATOMIC_LOAD(&p->event_head);
    uint32_t next = (head + 1) & (p->event_capacity - 1);
    uint32_t tail = FGE_ATOMIC_LOAD_ACQ(&p->event_tail);
    if (next == tail) return;
    p->events[head & (p->event_capacity - 1)] = *event;
    FGE_ATOMIC_STORE_REL(&p->event_head, next);
}

void fge_platform_run(fge_platform_t *p) {
    if (!p) return;
    fge_clock_t clock;
    fge_clock_init(&clock);
    uint64_t last_frame = fge_clock_now(&clock);

    while (p->running) {
        uint64_t now = fge_clock_now(&clock);
        double dt = fge_clock_ticks_to_sec(&clock, now - last_frame);
        last_frame = now;

        fge_event_t ev;
        while (fge_platform_poll_event(p, &ev)) {
            if (p->on_event) p->on_event(p, &ev);
            if (ev.type == FGE_EVENT_CLOSE) p->running = false;
        }

        if (p->on_frame) p->on_frame(p, dt);
        if (p->swap_buffers) p->swap_buffers(p);

        double target_dt = 1.0 / 60.0;
        if (dt < target_dt) {
            /* usleep not available on macOS without extra headers; use nanosleep */
            struct timespec ts = {
                .tv_sec = 0,
                .tv_nsec = (long)((target_dt - dt) * 1e9)
            };
            nanosleep(&ts, NULL);
        }
    }
}

void fge_platform_step(fge_platform_t *p, double dt) {
    if (!p) return;
    fge_event_t ev;
    while (fge_platform_poll_event(p, &ev)) {
        if (p->on_event) p->on_event(p, &ev);
    }
    if (p->on_frame) p->on_frame(p, dt);
    if (p->swap_buffers) p->swap_buffers(p);
}

#endif /* __APPLE__ */
