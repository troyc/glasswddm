/******************************Module*Header*******************************\
* Module Name: BDD.hxx
*
* Basic Display Driver header file
*
*
* Copyright (c) 2010 Microsoft Corporation
\**************************************************************************/

#ifndef _BDD_HXX_
#define _BDD_HXX_

extern "C"
{
    #define __CPLUSPLUS

    // Standard C-runtime headers
    #include <stddef.h>
    #include <string.h>
    #include <stdarg.h>
    #include <stdio.h>
    #include <stdlib.h>

    #include <initguid.h>

    // NTOS headers
    #include <ntddk.h>

    #ifndef FAR
    #define FAR
    #endif

    // Windows headers
    #include <windef.h>
    #include <winerror.h>

    // Windows GDI headers
    #include <wingdi.h>

    // Windows DDI headers
    #include <winddi.h>
    #include <ntddvdeo.h>

    #include <d3dkmddi.h>
    #include <d3dkmthk.h>

    #include <ntstrsafe.h>
    #include <ntintsafe.h>

    #include <dispmprt.h>
};

#define EDID_V1_BLOCK_SIZE 128
#define EDID_V1_SERIAL_BYTE 95

#include "PVChild.h"
#include "BDD_ErrorLog.hxx"
#
#define MIN_WIDTH                    640
#define MIN_HEIGHT                   480
#define MIN_BITS_PER_PIXEL_ALLOWED     8
#define MIN_BYTES_PER_PIXEL_REPORTED   4

#define DEFAULT_WIDTH               1024
#define DEFAULT_HEIGHT               768

#define MAX_INVALID_INHERITED_WIDTH 1024
#define MAX_INVALID_INHERITED_HEIGHT 768

#define BITS_PER_BYTE                  8

typedef struct _BLT_INFO
{
    PVOID pBits;
    UINT Pitch;
    UINT BitsPerPel;
    POINT Offset; // To unrotated top-left of dirty rects
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
    UINT Width; // For the unrotated image
    UINT Height; // For the unrotated image
} BLT_INFO;

#define MAX_CHILDREN                   6
#define MAX_VIEWS                      6

// Pool allocation tag for the xenwddm driver. All allocations use this tag.
#define BDDTAG 'DDVS'

typedef struct _BDD_FLAGS
{
    UINT DriverStarted           : 1; // ( 1) 1 after StartDevice and 0 after StopDevice
    UINT EDID_ValidHeader        : 1; // ( 2) Generated EDID has a valid header
                                      // IMPORTANT: All new flags must be added to just before _LastFlag (i.e. right above this comment), this allows different versions of diagnostics to still be useful.
    UINT _LastFlag               : 1; // (3) Always set to 1, is used to ensure that diagnostic version matches binary version
    UINT Unused                  : 29;
} BDD_FLAGS;

// Represents the current mode, may not always be set (i.e. frame buffer mapped) if representing the mode passed in on single mode setups.
typedef struct _CURRENT_BDD_MODE
{
    DXGK_DISPLAY_INFORMATION             DispInfo;

    // The rotation of the current mode. Rotation is performed in software during Present call
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION  Rotation;

    D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling;
    // This mode might be different from one which are supported for HW frame buffer
     UINT SrcModeWidth;
    UINT SrcModeHeight;

    // Various boolean flags the struct uses
    struct _CURRENT_BDD_MODE_FLAGS
    {
        UINT SourceNotVisible     : 1; // 0 if source is visible
        UINT FrameBufferIsActive  : 1; // 0 if not currently active (i.e. target not connected to source)
        UINT IsInternal           : 1; // 1 if it was determined (i.e. through ACPI) that an internal panel is being driven
        UINT OwnPostDisplay       : 1; // 1 if using the post device
        UINT Unused               : 26;
    } Flags;


    // Linear frame buffer pointer
    // A union with a ULONG64 is used here to ensure this struct looks the same on 32bit and 64bit builds
    // since the size of a VOID* changes depending on the build.
    union
    {
        VOID*                            Ptr;
        ULONG64                          Force8Bytes;
    } FrameBuffer;
    UINT32    TargetId;
    
    //Class wrapper for display-driver-helper functions.
    PVChild * pPVChild;

} CURRENT_BDD_MODE;

class BASIC_DISPLAY_DRIVER;

