/******************************Module*Header*******************************\
* Module Name: bdd.cxx
*
* Basic Display Driver functions implementation
*
*
* Copyright (c) 2010 Microsoft Corporation
\**************************************************************************/

#include "BDD.hxx"
#include "PVChild.h"
extern "C"
{
#include <pv_display_helper.h>
}
#define DEBUG_NO_PARAVIRT 1

#define DELETE_THIS(member) \
    if(member) { delete member; member = NULL;}

int BDD_TRACE::g_call_level = 0;

const char * BDD_TRACE::in_levels[] =  { ">>", ">>>", ">>>>", ">>>>>", ">>>>>>" };
const char * BDD_TRACE::out_levels[] = { "<<", "<<<", "<<<<", "<<<<<", "<<<<<<" };

#pragma code_seg("PAGE")

inline BOOL wi_pending(WORK_ITEM_STATE state) { return state == pending; }
inline BOOL wi_not_pending(WORK_ITEM_STATE state) { return state == not_pending; }
inline BOOL wi_valid(WORK_ITEM_STATE state) { return state != invalid; }


static void dh_deferred_connect(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PAGED_CODE();
    BDD_TRACER;
    BASIC_DISPLAY_DRIVER * pBDD = (BASIC_DISPLAY_DRIVER *) Context;
    pBDD->ReleaseRequestQueue();
    pBDD->DeferredConnection();
}

BASIC_DISPLAY_DRIVER::BASIC_DISPLAY_DRIVER(_In_ DEVICE_OBJECT* pPhysicalDeviceObject) : m_pPhysicalDevice(pPhysicalDeviceObject),
                                                                                        m_AdapterPowerState(PowerDeviceD0),
                                                                                        m_PowerAction(PowerActionNone)
{
    PAGED_CODE();
    *((UINT*)&m_Flags) = 0;
    m_io_work = NULL;
    m_queue_request_pending = not_pending;
    m_Flags._LastFlag = TRUE;
    m_provider = NULL;
    m_AddDisplayMutexHelper = NULL;
    m_dh_mutex = NULL;
    m_dh_lock = NULL;

    RtlZeroMemory(&m_DxgkInterface, sizeof(m_DxgkInterface));
    RtlZeroMemory(&m_StartInfo, sizeof(m_StartInfo));
    RtlZeroMemory(m_CurrentModes, sizeof(m_CurrentModes));
    RtlZeroMemory(&m_DeviceInfo, sizeof(m_DeviceInfo));

    for (UINT i=0;i<MAX_VIEWS;i++)
    {
        m_HardwareBlt[i].Initialize(this,i);
        m_MonitorPowerState[i] = PowerDeviceD0;
        m_CurrentModes[i].pPVChild = NULL;
    }

}

BASIC_DISPLAY_DRIVER::~BASIC_DISPLAY_DRIVER()
{
    PAGED_CODE();
    BDD_LOG_ERROR("XENWDDM!%s bye bye\n", __FUNCTION__);
    DestroyProvider();
}

NTSTATUS BASIC_DISPLAY_DRIVER::StartDevice(_In_  DXGK_START_INFO*   pDxgkStartInfo,
                                           _In_  DXGKRNL_INTERFACE* pDxgkInterface,
                                           _Out_ ULONG*             pNumberOfViews,
                                           _Out_ ULONG*             pNumberOfChildren)
{
    PAGED_CODE();

    BDD_ASSERT(pDxgkStartInfo != NULL);
    BDD_ASSERT(pDxgkInterface != NULL);
    BDD_ASSERT(pNumberOfViews != NULL);
    BDD_ASSERT(pNumberOfChildren != NULL);

    RtlCopyMemory(&m_StartInfo, pDxgkStartInfo, sizeof(m_StartInfo));
    RtlCopyMemory(&m_DxgkInterface, pDxgkInterface, sizeof(m_DxgkInterface));
    m_CurrentModes[0].DispInfo.TargetId = D3DDDI_ID_UNINITIALIZED;

    // Get device information from OS.
    NTSTATUS Status = m_DxgkInterface.DxgkCbGetDeviceInformation(m_DxgkInterface.DeviceHandle, &m_DeviceInfo);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ASSERTION("DxgkCbGetDeviceInformation failed with status 0x%I64x",
                           Status);
        return Status;
    }

    // Ignore return value, since it's not the end of the world if we failed to write these values to the registry
    RegisterHWInfo();

    // TODO: Uncomment the line below after updating the TODOs in the function CheckHardware
    Status = CheckHardware();
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    //Save post device info to use when we release it
    Status = m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle, &m_PostDevice);
    if(!NT_SUCCESS(Status))
    {
        return Status;
    }

    m_dh_mutex = new (NonPagedPoolNx) MutexHelper();

    //If we haven't gotten a framebuffer from the Display Handler, ask for one or use the Post device
    if(!m_CurrentModes[0].pPVChild)
    {
        //if fail to connect to remote display, carry on as a dumb framebuffer 
        //TODO: Add code to default to dumb framebuffer
        if(!NT_SUCCESS(InitPVChildren()))
        {
            m_CurrentModes[0].DispInfo = m_PostDevice;
            m_CurrentModes[0].Flags.OwnPostDisplay = 1;
            return STATUS_UNSUCCESSFUL;
        }
    }
    else
    {
        //Connected to Display Handler, update connection info
        for(UINT32 i = 0; i < MAX_CHILDREN; i++)
        {
            UpdateConnection(i, m_CurrentModes[i].pPVChild->connected());
        }
    }
    m_Flags.DriverStarted = TRUE;

    *pNumberOfViews = MAX_VIEWS;
    *pNumberOfChildren = MAX_CHILDREN;
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::StopDevice(VOID)
{
    PAGED_CODE();
    BDD_TRACER;
    CleanUp();

    m_Flags.DriverStarted = FALSE;

    return STATUS_SUCCESS;
}

VOID BASIC_DISPLAY_DRIVER::CleanUp()
{
    PAGED_CODE();
    BDD_TRACER;
    
    //Make sure there's nothing queued up before shutting down.
    if (m_io_work)
    {
        do {
            HoldScopedMutex mutex(m_dh_mutex, __FUNCTION__);
            if (!wi_pending(m_queue_request_pending)) {
                IoFreeWorkItem(m_io_work);
                m_io_work = NULL;
                break;
            }
            m_queue_request_pending = invalid;
            BDD_LOG_ERROR("XENWDDM%s: what? we have a pending work item\n");
        } while (1);
    }
    CleanUpChildren();
    if (m_provider)
    {
        m_provider->destroy(m_provider);
        m_provider = NULL;
    }
}

