#include <ntddk.h>
#include "bdd.hxx"
extern "C"
{
#include <pv_display_helper.h>
}
#include "PVChild.h"

//Specify whether hints should be ignored.
//If set, this ignores all display size hints and uses the default resolution.
static bool ignore_hints = FALSE;
#define BYTES_PER_PIXEL 4
#define FOURK_FRAMEBUFFER_WIDTH 3840
#define FOURK_FRAMEBUFFER_HEIGHT 2160
#define FOURK_FRAMEBUFFER_STRIDE 15360
#define HD_FRAMEBUFFER_WIDTH 1920
#define HD_FRAMEBUFFER_HEIGHT 1200
#define HD_FRAMEBUFFER_STRIDE 7680

//Specify the default width and height for a framebuffer. These should only
//be used in exceptional cases-- for example, if the Display Handler sends
//us an Add Display request without first sending a size hint.
static const size_t default_width_pixels = 1024;
static const size_t default_height_pixels = 768;

static struct display_size_hint display_hints[PV_MAX_DISPLAYS];

void sleep(UINT32 sec)
{
    LARGE_INTEGER delay;
    delay.QuadPart = (sec * 10000000 * -1); //sec number of 100ns intervals)
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
}

/**
/* Display handler error callback.
 * Disconnects the child device
 * @param DHDisplay pointer to display handler object
**/
static void dhcb_handle_display_error(DHDisplay * display, void * opaque)
{
    UNREFERENCED_PARAMETER(display);
    if(!display->driver_data)
        return;
    PVChild * child = static_cast<PVChild *>(opaque);
    HoldScopedMutex DisplayMutex(child->primary()->DisplayHelperMutex(), __FUNCTION__, MAX_CHILDREN);
    BDD_LOG_ERROR("XENWDDM!%s error for display key %u \n", __FUNCTION__, display->key);
    child->helper_disconnect();
	display->connected = false;
}

static BASIC_DISPLAY_DRIVER * dp_validate_request_parameters(DHProvider * provider, PVOID request, const char * function)
{
    BASIC_DISPLAY_DRIVER * pBDD(NULL);
    UNREFERENCED_PARAMETER(function);

    if(provider == NULL)
    {
        BDD_LOG_ERROR("xenwddm!%s: NULL provider.\n", function);
        return FALSE;
    }

    if(request == NULL)
    {
        BDD_LOG_ERROR("xenwddm!%s: NULL request\n", function);
        return FALSE;
    }

    pBDD = (BASIC_DISPLAY_DRIVER *) provider->owner;
    if(pBDD == NULL)
    {
        BDD_LOG_ERROR("xenwddm!%s: NULL owerner (BDD).\n", function);
    }
    return pBDD;
}

UINT32 MonitorFromKey(BASIC_DISPLAY_DRIVER * pBDD, UINT32 key)
{
    for(UINT32 i = 0; i < MAX_CHILDREN; i++)
    {
        DHDisplay * display(pBDD->ChildDisplay(i));
        if(!display) continue;
        if(key == display->key)
            return i;
    }
    return 0xffffffff;
}

PVChild * ChildFromKey(BASIC_DISPLAY_DRIVER *pBDD, UINT32 key)
{
    for(UINT32 i = 0; i < MAX_CHILDREN; i++)
    {
        PVChild * pChild(pBDD->GetCurrentMode(i)->pPVChild);
        if(pChild && pChild->key() == key)
        {
            return pChild;
        }
     }
    return NULL;
}

bool verify_new_hint(BASIC_DISPLAY_DRIVER * pBDD, UINT32 key, UINT32 width, UINT32 height)
{
    PVChild * pChild(ChildFromKey(pBDD, key));
    if(!pChild) return true;

    UINT32 newSize(height * pixels_to_bytes(width));
    DHDisplay * display(pChild->display_handler());

    if(display && newSize > display->framebuffer_size)
    {
        pChild->destroy();
        return FALSE;
    }
    return            TRUE;
}

void dp_set_buffer_size(BASIC_DISPLAY_DRIVER * pBDD, DisplayInfo display, display_size_hint hint)
{
    PVChild *pChild(ChildFromKey(pBDD, display.key));

    if (!pChild) {
        return;
    }

    if (pChild->buffer_type() == hint.buffer_type) {
        if (pChild->buffer_type() == STATIC_FRAMEBUFFER) {
            pChild->set_buffer_needs_resize(true);
        }
        return;
    }

    pChild->set_buffer_type(hint.buffer_type);
    pChild->set_buffer_needs_resize(true);
}