class BDD_HWBLT
{
public:
    D3DDDI_VIDEO_PRESENT_SOURCE_ID  m_SourceId;
    BASIC_DISPLAY_DRIVER*           m_BDD;
    BOOLEAN                         m_SynchExecution;
    HANDLE                          m_hPresentWorkerThread;
    PVOID                           m_pPresentWorkerThread;

    //  Events to contol thread execution
    KEVENT                          m_hThreadStartupEvent;
    KEVENT                          m_hThreadSuspendEvent;

    BDD_HWBLT();

    ~BDD_HWBLT();

    void Initialize(_In_ BASIC_DISPLAY_DRIVER* pBDD, _In_ UINT IdSrc) { m_BDD = pBDD; m_SourceId = IdSrc; }
    NTSTATUS ExecutePresentDisplayOnly(_In_ BYTE*             DstAddr,
                                       _In_ UINT              DstBitPerPixel,
                                       _In_ BYTE*             SrcAddr,
                                       _In_ UINT              SrcBytesPerPixel,
                                       _In_ LONG              SrcPitch,
                                       _In_ ULONG             NumMoves,
                                       _In_ D3DKMT_MOVE_RECT* pMoves,
                                       _In_ ULONG             NumDirtyRects,
                                       _In_ RECT*             pDirtyRect,
                                       _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation);
    int InvalidateRegion(CONST RECT * region);
};

//Debugging mutexes
inline BOOLEAN CreateMutex(FAST_MUTEX ** mutex)
{
    FAST_MUTEX * mut = (FAST_MUTEX *) ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(FAST_MUTEX), BDDTAG);
    if(!mut)
    {
        return FALSE;
    }
    ExInitializeFastMutex(mut);
    *mutex = mut;
    return TRUE;
}

inline void DeleteMutex(FAST_MUTEX * mutex)
{
    if(mutex)
        ExFreePoolWithTag(mutex, BDDTAG);
}

class MutexHelper {
private:
    FAST_MUTEX*  _mutex;
    BOOL         _held;
    const char * _holding;
    BOOLEAN      _ownMutex;
public:
    MutexHelper() : _held(FALSE), _holding(NULL), _ownMutex(TRUE)
    {
        CreateMutex(&_mutex);
    };
    MutexHelper(FAST_MUTEX * mutex)
        :_held(FALSE)
        , _holding(NULL)
        ,_ownMutex(FALSE)
    {
        _mutex = mutex;
    }
    ~MutexHelper()
    {
        if (_ownMutex)
            DeleteMutex(_mutex);
    }
    FAST_MUTEX * mutex()
    {
        return _mutex;
    }
    void setHolding(const char * function) { _holding = function; }
    void setState(BOOL state) { _held = state; }
    const char * holdingFunction() { return _holding; }
    BOOL state() { return _held; }
};


//Little class that cleans up a mutex held within a specific scope
class HoldScopedMutex
{ 
private:
    FAST_MUTEX *    m_mutex;
    MutexHelper *   m_helper;
    const char *    m_holding;
    ULONG           m_id;
    const char *    m_waited_on;

public:

    _IRQL_raises_(APC_LEVEL)
    _IRQL_saves_global_(FAST_MUTEX, m_mutex)
    HoldScopedMutex(MutexHelper *helper, const char * function, ULONG id = 0xffffffff) 
        : m_helper(helper)
        , m_waited_on(NULL)
    {
        m_mutex = m_helper->mutex();
        m_id = id;
        m_holding = function;
        if(helper->state())
        {
            BDD_LOG_ERROR("XENWDDM!%s:%d %s mutex is already held by %s\n", __FUNCTION__, 
                id, function, m_helper->holdingFunction());
            m_waited_on = m_helper->holdingFunction(); 
        }
        //BDD_LOG_INFORMATION("%s holding mutex\n", m_holding);
        m_helper->setHolding(function);
        m_helper->setState(TRUE);
        ExAcquireFastMutex(m_helper->mutex());
        if(m_waited_on)
        {
            BDD_LOG_EVENT("XENWDDM!%s:%d %s mutex is not longer waiting on %s\n", __FUNCTION__, 
                id, function, m_waited_on);
        }
    }