VOID BASIC_DISPLAY_DRIVER::DestroyProvider()
{
    DELETE_THIS(m_provider);
    DELETE_THIS(m_dh_lock);
    DELETE_THIS(m_dh_mutex);
    DELETE_THIS(m_AddDisplayMutexHelper);
}

VOID BASIC_DISPLAY_DRIVER::CleanUpChildren()
{
    PAGED_CODE();
    for(UINT Target = 0; Target < MAX_VIEWS; ++Target)
    {
        if(m_provider)
        {
            if (m_CurrentModes[Target].pPVChild)
            {    
                if(m_CurrentModes[Target].pPVChild->display_handler())
                {
                    //m_CurrentModes[Target].pPVChild->blank_display(true, true);
                    BDD_LOG_ERROR("XENWDDM!%s destroyed monitor 0x%x\n", __FUNCTION__, m_CurrentModes[Target].pPVChild->key());
                    m_provider->destroy_display(m_provider, m_CurrentModes[Target].pPVChild->display_handler());
                }
                delete m_CurrentModes[Target].pPVChild;
                m_CurrentModes[Target].pPVChild = NULL;
            }
        }
        m_CurrentModes[Target].FrameBuffer.Ptr = NULL;
        m_CurrentModes[Target].Flags.FrameBufferIsActive = FALSE;
    }
    PVChild::destroy_hints();
}