INT32 dp_set_display_hint(DHProvider * provider, BASIC_DISPLAY_DRIVER * pBDD, DisplayInfo display)
{
    INT32 free_index = -1;

    if(pBDD == NULL)
    {
        BDD_LOG_ERROR("XenWddm!%s NULL BDD.\n", __FUNCTION__);
        return -EINVAL;
    }

    if(provider == NULL)
    {
        BDD_LOG_ERROR("XenWddm!%s NULL provider.\n", __FUNCTION__);
        return -EINVAL;
    }

    if(!verify_new_hint(pBDD, display.key, display.width, display.height))
    {
        return -ECONNREFUSED;
    }

    HoldScopedMutex mutex(pBDD->ProviderLock(), __FUNCTION__);

    for (UINT32 i = 0; i < PV_MAX_DISPLAYS; i++) {
        // skip free hints
        if (!display_hints[i].present) {
            // keep track of the first free hint
            if (free_index < 0) {
                free_index = i;
            }
            continue;
        }

        // if the display hint already exists in the array, update it.
        if (display_hints[i].key == display.key) {
            PVChild * pChild = ChildFromKey(pBDD, display.key);
            if (pChild != NULL)
            {
                pChild->update_layout(&display);
            }
			display_hints[i].x = display.x;
			display_hints[i].y = display.y;
			display_hints[i].width = display.width;
			display_hints[i].height = display.height;

            if (display.key == 1) {
                if (display.width > HD_FRAMEBUFFER_WIDTH || display.height > HD_FRAMEBUFFER_HEIGHT) {
                    display_hints[i].buffer_type = FOURK_FRAMEBUFFER;
                } else {
                    display_hints[i].buffer_type = HD_FRAMEBUFFER;
                }
            } else {
                display_hints[i].buffer_type = STATIC_FRAMEBUFFER;
            }

            dp_set_buffer_size(pBDD, display, display_hints[i]);
            return STATUS_SUCCESS;
        }
    }

    // if a free hint was found, store our new hint
    if (free_index > -1) {
        display_hints[free_index].present = TRUE;
        display_hints[free_index].key = display.key;
		display_hints[free_index].x = display.x;
		display_hints[free_index].y = display.y;
		display_hints[free_index].width = display.width;
		display_hints[free_index].height = display.height;

        if (display.key == 1) {
            if (display.width > HD_FRAMEBUFFER_WIDTH || display.height > HD_FRAMEBUFFER_HEIGHT) {
                display_hints[free_index].buffer_type = FOURK_FRAMEBUFFER;
            } else {
                display_hints[free_index].buffer_type = HD_FRAMEBUFFER;
            }
        } else {
            display_hints[free_index].buffer_type = STATIC_FRAMEBUFFER;
        }

        dp_set_buffer_size(pBDD, display, display_hints[free_index]);
        return STATUS_SUCCESS;
    }

    return -EINVAL;
}

static BOOL key_is_in_display_list(DisplayInfo *display_list, UINT32 num_displays, UINT32 key)
{
    for (UINT32 i = 0; i < num_displays; i++) {
        if (key == display_list[i].key) {
            return TRUE;
        }
    }

    return FALSE;
}

static void dp_determine_displays_to_remove(BASIC_DISPLAY_DRIVER * pBDD, DisplayInfo *displays,
                                            UINT32 num_displays)
{
    UINT32 i, j;
    BOOL   remove(TRUE);
    const  CURRENT_BDD_MODE *pBDDMode;

    for(i = 0; i < PV_MAX_DISPLAYS; i++)
    {
        if (!display_hints[i].present) {
            continue;
        }

        if (!key_is_in_display_list(displays, num_displays, display_hints[i].key)) {
            pBDDMode = pBDD->GetCurrentMode(i);

            PVChild * pChild(pBDDMode->pPVChild);
            if(!pChild) {
                continue;
            }

            display_hints[i].present = FALSE;
            pChild->disconnect();
        }
    }
}

static INT32 dp_get_recommended_size(DHProvider * provider, UINT32 key, UINT32 * width, UINT32 *height)
{
    //In the event we don't find a matching hint, we set defaults.
    //Set our out arguments, and return.
    *width = default_width_pixels;
    *height = default_height_pixels;
    bool using_default = TRUE;
    BASIC_DISPLAY_DRIVER * pBDD = (BASIC_DISPLAY_DRIVER*) provider->owner;

    HoldScopedMutex mutex(pBDD->ProviderLock(), __FUNCTION__);
    //Iterate over each of the /existing/ hint entries...
	for (UINT32 i = 0; i < PV_MAX_DISPLAYS; i++) {
		if (display_hints[i].key == key) {
			*width = display_hints[i].width;
			*height = display_hints[i].height;
			using_default = FALSE;
			break;
		}
	}

    //Return whether or not
    return using_default;
}

DHDisplay * dp_find_display_target(BASIC_DISPLAY_DRIVER * pBDD, UINT32 key, ULONG * pSourceID)
{
    UINT32 i;
    for(i = 0; i < MAX_CHILDREN; i++)
    {
        DHDisplay * display(pBDD->ChildDisplay(i));
        if(display && display->key == key)
        {
            BDD_LOG_INFORMATION("XENWDDM!%s found matching key %d\n", __FUNCTION__, key);
            *pSourceID = i;
            return display;
        }
    }
    return NULL;
}