    _IRQL_requires_(APC_LEVEL)
    _IRQL_restores_global_(FAST_MUTEX, m_mutex)
    ~HoldScopedMutex()
    {
        //BDD_LOG_INFORMATION("%s has released the mutex\n", m_holding);
        ExReleaseFastMutex(m_helper->mutex());
        m_helper->setHolding(NULL);
        m_helper->setState(FALSE);
        m_holding = NULL;
        m_id = 0xFFFFFFFF;
        m_mutex = NULL;
    }
};

class PV_Helper;
class BASIC_DISPLAY_DRIVER;

typedef enum WORK_ITEM_STATE { invalid, pending, not_pending };
class BASIC_DISPLAY_DRIVER
{
private:
    DEVICE_OBJECT* m_pPhysicalDevice;
    DXGKRNL_INTERFACE m_DxgkInterface;

    // Information passed in by StartDevice DDI
    DXGK_START_INFO m_StartInfo;


    // Array of EDIDs, currently only supporting base block, hence EDID_V1_BLOCK_SIZE for size of each EDID
    BYTE m_EDIDs[MAX_CHILDREN][EDID_V1_BLOCK_SIZE];

    CURRENT_BDD_MODE m_CurrentModes[MAX_VIEWS];

    DXGK_DISPLAY_INFORMATION   m_PostDevice;

    BDD_HWBLT        m_HardwareBlt[MAX_VIEWS];

    // Current monitor power state 
    DEVICE_POWER_STATE m_MonitorPowerState[MAX_VIEWS];

    // Current adapter power state
    DEVICE_POWER_STATE m_AdapterPowerState;

    // Current adapter power action
    POWER_ACTION       m_PowerAction;

    // Source ID to be used by SystemDisplay functions
    D3DDDI_VIDEO_PRESENT_SOURCE_ID m_SystemDisplaySourceId;

    // Various boolean flags the class uses
    BDD_FLAGS m_Flags;

    // Device information
    DXGK_DEVICE_INFO m_DeviceInfo;

    //Display Handler support
    MutexHelper * m_dh_mutex;
    MutexHelper * m_dh_lock;
    DHProvider * m_provider;
    MutexHelper * m_AddDisplayMutexHelper;
    PIO_WORKITEM  m_io_work;
    WORK_ITEM_STATE   m_queue_request_pending;

public:
    BASIC_DISPLAY_DRIVER(_In_ DEVICE_OBJECT* pPhysicalDeviceObject);
    ~BASIC_DISPLAY_DRIVER();

#pragma code_seg(push)
#pragma code_seg()
    BOOLEAN IsDriverActive() const
    {
        return m_Flags.DriverStarted;
    }
    BOOLEAN IsChildActive(ULONG SourceId) const
    {
        return IsDriverActive() && 
            m_CurrentModes[SourceId].Flags.FrameBufferIsActive &&
            m_CurrentModes[SourceId].pPVChild->connected();
    }
#pragma code_seg(pop)

    NTSTATUS StartDevice(_In_  DXGK_START_INFO*   pDxgkStartInfo,
                         _In_  DXGKRNL_INTERFACE* pDxgkInterface,
                         _Out_ ULONG*             pNumberOfViews,
                         _Out_ ULONG*             pNumberOfChildren);

    NTSTATUS StopDevice(VOID);

    // Must be Non-Paged
    VOID ResetDevice(VOID);


    const CURRENT_BDD_MODE* GetCurrentMode(UINT SourceId) const
    {
        return (SourceId < MAX_VIEWS)?&m_CurrentModes[SourceId]:NULL;
    }
    const DXGKRNL_INTERFACE* GetDxgkInterface() const { return &m_DxgkInterface;}

    // Not implemented since no IOCTLs currently handled.
    NTSTATUS DispatchIoRequest(_In_  ULONG                 VidPnSourceId,
                               _In_  VIDEO_REQUEST_PACKET* pVideoRequestPacket);

    // Used to either turn off/on monitor (if possible), or mark that system is going into hibernate
    NTSTATUS SetPowerState(_In_  ULONG              HardwareUid,
                           _In_  DEVICE_POWER_STATE DevicePowerState,
                           _In_  POWER_ACTION       ActionType);