NTSTATUS BASIC_DISPLAY_DRIVER::DispatchIoRequest(_In_  ULONG                 VidPnSourceId,
                                                 _In_  VIDEO_REQUEST_PACKET* pVideoRequestPacket)
{
    PAGED_CODE();

    BDD_ASSERT(pVideoRequestPacket != NULL);
    BDD_ASSERT(VidPnSourceId < MAX_VIEWS);

    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS BASIC_DISPLAY_DRIVER::SetPowerState(_In_  ULONG              HardwareUid,
                                             _In_  DEVICE_POWER_STATE DevicePowerState,
                                             _In_  POWER_ACTION       ActionType)
{
    PAGED_CODE();

    UNREFERENCED_PARAMETER(ActionType);
    NTSTATUS Status;
    DXGK_DISPLAY_INFORMATION DisplayInfo;

    BDD_ASSERT((HardwareUid < MAX_CHILDREN) || (HardwareUid == DISPLAY_ADAPTER_HW_ID));
    BDD_LOG_ERROR("XENWDDM!%s: ID 0x%x PowerState %d ActionType %d\n", __FUNCTION__, HardwareUid, DevicePowerState, ActionType);

    if (HardwareUid == DISPLAY_ADAPTER_HW_ID)
    {
        //If waking up
        if (DevicePowerState == PowerDeviceD0)
        {
            BDD_LOG_ERROR("XENWDDM!%s powering up ADAPTER from %d\n", __FUNCTION__, m_AdapterPowerState);
            if(m_PowerAction == PowerActionHibernate)
            {
                Status = m_DxgkInterface.DxgkCbAcquirePostDisplayOwnership(m_DxgkInterface.DeviceHandle, &DisplayInfo);
                if(!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR("XENWDDM!%s DxgkCbAcquirePostDisplayOwnership failed with 0%x \n", __FUNCTION__, Status);
                }
                else
                {
                    //update the display info
                    if (DisplayInfo.TargetId != D3DDDI_ID_UNINITIALIZED)
                    {
                        BDD_LOG_ERROR("XENWDDM!%s updating %d to current res of (%d x %d)\n", __FUNCTION__, DisplayInfo.TargetId,
                            DisplayInfo.Width, DisplayInfo.Height);
                        memcpy(&m_CurrentModes[DisplayInfo.TargetId].DispInfo, &DisplayInfo, sizeof(DisplayInfo));
                    }
                    }
                }

            // When returning from D3 the device visibility defined to be off for all targets
            //(to avoid flashing?)
            if (m_AdapterPowerState == PowerDeviceD3)
            {
                for(UINT32 target = 0; target < MAX_CHILDREN; target++)
                {
                    if(m_CurrentModes[target].pPVChild->connected())
                    {
                        m_CurrentModes[target].pPVChild->blank_display(true, true);
                        m_CurrentModes[target].pPVChild->reset_guest_mode();
                    }
                }
            }
        }

        // Store new adapter power state
        // Tell display handler that we are back
        if (DevicePowerState == PowerDeviceD0)
                m_CurrentModes[0].pPVChild->connect_resume();
        m_AdapterPowerState = DevicePowerState;
        m_PowerAction = ActionType;

        return STATUS_SUCCESS;
    }
    else
    {
        // This is where the specified monitor should be powered up/down
        // Restore the device state
        if(DevicePowerState == PowerDeviceD0)
        {
            m_CurrentModes[HardwareUid].pPVChild->update_wake_state();
        }

        m_CurrentModes[HardwareUid].pPVChild->blank_display(true,
            DevicePowerState == PowerDeviceD0 ? false : true);
        m_MonitorPowerState[HardwareUid] = DevicePowerState;
        return STATUS_SUCCESS;
    }
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryChildRelations(_Out_writes_bytes_(ChildRelationsSize) DXGK_CHILD_DESCRIPTOR* pChildRelations,
                                                   _In_                             ULONG                  ChildRelationsSize)
{
    PAGED_CODE();

    BDD_ASSERT(pChildRelations != NULL);

    // The last DXGK_CHILD_DESCRIPTOR in the array of pChildRelations must remain zeroed out, so we subtract this from the count
    ULONG ChildRelationsCount = (ChildRelationsSize / sizeof(DXGK_CHILD_DESCRIPTOR)) - 1;
    BDD_ASSERT(ChildRelationsCount <= MAX_CHILDREN);

    for (UINT ChildIndex = 0; ChildIndex < ChildRelationsCount; ++ChildIndex)
    {
        pChildRelations[ChildIndex].ChildDeviceType = TypeVideoOutput;
        pChildRelations[ChildIndex].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_HD15;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        pChildRelations[ChildIndex].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        
        // TODO: Replace 0 with the actual ACPI ID of the child device, if available
        pChildRelations[ChildIndex].AcpiUid = 0;
        pChildRelations[ChildIndex].ChildUid = ChildIndex;
    }

    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryChildStatus(_Inout_ DXGK_CHILD_STATUS* pChildStatus,
                                                _In_    BOOLEAN            NonDestructiveOnly)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    BDD_ASSERT(pChildStatus != NULL);
    BDD_ASSERT(pChildStatus->ChildUid < MAX_CHILDREN);

    switch (pChildStatus->Type)
    {
        case StatusConnection:
        {
            pChildStatus->HotPlug.Connected = IsChildActive(pChildStatus->ChildUid);
            BDD_LOG_INFORMATION("XENWDDM!%s device %d:%d is %s\n", __FUNCTION__, pChildStatus->ChildUid,
                m_CurrentModes[pChildStatus->ChildUid].pPVChild->key(),
                IsChildActive(pChildStatus->ChildUid) ? "connected" : "not connected");
            return STATUS_SUCCESS;
        }

        case StatusRotation:
        {
            // D3DKMDT_MOA_NONE was reported, so this should never be called
            BDD_LOG_EVENT("Child status being queried for StatusRotation even though D3DKMDT_MOA_NONE was reported");
            return STATUS_INVALID_PARAMETER;
        }

        default:
        {
            BDD_LOG_WARNING("Unknown pChildStatus->Type (0x%x) requested.", pChildStatus->Type);
            return STATUS_NOT_SUPPORTED;
        }
    }
}

// EDID retrieval
NTSTATUS BASIC_DISPLAY_DRIVER::QueryDeviceDescriptor(_In_    ULONG                   ChildUid,
                                                     _Inout_ DXGK_DEVICE_DESCRIPTOR* pDeviceDescriptor)
{
    PAGED_CODE();

    BDD_ASSERT(pDeviceDescriptor != NULL);
    BDD_ASSERT(ChildUid < MAX_CHILDREN);

    // If we haven't successfully retrieved an EDID yet (invalid ones are ok, so long as it was retrieved)
    if (!ValidateEdid(ChildUid))
    {
        // Report no EDID if a valid one wasn't generated
        return STATUS_GRAPHICS_CHILD_DESCRIPTOR_NOT_SUPPORTED;
    }
    else if (pDeviceDescriptor->DescriptorOffset == 0)
    {
        // Only the base block is supported
        RtlCopyMemory(pDeviceDescriptor->DescriptorBuffer,
                      m_EDIDs[ChildUid],
                      min(pDeviceDescriptor->DescriptorLength, EDID_V1_BLOCK_SIZE));

        return STATUS_SUCCESS;
    }
    else
    {
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    }
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryAdapterInfo(_In_ CONST DXGKARG_QUERYADAPTERINFO* pQueryAdapterInfo)
{
    PAGED_CODE();

    BDD_ASSERT(pQueryAdapterInfo != NULL);

    switch (pQueryAdapterInfo->Type)
    {
        case DXGKQAITYPE_DRIVERCAPS:
        {
            if (pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS))
            {
                BDD_LOG_ERROR("pQueryAdapterInfo->OutputDataSize (0x%x) is smaller than sizeof(DXGK_DRIVERCAPS) (0x%x)",
                    pQueryAdapterInfo->OutputDataSize, sizeof(DXGK_DRIVERCAPS));
                return STATUS_BUFFER_TOO_SMALL;
            }

            DXGK_DRIVERCAPS* pDriverCaps = (DXGK_DRIVERCAPS*)pQueryAdapterInfo->pOutputData;

            // Nearly all fields must be initialized to zero, so zero out to start and then change those that are non-zero.
            // Fields are zero since BDD is Display-Only and therefore does not support any of the render related fields.
            // It also doesn't support hardware interrupts, gamma ramps, etc.
            RtlZeroMemory(pDriverCaps, sizeof(DXGK_DRIVERCAPS));

            pDriverCaps->WDDMVersion = DXGKDDI_WDDMv1_2;
            pDriverCaps->HighestAcceptableAddress.QuadPart = -1;

            pDriverCaps->SupportNonVGA = FALSE;
            pDriverCaps->SupportSmoothRotation = TRUE;

            //Pointer support
            pDriverCaps->PointerCaps.Color = 1;
            pDriverCaps->PointerCaps.Monochrome = 0;
            pDriverCaps->MaxPointerWidth = MAX_CURSOR_WIDTH;
            pDriverCaps->MaxPointerHeight = MAX_CURSOR_HEIGHT;

            return STATUS_SUCCESS;
        }
#if  (DXGKDDI_INTERFACE_VERSION > DXGKDDI_INTERFACE_VERSION_WDDM1_3)
        case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        {
            DXGK_DISPLAY_DRIVERCAPS_EXTENSION* pDriverDisplayCaps;

            if (pQueryAdapterInfo->OutputDataSize < sizeof(*pDriverDisplayCaps))
            {
                BDD_LOG_ERROR("pQueryAdapterInfo->OutputDataSize (0x%x) is smaller than sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION) (0x%x)",
                               pQueryAdapterInfo->OutputDataSize,
                               sizeof(DXGK_DISPLAY_DRIVERCAPS_EXTENSION));

                return STATUS_INVALID_PARAMETER;
            }

            pDriverDisplayCaps = (DXGK_DISPLAY_DRIVERCAPS_EXTENSION*)pQueryAdapterInfo->pOutputData;

            // Reset all caps values
            RtlZeroMemory(pDriverDisplayCaps, pQueryAdapterInfo->OutputDataSize);

            // We claim to support virtual display mode.
            pDriverDisplayCaps->VirtualModeSupport = 1;

            return STATUS_SUCCESS;
        }
#endif
        default:
        {
            // BDD does not need to support any other adapter information types
            BDD_LOG_WARNING("Unknown QueryAdapterInfo Type (0x%x) requested", pQueryAdapterInfo->Type);
            return STATUS_NOT_SUPPORTED;
        }
    }
}

NTSTATUS BASIC_DISPLAY_DRIVER::CheckHardware()
{
    PAGED_CODE();

    NTSTATUS Status;
    ULONG VendorID;
    ULONG DeviceID;

// TODO: If developing a driver for PCI based hardware, then use the second method to retrieve Vendor/Device IDs.
// If developing for non-PCI based hardware (i.e. ACPI based hardware), use the first method to retrieve the IDs.
#if 0 // ACPI-based device

    // Get the Vendor & Device IDs on non-PCI system
    ACPI_EVAL_INPUT_BUFFER_COMPLEX AcpiInputBuffer = {0};
    AcpiInputBuffer.Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    AcpiInputBuffer.MethodNameAsUlong = ACPI_METHOD_HARDWARE_ID;
    AcpiInputBuffer.Size = 0;
    AcpiInputBuffer.ArgumentCount = 0;

    BYTE OutputBuffer[sizeof(ACPI_EVAL_OUTPUT_BUFFER) + 0x10];
    RtlZeroMemory(OutputBuffer, sizeof(OutputBuffer));
    ACPI_EVAL_OUTPUT_BUFFER* pAcpiOutputBuffer = reinterpret_cast<ACPI_EVAL_OUTPUT_BUFFER*>(&OutputBuffer);

    Status = m_DxgkInterface.DxgkCbEvalAcpiMethod(m_DxgkInterface.DeviceHandle,
                                                  DISPLAY_ADAPTER_HW_ID,
                                                  &AcpiInputBuffer,
                                                  sizeof(AcpiInputBuffer),
                                                  pAcpiOutputBuffer,
                                                  sizeof(OutputBuffer));
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("DxgkCbReadDeviceSpace failed to get hardware IDs with status 0x%x", Status);
        return Status;
    }

    VendorID = ((ULONG*)(pAcpiOutputBuffer->Argument[0].Data))[0];
    DeviceID = ((ULONG*)(pAcpiOutputBuffer->Argument[0].Data))[1];

#else // PCI-based device

    // Get the Vendor & Device IDs on PCI system
    PCI_COMMON_HEADER Header = {0};
    ULONG BytesRead;

    Status = m_DxgkInterface.DxgkCbReadDeviceSpace(m_DxgkInterface.DeviceHandle,
                                                   DXGK_WHICHSPACE_CONFIG,
                                                   &Header,
                                                   0,
                                                   sizeof(Header),
                                                   &BytesRead);

    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("DxgkCbReadDeviceSpace failed with status 0x%I64x", Status);
        return Status;
    }

    VendorID = Header.VendorID;
    DeviceID = Header.DeviceID;

#endif

    // TODO: Replace 0x1414 with your Vendor ID
    if (VendorID == 0x1234)
    {
        switch (DeviceID)
        {
            // TODO: Replace the case statements below with the Device IDs supported by this driver
            case 0x0000:
            case 0x1111: return STATUS_SUCCESS;
        }
    }

    return STATUS_GRAPHICS_DRIVER_MISMATCH;
}