static void dp_set_display_hints(BASIC_DISPLAY_DRIVER *pBDD, DHProvider * provider,
    DisplayInfo * displays, UINT32 num_displays)
{
    DisplayInfo     newDisplayList[MAX_CHILDREN];
    UINT32          newDisplays(0);
    bool            bReAdvertiseCaps(false);

    if(!ignore_hints)
    {
        UINT32  i;
        for(i = 0; i < num_displays; i++)
        {
            if(dp_set_display_hint(provider, pBDD, displays[i]) == -ECONNREFUSED)
            {
                //We have rejected this hint and removed the display
                bReAdvertiseCaps = true;
            }
            else
            {
                //Hint is good, put it in the list of advertised displays
                PVChild * pChild(ChildFromKey(pBDD, displays[i].key));
                if(pChild && pChild->display_handler())
                {
                    pChild->update_layout(&displays[i]);
                    pChild->update_available_resolutions(displays[i].width, displays[i].height);
                }
                newDisplayList[newDisplays++] = displays[i];
            }
        }
        provider->advertise_displays(provider, &newDisplayList[0], newDisplays);
        if(bReAdvertiseCaps)
            provider->advertise_capabilities(provider, MAX_CHILDREN);
        return;
    }
    provider->advertise_displays(provider, displays, num_displays);
}

void dpcb_host_displays_changed(DHProvider * provider, DisplayInfo * displays, UINT32 num_displays)
{
    BASIC_DISPLAY_DRIVER * pBDD = dp_validate_request_parameters(provider, displays, __FUNCTION__);

    if(pBDD)
    {
        dp_determine_displays_to_remove(pBDD, displays, num_displays);
        dp_set_display_hints(pBDD, provider, displays, num_displays);
   }
}

static void dh_react_host_modeset(DHDisplay * existing_display, UINT32 width, UINT32 height)
{
    PVChild * pChild( PVCHILD(existing_display));
    if(!pChild)
        return;
    existing_display->change_resolution(existing_display, width, height, (uint32_t) pixels_to_bytes(width));
    existing_display->invalidate_region(existing_display, 0, 0, width, height);
    pChild->update_available_resolutions(width, height);
    pChild->reset_guest_mode();
}

void dp_reconnect_display(DHDisplay * existing_display, PVChild * pChild, AddDisplay * request, BOOL connected,
                       UINT32 width, UINT32 height)
{
    BOOL        requires_reconnect = ( (request->key != existing_display->key) || !connected);
    if(!pChild)
    {
        BDD_LOG_ERROR("XENWDDM!%s No child here\n", __FUNCTION__);
        return;
    }
    if(requires_reconnect)
    {
        BDD_LOG_EVENT("XENWDDM!%s Host wants to reconnect existing %ux%u source %u with key(0x%x) port:%d.\n",
            __FUNCTION__,   (UINT32) existing_display->width, (UINT32) existing_display->height,
            (UINT32)pChild->target_id(), (UINT32) request->key, request->event_port);
        pChild->reset_guest_mode();
        if (dh_reconnect_display(existing_display, request)) {
            existing_display->connected = false;
        }
    }
    else
    {
        BDD_LOG_EVENT("XENWDDM!%s Host wants to perform a mode set of (%d x %d) for %d:0x%x port:%d.\n", __FUNCTION__,
            width, height, pChild->target_id(),pChild->key(), request->event_port);
        dh_react_host_modeset(existing_display, existing_display->width, existing_display->height);
    }
    pChild->load_cursor_image();
	pChild->set_event_port(request->event_port);
}

DHDisplay * dh_create_display(DHProvider * provider, AddDisplay * request, UINT32 width, UINT32 height)
{
    DHDisplay * display;
    INT32 rc;

    // If the display is in windowed mode, create a the biggest display we can handle.
    if (request->key == 1) {
        if (width > HD_FRAMEBUFFER_WIDTH || height > HD_FRAMEBUFFER_HEIGHT) {
            rc = provider->create_display(provider, &display, request, FOURK_FRAMEBUFFER_WIDTH,
                                          FOURK_FRAMEBUFFER_HEIGHT, FOURK_FRAMEBUFFER_STRIDE, NULL);
        } else {
            rc = provider->create_display(provider, &display, request, HD_FRAMEBUFFER_WIDTH,
                                          HD_FRAMEBUFFER_HEIGHT, HD_FRAMEBUFFER_STRIDE, NULL);
        }
    } else {
        rc = provider->create_display(provider, &display, request, width, height, width*BYTES_PER_PIXEL, NULL);
    }


    //If we weren't able to create the display object, we won't be able to bring
    //up a framebuffer. Fail out!
    if(rc)
    {
        BDD_LOG_ERROR("XENWDDM!%s Failed to create a PV display object!\n", __FUNCTION__);
        return NULL;
    }
    display->register_fatal_error_handler(display, dhcb_handle_display_error);
    return display;
}