    // Report back child capabilities
    NTSTATUS QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR* pChildRelations,
                                 _In_                             ULONG                  ChildRelationsSize);

    NTSTATUS QueryChildStatus(_Inout_ DXGK_CHILD_STATUS* pChildStatus,
                              _In_    BOOLEAN            NonDestructiveOnly);

    // Return EDID if previously retrieved
    NTSTATUS QueryDeviceDescriptor(_In_    ULONG                   ChildUid,
                                   _Inout_ DXGK_DEVICE_DESCRIPTOR* pDeviceDescriptor);

    // Must be Non-Paged
    // BDD doesn't have interrupts, so just returns false
    BOOLEAN InterruptRoutine(_In_  ULONG MessageNumber);

    VOID DpcRoutine(VOID);

    // Return DriverCaps, doesn't support other queries though
    NTSTATUS QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO* pQueryAdapterInfo);

    NTSTATUS SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION* pSetPointerPosition);

    NTSTATUS SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE* pSetPointerShape);

    NTSTATUS Escape(_In_ CONST DXGKARG_ESCAPE* pEscape);

    NTSTATUS PresentDisplayOnly(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY* pPresentDisplayOnly);

    NTSTATUS IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN* pIsSupportedVidPn);

    NTSTATUS RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN* CONST pRecommendFunctionalVidPn);

    NTSTATUS RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY* CONST pRecommendVidPnTopology);

    NTSTATUS RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes);

    NTSTATUS EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY* CONST pEnumCofuncModality);

    NTSTATUS SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY* pSetVidPnSourceVisibility);

    NTSTATUS CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN* CONST pCommitVidPn);

    NTSTATUS UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* CONST pUpdateActiveVidPnPresentPath);

    NTSTATUS QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY* pVidPnHWCaps);

    // Part of PnPStop (PnP instance only), returns current mode information (which will be passed to fallback instance by dxgkrnl)
    NTSTATUS StopDeviceAndReleasePostDisplayOwnership(_In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                      _Out_ DXGK_DISPLAY_INFORMATION*      pDisplayInfo);

    // Must be Non-Paged
    // Call to initialize as part of bugcheck
    NTSTATUS SystemDisplayEnable(_In_  D3DDDI_VIDEO_PRESENT_TARGET_ID       TargetId,
                                 _In_  PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                 _Out_ UINT*                                pWidth,
                                 _Out_ UINT*                                pHeight,
                                 _Out_ D3DDDIFORMAT*                        pColorFormat);

    // Must be Non-Paged
    // Write out pixels as part of bugcheck
    VOID SystemDisplayWrite(_In_reads_bytes_(SourceHeight * SourceStride) VOID* pSource,
                            _In_                                     UINT  SourceWidth,
                            _In_                                     UINT  SourceHeight,
                            _In_                                     UINT  SourceStride,
                            _In_                                     INT   PositionX,
                            _In_                                     INT   PositionY);

    //SV Display Handler Interface

    void            SetProvider(DHProvider * provider)  { m_provider = provider; }
    DHProvider *    GetProvider()                       { return m_provider; }
    MutexHelper *   DisplayHelperMutex()                { return m_AddDisplayMutexHelper; }
    MutexHelper *   ProviderLock()                      { return m_dh_lock; }
    DHDisplay *     ChildDisplay(ULONG SourceID)        { return m_CurrentModes[SourceID].pPVChild? m_CurrentModes[SourceID].pPVChild->display_handler() :NULL; }
    BOOL            ChildConnected(ULONG SourceID)      { return m_CurrentModes[SourceID].pPVChild? m_CurrentModes[SourceID].pPVChild->connected(): FALSE; }
    void            UpdatePowerState(UINT32 target, DEVICE_POWER_STATE state) { m_MonitorPowerState[target] = state; }
    PVChild *       GetPVChild(UINT32 SourceID);
    PVChild *       FindAvailableChild(UINT32 key);
    void            UpdateConnection(ULONG SourceID, BOOL connected);
    void            UpdateConnectionDPC();
    void            DeferredConnection();
    void            ReleaseRequestQueue();
    BOOLEAN         UpdateCurrentMode(ULONG SourceId, UINT32 width, UINT32 height);
    void            MapFramebuffer(ULONG SourceId, UINT32 width, UINT32 height);
    void            ProcessHandlerError();
    void            QueueConnectionRequest();
    void            GenerateEdid(UINT32 child, UINT32 key);;