NTSTATUS BASIC_DISPLAY_DRIVER::SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION* pSetPointerPosition)
{
    PAGED_CODE();

    BDD_ASSERT(pSetPointerPosition != NULL);
    BDD_ASSERT(pSetPointerPosition->VidPnSourceId < MAX_VIEWS);
    UINT32    TargetId(m_CurrentModes[pSetPointerPosition->VidPnSourceId].TargetId);
    PVChild * pChild(m_CurrentModes[TargetId].pPVChild);
    DHDisplay * display(pChild->display_handler());

    if(!pChild->connected()) return STATUS_SUCCESS;

    if(pSetPointerPosition->Flags.Visible != display->cursor.visible)
    {
        pChild->set_cursor_state(pSetPointerPosition->Flags.Visible);
    }

    if((pSetPointerPosition->Flags.Visible))
    {
        display->move_cursor(display,pSetPointerPosition->X, pSetPointerPosition->Y);
    }

    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::SetPointerShape(_In_ CONST DXGKARG_SETPOINTERSHAPE* pSetPointerShape)
{
    PAGED_CODE();
    BDD_ASSERT(pSetPointerShape != NULL);

    INT32       Status;
    UINT32      Target(m_CurrentModes[pSetPointerShape->VidPnSourceId].TargetId);
    PVChild *   pChild(m_CurrentModes[Target].pPVChild);
    DHDisplay * display(pChild->display_handler());
    UINT8       width, height;

    if(!pChild->connected()) return STATUS_SUCCESS;

    //validate pointer
    width = (UINT8)( pSetPointerShape->Width < MAX_CURSOR_WIDTH ? pSetPointerShape->Width : MAX_CURSOR_WIDTH);
    height = (UINT8)(pSetPointerShape->Height < MAX_CURSOR_HEIGHT ? pSetPointerShape->Height : MAX_CURSOR_HEIGHT);

    pChild->update_hotspot(pSetPointerShape);

    Status = display->load_cursor_image(display, (void *) pSetPointerShape->pPixels, 
                                        (UINT8) pSetPointerShape->Width, (UINT8) pSetPointerShape->Height);

    pChild->save_cursor((PVOID) pSetPointerShape->pPixels, pSetPointerShape->Width, pSetPointerShape->Height);

    if(Status)
    {
        BDD_LOG_ERROR("XENWDDM !%s failed to load cursor image with status %d\n", __FUNCTION__, Status);
        return STATUS_UNSUCCESSFUL;
    }
    pChild->pointer()->set(pSetPointerShape);
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::Escape(_In_ CONST DXGKARG_ESCAPE* pEscape)
{
    PAGED_CODE();
    NTSTATUS status = STATUS_SUCCESS;

    BDD_ASSERT(pEscape != NULL);
    
    size_t data_size(sizeof(uint32_t));
    monitor_config_escape* pmonitor_escape((monitor_config_escape*) pEscape->pPrivateDriverData);
    if (pmonitor_escape->_ioctl != MONITOR_CONFIG_ESCAPE)
    {
        return STATUS_INVALID_PARAMETER;
    }

    data_size = sizeof(monitor_config_escape);
    if (pEscape->PrivateDriverDataSize != data_size)
    {
        BDD_LOG_ERROR("XENWDDM: %s: data_size is bad %d vs %d\n" __FUNCTION__, pEscape->PrivateDriverDataSize, data_size);
        return STATUS_INVALID_BUFFER_SIZE;
    }

    BDD_LOG_ERROR("XENWDDM: %s we've got a ioctl configuring monitor 0x%x at (%d, %d) to (%d, %d)\n", __FUNCTION__,
        pmonitor_escape->_id,
        pmonitor_escape->_rect.left, pmonitor_escape->_rect.top,
        pmonitor_escape->_rect.right, pmonitor_escape->_rect.bottom);

    
    for (UINT32 i = 0; i < MAX_CHILDREN; i++)
    {
        if (m_CurrentModes[i].pPVChild->key() == pmonitor_escape->_id)
        {
            m_CurrentModes[i].pPVChild->update_layout(pmonitor_escape->_rect);
            BDD_LOG_ERROR("XENWDDM: %s configuring monitor 0x%x at (%d, %d) to (%d, %d)\n", __FUNCTION__,
                pmonitor_escape->_id,
                pmonitor_escape->_rect.right, pmonitor_escape->_rect.top, 
                pmonitor_escape->_rect.left, pmonitor_escape->_rect.bottom);
        }
    }
    
    return status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::PresentDisplayOnly(_In_ CONST DXGKARG_PRESENT_DISPLAYONLY* pPresentDisplayOnly)
{
    PAGED_CODE();

    BDD_ASSERT(pPresentDisplayOnly != NULL);
    BDD_ASSERT(pPresentDisplayOnly->VidPnSourceId < MAX_VIEWS);

    UINT32  TargetId(m_CurrentModes[pPresentDisplayOnly->VidPnSourceId].TargetId);

    //grab the mutex just in case IVC wants to change the frame buffer
    HoldScopedMutex HeldMutex(fb_mutex(TargetId),  __FUNCTION__, TargetId);

    //Is this source active?
    if(!m_CurrentModes[TargetId].Flags.FrameBufferIsActive)
    {
        BDD_LOG_ERROR("XENWDDM!%s source %d -> %d trying to render to inactive framebuffer\n",
                       __FUNCTION__, pPresentDisplayOnly->VidPnSourceId, TargetId);
       return STATUS_SUCCESS;
	}
    if (pPresentDisplayOnly->BytesPerPixel < MIN_BYTES_PER_PIXEL_REPORTED)
    {
        // Only >=32bpp modes are reported, therefore this Present should never pass anything less than 4 bytes per pixel
        BDD_LOG_ERROR("pPresentDisplayOnly->BytesPerPixel is 0x%x, which is lower than the allowed.", pPresentDisplayOnly->BytesPerPixel);
        return STATUS_INVALID_PARAMETER;
    }

    // If it is in monitor off state or source is not supposed to be visible, don't present anything to the screen
    if ((m_MonitorPowerState[TargetId] > PowerDeviceD0) ||
        (m_CurrentModes[TargetId].Flags.SourceNotVisible))
    {
        return STATUS_SUCCESS;
    }

    // Present is only valid if the target is actively connected to this source
    if (m_CurrentModes[TargetId].Flags.FrameBufferIsActive)
    {

            D3DKMDT_VIDPN_PRESENT_PATH_ROTATION RotationNeededByFb = pPresentDisplayOnly->Flags.Rotate ?
                                                                 m_CurrentModes[TargetId].Rotation :
                                                                 D3DKMDT_VPPR_IDENTITY;
            BYTE* pDst = (BYTE*)m_CurrentModes[TargetId].FrameBuffer.Ptr;
            UINT DstBitPerPixel = BPPFromPixelFormat(m_CurrentModes[TargetId].DispInfo.ColorFormat);

            return m_HardwareBlt[TargetId].ExecutePresentDisplayOnly(pDst,
                                                                    DstBitPerPixel,
                                                                    (BYTE*)pPresentDisplayOnly->pSource,
                                                                    pPresentDisplayOnly->BytesPerPixel,
                                                                    pPresentDisplayOnly->Pitch,
                                                                    pPresentDisplayOnly->NumMoves,
                                                                    pPresentDisplayOnly->pMoves,
                                                                    pPresentDisplayOnly->NumDirtyRects,
                                                                    pPresentDisplayOnly->pDirtyRect,
                                                                    RotationNeededByFb);
    }

    return STATUS_SUCCESS;
}

// To indicate to the operating system that this function is supported, 
// the driver must set the NonVGASupport member of the DXGK_DRIVERCAPS 
// structure when the DxgkDdiQueryAdapterInfo function is called.

NTSTATUS BASIC_DISPLAY_DRIVER::StopDeviceAndReleasePostDisplayOwnership(_In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                                        _Out_ DXGK_DISPLAY_INFORMATION*      pDisplayInfo)
{
    PAGED_CODE();
    BDD_TRACER;
    BDD_ASSERT(TargetId < MAX_CHILDREN);

    D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = FindSourceForTarget(TargetId, TRUE);

    // In case BDD is the next driver to run, the monitor should not be off, since
    // this could cause the BIOS to hang when the EDID is retrieved on Start.
    if (m_MonitorPowerState[TargetId] > PowerDeviceD0)
    {
        SetPowerState(TargetId, PowerDeviceD0, PowerActionNone);
    }

    // The driver has to black out the display and ensure it is invisible when releasing ownership
    m_CurrentModes[SourceId].pPVChild->blank_display(true, true);
    
    //Restore the Post Device
    if(m_CurrentModes[SourceId].Flags.OwnPostDisplay)
        *pDisplayInfo = m_CurrentModes[SourceId].DispInfo;
    else
        *pDisplayInfo = m_PostDevice;

    pDisplayInfo->TargetId = TargetId;
    StopDevice();

    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::QueryVidPnHWCapability(_Inout_ DXGKARG_QUERYVIDPNHWCAPABILITY* pVidPnHWCaps)
{
    PAGED_CODE();

    BDD_ASSERT(pVidPnHWCaps != NULL);
    BDD_ASSERT(pVidPnHWCaps->SourceId < MAX_VIEWS);
    BDD_ASSERT(pVidPnHWCaps->TargetId < MAX_CHILDREN);

    pVidPnHWCaps->VidPnHWCaps.DriverRotation             = 0; // BDD does not support rotation in software
    pVidPnHWCaps->VidPnHWCaps.DriverScaling              = 0; // BDD does not support scaling
    pVidPnHWCaps->VidPnHWCaps.DriverCloning              = 0; // BDD does not support clone
    pVidPnHWCaps->VidPnHWCaps.DriverColorConvert         = 1; // BDD does color conversions in software
    pVidPnHWCaps->VidPnHWCaps.DriverLinkedAdapaterOutput = 0; // BDD does not support linked adapters
    pVidPnHWCaps->VidPnHWCaps.DriverRemoteDisplay        = 0; // BDD does not support remote displays

    return STATUS_SUCCESS;
}

BOOLEAN BASIC_DISPLAY_DRIVER::ValidateEdid(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId)
{
    PAGED_CODE();

	m_Flags.EDID_ValidHeader = TRUE;
    if(m_CurrentModes[TargetId].pPVChild->display_handler())
        return TRUE;

    BDD_LOG_ERROR("XENWDDM!%s: %d uninitialized EDID\n", __FUNCTION__, TargetId);
    m_Flags.EDID_ValidHeader = FALSE;
    return FALSE;
}

VOID BASIC_DISPLAY_DRIVER::BlackOutScreen(D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();
    BDD_TRACER;
    UINT32 TargetId = m_CurrentModes[SourceId].TargetId;
    //grab the mutex just in case IVC wants to change the frame buffer
    HoldScopedMutex HeldMutex(fb_mutex(TargetId), __FUNCTION__, TargetId);

    UINT ScreenHeight = m_CurrentModes[TargetId].DispInfo.Height;
    UINT ScreenPitch = m_CurrentModes[TargetId].DispInfo.Pitch;

    PHYSICAL_ADDRESS NewPhysAddrStart = m_CurrentModes[TargetId].DispInfo.PhysicAddress;
    PHYSICAL_ADDRESS NewPhysAddrEnd;
    NewPhysAddrEnd.QuadPart = NewPhysAddrStart.QuadPart + (ScreenHeight * ScreenPitch);
    if(m_CurrentModes[TargetId].Flags.FrameBufferIsActive)
    {
        BYTE* MappedAddr = reinterpret_cast<BYTE*>(m_CurrentModes[TargetId].FrameBuffer.Ptr);
        RtlZeroMemory(MappedAddr, ScreenHeight * ScreenPitch);
        m_CurrentModes[TargetId].pPVChild->send_dirty_rect(0, 0, m_CurrentModes[TargetId].DispInfo.Width,
            m_CurrentModes[TargetId].DispInfo.Height);
    }
}

NTSTATUS BASIC_DISPLAY_DRIVER::WriteHWInfoStr(_In_ HANDLE DevInstRegKeyHandle, _In_ PCWSTR pszwValueName, _In_ PCSTR pszValue)
{
    PAGED_CODE();

    NTSTATUS Status;
    ANSI_STRING AnsiStrValue;
    UNICODE_STRING UnicodeStrValue;
    UNICODE_STRING UnicodeStrValueName;

    // ZwSetValueKey wants the ValueName as a UNICODE_STRING
    RtlInitUnicodeString(&UnicodeStrValueName, pszwValueName);

    // REG_SZ is for WCHARs, there is no equivalent for CHARs
    // Use the ansi/unicode conversion functions to get from PSTR to PWSTR
    RtlInitAnsiString(&AnsiStrValue, pszValue);
    Status = RtlAnsiStringToUnicodeString(&UnicodeStrValue, &AnsiStrValue, TRUE);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("RtlAnsiStringToUnicodeString failed with Status: 0x%x", Status);
        return Status;
    }

    // Write the value to the registry
    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &UnicodeStrValueName,
                           0,
                           REG_SZ,
                           UnicodeStrValue.Buffer,
                           UnicodeStrValue.MaximumLength);

    // Free the earlier allocated unicode string
    RtlFreeUnicodeString(&UnicodeStrValue);

    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("ZwSetValueKey failed with Status: 0x%x", Status);
    }

    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::RegisterHWInfo()
{
    PAGED_CODE();

    NTSTATUS Status;

    // TODO: Replace these strings with proper information
    PCSTR StrHWInfoChipType = "Replace with the chip name";
    PCSTR StrHWInfoDacType = "Replace with the DAC name or identifier (ID)";
    PCSTR StrHWInfoAdapterString = "Replace with the name of the adapter";
    PCSTR StrHWInfoBiosString = "Replace with information about the BIOS";

    HANDLE DevInstRegKeyHandle;
    Status = IoOpenDeviceRegistryKey(m_pPhysicalDevice, PLUGPLAY_REGKEY_DRIVER, KEY_SET_VALUE, &DevInstRegKeyHandle);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("IoOpenDeviceRegistryKey failed for PDO: 0x%p, Status: 0x%x", m_pPhysicalDevice, Status);
        return Status;
    }

    Status = WriteHWInfoStr(DevInstRegKeyHandle, L"HardwareInformation.ChipType", StrHWInfoChipType);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WriteHWInfoStr(DevInstRegKeyHandle, L"HardwareInformation.DacType", StrHWInfoDacType);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WriteHWInfoStr(DevInstRegKeyHandle, L"HardwareInformation.AdapterString", StrHWInfoAdapterString);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = WriteHWInfoStr(DevInstRegKeyHandle, L"HardwareInformation.BiosString", StrHWInfoBiosString);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    // MemorySize is a ULONG, unlike the others which are all strings
    UNICODE_STRING ValueNameMemorySize;
    RtlInitUnicodeString(&ValueNameMemorySize, L"HardwareInformation.MemorySize");
    DWORD MemorySize = 0; // BDD has no access to video memory
    Status = ZwSetValueKey(DevInstRegKeyHandle,
                           &ValueNameMemorySize,
                           0,
                           REG_DWORD,
                           &MemorySize,
                           sizeof(MemorySize));
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("ZwSetValueKey for MemorySize failed with Status: 0x%x", Status);
        return Status;
    }

    return Status;
}