INT32 dh_change_resolution(DHDisplay * display, UINT32 width, UINT32 height)
{
    UINT32 rc;
    UINT32 stride;

    //Determine the stride that the given framebuffer should have--
    //aligning each scanline to a page boundary. This marginally improves
    //the speed of copying in and out of the framebuffer.
    //Once we set buffer size to 4k, stride value is simply FRAMEBUFFER_STRIDE
    stride = (UINT32) pixels_to_bytes(width);

    //Finally, notify the Display Handler to expect our new resolution.
    rc = display->change_resolution(display, width, height, stride);
    BDD_LOG_EVENT("XENWDDM!%s %d (%dx%d)\n", __FUNCTION__, display->key,
                    width, height);

    //If we couldn't set the resolution, fail out!
    if(rc && (display->width != width || display->height != height))
    {
        BDD_LOG_ERROR("XENWDDM!%s Failed to change resolution!\n", __FUNCTION__);
        //DbgBreakPoint();
        return rc;
    }
    display->invalidate_region(display, 0, 0, width, height);
    return STATUS_SUCCESS;
}

INT32 dh_update_resolution(DHDisplay * display, UINT32 width, UINT32 height)
{

    //Maybe nothing needs to change
    if(display->width == width && display->height == height)
    {
        return STATUS_SUCCESS;
    }

    return dh_change_resolution(display, width, height);
}

DHDisplay * dp_create_display(PVChild * pChild, DHProvider * provider, AddDisplay * request,
                        UINT32 width, UINT32 height)
{
    UINT32 i;
    UINT32 rc;
    DHDisplay * display = NULL;
    BDD_TRACER;

    for(i = 0; i < 5; i++)
    {
        display = dh_create_display(provider, request, width, height);

        if(display)
        {
            if((rc = dh_change_resolution(display, width, height)) != STATUS_SUCCESS)
			{
                display->destroy(display);
				return NULL;
			}
            pChild->register_display(display, width, height);
            break;
        }
        else
        {
            BDD_LOG_ERROR("XENWDDM!%s: error connecting, trying again in 1 sec.\n", __FUNCTION__);
            sleep(1);
        }
    }
	pChild->set_event_port(request->event_port);
    return display;
}

static int dh_reconnect_display(DHProvider * provider,
                                PVChild * pChild,
                                struct pv_display * display,
                                struct dh_add_display * request,
                                UINT32 width,
                                UINT32 height)
{
    if (pChild->buffer_needs_resize()) {
        DHDisplay * new_display;
        new_display = dp_create_display(pChild, provider, request, width, height);
        return 0;
    }
    INT32 rc = display->reconnect(display, request, 0);

    if(unlikely(rc))
    {
        BDD_LOG_ERROR("XenWddm!%s Failed to perform reconnect\n", __FUNCTION__);
        return rc;
    }

    //In a reconnect scenario for Windows, these keys should be the same
    if(display->key != request->key)
    {
        BDD_LOG_ERROR("XenWddm!%s display/request key mismatch. Trying to reconnect on a different display?\n",
            __FUNCTION__);
    }

    rc = display->change_resolution(display, display->width, display->height, display->stride);

    if(unlikely(rc))
    {
        BDD_LOG_ERROR("XenWddm:%s failed to change resolution.\n", __FUNCTION__);
        return -EAGAIN;
    }

    display->invalidate_region(display, 0, 0, display->width, display->height);
    display->set_cursor_visibility(display, display->cursor.visible);
    return 0;
}

void dp_reconnect_display(DHProvider * provider, DHDisplay * existing_display,
                          PVChild * pChild, AddDisplay * request, BOOL connected,
                          UINT32 width, UINT32 height)
{
    BOOL        requires_reconnect = ( (request->key != existing_display->key) || !connected);
    if(!pChild)
    {
        BDD_LOG_ERROR("XENWDDM!%s No child here\n", __FUNCTION__);
        return;
    }
    BDD_TRACE_KEY(existing_display->key);
    if(requires_reconnect)
    {
        BDD_LOG_EVENT("XENWDDM!%s Host wants to reconnect existing %ux%u source %u with key(0x%x).\n",
            __FUNCTION__,   (UINT32) existing_display->width, (UINT32) existing_display->height,
            (UINT32)pChild->target_id(), (UINT32) request->key);
        pChild->reset_guest_mode();
        dh_reconnect_display(provider, pChild, existing_display, request, width, height);
    }
    else
    {
        BDD_LOG_EVENT("XENWDDM!%s Host wants to perform a mode set of (%d x %d) for %d:%d.\n", __FUNCTION__,
            width, height, pChild->target_id(),pChild->key());
        dh_react_host_modeset(existing_display, width, height);
    }
    pChild->load_cursor_image();
}