private:
    VOID CleanUp();
    VOID DestroyProvider();
    VOID CleanUpChildren();
    NTSTATUS CommonStart();

    BOOLEAN ValidateEdid(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId);


    // Given pixel format, give back the bits per pixel. Only supports pixel formats expected by BDD
    // (i.e. the ones found below in PixelFormatFromBPP or that may come in from FallbackStart)
    // This is because these two functions combine to allow BDD to store the bpp of a VBE mode in the
    // ColorFormat field of a DispInfo
    UINT BPPFromPixelFormat(D3DDDIFORMAT Format) const
    {
        switch (Format)
        {
            case D3DDDIFMT_UNKNOWN: return 0;
            case D3DDDIFMT_P8: return 8;
            case D3DDDIFMT_R5G6B5: return 16;
            case D3DDDIFMT_R8G8B8: return 24;
            case D3DDDIFMT_X8R8G8B8: // fall through
            case D3DDDIFMT_A8R8G8B8: return 32;
            default: BDD_LOG_ASSERTION("Unknown D3DDDIFORMAT 0x%I64x", Format); return 0;
        }
    }

    // Given bits per pixel, return the pixel format at the same bpp
    D3DDDIFORMAT PixelFormatFromBPP(UINT BPP) const
    {
        switch (BPP)
        {
            case  8: return D3DDDIFMT_P8;
            case 16: return D3DDDIFMT_R5G6B5;
            case 24: return D3DDDIFMT_R8G8B8;
            case 32: return D3DDDIFMT_X8R8G8B8;
            default: BDD_LOG_ASSERTION("A bit per pixel of 0x%I64x is not supported.", BPP); return D3DDDIFMT_UNKNOWN;
        }
    }

    // These two functions make checks on the values of some of the fields of their respective structures to ensure
    // that the specified fields are supported by BDD, i.e. gamma ramp must be D3DDDI_GAMMARAMP_DEFAULT
    NTSTATUS IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH* pPath) const;
    NTSTATUS IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE* pSourceMode) const;

    VOID BlackOutScreen(D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);

    // Returns the index into gBddBiosData.BddModes of the VBE mode that matches the given VidPnSourceMode.
    // If such a mode cannot be found, returns a number outside of [0, gBddBiosData.CountBddModes)
    UINT FindMatchingVBEMode(CONST D3DKMDT_VIDPN_SOURCE_MODE* pSourceMode) const;

    // Must be Non-Paged
    // Returns the SourceId that has TargetId as a valid frame buffer or D3DDDI_ID_UNINITIALIZED if no such SourceId exists
    D3DDDI_VIDEO_PRESENT_SOURCE_ID FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, BOOLEAN DefaultToZero);

    // Set the given source mode on the given path
    NTSTATUS SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE* pSourceMode,
                                  CONST D3DKMDT_VIDPN_PRESENT_PATH* pPath);

    // Add the current mode to the given monitor source mode set
    NTSTATUS AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes, UINT32 width, UINT32 height, bool bPreferred);
    NTSTATUS AddRecommendedMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes, UINT32 width, UINT32 height);
    NTSTATUS GetPreferredMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes, UINT32 width, UINT32 height);
    // Add the current mode to the given VidPn source mode set
    NTSTATUS AddAvailableSourceModes(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface,
                                 D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId);
    
    // Add the current mode (or the matching to pinned source mode) to the give VidPn target mode set
    NTSTATUS AddAvailableTargetModes(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pVidPnTargetModeSetInterface,
                                 D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                 _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE* pVidPnPinnedSourceModeInfo,
                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId,
                                 UINT32 * numModes);
    NTSTATUS AddTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pVidPnTargetModeSetInterface, D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet, _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE* pVidPnPinnedSourceModeInfo, UINT32 width, UINT32 height, D3DKMDT_MODE_PREFERENCE bPreferred);
    
    // Check that the hardware the driver is running on is hardware it is capable of driving.
    NTSTATUS CheckHardware();

    // Helper function for RegisterHWInfo
    NTSTATUS WriteHWInfoStr(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PCSTR pszValue);

    // Set the information in the registry as described here: http://msdn.microsoft.com/en-us/library/windows/hardware/ff569240(v=vs.85).aspx
    NTSTATUS RegisterHWInfo();

    //SV Display Handler Interface
    INT32           InitPVChildren();
    INT32           CreateProvider();
    INT32           RecreateProvider();
    MutexHelper *   fb_mutex(D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceID) { return m_CurrentModes[SourceID].pPVChild->fb_mutex(); }

    NTSTATUS        AddAvailableModes(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface,
                                D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet, D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId, D3DKMDT_VIDPN_SOURCE_MODE* pVidPnSourceModeInfo);
    BOOLEAN         DisplayConnected() { return m_provider != NULL; }

};