INT32 BASIC_DISPLAY_DRIVER::InitPVChildren()
{
    PAGED_CODE();
//Template EDID
#include "xenwddm_edid_1280_1024.c"

    INT32 rc;

    //Make sure we can connect to remote display domain

    m_AddDisplayMutexHelper = new (NonPagedPoolNx) MutexHelper( );
    HoldScopedMutex AddDisplays(DisplayHelperMutex(), __FUNCTION__, MAX_CHILDREN);
     
    //Creates a helper for each child
    for(UINT32 i = 0; i < MAX_CHILDREN; i++)
    {
        UINT32 width, height;
        m_CurrentModes[i].pPVChild =  new(NonPagedPoolNx) PVChild(this, i);
        m_CurrentModes[i].pPVChild->get_recommended_mode(&width, &height);
        m_CurrentModes[i].TargetId = i;
        UpdateCurrentMode(i, width, height);

        //Copy in template EDID
        memcpy(m_EDIDs[i], edid_1280_1024, EDID_V1_BLOCK_SIZE);

        //Update serial number with monitor id
        m_EDIDs[i][104] = (BYTE) (48 + i);
    }

    rc = CreateProvider();
    if(rc)
    {
        return rc;
    }

    m_io_work = IoAllocateWorkItem(m_pPhysicalDevice);
    return STATUS_SUCCESS;
}