void dpcb_add_display_request(DHProvider * provider, AddDisplay * request)
{
    UINT32          width;
    UINT32          height;
    ULONG           SourceId;
    BOOL            connected;
    DHDisplay *     existing_display = NULL;
    BASIC_DISPLAY_DRIVER *  pBDD;
    PVChild * pChild = NULL;

	BDD_LOG_ERROR("Xenwddm!%s: add key:event port 0x%x:%d\n", __FUNCTION__, request->key, request->event_port);
    
	//Validate parameters
    if((pBDD = dp_validate_request_parameters(provider, (PVOID) request, __FUNCTION__)) == NULL)
    {
        return;
    }

    HoldScopedMutex AddDisplay(pBDD->DisplayHelperMutex(), __FUNCTION__, MAX_CHILDREN);
    dp_get_recommended_size(provider, request->key, &width, &height);

    //Is this an existing display
    existing_display = dp_find_display_target(pBDD, request->key, &SourceId);
    if(existing_display)
    {
        pChild = PVCHILD(existing_display);
		//If this child is still connected to another port, we need to delay this connection
		//until the current port is disconnected.
		if (existing_display->connected && (pChild->event_port() != request->event_port)){
			BDD_LOG_ERROR("XenWddm!%s: OUT OF ORDER OPERATION for 0x%x, with current port/ new port %d / %d\n", __FUNCTION__,
				request->key, pChild->event_port(), request->event_port);
			return;
		}
        connected = pBDD->ChildConnected(SourceId) && existing_display->connected;
        dp_reconnect_display(provider, existing_display, pChild, request, connected, width, height);
        return;
    }

    BDD_LOG_INFORMATION("XenWddm!%s: Past check for existing display\n", __FUNCTION__);

    //Didn't find an existing one, find a free mode to use
    pChild = pBDD->FindAvailableChild(request->key);
    if(!pChild)
    {
        BDD_LOG_ERROR("XenWddm!%s Error - All available devices are utilized. Cannot complete add_display request.\n",
            __FUNCTION__);
        return;
    }

    BDD_LOG_EVENT("XENWDDM!%s Host wants to add %ux%u display (%u) on port %u.\n",
        __FUNCTION__, (UINT32) width, (UINT32) height,
        (UINT32) request->key, (UINT32) request->framebuffer_port);

     DHDisplay * display = dp_create_display(pChild, provider, request, width, height);

    //Success?
    if(!display || !pChild->display_handler())
    {
        BDD_LOG_ERROR("XENWDDM!%s: Failed to create framebuffer. \n", __FUNCTION__);
        return;
    }
    pBDD->GenerateEdid(pChild->target_id(), request->key);
    pChild->update_connection_status(TRUE);
    display->connected = true;
}

//Unimplemented for now since display removal is handles in display_list changed and
//since this protocol did not exist at the time the removal logic was implemented
void dpcb_remove_display_request(DHProvider * provider, RemoveDisplay * request)
{
    UNREFERENCED_PARAMETER(provider);
    UNREFERENCED_PARAMETER(request);
}

void dpcb_handle_provider_error(DHProvider * provider)
{
    BASIC_DISPLAY_DRIVER * pBDD = (BASIC_DISPLAY_DRIVER *) provider->owner;
    pBDD->ProcessHandlerError();
}

POINTER_BUFFER::POINTER_BUFFER( UINT32 id)
    : _mutex(NULL)
    , _id(id)
{
    _bits = (UINT8 *) ExAllocatePoolWithTag(NonPagedPoolNx, (MAX_CURSOR_WIDTH * MAX_CURSOR_HEIGHT * 4), BDDTAG);
    _mask = (UINT8 *) ExAllocatePoolWithTag(NonPagedPoolNx, (MAX_CURSOR_WIDTH * MAX_CURSOR_HEIGHT), BDDTAG);
}

POINTER_BUFFER::~POINTER_BUFFER()
{
    if(_bits) ExFreePoolWithTag(_bits, BDDTAG);
    if(_mask) ExFreePoolWithTag(_mask, BDDTAG);
}

void POINTER_BUFFER::set(CONST DXGKARG_SETPOINTERSHAPE * pSetPointerShape)
{
    HoldScopedMutex mutex(_mutex, __FUNCTION__, _id);
    _c.set(pSetPointerShape);
}

void POINTER_BUFFER::clear()
{
    memset(_bits, 0, MAX_CURSOR_HEIGHT * MAX_CURSOR_WIDTH * 8);
}

PVChild::PVChild(BASIC_DISPLAY_DRIVER * pBDD, ULONG SourceId /*= 0*/)
	: _pBDD(pBDD)
	, _TargetId(SourceId)
	, _display(NULL)
	, _connected(FALSE)
	, _display_lock(NULL)
	, _num_resolutions(0)
	, _cursor_mutex(NULL)
	, _key(0)
	, _blanked(false)
	, _SourceId(SourceId)
	, _pending(FALSE)
	, _event_port(0xffffffff)
{
    //Create mutex helpers for child's framebuffer and pointer data
    _fb_mutex = (MutexHelper *) new (NonPagedPoolNx) MutexHelper( );
    _cursor_mutex = (MutexHelper *) new (NonPagedPoolNx)MutexHelper();
    _mode_mutex = (MutexHelper *) new (NonPagedPoolNx)MutexHelper();
    _pointer  = (POINTER_BUFFER *)new (NonPagedPoolNx)POINTER_BUFFER( SourceId);
    _layout.x = _layout.y = UNINITIALIZED_INT;
    initialize_available_resolutions();
}

PVChild::~PVChild()
{
    disconnect();
    if(_fb_mutex) delete _fb_mutex;
    if(_cursor_mutex) delete _cursor_mutex;
    if(_mode_mutex) delete _mode_mutex;
    if(_pointer) delete _pointer;
    if(_available_resolutions) ExFreePoolWithTag(_available_resolutions, BDDTAG);
    if(_display_lock) ExFreePool(_display_lock);
    _fb_mutex = _cursor_mutex = _mode_mutex = NULL;
    _pointer = NULL;
    _available_resolutions = NULL;
    _display_lock = NULL;
}

