#pragma once

#include "bdd.hxx"

#define DOMAIN_ZERO 0
#define IVC_CONTROL_PORT 1000
#define NUM_BASE_RESOLUTIONS 5

//TODO: multi monitor support
#define MAX_FRAMEBUFFERS MAX_VIEWS
#define PV_MAX_DISPLAYS MAX_VIEWS

#define CURSOR_DISABLED    0
#define CURSOR_MONOCHROME  1
#define CURSOR_ARGB_PREMULTIPLIED       2

#define MAX_CURSOR_WIDTH              64
#define MAX_CURSOR_HEIGHT             64

#define UNINITIALIZED_INT              0xFFFFFFFF

typedef struct pv_display_provider DHProvider;
typedef struct pv_display          DHDisplay;
typedef struct dh_add_display      AddDisplay;
typedef struct dh_display_info     DisplayInfo;
typedef struct dh_remove_display   RemoveDisplay;

typedef struct  {
    UINT32 width;
    UINT32 height;
} disp_dims;

//Base display list, height is -25 per to account for banner.
//Real display list built from this+ list of 'native' monitor resolutions passed from DH
static disp_dims base_disp_list [] =
{
    { 640, 480 },
    { 800, 600 },
    { 1024, 768 },
    { 1280, 1024 },
    { 1680, 1050 }
};

#define MONITOR_CONFIG_ESCAPE 0x10001
struct monitor_config_escape
{
    INT32   _id;
    RECT    _rect;
    INT32   _ioctl;
};

void dpcb_host_displays_changed(DHProvider * provider, DisplayInfo * displays, UINT32 num_displays);
void dpcb_add_display_request(DHProvider * provider, AddDisplay * request);
void dpcb_remove_display_request(DHProvider * provider, RemoveDisplay * request);
void dpcb_handle_provider_error(DHProvider * provider);

void sleep(UINT32 sec);

class BASIC_DISPLAY_DRIVER;
class MutexHelper;


class POINTER_DATA {
public:
    UINT32  _enabled;
    UINT32  _width;
    UINT32  _height;
    UINT32  _xhot;
    UINT32  _yhot;
    UINT8 * _x;
    UINT8 * _y;
    UINT    _visible;
    POINTER_DATA()
    {
        _visible = FALSE;
        _enabled = FALSE;
        _width = _height = 0;
        _xhot = _yhot = 0;
        _x = _y = NULL;
    }
    void set(CONST DXGKARG_SETPOINTERSHAPE * pSetPointerShape)
    {
        if(pSetPointerShape->Flags.Monochrome)
            _enabled = CURSOR_MONOCHROME;
        if(pSetPointerShape->Flags.Color)
            _enabled = CURSOR_ARGB_PREMULTIPLIED ;

        _width = pSetPointerShape->Width;
        _height = pSetPointerShape->Height;
        _xhot = pSetPointerShape->XHot;
        _yhot = pSetPointerShape->YHot;
    }
};

class POINTER_BUFFER {
public:
    UINT8 *         _bits;
    UINT8 *         _mask;
    POINTER_DATA    _c;
    MutexHelper *   _mutex;
    UINT32          _id;

    POINTER_BUFFER( UINT32 id);
    ~POINTER_BUFFER();
    void set(CONST DXGKARG_SETPOINTERSHAPE * pSetPointerShape);
    void clear();
    BOOLEAN isMonochrome() { return _c._enabled == CURSOR_MONOCHROME; }
};

#define PVCHILD(display) ((PVChild *)display->get_driver_data(display))

class PVChild {
public:
    PVChild(BASIC_DISPLAY_DRIVER * pBDD, ULONG SourceId = 0);
    ~PVChild();
    BASIC_DISPLAY_DRIVER * primary() { return _pBDD; }
    static void destroy_hints();
    UINT32      num_available_modes() { return _num_resolutions; }
    UINT        mode_width(UINT index) { return index < _num_resolutions ? _available_resolutions[index].width : 0; }
    UINT        mode_height(UINT index) { return index < _num_resolutions ? _available_resolutions[index].height : 0; }
    DHDisplay * display_handler() { return _display; }
    int         send_dirty_rect(UINT32 x, UINT32 y, UINT32 width, UINT32 height);
    NTSTATUS    connect_resume();
    BOOL        connected() { return _connected; }
    void        update_available_resolutions(UINT32 width, UINT32 height);
    ULONG       target_id() { return _TargetId; }
    void        disconnect();
    void        destroy();
    void        update_connection_status(BOOL connected);
    void        register_display(DHDisplay * display, UINT32 width, UINT32 height);
    void        reset_guest_mode();
    void        update_hotspot(CONST DXGKARG_SETPOINTERSHAPE * pSetPointerShape);
    void        apply_cursor_mask(INT32 x, INT32 y);
    void        load_cursor_image();
    void        save_cursor(PVOID pPixels, UINT32 Width, UINT32 Height);
    NTSTATUS    update_mode(UINT32 width, UINT32 height);
    UINT32      get_recommended_mode(UINT32 * width, UINT32 * height);
    void        set_recommended_mode(UINT32 width, UINT32 height);
    int         blank_display(BOOLEAN bSleep, BOOLEAN blanked);
    POINTER_BUFFER * pointer() { return _pointer; }
    MutexHelper *fb_mutex() { return _fb_mutex; }
    MutexHelper *cursor_mutex() { return _display_lock; }
    MutexHelper *mode_mutex() { return _mode_mutex; }
    MutexHelper *display_lock() { return _display_lock; }
    UINT32      key() { return _key; }
    void        set_key(UINT32 key) { _key = key; }
    BOOL        blanked() { return _blanked; }
    void        set_cursor_state(UINT visbility);
    void        update_wake_state();
    void        helper_disconnect();
    ULONG       source() { return _SourceId; }
    void        update_layout(RECT rect);
    void        update_layout(DisplayInfo* display_info);
    BOOL        pending_dpc() { return _pending; }
    void        reset_dpc() { _pending = FALSE; }
    void        set_dpc() { _pending = TRUE; }
private:
    void        set_event();
    void        initialize_available_resolutions();

private:
    BASIC_DISPLAY_DRIVER  *      _pBDD;
    ULONG                        _TargetId;
    DHDisplay  *                 _display;
    BOOL                         _connected;
    disp_dims  *                 _available_resolutions;
    UINT32                       _num_resolutions;
    MutexHelper *                _fb_mutex;
    MutexHelper *                _cursor_mutex;
    MutexHelper *                _mode_mutex;
    POINTER_BUFFER *             _pointer;
    MutexHelper *                _display_lock;
    UINT32                       _key;
    BOOL                         _blanked;
    ULONG                        _SourceId;
    POINT                        _layout;
    BOOL                         _pending;
};
typedef struct _Mode
{
    UINT32  _width;
    UINT32  _height;
    UINT32  _stride;
}Mode;

class CurrentModes
{
public:
    CurrentModes(PVChild * pchild);
    ~CurrentModes();
    UINT32  width(UINT i) { return (i < _num_modes) ? _modes[i]._width : 0; }
    UINT32  height(UINT i) { return (i < _num_modes) ? _modes[i]._height : 0; }
    UINT32  stride(UINT i) { return (i < _num_modes) ? _modes[i]._stride : 0; }
    UINT    modes() { return _num_modes; }
    UINT32  _num_modes;
    Mode *  _modes;
};