//
// Blt functions
//

// Must be Non-Paged
VOID BltBits(
    BLT_INFO* pDst,
    CONST BLT_INFO* pSrc,
    UINT  NumRects,
    _In_reads_(NumRects) CONST RECT *pRects);

//
// Driver Entry point
//

extern "C"
DRIVER_INITIALIZE DriverEntry;

//
// PnP DDIs
//

VOID
BddDdiUnload(VOID);

// If uncommenting ENABLE_DXGK_SAL in the sources file, all the below function prototypes should be updated to use
// the function typedef's from the header files. Additionally, annotations on the function definitions can be removed
// as they are inherited from the prototype definition here. As an example the entire 4-line prototype for BddDdiAddDevice
// is replaced by the single commented line below:
// DXGKDDI_ADD_DEVICE BddDdiAddDevice;
NTSTATUS
BddDdiAddDevice(
    _In_ DEVICE_OBJECT* pPhysicalDeviceObject,
    _Outptr_ PVOID*  ppDeviceContext);

NTSTATUS
BddDdiRemoveDevice(
    _In_  VOID* pDeviceContext);

NTSTATUS
BddDdiStartDevice(
    _In_  VOID*              pDeviceContext,
    _In_  DXGK_START_INFO*   pDxgkStartInfo,
    _In_  DXGKRNL_INTERFACE* pDxgkInterface,
    _Out_ ULONG*             pNumberOfViews,
    _Out_ ULONG*             pNumberOfChildren);

NTSTATUS
BddDdiStopDevice(
    _In_  VOID* pDeviceContext);

VOID
BddDdiResetDevice(
    _In_  VOID* pDeviceContext);


NTSTATUS
BddDdiDispatchIoRequest(
    _In_  VOID*                 pDeviceContext,
    _In_  ULONG                 VidPnSourceId,
    _In_  VIDEO_REQUEST_PACKET* pVideoRequestPacket);

NTSTATUS
BddDdiSetPowerState(
    _In_  VOID*              pDeviceContext,
    _In_  ULONG              HardwareUid,
    _In_  DEVICE_POWER_STATE DevicePowerState,
    _In_  POWER_ACTION       ActionType);

NTSTATUS
BddDdiQueryChildRelations(
    _In_                             VOID*                  pDeviceContext,
    _Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR* pChildRelations,
    _In_                             ULONG                  ChildRelationsSize);

NTSTATUS
BddDdiQueryChildStatus(
    _In_    VOID*              pDeviceContext,
    _Inout_ DXGK_CHILD_STATUS* pChildStatus,
    _In_    BOOLEAN            NonDestructiveOnly);

NTSTATUS
BddDdiQueryDeviceDescriptor(
    _In_  VOID*                     pDeviceContext,
    _In_  ULONG                     ChildUid,
    _Inout_ DXGK_DEVICE_DESCRIPTOR* pDeviceDescriptor);

// Must be Non-Paged
BOOLEAN
BddDdiInterruptRoutine(
    _In_  VOID* pDeviceContext,
    _In_  ULONG MessageNumber);

VOID
BddDdiDpcRoutine(
    _In_  VOID* pDeviceContext);

//
// WDDM Display Only Driver DDIs
//

NTSTATUS
APIENTRY
BddDdiQueryAdapterInfo(
    _In_ CONST HANDLE                         hAdapter,
    _In_ CONST DXGKARG_QUERYADAPTERINFO*      pQueryAdapterInfo);

NTSTATUS
APIENTRY
BddDdiSetPointerPosition(
    _In_ CONST HANDLE                         hAdapter,
    _In_ CONST DXGKARG_SETPOINTERPOSITION*    pSetPointerPosition);

NTSTATUS
APIENTRY
BddDdiSetPointerShape(
    _In_ CONST HANDLE                         hAdapter,
    _In_ CONST DXGKARG_SETPOINTERSHAPE*       pSetPointerShape);

NTSTATUS
APIENTRY
BddDdiEscape(
    _In_ CONST HANDLE                        hAdapter,
    _In_ CONST DXGKARG_ESCAPE*               pEscape);