void PVChild::destroy_hints()
{
	for (UINT32 i = 0; i < PV_MAX_DISPLAYS; i++) {
		display_hints[i].present = FALSE;
	}
}

void PVChild::initialize_available_resolutions()
{
    _available_resolutions = (disp_dims *) ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(disp_dims)*NUM_BASE_RESOLUTIONS, BDDTAG);
    if(!_available_resolutions)
    {
        BDD_LOG_ERROR("XENWDDM!%s pv_initialize_available_resolutions: Memory Allocation failed.\n", __FUNCTION__);
        _num_resolutions = 0;
        return;
    }
    for(UINT i = 0; i < NUM_BASE_RESOLUTIONS; i++)
    {
        memcpy(&_available_resolutions[i], &base_disp_list[i], sizeof(disp_dims));
    }
    _num_resolutions = NUM_BASE_RESOLUTIONS;
}

/**
* This function takes the width and height of the add-display request.  This width and height
* should be the largest resolution available to the monitor (eg, we can scale down, but not up).
* This function should rebase on the base_disp_list and add the supplied resolution to
* the list, and update num_resolutions in the device extension accordingly.
* No entry is greater than the provided width and height.
* @param width  - preferred width from the add display request
* @param height - preferred height from the add display request
*/
void PVChild::update_available_resolutions(UINT32 width, UINT32 height)
{
    INT32  old_size;
    UINT32 loop_size, i;
    BDD_LOG_INFORMATION("XENWDDM!%s: width:%d, height:%d \n",__FUNCTION__, width, height);
    HoldScopedMutex mode_mutex(_mode_mutex, __FUNCTION__, _TargetId);
    if(!_available_resolutions)
    {
        BDD_LOG_ERROR("XENWDDM!%s: pExt->available_resolutions is NULL. \n", __FUNCTION__);
        return;
    }

    INT32 new_size = old_size = NUM_BASE_RESOLUTIONS;
    UINT32 framebuffer_size = pixels_to_bytes(width) * height;
    disp_dims * new_resolutions(NULL);

    //Validate that this new resolution will fit in the current framebuffer
    if(framebuffer_size > _display->framebuffer_size)
        return;

    //Iterate over resolutions until we reach one that is out of bounds (equal or greater)
    for(i = 0; i < _num_resolutions; i++)
    {
        UINT32 resolution_fb(pixels_to_bytes(_available_resolutions[i].width) * _available_resolutions[i].height);
        if ( resolution_fb> framebuffer_size)
        {
            new_size = i + 1; //Entries up to this case, and space for the new size
            break;
        }
        else if (resolution_fb == framebuffer_size) {
            if (_available_resolutions[i].width == width && _available_resolutions[i].height == height) {
                BDD_LOG_ERROR("XENWDDM!%s we've got this resolution already (%dx%d) so we are done\n", width, height);
                return;
            }
        }
    }

    //Either w/h == last entry or its greater.  make sure we inc new_size 1 more time if it's actually greater
    if(new_size == NUM_BASE_RESOLUTIONS)
        if(width > base_disp_list[NUM_BASE_RESOLUTIONS - 1].width || height > base_disp_list[NUM_BASE_RESOLUTIONS - 1].height)
            new_size++;

    new_resolutions = (disp_dims *)ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(disp_dims)*new_size, BDDTAG);
    if(!new_resolutions)
    {
        BDD_LOG_ERROR("XENWDDM!%s: Allocating buffer for new resolutions failed.\n", __FUNCTION__);
        return;
    }
    loop_size = new_size > NUM_BASE_RESOLUTIONS ? NUM_BASE_RESOLUTIONS : new_size;
    for(i = 0; i < loop_size; i++)
    {
        memcpy(&new_resolutions[i], &base_disp_list[i], sizeof(disp_dims));
    }
    new_resolutions[new_size - 1].width = width;
    new_resolutions[new_size - 1].height = height;
    _num_resolutions = new_size;

    ExFreePoolWithTag(_available_resolutions, BDDTAG);
    _available_resolutions = new_resolutions;
    BDD_LOG_EVENT("XENWDDM!%s:%d old_num_resolutions: %d new_num_resolutions: %d\n", __FUNCTION__, _TargetId, old_size, new_size);
    BDD_LOG_EVENT("        ! new resolution (%d x %d)\n", _available_resolutions[new_size - 1].width, _available_resolutions[new_size - 1].height);
}

void PVChild::destroy()
{
    BDD_TRACER;

    HoldScopedMutex fb_mutex(_fb_mutex, __FUNCTION__, _TargetId);
    if(_connected)
    {
        _connected = FALSE;
        BDD_LOG_EVENT("XENWDDM!%s: disconnection child for %d\n",__FUNCTION__, _TargetId);
        _pBDD->UpdateConnection(_TargetId, FALSE);
    }
    if (_display) {
        _display->set_driver_data(_display, NULL);
        _pBDD->GetProvider()->destroy_display(_pBDD->GetProvider(), _display);
        _display = NULL;
        _key = 0;
    }
}


