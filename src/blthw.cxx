/******************************Module*Header*******************************\
* Module Name: blthw.cxx
*
* Sample display driver functions for a HW blt simulation. This file is 
* only provided to simulate how a real hardware-accelerated display-only 
* driver functions, and should not be used in a real driver.
*
* Copyright (c) 2011 Microsoft Corporation
\**************************************************************************/

#include "BDD.hxx"

typedef struct
{
    CONST DXGKRNL_INTERFACE*        DxgkInterface;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA NotifyInterrupt;
} SYNC_NOTIFY_INTERRUPT;

#pragma code_seg("PAGE")


struct DoPresentMemory
{
    PVOID                     DstAddr;
    UINT                      DstStride;
    ULONG                     DstBitPerPixel;
    UINT                      SrcWidth;
    UINT                      SrcHeight;
    BYTE*                     SrcAddr;
    LONG                      SrcPitch;
    ULONG                     NumMoves;             // in:  Number of screen to screen moves
    D3DKMT_MOVE_RECT*         Moves;               // in:  Point to the list of moves
    ULONG                     NumDirtyRects;        // in:  Number of direct rects
    RECT*                     DirtyRect;           // in:  Point to the list of dirty rects
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
    BOOLEAN                   SynchExecution;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID  SourceID;
    HANDLE                    hAdapter;
    PMDL                      Mdl;
    BDD_HWBLT*                DisplaySource;
};


BDD_HWBLT::BDD_HWBLT():m_BDD (NULL),
                m_SynchExecution(TRUE),
                m_hPresentWorkerThread(NULL),
                m_pPresentWorkerThread(NULL)
{
    PAGED_CODE();
}


BDD_HWBLT::~BDD_HWBLT()
/*++

  Routine Description:

    This routine waits on present worker thread to exit before
    destroying the object

  Arguments:

    None

  Return Value:

    None

--*/
{
    PAGED_CODE();
}

NTSTATUS
BDD_HWBLT::ExecutePresentDisplayOnly(
    _In_ BYTE*             DstAddr,
    _In_ UINT              DstBitPerPixel,
    _In_ BYTE*             SrcAddr,
    _In_ UINT              SrcBytesPerPixel,
    _In_ LONG              SrcPitch,
    _In_ ULONG             NumMoves,
    _In_ D3DKMT_MOVE_RECT* Moves,
    _In_ ULONG             NumDirtyRects,
    _In_ RECT*             DirtyRect,
    _In_ D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation)
/*++

  Routine Description:

    The method creates present worker thread and provides context
    for it filled with present commands

  Arguments:

    DstAddr - address of destination surface
    DstBitPerPixel - color depth of destination surface
    SrcAddr - address of source surface
    SrcBytesPerPixel - bytes per pixel of source surface
    SrcPitch - source surface pitch (bytes in a row)
    NumMoves - number of moves to be copied
    Moves - moves' data
    NumDirtyRects - number of rectangles to be copied
    DirtyRect - rectangles' data
    Rotation - rotation to be performed when executing copy
    CallBack - callback for present worker thread to report execution status

  Return Value:

    Status

--*/
{

    PAGED_CODE();
    const CURRENT_BDD_MODE* pModeCur = m_BDD->GetCurrentMode(m_SourceId);
    UNREFERENCED_PARAMETER(SrcBytesPerPixel);

    //Do this copy here...
    // Set up source blt info

    BLT_INFO DstBltInfo;
    DstBltInfo.pBits = DstAddr;
    DstBltInfo.Pitch = pModeCur->DispInfo.Pitch;
    DstBltInfo.BitsPerPel = DstBitPerPixel;
    DstBltInfo.Offset.x = 0;
    DstBltInfo.Offset.y = 0;
    DstBltInfo.Rotation = Rotation;
    DstBltInfo.Width = pModeCur->SrcModeWidth;
    DstBltInfo.Height = pModeCur->SrcModeHeight;

    // Set up source blt info
    BLT_INFO SrcBltInfo;
    SrcBltInfo.pBits = SrcAddr;
    SrcBltInfo.Pitch = SrcPitch;
    SrcBltInfo.BitsPerPel = 32;
    SrcBltInfo.Offset.x = 0;
    SrcBltInfo.Offset.y = 0;
    SrcBltInfo.Rotation = D3DKMDT_VPPR_IDENTITY;
    if (Rotation == D3DKMDT_VPPR_ROTATE90 ||
        Rotation == D3DKMDT_VPPR_ROTATE270)
    {
        SrcBltInfo.Width = pModeCur->SrcModeHeight;
        SrcBltInfo.Height = pModeCur->SrcModeWidth;
    }
    else {
        SrcBltInfo.Width = pModeCur->SrcModeWidth;
        SrcBltInfo.Height = pModeCur->SrcModeHeight;
    }


    // Copy all the scroll rects from source image to video frame buffer.
    for (UINT i = 0; i < NumMoves; i++)
    {
        BltBits(&DstBltInfo,
            &SrcBltInfo,
            1, // NumRects
            &Moves[i].DestRect);

        //Send dirty rects to display handler
        InvalidateRegion(&Moves[i].DestRect);

    }

    // Copy all the dirty rects from source image to video frame buffer.
    for (UINT i = 0; i < NumDirtyRects; i++)
    {

        BltBits(&DstBltInfo,
            &SrcBltInfo,
            1, // NumRects
            &DirtyRect[i]);

        //Send dirty rects to display handler
        InvalidateRegion(&DirtyRect[i]);
    }
 
    return STATUS_SUCCESS;
}


int BDD_HWBLT::InvalidateRegion(CONST RECT * region)
{
    PAGED_CODE();
    //m_sourceID is the target id ....
    PVChild * child(m_BDD->GetPVChild(m_SourceId));
    return child->send_dirty_rect(region->left, region->top,
        region->right - region->left, region->bottom - region->top);
}