NTSTATUS
APIENTRY
BddDdiPresentDisplayOnly(
    _In_ CONST HANDLE                         hAdapter,
    _In_ CONST DXGKARG_PRESENT_DISPLAYONLY*   pPresentDisplayOnly);

NTSTATUS
APIENTRY
BddDdiIsSupportedVidPn(
    _In_ CONST HANDLE                         hAdapter,
    _Inout_ DXGKARG_ISSUPPORTEDVIDPN*         pIsSupportedVidPn);

NTSTATUS
APIENTRY
BddDdiRecommendFunctionalVidPn(
    _In_ CONST HANDLE                                   hAdapter,
    _In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN* CONST  pRecommendFunctionalVidPn);

NTSTATUS
APIENTRY
BddDdiRecommendVidPnTopology(
    _In_ CONST HANDLE                                 hAdapter,
    _In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY* CONST  pRecommendVidPnTopology);

NTSTATUS
APIENTRY
BddDdiRecommendMonitorModes(
    _In_ CONST HANDLE                                hAdapter,
    _In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST  pRecommendMonitorModes);

NTSTATUS
APIENTRY
BddDdiEnumVidPnCofuncModality(
    _In_ CONST HANDLE                                  hAdapter,
    _In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY* CONST  pEnumCofuncModality);

NTSTATUS
APIENTRY
BddDdiSetVidPnSourceVisibility(
    _In_ CONST HANDLE                             hAdapter,
    _In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY*  pSetVidPnSourceVisibility);

NTSTATUS
APIENTRY
BddDdiCommitVidPn(
    _In_ CONST HANDLE                         hAdapter,
    _In_ CONST DXGKARG_COMMITVIDPN* CONST     pCommitVidPn);

NTSTATUS
APIENTRY
BddDdiUpdateActiveVidPnPresentPath(
    _In_ CONST HANDLE                                       hAdapter,
    _In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* CONST  pUpdateActiveVidPnPresentPath);

NTSTATUS
APIENTRY
BddDdiQueryVidPnHWCapability(
    _In_ CONST HANDLE                         hAdapter,
    _Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY*   pVidPnHWCaps);

NTSTATUS
APIENTRY
BddDdiStopDeviceAndReleasePostDisplayOwnership(
    _In_  VOID*                          pDeviceContext,
    _In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
    _Out_ DXGK_DISPLAY_INFORMATION*      DisplayInfo);

// Must be Non-Paged
NTSTATUS
APIENTRY
BddDdiSystemDisplayEnable(
    _In_  VOID* pDeviceContext,
    _In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
    _In_  PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
    _Out_ UINT* Width,
    _Out_ UINT* Height,
    _Out_ D3DDDIFORMAT* ColorFormat);

// Must be Non-Paged
VOID
APIENTRY
BddDdiSystemDisplayWrite(
    _In_  VOID* pDeviceContext,
    _In_  VOID* Source,
    _In_  UINT  SourceWidth,
    _In_  UINT  SourceHeight,
    _In_  UINT  SourceStride,
    _In_  UINT  PositionX,
    _In_  UINT  PositionY);

BOOLEAN
IsEdidHeaderValid(_In_reads_bytes_(EDID_V1_BLOCK_SIZE) const BYTE* pEdid);

BOOLEAN
IsEdidChecksumValid(_In_reads_bytes_(EDID_V1_BLOCK_SIZE) const BYTE* pEdid);

//
// Memory handling
//

// Defaulting the value of PoolType means that any call to new Foo()
// will raise a compiler error for being ambiguous. This is to help keep
// any calls to allocate memory from accidentally NOT going through
// these functions.
_When_((PoolType & NonPagedPoolMustSucceed) != 0,
    __drv_reportError("Must succeed pool allocations are forbidden. "
            "Allocation failures cause a system crash"))
void* __cdecl operator new(size_t Size, POOL_TYPE PoolType = PagedPool);
_When_((PoolType & NonPagedPoolMustSucceed) != 0,
    __drv_reportError("Must succeed pool allocations are forbidden. "
            "Allocation failures cause a system crash"))
void* __cdecl operator new[](size_t Size, POOL_TYPE PoolType = PagedPool);
void  __cdecl operator delete(void* pObject);
void  __cdecl operator delete(void* pObject, size_t s);
void  __cdecl operator delete[](void* pObject);




#endif // _BDD_HXX_