void PVChild::disconnect()
{
	update_connection_status(FALSE);
    BDD_LOG_EVENT("XENWDDM!%s %d/%d key:%d\n", __FUNCTION__, _SourceId, _TargetId, _key);
}

/**
* Perform a series of operations in response to a host triggered mode set.
* Update the resolution of the existing display, update the resolution list passed
* to windows, reload the cursor image as it could have changed
* and tell windows to re-enumerate any child devices it has.
*
*/
void PVChild::update_connection_status(BOOL connected)
{
    BDD_TRACE_KEY(_key);
    HoldScopedMutex fb_mutex(_fb_mutex, __FUNCTION__, _TargetId);
    _connected = connected;
    _pBDD->UpdateConnection(_TargetId, _connected);
    if (connected)
        load_cursor_image();
}

void PVChild::register_display(DHDisplay * display, UINT32 width, UINT32 height)
{
    _display = display;
    if(_display_lock) delete _display_lock;
    _display_lock = (MutexHelper *)new (NonPagedPoolNx)MutexHelper(&_display->lock);
    _pointer->_mutex = _cursor_mutex;
    _key = display->key;

    display->set_driver_data(display, (PVOID)this);
    _pBDD->MapFramebuffer(_TargetId, width, height);
    update_mode(width, height);
}

void PVChild::reset_guest_mode()
{
    HoldScopedMutex fb_mutex(_fb_mutex, __FUNCTION__, _TargetId);

    //Update power state
    _pBDD->UpdatePowerState(_TargetId, PowerDeviceD0);
    if(_blanked)
        blank_display(false, false);

    _connected = true;
    _pBDD->UpdateConnection(_TargetId, true);
    BDD_LOG_INFORMATION("XENWDDM!%s: setting %d connected\n", __FUNCTION__, _TargetId);
    load_cursor_image();
}

void PVChild::update_hotspot(CONST DXGKARG_SETPOINTERSHAPE * pSetPointerShape)
{
    HoldScopedMutex mutex(_display_lock, __FUNCTION__, pSetPointerShape->VidPnSourceId);

    _pointer->_c._xhot =  _display->cursor.hotspot_x = pSetPointerShape->XHot;
    _pointer->_c._yhot =  _display->cursor.hotspot_y = pSetPointerShape->YHot;
}

void PVChild::apply_cursor_mask(INT32 x, INT32 y)
{
    HoldScopedMutex mutex(fb_mutex(), __FUNCTION__, _TargetId);
    UINT32 *    cursorBuff((UINT32*)_pointer->_bits);
    UINT32 *    screenBuff((UINT32*)_display->framebuffer);
    UINT32      width(_pointer->_c._width);
    UINT32      height(_pointer->_c._height);

    if(x < 0 || y < 0 || !cursorBuff)
    {
        return;
    }

    UINT32      i;
    INT32       j;
    UINT32      inv_x, inv_y;
    UINT32      pixnum;
    INT32       index;
    UINT8 *     buffer = _pointer->_mask;
    UINT8 *     and_mask = buffer;
    UINT32      mask_words = (width * height) / 8;
    UINT8 *     or_mask = buffer + mask_words;

    UINT32      xResolution(_pBDD->GetCurrentMode(_TargetId)->DispInfo.Width);
    UINT32      yResolution(_pBDD->GetCurrentMode(_TargetId)->DispInfo.Height);

    for(i = 0; i < mask_words; i++)
    {
        for(j = 7; j >= 0; --j)
        {
            UINT32 and_set = and_mask[i] & (1 << j);
            UINT32 or_set = or_mask[i] & (1 << j);
            if(and_set == 0)
            {
                if(or_set == 0)
                    *cursorBuff = 0xFF000000;
                else
                    *cursorBuff = 0xFFFFFFFF;
            }
            else
            {
                if(or_set == 0)
                    *cursorBuff = 0;
                else
                {
                    pixnum = (i * 8 + j);
                    inv_x = (pixnum % width) + x;
                    inv_y = (pixnum / width) + y;
                    index = inv_y * xResolution + inv_x;

                    //somehow, index is negative our out of bounds for the screen buffer. Just make it a black pixel
                    if(index < 0 || index > (INT32)(xResolution * yResolution))
                        *cursorBuff = 0XFF000000;
                    else
                        *cursorBuff = ~screenBuff[index] | 0xFF000000;
                }
            }
            ++cursorBuff;
        }
    }
}

void PVChild::load_cursor_image()
{
    if (_display)
        _display->load_cursor_image(_display, _pointer->_bits, (UINT8) _pointer->_c._width, (UINT8) _pointer->_c._height);
}

void PVChild::save_cursor(PVOID pPixels, UINT32 Width, UINT32 Height)
{
    size_t bytes = pixels_to_bytes(Width) * Height;
    memcpy(_pointer->_bits, (UINT8 *)pPixels, bytes);
}