// 
// Non-Paged Code
//
#pragma code_seg(push)
#pragma code_seg()
D3DDDI_VIDEO_PRESENT_SOURCE_ID BASIC_DISPLAY_DRIVER::FindSourceForTarget(D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId, BOOLEAN DefaultToZero)
{
    UNREFERENCED_PARAMETER(TargetId);
    BDD_ASSERT_CHK(TargetId < MAX_CHILDREN);

    if(m_CurrentModes[TargetId].FrameBuffer.Ptr)
    {
        return TargetId;
    }

    return DefaultToZero ? 0 : D3DDDI_ID_UNINITIALIZED;
}

VOID BASIC_DISPLAY_DRIVER::DpcRoutine(VOID)
{
    m_DxgkInterface.DxgkCbNotifyDpc((HANDLE)m_DxgkInterface.DeviceHandle);
    UpdateConnectionDPC();
}

BOOLEAN BASIC_DISPLAY_DRIVER::InterruptRoutine(_In_  ULONG MessageNumber)
{
    UNREFERENCED_PARAMETER(MessageNumber);

    // BDD cannot handle interrupts
    return FALSE;
}

VOID BASIC_DISPLAY_DRIVER::ResetDevice(VOID)
{

}

// Must be Non-Paged, as it sets up the display for a bugcheck
NTSTATUS BASIC_DISPLAY_DRIVER::SystemDisplayEnable(_In_  D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                   _In_  PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags,
                                                   _Out_ UINT* pWidth,
                                                   _Out_ UINT* pHeight,
                                                   _Out_ D3DDDIFORMAT* pColorFormat)
{
    UNREFERENCED_PARAMETER(Flags);

    m_SystemDisplaySourceId = D3DDDI_ID_UNINITIALIZED;

    BDD_ASSERT((TargetId < MAX_CHILDREN) || (TargetId == D3DDDI_ID_UNINITIALIZED));

    // Find the frame buffer for displaying the bugcheck, if it was successfully mapped
    if (TargetId == D3DDDI_ID_UNINITIALIZED)
    {
        for (UINT SourceIdx = 0; SourceIdx < MAX_VIEWS; ++SourceIdx)
        {
            if (m_CurrentModes[SourceIdx].FrameBuffer.Ptr != NULL)
            {
                m_SystemDisplaySourceId = SourceIdx;
                break;
            }
        }
    }
    else
    {
        m_SystemDisplaySourceId = FindSourceForTarget(TargetId, FALSE);
    }

    if (m_SystemDisplaySourceId == D3DDDI_ID_UNINITIALIZED)
    {
        {
            return STATUS_UNSUCCESSFUL;
        }
    }

    if ((m_CurrentModes[m_SystemDisplaySourceId].Rotation == D3DKMDT_VPPR_ROTATE90) ||
        (m_CurrentModes[m_SystemDisplaySourceId].Rotation == D3DKMDT_VPPR_ROTATE270))
    {
        *pHeight = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
        *pWidth = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;
    }
    else
    {
        *pWidth = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
        *pHeight = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;
    }

    *pColorFormat = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat;


    return STATUS_SUCCESS;
}