NTSTATUS PVChild::update_mode(UINT32 width, UINT32 height)
{
    //Validate the resolution
    if(width > FOURK_FRAMEBUFFER_WIDTH || height > FOURK_FRAMEBUFFER_HEIGHT)
    {
        BDD_LOG_ERROR("XENWDDM!%s (%d x %d) is too big\n", __FUNCTION__, width, height);
        return FALSE;
    }

    //Update it
    dh_update_resolution(_display, width, height);
    return STATUS_SUCCESS;
}

UINT32 PVChild::get_recommended_mode(UINT32 * width, UINT32 * height)
{
    if(!_display)
    {
        *width = default_width_pixels;
        *height = default_height_pixels;
        BDD_LOG_EVENT("XENWDDM!%s returning default resolution (%d x %d)\n", __FUNCTION__, *width, *height);
        return TRUE;
    }
    return dp_get_recommended_size(_pBDD->GetProvider(), _display->key, width, height);
}

int PVChild::blank_display(BOOLEAN bSleep, BOOLEAN blanked)
{
    UNREFERENCED_PARAMETER(bSleep);
    UNREFERENCED_PARAMETER(blanked);
    int Status (0);
#ifdef DH_CAP_BLANKING
    Status = _display->blank_display(_display, bSleep, blanked);

    //invalidate the whole screen on blank.
    const CURRENT_BDD_MODE * pCurrentMode(_pBDD->GetCurrentMode(_TargetId));
    if(pCurrentMode->DispInfo.Width != _display->width ||
        pCurrentMode->DispInfo.Height != _display->height)
    {
        BDD_TRACE_CHILD(this);
        BDD_LOG_ERROR("XENWDDM!%s (%d x %d) vs (%d x %d)\n", __FUNCTION__,
            pCurrentMode->DispInfo.Width, pCurrentMode->DispInfo.Height,
            _display->width, _display->height);
    }
    send_dirty_rect(0, 0, pCurrentMode->DispInfo.Width, pCurrentMode->DispInfo.Height);
    _blanked = blanked;
#endif
    return Status;
}

void PVChild::set_cursor_state(UINT visbility)
{
    INT rc  = _display->set_cursor_visibility(_display, visbility? true : false);
    if(rc)
    {
        BDD_LOG_ERROR("XENWDDM !%s %d:%d->%d failed to set %d\n", __FUNCTION__, _TargetId,
                        _key, _TargetId, visbility);
    }
    _pointer->_c._visible = visbility;
}

void PVChild::update_wake_state()
{
    BDD_LOG_EVENT("XENWDDM!%s: %d:%d cursor state %d\n", __FUNCTION__,
        _TargetId, _key, _pointer->_c._visible);
    set_cursor_state(_pointer->_c._visible);
    reset_guest_mode();
}

void PVChild::helper_disconnect()
{
    BDD_TRACE_KEY(_key);
    _connected = FALSE;
    BDD_LOG_EVENT("XENWDDM!%s: bye bye\n", __FUNCTION__);
    _pBDD->UpdateConnection(_TargetId, _connected);
}

void PVChild::update_layout(RECT rect)
{

    _layout.x = rect.left;
    _layout.y = rect.top;
    connect_resume();
}

void PVChild::update_layout(DisplayInfo* display_info)
{
    if (_layout.x == UNINITIALIZED_INT)
    {
        _layout.x = display_info->x;
    }
    else {
        display_info->x = _layout.x;
    }
    if (_layout.y == UNINITIALIZED_INT)
    {
        _layout.y = display_info->y;
    }
    else {
        display_info->y = _layout.y;
    }
}

int PVChild::send_dirty_rect(UINT32 x, UINT32 y, UINT32 width, UINT32 height)
{
    if (_connected)
        return _display->invalidate_region(_display, x, y, width, height);
    return STATUS_SUCCESS;
}

NTSTATUS PVChild::connect_resume()
{
    NTSTATUS Status(STATUS_SUCCESS);
    DHProvider  * provider(_pBDD ? _pBDD->GetProvider() : NULL);

    if(provider)
    {
        Status = provider->advertise_capabilities(provider, MAX_FRAMEBUFFERS);
    }
    return Status;
}

CurrentModes::CurrentModes(PVChild * pchild) :_num_modes(0)
, _modes(NULL)
{
    HoldScopedMutex mode_mutex(pchild->mode_mutex(), __FUNCTION__, pchild->target_id());

    _num_modes = pchild->num_available_modes();
    if(_num_modes)
    {
        _modes = (Mode *) ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(Mode)*_num_modes, BDDTAG);
        if(!_modes)
        {
            BDD_LOG_ERROR("XENWDDM!%s not enough memory for %d modes\n", __FUNCTION__, _num_modes);
            return;
        }
        for(UINT i = 0; i < _num_modes; i++)
        {
            _modes[i]._width = pchild->mode_width(i);
            _modes[i]._height = pchild->mode_height(i);
            _modes[i]._stride = pchild->mode_width(i) * 4;
        }
    }
}

CurrentModes::~CurrentModes()
{
    if(_modes)
        ExFreePoolWithTag(_modes, BDDTAG);
}