// Must be Non-Paged, as it is called to display the bugcheck screen
VOID BASIC_DISPLAY_DRIVER::SystemDisplayWrite(_In_reads_bytes_(SourceHeight * SourceStride) VOID* pSource,
                                              _In_ UINT SourceWidth,
                                              _In_ UINT SourceHeight,
                                              _In_ UINT SourceStride,
                                              _In_ INT PositionX,
                                              _In_ INT PositionY)
{

    // Rect will be Offset by PositionX/Y in the src to reset it back to 0
    RECT Rect;
    Rect.left = PositionX;
    Rect.top = PositionY;
    Rect.right =  Rect.left + SourceWidth;
    Rect.bottom = Rect.top + SourceHeight;

    // Set up destination blt info
    HoldScopedMutex HeldMutex(fb_mutex(m_SystemDisplaySourceId), 
        __FUNCTION__, m_SystemDisplaySourceId);

    // Set up destination blt info
    BLT_INFO DstBltInfo;
    DstBltInfo.pBits = m_CurrentModes[m_SystemDisplaySourceId].FrameBuffer.Ptr;
    DstBltInfo.Pitch = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Pitch;
    DstBltInfo.BitsPerPel = BPPFromPixelFormat(m_CurrentModes[m_SystemDisplaySourceId].DispInfo.ColorFormat);
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = m_CurrentModes[m_SystemDisplaySourceId].Rotation;
    DstBltInfo.Width = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Width;
    DstBltInfo.Height = m_CurrentModes[m_SystemDisplaySourceId].DispInfo.Height;

    // Set up source blt info
    BLT_INFO SrcBltInfo;
    SrcBltInfo.pBits = pSource;
    SrcBltInfo.Pitch = SourceStride;
    SrcBltInfo.BitsPerPel = 32;

    SrcBltInfo.Offset.x = -PositionX;
    SrcBltInfo.Offset.y = -PositionY;
    SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    SrcBltInfo.Width = SourceWidth;
    SrcBltInfo.Height = SourceHeight;

    BltBits(&DstBltInfo,
            &SrcBltInfo,
            1, // NumRects
            &Rect);

    //Send dirty rects to display handler
    m_CurrentModes[m_SystemDisplaySourceId].pPVChild->send_dirty_rect(
        PositionX, PositionY, SourceWidth, SourceHeight);
}

PVChild * BASIC_DISPLAY_DRIVER::GetPVChild(UINT32 SourceID)
{
    if(SourceID < MAX_VIEWS)
        return m_CurrentModes[SourceID].pPVChild;
    return NULL;
}

BOOLEAN BASIC_DISPLAY_DRIVER::UpdateCurrentMode(ULONG TargetId, UINT32 width, UINT32 height)
{
    BDD_ASSERT(TargetId < MAX_CHILDREN);

    BOOLEAN newMode = false;
    if(m_CurrentModes[TargetId].DispInfo.Width != width ||
        m_CurrentModes[TargetId].DispInfo.Height != height)
    {
        newMode = true;
        m_CurrentModes[TargetId].DispInfo.Width = width;
        m_CurrentModes[TargetId].DispInfo.Height = height;
        m_CurrentModes[TargetId].DispInfo.Pitch = width * 4;
    }
    return newMode;
}

void BASIC_DISPLAY_DRIVER::UpdateConnection(ULONG TargetId, BOOL connected)
{
    NTSTATUS Status;
    if (!m_Flags.DriverStarted) return;
    m_CurrentModes[TargetId].pPVChild->set_dpc();
    Status = m_DxgkInterface.DxgkCbQueueDpc(m_DxgkInterface.DeviceHandle);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("XENWDDM!%s Failed with 0x%x for child %d\n", __FUNCTION__, Status, TargetId);
    }
}

void BASIC_DISPLAY_DRIVER::UpdateConnectionDPC()
{
    DXGK_CHILD_STATUS ChildStatus;
    NTSTATUS Status;
    for (INT i = 0; i < MAX_CHILDREN; i++)
    {
        if (m_CurrentModes[i].pPVChild == NULL)
        {
            continue;
        }
        if (m_CurrentModes[i].pPVChild->pending_dpc())
        {
            BOOLEAN connection_status = m_CurrentModes[i].pPVChild->connected();
            m_CurrentModes[i].Flags.FrameBufferIsActive = connection_status;
            ChildStatus.ChildUid = i;
            ChildStatus.Type = StatusConnection;
            ChildStatus.HotPlug.Connected = connection_status ? 1 : 0;
            Status = m_DxgkInterface.DxgkCbIndicateChildStatus(m_DxgkInterface.DeviceHandle, &ChildStatus);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR("XENWDDM!%s Failed with 0x%x for child %d\n", __FUNCTION__, Status, i);
            }
            m_CurrentModes[i].pPVChild->reset_dpc();
            BDD_LOG_EVENT("XENWDDM!%s %d:0x%x \n   %s. Current Status was %s\n", __FUNCTION__, i,
                          m_CurrentModes[i].pPVChild->key(), connection_status ? "reconnected" : "disconnected",
                          m_CurrentModes[i].Flags.FrameBufferIsActive ? "Connected" : "disconnected");

        }
    }
}

PVChild * BASIC_DISPLAY_DRIVER::FindAvailableChild(UINT32 key)
{
    UINT32 i;
    for(i = 0; i < MAX_CHILDREN; i++)
    {
        PVChild * pChild = m_CurrentModes[i].pPVChild;
        if (!pChild) 
            continue;
        if(pChild->key() == key || !pChild->key())
        {
            pChild->set_key(key);
            BDD_LOG_ERROR("XENWDDM!%s %d:0x%x \n", __FUNCTION__, i, key);
            return pChild;
        }
    }
    return NULL;
}


void BASIC_DISPLAY_DRIVER::ReleaseRequestQueue()
{
    HoldScopedMutex mutex(m_dh_mutex, __FUNCTION__);
    if (wi_valid(m_queue_request_pending))
    {
        m_queue_request_pending = not_pending;
    }
}

void BASIC_DISPLAY_DRIVER::DeferredConnection()
{
    INT32 rc(RecreateProvider());

    if(rc)
    {
        BDD_LOG_ERROR("%s: Couldn't reconnect, trying again in a few seconds...\n", __FUNCTION__);
        sleep(5);
        QueueConnectionRequest();
    }
}

INT32 BASIC_DISPLAY_DRIVER::RecreateProvider()
{
    HoldScopedMutex mutex(m_dh_mutex, __FUNCTION__);
    if (!wi_valid(m_queue_request_pending))
    {
        return 0;
    }

    if(m_provider)
    {
        m_provider->destroy(m_provider);
        m_provider = NULL;
    }
    return CreateProvider();
}

INT32 BASIC_DISPLAY_DRIVER::CreateProvider()
{
    INT32 rc;
    //DbgBreakPoint();
    rc = create_pv_display_provider(&m_provider, DOMAIN_ZERO, IVC_CONTROL_PORT);
    if(rc || !m_provider)
    {
        BDD_LOG_ERROR("XENWDDM!%s could not connect to the remote display domain! fail out. \n", __FUNCTION__);
        return -ENOMEM;
    }

    m_provider->owner = (void *) this;
    m_provider->register_host_display_change_handler(m_provider, dpcb_host_displays_changed);
    m_provider->register_add_display_request_handler(m_provider, dpcb_add_display_request);
    m_provider->register_remove_display_request_handler(m_provider, dpcb_remove_display_request);
    m_provider->register_fatal_error_handler(m_provider, dpcb_handle_provider_error);

    m_provider->advertise_capabilities(m_provider, MAX_CHILDREN);
    if(m_dh_lock)
    {
        delete m_dh_lock;
    }
    m_dh_lock = new (NonPagedPoolNx)MutexHelper(&m_provider->lock);
    SetProvider(m_provider);
    return rc;
} 

void BASIC_DISPLAY_DRIVER::QueueConnectionRequest()
{
    HoldScopedMutex mutex(m_dh_mutex, __FUNCTION__);
    if (wi_not_pending(m_queue_request_pending) && m_io_work)
    {
        IoQueueWorkItem(m_io_work, dh_deferred_connect, DelayedWorkQueue, this);
        m_queue_request_pending = pending;
    }
}

void BASIC_DISPLAY_DRIVER::GenerateEdid(UINT32 child, UINT32 key)
{
    CHAR SerialString[14];

    BDD_TRACE_CHILD(m_CurrentModes[child].pPVChild);

    //Generate and add a serial number based on the monitor key
    RtlStringCbPrintfA(SerialString, sizeof(SerialString), "%010x", key);
    memcpy(&m_EDIDs[child][EDID_V1_SERIAL_BYTE], SerialString, 10);
    BDD_LOG_ERROR("%s: SerialString %s child:%d\n", __FUNCTION__, SerialString, child);

    //Generate a monitor name based on the key as well.
    //This will be accessible in the CCD file target path fields
    RtlStringCbPrintfA(SerialString, sizeof(SerialString), "AIS#%08x", key);
    memcpy(&m_EDIDs[child][EDID_V1_SERIAL_BYTE + 18], SerialString, 12);

    //update EDID checksum
    BYTE sum = 0;
    for(UINT i = 0; i < EDID_V1_BLOCK_SIZE; i++)
    {
        sum += m_EDIDs[child][i];
    }

    m_EDIDs[child][EDID_V1_BLOCK_SIZE - 1] = -sum;
}

void BASIC_DISPLAY_DRIVER::MapFramebuffer(ULONG TargetId, UINT32 width, UINT32 height)
{
    BDD_TRACER;

    const CURRENT_BDD_MODE *  pCurrentMode = &m_CurrentModes[TargetId];
    PVChild *pChild(pCurrentMode->pPVChild);
    DHDisplay * display = pChild->display_handler();

    UpdateCurrentMode(TargetId, width, height);

    //Update new framebuffer info
    m_CurrentModes[TargetId].FrameBuffer.Ptr = display->framebuffer;
    m_CurrentModes[TargetId].DispInfo.PhysicAddress = MmGetPhysicalAddress(display->framebuffer);
    m_CurrentModes[TargetId].DispInfo.Pitch = display->stride;
    m_CurrentModes[TargetId].DispInfo.ColorFormat = D3DDDIFMT_A8R8G8B8;
    m_CurrentModes[TargetId].Flags.OwnPostDisplay = 0;
    pChild->update_available_resolutions(width, height);
}

void BASIC_DISPLAY_DRIVER::ProcessHandlerError()
{
    UINT32 i;
    for(i = 0; i < MAX_CHILDREN; i++)
    {
        m_CurrentModes[i].pPVChild->disconnect();
    }
    BDD_LOG_ERROR("XENWDDM:%s: Here we are\n");
    QueueConnectionRequest();
}

#pragma code_seg(pop) // End Non-Paged Code

