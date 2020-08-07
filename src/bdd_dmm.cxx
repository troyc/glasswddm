/******************************Module*Header*******************************\
* Module Name: bdd_dmm.hxx
*
* Basic Display Driver display-mode management (DMM) function implementations
*
*
* Copyright (c) 2010 Microsoft Corporation
\**************************************************************************/

#include "BDD.hxx"
#include "PVChild.h"

#pragma warning(push)
#pragma warning(disable:4390)

#pragma code_seg("PAGE")

// Display-Only Devices can only return display modes of D3DDDIFMT_A8R8G8B8.
// Color conversion takes place if the app's fullscreen backbuffer has different format.
// Full display drivers can add more if the hardware supports them.
D3DDDIFORMAT gBddPixelFormats[] = {
    D3DDDIFMT_A8R8G8B8
};

// TODO: Need to also check pinned modes and the path parameters, not just topology
NTSTATUS BASIC_DISPLAY_DRIVER::IsSupportedVidPn(_Inout_ DXGKARG_ISSUPPORTEDVIDPN* pIsSupportedVidPn)
{
    PAGED_CODE();

    BDD_ASSERT(pIsSupportedVidPn != NULL);

    if (pIsSupportedVidPn->hDesiredVidPn == 0)
    {
        // A null desired VidPn is supported
        pIsSupportedVidPn->IsVidPnSupported = TRUE;
        return STATUS_SUCCESS;
    }

    // Default to not supported, until shown it is supported
    pIsSupportedVidPn->IsVidPnSupported = FALSE;

    CONST DXGK_VIDPN_INTERFACE* pVidPnInterface;
    NTSTATUS Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pIsSupportedVidPn->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("DxgkCbQueryVidPnInterface failed with Status = 0x%x, hDesiredVidPn = 0x%p", Status, pIsSupportedVidPn->hDesiredVidPn);
        return Status;
    }

    D3DKMDT_HVIDPNTOPOLOGY hVidPnTopology;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE* pVidPnTopologyInterface;
    Status = pVidPnInterface->pfnGetTopology(pIsSupportedVidPn->hDesiredVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("pfnGetTopology failed with Status = 0x%x, hDesiredVidPn = 0x%p", Status, pIsSupportedVidPn->hDesiredVidPn);
        return Status;
    }

    // For every source in this topology, make sure they don't have more paths than there are targets
    for (D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId = 0; SourceId < MAX_VIEWS; ++SourceId)
    {
        SIZE_T NumPathsFromSource = 0;
        Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology, SourceId, &NumPathsFromSource);
        if (Status == STATUS_GRAPHICS_SOURCE_NOT_IN_TOPOLOGY)
        {
            continue;
        }
        else if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR("pfnGetNumPathsFromSource failed with Status = 0x%x. hVidPnTopology = 0x%p, SourceId = 0x%x",
                           Status, hVidPnTopology, SourceId);
            return Status;
        }
        else if (NumPathsFromSource > MAX_CHILDREN)
        {
            // This VidPn is not supported, which has already been set as the default
            return STATUS_SUCCESS;
        }
    }

    // All sources succeeded so this VidPn is supported
    pIsSupportedVidPn->IsVidPnSupported = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::RecommendFunctionalVidPn(_In_ CONST DXGKARG_RECOMMENDFUNCTIONALVIDPN* CONST pRecommendFunctionalVidPn)
{
    PAGED_CODE();

    BDD_ASSERT(pRecommendFunctionalVidPn == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS BASIC_DISPLAY_DRIVER::RecommendVidPnTopology(_In_ CONST DXGKARG_RECOMMENDVIDPNTOPOLOGY* CONST pRecommendVidPnTopology)
{
    PAGED_CODE();

    BDD_ASSERT(pRecommendVidPnTopology == NULL);

    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

NTSTATUS BASIC_DISPLAY_DRIVER::RecommendMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes)
{
    PAGED_CODE();
    UINT32 width;
    UINT32 height;
    UINT32 default_dh_mode;
    PVChild *pChild = m_CurrentModes[pRecommendMonitorModes->VideoPresentTargetId].pPVChild;

    //Get the display handler's recommended mode
    if(!DisplayConnected())
        return STATUS_GRAPHICS_ADAPTER_WAS_RESET;
    default_dh_mode = pChild->get_recommended_mode(&width, &height);
    return AddRecommendedMonitorModes(pRecommendMonitorModes, width, height);

}

// Tell DMM about all the modes, etc. that are supported
NTSTATUS BASIC_DISPLAY_DRIVER::EnumVidPnCofuncModality(_In_ CONST DXGKARG_ENUMVIDPNCOFUNCMODALITY* CONST pEnumCofuncModality)
{
    PAGED_CODE();

    BDD_ASSERT(pEnumCofuncModality != NULL);

    D3DKMDT_HVIDPNTOPOLOGY                   hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET              hVidPnSourceModeSet = 0;
    D3DKMDT_HVIDPNTARGETMODESET              hVidPnTargetModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE*              pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE*      pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface = NULL;
    CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pVidPnTargetModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH*        pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH*        pVidPnPresentPathTemp = NULL; // Used for AcquireNextPathInfo
    CONST D3DKMDT_VIDPN_SOURCE_MODE*         pVidPnPinnedSourceModeInfo = NULL;
    CONST D3DKMDT_VIDPN_TARGET_MODE*         pVidPnPinnedTargetModeInfo = NULL;

    // Verify that we are connected to the display handler
    if(!DisplayConnected())
        return STATUS_GRAPHICS_ADAPTER_WAS_RESET;

    // Get the VidPn Interface so we can get the 'Source Mode Set', 'Target Mode Set' and 'VidPn Topology' interfaces
    NTSTATUS Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pEnumCofuncModality->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("DxgkCbQueryVidPnInterface failed with Status = 0x%x, hFunctionalVidPn = 0x%p", Status, pEnumCofuncModality->hConstrainingVidPn);
        return Status;
    }

    // Get the VidPn Topology interface so we can enumerate all paths
    Status = pVidPnInterface->pfnGetTopology(pEnumCofuncModality->hConstrainingVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("pfnGetTopology failed with Status = 0x%x, hFunctionalVidPn = 0x%p", Status, pEnumCofuncModality->hConstrainingVidPn);
        return Status;
    }

    // Get the first path before we start looping through them
    Status = pVidPnTopologyInterface->pfnAcquireFirstPathInfo(hVidPnTopology, &pVidPnPresentPath);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("pfnAcquireFirstPathInfo failed with Status = 0x%x, hVidPnTopology = 0x%p", Status, hVidPnTopology);
        return Status;
    }

    // Loop through all available paths.
    while (Status != STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        // Get the Source Mode Set interface so the pinned mode can be retrieved
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                          pVidPnPresentPath->VidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);

        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR("pfnAcquireSourceModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%p, SourceId = 0x%x",
                           Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnSourceId);
            break;
        }

        // Get the pinned mode, needed when VidPnSource isn't pivot, and when VidPnTarget isn't pivot
        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet, &pVidPnPinnedSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR("pfnAcquirePinnedModeInfo failed with Status = 0x%x, hVidPnSourceModeSet = 0x%p", Status, hVidPnSourceModeSet);
            break;
        }

        // SOURCE MODES: If this source mode isn't the pivot point, do work on the source mode set
        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId)))
        {
            // If there's no pinned source add possible modes (otherwise they've already been added)
            if (pVidPnPinnedSourceModeInfo == NULL)
            {
                // Release the acquired source mode set, since going to create a new one to put all modes in
                Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR("pfnReleaseSourceModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%p, hVidPnSourceModeSet = 0x%p",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
                    break;
                }
                hVidPnSourceModeSet = 0; // Successfully released it

                // Create a new source mode set which will be added to the constraining VidPn with all the possible modes
                Status = pVidPnInterface->pfnCreateNewSourceModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnSourceId,
                                                                    &hVidPnSourceModeSet,
                                                                    &pVidPnSourceModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR("pfnCreateNewSourceModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%p, SourceId = 0x%x",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnSourceId);
                    break;
                }

                // Add the appropriate modes to the source mode set
                {
                    Status = AddAvailableSourceModes(pVidPnSourceModeSetInterface, hVidPnSourceModeSet, pVidPnPresentPath->VidPnTargetId);
                }

                if (!NT_SUCCESS(Status))
                {
                    break;
                }

                // Give DMM back the source modes just populated
                Status = pVidPnInterface->pfnAssignSourceModeSet(pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnSourceId, hVidPnSourceModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ERROR("pfnAssignSourceModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%p, SourceId = 0x%x, hVidPnSourceModeSet = 0x%p",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnSourceId, hVidPnSourceModeSet);
                    break;
                }
                hVidPnSourceModeSet = 0; // Successfully assigned it (equivalent to releasing it)
            }
        }// End: SOURCE MODES

        // TARGET MODES: If this target mode isn't the pivot point, do work on the target mode set
        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            // Get the Target Mode Set interface so modes can be added if necessary
            Status = pVidPnInterface->pfnAcquireTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                              pVidPnPresentPath->VidPnTargetId,
                                                              &hVidPnTargetModeSet,
                                                              &pVidPnTargetModeSetInterface);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR("pfnAcquireTargetModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%p, TargetId = 0x%x",
                               Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnTargetId);
                break;
            }

            Status = pVidPnTargetModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnTargetModeSet, &pVidPnPinnedTargetModeInfo);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR("pfnAcquirePinnedModeInfo failed with Status = 0x%x, hVidPnTargetModeSet = 0x%p", Status, hVidPnTargetModeSet);
                break;
            }

            // If there's no pinned target add possible modes (otherwise they've already been added)
            if (pVidPnPinnedTargetModeInfo == NULL)
            {
                // Release the acquired target mode set, since going to create a new one to put all modes in
                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ASSERTION("pfnReleaseTargetModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%x, hVidPnTargetModeSet = 0x%x",
                                       Status, pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
                    break;
                }
                hVidPnTargetModeSet = 0; // Successfully released it

                // Create a new target mode set which will be added to the constraining VidPn with all the possible modes
                Status = pVidPnInterface->pfnCreateNewTargetModeSet(pEnumCofuncModality->hConstrainingVidPn,
                                                                    pVidPnPresentPath->VidPnTargetId,
                                                                    &hVidPnTargetModeSet,
                                                                    &pVidPnTargetModeSetInterface);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_INFORMATION("pfnCreateNewTargetModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%p, TargetId = 0x%x\n",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnTargetId);
                    break;
                }

                UINT32 numModes;
                Status = AddAvailableTargetModes(pVidPnTargetModeSetInterface, hVidPnTargetModeSet, pVidPnPinnedSourceModeInfo, pVidPnPresentPath->VidPnTargetId, &numModes);

                if (!NT_SUCCESS(Status))
                {
                    break;
                }

                // Give DMM back the source modes just populated
                Status = pVidPnInterface->pfnAssignTargetModeSet(pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnTargetId, hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status) && numModes !=0)
                {
                    BDD_LOG_ERROR("pfnAssignTargetModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%p, TargetId = 0x%x, hVidPnTargetModeSet = 0x%p",
                                   Status, pEnumCofuncModality->hConstrainingVidPn, pVidPnPresentPath->VidPnTargetId, hVidPnTargetModeSet);
                    break;
                }
                hVidPnTargetModeSet = 0; // Successfully assigned it (equivalent to releasing it)
            }
            else
            {
                // Release the pinned target as there's no other work to do
                Status = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ASSERTION("pfnReleaseModeInfo failed with Status = 0x%I64x, hVidPnTargetModeSet = 0x%I64x, pVidPnPinnedTargetModeInfo = 0x%I64x",
                                        Status, hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
                    break;
                }
                pVidPnPinnedTargetModeInfo = NULL; // Successfully released it

                // Release the acquired target mode set, since it is no longer needed
                Status = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
                if (!NT_SUCCESS(Status))
                {
                    BDD_LOG_ASSERTION("pfnReleaseTargetModeSet failed with Status = 0x%I64x, hConstrainingVidPn = 0x%I64x, hVidPnTargetModeSet = 0x%I64x",
                                       Status, pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
                    break;
                }
                hVidPnTargetModeSet = 0; // Successfully released it
            }
        }// End: TARGET MODES

        // Nothing else needs the pinned source mode so release it
        if (pVidPnPinnedSourceModeInfo != NULL)
        {
            Status = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ASSERTION("pfnReleaseModeInfo failed with Status = 0x%I64x, hVidPnSourceModeSet = 0x%I64x, pVidPnPinnedSourceModeInfo = 0x%I64x",
                                    Status, hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
                break;
            }
            pVidPnPinnedSourceModeInfo = NULL; // Successfully released it
        }

        // With the pinned source mode now released, if the source mode set hasn't been released, release that as well
        if (hVidPnSourceModeSet != 0)
        {
            Status = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR("pfnReleaseSourceModeSet failed with Status = 0x%x, hConstrainingVidPn = 0x%p, hVidPnSourceModeSet = 0x%p",
                               Status, pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
                break;
            }
            hVidPnSourceModeSet = 0; // Successfully released it
        }

        // If modifying support fields, need to modify a local version of a path structure since the retrieved one is const
        D3DKMDT_VIDPN_PRESENT_PATH LocalVidPnPresentPath = *pVidPnPresentPath;
        BOOLEAN SupportFieldsModified = FALSE;

        // SCALING: If this path's scaling isn't the pivot point, do work on the scaling support
        if (!((pEnumCofuncModality->EnumPivotType == D3DKMDT_EPT_SCALING) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            // If the scaling is unpinned, then modify the scaling support field
            if (pVidPnPresentPath->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED)
            {
                // Identity and centered scaling are supported, but not any stretch modes
                RtlZeroMemory(&(LocalVidPnPresentPath.ContentTransformation.ScalingSupport), sizeof(D3DKMDT_VIDPN_PRESENT_PATH_SCALING_SUPPORT));
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Identity = 1;
                LocalVidPnPresentPath.ContentTransformation.ScalingSupport.Centered = 1;
                SupportFieldsModified = TRUE;
            }
        } // End: SCALING

        // ROTATION: If this path's rotation isn't the pivot point, do work on the rotation support
        if (!((pEnumCofuncModality->EnumPivotType != D3DKMDT_EPT_ROTATION) &&
              (pEnumCofuncModality->EnumPivot.VidPnSourceId == pVidPnPresentPath->VidPnSourceId) &&
              (pEnumCofuncModality->EnumPivot.VidPnTargetId == pVidPnPresentPath->VidPnTargetId)))
        {
            // If the rotation is unpinned, then modify the rotation support field
            if (pVidPnPresentPath->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)
            {
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Identity = 1;
                // Sample supports only Rotate90
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate90 = 1;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate180 = 0;
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Rotate270 = 0;

                // Since clone is not supported, should not support path-independent rotations
#if(DXGKDDI_INTERFACE_VERSION > DXGKDDI_INTERFACE_VERSION_WDDM1_3)
                LocalVidPnPresentPath.ContentTransformation.RotationSupport.Offset0 = 1;
#endif

                SupportFieldsModified = TRUE;
            }
        } // End: ROTATION

        if (SupportFieldsModified)
        {
            // The correct path will be found by this function and the appropriate fields updated
            Status = pVidPnTopologyInterface->pfnUpdatePathSupportInfo(hVidPnTopology, &LocalVidPnPresentPath);
            if (!NT_SUCCESS(Status))
            {
                BDD_LOG_ERROR("pfnUpdatePathSupportInfo failed with Status = 0x%x, hVidPnTopology = 0x%p", Status, hVidPnTopology);
                break;
            }
        }

        // Get the next path...
        // (NOTE: This is the value of Status that will return STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET when it's time to quit the loop)
        pVidPnPresentPathTemp = pVidPnPresentPath;
        Status = pVidPnTopologyInterface->pfnAcquireNextPathInfo(hVidPnTopology, pVidPnPresentPathTemp, &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR("pfnAcquireNextPathInfo failed with Status = 0x%x, hVidPnTopology = 0x%p, pVidPnPresentPathTemp = 0x%p", Status, hVidPnTopology, pVidPnPresentPathTemp);
            break;
        }

        // ...and release the last path
        NTSTATUS TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        if (!NT_SUCCESS(TempStatus))
        {
            BDD_LOG_ERROR("pfnReleasePathInfo failed with Status = 0x%x, hVidPnTopology = 0x%p, pVidPnPresentPathTemp = 0x%p", TempStatus, hVidPnTopology, pVidPnPresentPathTemp);
            Status = TempStatus;
            break;
        }
        pVidPnPresentPathTemp = NULL; // Successfully released it
    }// End: while loop for paths in topology

    // If quit the while loop normally, set the return value to success
    if (Status == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET)
    {
        Status = STATUS_SUCCESS;
    }

    // Release any resources hanging around because the loop was quit early.
    // Since in normal execution everything should be released by this point, TempStatus is initialized to a bogus error to be used as an
    //  assertion that if anything had to be released now (TempStatus changing) Status isn't successful.
    NTSTATUS TempStatus = STATUS_NOT_FOUND;

    if ((pVidPnSourceModeSetInterface != NULL) &&
        (pVidPnPinnedSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnPinnedSourceModeInfo);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTargetModeSetInterface != NULL) &&
        (pVidPnPinnedTargetModeInfo != NULL))
    {
        TempStatus = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnPinnedTargetModeInfo);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPath != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (pVidPnPresentPathTemp != NULL)
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPathTemp);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnSourceModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnSourceModeSet);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    if (hVidPnTargetModeSet != 0)
    {
        TempStatus = pVidPnInterface->pfnReleaseTargetModeSet(pEnumCofuncModality->hConstrainingVidPn, hVidPnTargetModeSet);
        BDD_ASSERT_CHK(NT_SUCCESS(TempStatus));
    }

    BDD_ASSERT_CHK(TempStatus == STATUS_NOT_FOUND || Status != STATUS_SUCCESS);

    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::SetVidPnSourceVisibility(_In_ CONST DXGKARG_SETVIDPNSOURCEVISIBILITY* pSetVidPnSourceVisibility)
{
    PAGED_CODE();

    BDD_ASSERT(pSetVidPnSourceVisibility != NULL);
    BDD_ASSERT((pSetVidPnSourceVisibility->VidPnSourceId < MAX_VIEWS) ||
               (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL));

    UINT StartVidPnSourceId = (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL) ? 0 : pSetVidPnSourceVisibility->VidPnSourceId;
    UINT MaxVidPnSourceId = (pSetVidPnSourceVisibility->VidPnSourceId == D3DDDI_ID_ALL) ? MAX_VIEWS : pSetVidPnSourceVisibility->VidPnSourceId + 1;

    for (UINT SourceId = StartVidPnSourceId; SourceId < MaxVidPnSourceId; ++SourceId)
    {
        BDD_ASSERT(SourceId < MAX_VIEWS);
        UINT32 TargetId(m_CurrentModes[SourceId].TargetId);
        if(!m_CurrentModes[TargetId].pPVChild->connected()) 
			continue;
        
        m_CurrentModes[TargetId].pPVChild->blank_display(true, !pSetVidPnSourceVisibility->Visible);

        // Store current visibility so it can be dealt with during Present call
        m_CurrentModes[TargetId].Flags.SourceNotVisible = !(pSetVidPnSourceVisibility->Visible);
    }

    return STATUS_SUCCESS;
}

// NOTE: The value of pCommitVidPn->hPrimaryAllocation is also ignored, since BDD is a display only driver and does not deal with allocations
NTSTATUS BASIC_DISPLAY_DRIVER::CommitVidPn(_In_ CONST DXGKARG_COMMITVIDPN* CONST pCommitVidPn)
{
    PAGED_CODE();

    BDD_ASSERT(pCommitVidPn != NULL);
    BDD_ASSERT(pCommitVidPn->AffectedVidPnSourceId < MAX_VIEWS);

    NTSTATUS                                 Status;
    SIZE_T                                   NumPaths = 0;
    D3DKMDT_HVIDPNTOPOLOGY                   hVidPnTopology = 0;
    D3DKMDT_HVIDPNSOURCEMODESET              hVidPnSourceModeSet = 0;
    CONST DXGK_VIDPN_INTERFACE*              pVidPnInterface = NULL;
    CONST DXGK_VIDPNTOPOLOGY_INTERFACE*      pVidPnTopologyInterface = NULL;
    CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface = NULL;
    CONST D3DKMDT_VIDPN_PRESENT_PATH*        pVidPnPresentPath = NULL;
    CONST D3DKMDT_VIDPN_SOURCE_MODE*         pPinnedVidPnSourceModeInfo = NULL;
    D3DDDI_VIDEO_PRESENT_TARGET_ID           TargetId = D3DDDI_ID_UNINITIALIZED;

    // Check this CommitVidPn is for the mode change notification when monitor is in power off state.
    if (pCommitVidPn->Flags.PathPoweredOff)
    {
        // Ignore the commitVidPn call for the mode change notification when monitor is in power off state.
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    // Get the VidPn Interface so we can get the 'Source Mode Set' and 'VidPn Topology' interfaces
    Status = m_DxgkInterface.DxgkCbQueryVidPnInterface(pCommitVidPn->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &pVidPnInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("DxgkCbQueryVidPnInterface failed with Status = 0x%x, hFunctionalVidPn = 0x%p", Status, pCommitVidPn->hFunctionalVidPn);
        goto CommitVidPnExit;
    }

    // Get the VidPn Topology interface so can enumerate paths from source
    Status = pVidPnInterface->pfnGetTopology(pCommitVidPn->hFunctionalVidPn, &hVidPnTopology, &pVidPnTopologyInterface);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("pfnGetTopology failed with Status = 0x%x, hFunctionalVidPn = 0x%p", Status, pCommitVidPn->hFunctionalVidPn);
        goto CommitVidPnExit;
    }

    // Find out the number of paths now, if it's 0 don't bother with source mode set and pinned mode, just clear current and then quit
    Status = pVidPnTopologyInterface->pfnGetNumPaths(hVidPnTopology, &NumPaths);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("pfnGetNumPaths failed with Status = 0x%x, hVidPnTopology = 0x%p", Status, hVidPnTopology);
        goto CommitVidPnExit;
    }

    if (NumPaths != 0)
    {
        // Get the Source Mode Set interface so we can get the pinned mode
        Status = pVidPnInterface->pfnAcquireSourceModeSet(pCommitVidPn->hFunctionalVidPn,
                                                          pCommitVidPn->AffectedVidPnSourceId,
                                                          &hVidPnSourceModeSet,
                                                          &pVidPnSourceModeSetInterface);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR("pfnAcquireSourceModeSet failed with Status = 0x%x, hFunctionalVidPn = 0x%p, SourceId = 0x%x", Status, pCommitVidPn->hFunctionalVidPn, pCommitVidPn->AffectedVidPnSourceId);
            goto CommitVidPnExit;
        }

        // Get the mode that is being pinned
        Status = pVidPnSourceModeSetInterface->pfnAcquirePinnedModeInfo(hVidPnSourceModeSet, &pPinnedVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR("pfnAcquirePinnedModeInfo failed with Status = 0x%x, hFunctionalVidPn = 0x%p", Status, pCommitVidPn->hFunctionalVidPn);
            goto CommitVidPnExit;
        }
    }
    else
    {
        // This will cause the successful quit below
        pPinnedVidPnSourceModeInfo = NULL;
    }

    if (pPinnedVidPnSourceModeInfo == NULL)
    {
        // There is no mode to pin on this source, any old paths here have already been cleared
        Status = STATUS_SUCCESS;
        goto CommitVidPnExit;
    }

    Status = IsVidPnSourceModeFieldsValid(pPinnedVidPnSourceModeInfo);
    if (!NT_SUCCESS(Status))
    {
        goto CommitVidPnExit;
    }

    // Get the number of paths from this source so we can loop through all paths
    SIZE_T NumPathsFromSource = 0;
    Status = pVidPnTopologyInterface->pfnGetNumPathsFromSource(hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, &NumPathsFromSource);
    if (!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("pfnGetNumPathsFromSource failed with Status = 0x%x, hVidPnTopology = 0x%p", Status, hVidPnTopology);
        goto CommitVidPnExit;
    }

    // Loop through all paths to set this mode
    for (SIZE_T PathIndex = 0; PathIndex < NumPathsFromSource; ++PathIndex)
    {
        // Get the target id for this path
		TargetId = D3DDDI_ID_UNINITIALIZED;
        Status = pVidPnTopologyInterface->pfnEnumPathTargetsFromSource(hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, PathIndex, &TargetId);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR("pfnEnumPathTargetsFromSource failed with Status = 0x%x, hVidPnTopology = 0x%p, SourceId = 0x%x, PathIndex = 0x%x",
                            Status, hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, PathIndex);
            goto CommitVidPnExit;
        }

        // Get the actual path info
        Status = pVidPnTopologyInterface->pfnAcquirePathInfo(hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, TargetId, &pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            BDD_LOG_ERROR("pfnAcquirePathInfo failed with Status = 0x%x, hVidPnTopology = 0x%p, SourceId = 0x%x, TargetId = 0x%x",
                            Status, hVidPnTopology, pCommitVidPn->AffectedVidPnSourceId, TargetId);
            goto CommitVidPnExit;
        }

        Status = IsVidPnPathFieldsValid(pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        if(pCommitVidPn->MonitorConnectivityChecks == D3DKMDT_MCC_ENFORCE && !m_CurrentModes[pVidPnPresentPath->VidPnTargetId].pPVChild->connected())
        {
            BDD_LOG_ERROR("XENWDDM!%s cannot force a disconnected monitor #%u\n", __FUNCTION__, TargetId);
            return STATUS_GRAPHICS_INVALID_VIDPN_TOPOLOGY;
        }

        Status = SetSourceModeAndPath(pPinnedVidPnSourceModeInfo, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
            goto CommitVidPnExit;
        }

        Status = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        if (!NT_SUCCESS(Status))
        {
           BDD_LOG_ERROR("pfnReleasePathInfo failed with Status = 0x%x, hVidPnTopoogy = 0x%p, pVidPnPresentPath = 0x%p",
                            Status, hVidPnTopology, pVidPnPresentPath);
            goto CommitVidPnExit;
        }
        pVidPnPresentPath = NULL; // Successfully released it
    }

CommitVidPnExit:

    NTSTATUS TempStatus;

    if ((pVidPnSourceModeSetInterface != NULL) &&
        (hVidPnSourceModeSet != 0) &&
        (pPinnedVidPnSourceModeInfo != NULL))
    {
        TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pPinnedVidPnSourceModeInfo);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnInterface != NULL) &&
        (pCommitVidPn->hFunctionalVidPn != 0) &&
        (hVidPnSourceModeSet != 0))
    {
        TempStatus = pVidPnInterface->pfnReleaseSourceModeSet(pCommitVidPn->hFunctionalVidPn, hVidPnSourceModeSet);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }

    if ((pVidPnTopologyInterface != NULL) &&
        (hVidPnTopology != 0) &&
        (pVidPnPresentPath != NULL))
    {
        TempStatus = pVidPnTopologyInterface->pfnReleasePathInfo(hVidPnTopology, pVidPnPresentPath);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }
    if(!NT_SUCCESS(Status))
    {
       // DbgBreakPoint();
    }
    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::UpdateActiveVidPnPresentPath(_In_ CONST DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* CONST pUpdateActiveVidPnPresentPath)
{
    PAGED_CODE();

    BDD_ASSERT(pUpdateActiveVidPnPresentPath != NULL);

    NTSTATUS Status = IsVidPnPathFieldsValid(&(pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo));
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    m_CurrentModes[pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.VidPnSourceId].Rotation = 
        pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo.ContentTransformation.Rotation;

    return STATUS_SUCCESS;
}

//
// Private BDD DMM functions
//

NTSTATUS BASIC_DISPLAY_DRIVER::SetSourceModeAndPath(CONST D3DKMDT_VIDPN_SOURCE_MODE* pSourceMode,
                                                    CONST D3DKMDT_VIDPN_PRESENT_PATH* pPath)
{
    PAGED_CODE();
    BOOLEAN bNewMode = FALSE;

    //Set Path
    CURRENT_BDD_MODE* pCurrentBddMode = &m_CurrentModes[pPath->VidPnSourceId];
    pCurrentBddMode->TargetId = pPath->VidPnTargetId;
    pCurrentBddMode = &m_CurrentModes[pPath->VidPnTargetId];

    BDD_TRACE_SOURCE(pPath->VidPnSourceId); 

    //Update target Mode
    bNewMode = UpdateCurrentMode(pPath->VidPnTargetId, pSourceMode->Format.Graphics.PrimSurfSize.cx,
                      pSourceMode->Format.Graphics.PrimSurfSize.cy);
    
    //This is the actual target for this SOURCE VIDPN
    PVChild * pTarget(m_CurrentModes[pPath->VidPnTargetId].pPVChild);
    HoldScopedMutex fb_mutex(pTarget->fb_mutex(), __FUNCTION__, pTarget->target_id());
    NTSTATUS Status;

    Status = pTarget->update_mode(pSourceMode->Format.Graphics.PrimSurfSize.cx,
                        pSourceMode->Format.Graphics.PrimSurfSize.cy);

    if(!NT_SUCCESS(Status))
    {
        BDD_LOG_ERROR("XENWDDM!%s %d:%d FAILED~!~~~~\n", __FUNCTION__, pPath->VidPnSourceId, pCurrentBddMode->pPVChild->key());
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }

    
    pCurrentBddMode->Scaling = pPath->ContentTransformation.Scaling;
    pCurrentBddMode->SrcModeWidth = pSourceMode->Format.Graphics.PrimSurfSize.cx;
    pCurrentBddMode->SrcModeHeight = pSourceMode->Format.Graphics.PrimSurfSize.cy;
    pCurrentBddMode->Rotation = pPath->ContentTransformation.Rotation;

    if(!pCurrentBddMode->Flags.FrameBufferIsActive)
    {
        BDD_LOG_ERROR("XENWDDM!%s source %d mapping inactive framebuffer \n", __FUNCTION__, pPath->VidPnSourceId);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }

    BDD_LOG_EVENT("XENWDDM:%s adding source/target %d/%d (%d x %d)\n", __FUNCTION__, pPath->VidPnSourceId,
        pPath->VidPnTargetId, pCurrentBddMode->SrcModeWidth, pCurrentBddMode->SrcModeHeight);
    return STATUS_SUCCESS;
}


NTSTATUS BASIC_DISPLAY_DRIVER::IsVidPnPathFieldsValid(CONST D3DKMDT_VIDPN_PRESENT_PATH* pPath) const
{
    PAGED_CODE();

    if (pPath->VidPnSourceId >= MAX_VIEWS)
    {
        BDD_LOG_ERROR("VidPnSourceId is 0x%x is too high (MAX_VIEWS is 0x%x)",
            pPath->VidPnSourceId, MAX_VIEWS);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE;
    }
    else if (pPath->VidPnTargetId >= MAX_CHILDREN)
    {
        BDD_LOG_ERROR("VidPnTargetId is 0x%x is too high (MAX_CHILDREN is 0x%x)",
            pPath->VidPnTargetId, MAX_CHILDREN);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET;
    }
    else if (pPath->GammaRamp.Type != D3DDDI_GAMMARAMP_DEFAULT)
    {
        BDD_LOG_ERROR("pPath contains a gamma ramp (0x%x)", pPath->GammaRamp.Type);
        return STATUS_GRAPHICS_GAMMA_RAMP_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_IDENTITY) &&
        (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_CENTERED) &&
        (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_NOTSPECIFIED) &&
        (pPath->ContentTransformation.Scaling != D3DKMDT_VPPS_UNINITIALIZED))
    {
        BDD_LOG_ERROR("pPath contains a non-identity scaling (0x%x)", pPath->ContentTransformation.Scaling);
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_IDENTITY) &&
        (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_ROTATE90) &&
        (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_NOTSPECIFIED) &&
        (pPath->ContentTransformation.Rotation != D3DKMDT_VPPR_UNINITIALIZED))
    {
        BDD_LOG_ERROR("pPath contains a not-supported rotation (0x%x)", pPath->ContentTransformation.Rotation);
        return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    else if ((pPath->VidPnTargetColorBasis != D3DKMDT_CB_SCRGB) &&
        (pPath->VidPnTargetColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        BDD_LOG_ERROR("pPath has a non-linear RGB color basis (0x%x)", pPath->VidPnTargetColorBasis);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else
    {
        return STATUS_SUCCESS;
    }
}

NTSTATUS BASIC_DISPLAY_DRIVER::IsVidPnSourceModeFieldsValid(CONST D3DKMDT_VIDPN_SOURCE_MODE* pSourceMode) const
{
    PAGED_CODE();

    if (pSourceMode->Type != D3DKMDT_RMT_GRAPHICS)
    {
        BDD_LOG_ERROR("pSourceMode is a non-graphics mode (0x%x)", pSourceMode->Type);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if ((pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_SCRGB) &&
             (pSourceMode->Format.Graphics.ColorBasis != D3DKMDT_CB_UNINITIALIZED))
    {
        BDD_LOG_ERROR("pSourceMode has a non-linear RGB color basis (0x%x)", pSourceMode->Format.Graphics.ColorBasis);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else if (pSourceMode->Format.Graphics.PixelValueAccessMode != D3DKMDT_PVAM_DIRECT)
    {
        BDD_LOG_ERROR("pSourceMode has a palettized access mode (0x%x)", pSourceMode->Format.Graphics.PixelValueAccessMode);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
    else
    {
        for (UINT PelFmtIdx = 0; PelFmtIdx < ARRAYSIZE(gBddPixelFormats); ++PelFmtIdx)
        {
            if (pSourceMode->Format.Graphics.PixelFormat == gBddPixelFormats[PelFmtIdx])
            {
                return STATUS_SUCCESS;
            }
        }

        BDD_LOG_ERROR("pSourceMode has an unknown pixel format (0x%x)", pSourceMode->Format.Graphics.PixelFormat);
        return STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_SOURCE_MODE;
    }
}


NTSTATUS BASIC_DISPLAY_DRIVER::AddAvailableModes(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface,
                                                 D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                                 D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId,
                                                 D3DKMDT_VIDPN_SOURCE_MODE* pVidPnSourceModeInfo)
{
    PAGED_CODE();
    NTSTATUS Status;

    //Get the modes 
    CurrentModes modes(m_CurrentModes[SourceId].pPVChild);

    //Add available modes    
    for(UINT32 i = 0; i <modes.modes(); i++)
    {
        Status = pVidPnSourceModeSetInterface->pfnCreateNewModeInfo(hVidPnSourceModeSet, &pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            // If failed to create a new mode info, mode doesn't need to be released since it was never created
            BDD_LOG_ERROR("pfnCreateNewModeInfo failed with Status = 0x%x, hVidPnSourceModeSet = 0x%p", Status, hVidPnSourceModeSet);
            return Status;
        }

        // Populate mode info with values from mode at ModeIndex and hard-coded values
        // Always report 32 bpp format, this will be color converted during the present if the mode at ModeIndex was < 32bpp
        pVidPnSourceModeInfo->Type = D3DKMDT_RMT_GRAPHICS;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx = modes.width(i);
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy = modes.height(i);
        pVidPnSourceModeInfo->Format.Graphics.VisibleRegionSize = pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize;
        pVidPnSourceModeInfo->Format.Graphics.Stride = modes.stride(i);
        pVidPnSourceModeInfo->Format.Graphics.PixelFormat = gBddPixelFormats[0];
        pVidPnSourceModeInfo->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        pVidPnSourceModeInfo->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

        // Add the mode to the source mode set
        Status = pVidPnSourceModeSetInterface->pfnAddMode(hVidPnSourceModeSet, pVidPnSourceModeInfo);
        if(!NT_SUCCESS(Status))
        {
            if(Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                BDD_LOG_ERROR("pfnAddMode failed with Status = 0x%x, hVidPnSourceModeSet = 0x%p, pVidPnSourceModeInfo = 0x%p", Status, hVidPnSourceModeSet, pVidPnSourceModeInfo);
            }

            // If adding the mode failed, release the mode, if this doesn't work there is nothing that can be done, some memory will get leaked, continue to next mode anyway
            Status = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnSourceModeInfo);
            BDD_ASSERT_CHK(NT_SUCCESS(Status));
        }
    }
    BDD_LOG_INFORMATION("pfnAddMode SourceId %d added %d modes \n", SourceId, modes.modes());
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::AddAvailableSourceModes(_In_ CONST DXGK_VIDPNSOURCEMODESET_INTERFACE* pVidPnSourceModeSetInterface,
                                                   D3DKMDT_HVIDPNSOURCEMODESET hVidPnSourceModeSet,
                                                   D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId)
{
    PAGED_CODE();
    // Create new mode info that will be populated
    D3DKMDT_VIDPN_SOURCE_MODE* pVidPnSourceModeInfo = NULL;
    

    for (UINT PelFmtIdx = 0; PelFmtIdx < ARRAYSIZE(gBddPixelFormats); ++PelFmtIdx)
    {
 
        NTSTATUS Status = pVidPnSourceModeSetInterface->pfnCreateNewModeInfo(hVidPnSourceModeSet, &pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            // If failed to create a new mode info, mode doesn't need to be released since it was never created
            BDD_LOG_ERROR("pfnCreateNewModeInfo failed with Status = 0x%x, hVidPnSourceModeSet = 0x%p", Status, hVidPnSourceModeSet);
            return Status;
        }

        // Populate mode info with values from current mode and hard-coded values
        // Always report 32 bpp format, this will be color converted during the present if the mode is < 32bpp
        pVidPnSourceModeInfo->Type = D3DKMDT_RMT_GRAPHICS;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cx = m_CurrentModes[SourceId].DispInfo.Width;
        pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize.cy = m_CurrentModes[SourceId].DispInfo.Height;
        pVidPnSourceModeInfo->Format.Graphics.VisibleRegionSize = pVidPnSourceModeInfo->Format.Graphics.PrimSurfSize;
        pVidPnSourceModeInfo->Format.Graphics.Stride = m_CurrentModes[SourceId].DispInfo.Pitch;
        pVidPnSourceModeInfo->Format.Graphics.PixelFormat = gBddPixelFormats[PelFmtIdx];
        pVidPnSourceModeInfo->Format.Graphics.ColorBasis = D3DKMDT_CB_SCRGB;
        pVidPnSourceModeInfo->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;

        // Add the mode to the source mode set
        Status = pVidPnSourceModeSetInterface->pfnAddMode(hVidPnSourceModeSet, pVidPnSourceModeInfo);
        if (!NT_SUCCESS(Status))
        {
            // If adding the mode failed, release the mode, if this doesn't work there is nothing that can be done, some memory will get leaked
            NTSTATUS TempStatus = pVidPnSourceModeSetInterface->pfnReleaseModeInfo(hVidPnSourceModeSet, pVidPnSourceModeInfo);
            UNREFERENCED_PARAMETER(TempStatus);
            NT_ASSERT(NT_SUCCESS(TempStatus));

            if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
            {
                BDD_LOG_ERROR("pfnAddMode failed with Status = 0x%x, hVidPnSourceModeSet = 0x%p, pVidPnSourceModeInfo = 0x%p", Status, hVidPnSourceModeSet, pVidPnSourceModeInfo);
                return Status;
            }
        }
    }

    AddAvailableModes(pVidPnSourceModeSetInterface, hVidPnSourceModeSet, SourceId, pVidPnSourceModeInfo);
    return STATUS_SUCCESS;
}

NTSTATUS BASIC_DISPLAY_DRIVER::AddTargetMode(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pVidPnTargetModeSetInterface, 
                                             D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet, 
                                            _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE* pVidPnPinnedSourceModeInfo,
                                            UINT32 width, UINT32 height, D3DKMDT_MODE_PREFERENCE Preferred)
{
    PAGED_CODE();
    NTSTATUS Status;
    D3DKMDT_VIDPN_TARGET_MODE* pVidPnTargetModeInfo = NULL;

    Status = pVidPnTargetModeSetInterface->pfnCreateNewModeInfo(hVidPnTargetModeSet, &pVidPnTargetModeInfo);
    if(!NT_SUCCESS(Status))
    {
        // If failed to create a new mode info, mode doesn't need to be released since it was never created
        BDD_LOG_ERROR("pfnCreateNewModeInfo failed with Status = 0x%x, hVidPnTargetModeSet = 0x%p", Status, hVidPnTargetModeSet);
        return Status;
    }

    pVidPnTargetModeInfo->VideoSignalInfo.VideoStandard = D3DKMDT_VSS_VESA_DMT;
    UNREFERENCED_PARAMETER(pVidPnPinnedSourceModeInfo);

    pVidPnTargetModeInfo->VideoSignalInfo.TotalSize.cx = width;
    pVidPnTargetModeInfo->VideoSignalInfo.TotalSize.cy = height;
    pVidPnTargetModeInfo->VideoSignalInfo.ActiveSize = pVidPnTargetModeInfo->VideoSignalInfo.TotalSize;
    pVidPnTargetModeInfo->VideoSignalInfo.VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pVidPnTargetModeInfo->VideoSignalInfo.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
    pVidPnTargetModeInfo->Preference = Preferred;

    Status = pVidPnTargetModeSetInterface->pfnAddMode(hVidPnTargetModeSet, pVidPnTargetModeInfo);
    if(!NT_SUCCESS(Status))
    {
        if(Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            BDD_LOG_ERROR("pfnAddMode failed with Status = 0x%x, hVidPnTargetModeSet = 0x%p, pVidPnTargetModeInfo = 0x%p", Status, hVidPnTargetModeSet, pVidPnTargetModeInfo);
        }
        else
        {
            Status = STATUS_SUCCESS;
        }

        // If adding the mode failed, release the mode, if this doesn't work there is nothing that can be done, some memory will get leaked
        NTSTATUS TempStatus = pVidPnTargetModeSetInterface->pfnReleaseModeInfo(hVidPnTargetModeSet, pVidPnTargetModeInfo);
        UNREFERENCED_PARAMETER(TempStatus);
        NT_ASSERT(NT_SUCCESS(TempStatus));
    }
    return Status;
}



// Add the current mode information (acquired from the POST frame buffer) as the target mode.
NTSTATUS BASIC_DISPLAY_DRIVER::AddAvailableTargetModes(_In_ CONST DXGK_VIDPNTARGETMODESET_INTERFACE* pVidPnTargetModeSetInterface,
                                                   D3DKMDT_HVIDPNTARGETMODESET hVidPnTargetModeSet,
                                                   _In_opt_ CONST D3DKMDT_VIDPN_SOURCE_MODE* pVidPnPinnedSourceModeInfo,
                                                   D3DDDI_VIDEO_PRESENT_SOURCE_ID TargetId,
                                                   UINT32 * numModes)
{
    PAGED_CODE();
    NTSTATUS Status(STATUS_SUCCESS);
    PVChild * pChild(m_CurrentModes[TargetId].pPVChild);
    UINT32 recommended_width;
    UINT32 recommended_height;
    
    //Get Display Handler's recommended mode
    pChild->get_recommended_mode(&recommended_width, &recommended_height);


    //If not connected nothing to add
    if(!pChild->connected())
    {
        BDD_LOG_INFORMATION("XENWDDM!%s: %d:%d is not connected\n", __FUNCTION__, TargetId, pChild->key());
        *numModes = 0;
        return Status;
    }

    CurrentModes modes(m_CurrentModes[TargetId].pPVChild);
    *numModes = modes.modes();

    if (!pVidPnPinnedSourceModeInfo)
        BDD_LOG_INFORMATION("XENWDDM!%s no pinned source mode %d\n", __FUNCTION__);
    
    for(UINT i = 0; i < modes._num_modes; i++)
    {
        D3DKMDT_MODE_PREFERENCE preferred = (modes.width(i) == recommended_width &&
                                             modes.height(i) == recommended_height)?
                                                    D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        if (preferred == D3DKMDT_MP_PREFERRED)
        {
            BDD_LOG_INFORMATION("XENWDDM!%s: adding preferred mode to %d (%d x %d)\n", __FUNCTION__, TargetId, recommended_width, recommended_height);
            if (pVidPnPinnedSourceModeInfo) {
                BDD_LOG_INFORMATION("XENWDDM!%s: pinned source mode: %d (%d x %d) \n", __FUNCTION__, pVidPnPinnedSourceModeInfo->Id,
                    pVidPnPinnedSourceModeInfo->Format.Graphics.PrimSurfSize.cx,
                    pVidPnPinnedSourceModeInfo->Format.Graphics.PrimSurfSize.cy);
            }
        }
        Status = AddTargetMode(pVidPnTargetModeSetInterface, hVidPnTargetModeSet, pVidPnPinnedSourceModeInfo, 
                               modes.width(i), modes.height(i), preferred);
    
        if (!NT_SUCCESS(Status)) {
            BDD_LOG_ERROR("XENWDMM!%s: Target %d failed to add mode %d \n\t Status is 0x%x", __FUNCTION__, TargetId, i, Status);
            break;
        }
    }
    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::GetPreferredMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes, UINT32 width, UINT32 height)
{
    PAGED_CODE();
    const D3DKMDT_MONITOR_SOURCE_MODE* pMonitorPreferredMode;
    NTSTATUS Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAcquirePreferredModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet, &pMonitorPreferredMode);
    if (!NT_SUCCESS(Status)) {
        BDD_LOG_ERROR("XENWDDM!%s pfnAcquirePreferredModeInfo failed with 0x%x\n", __FUNCTION__, Status);
        return Status;
    }

    if (pMonitorPreferredMode->VideoSignalInfo.TotalSize.cx != width ||
        pMonitorPreferredMode->VideoSignalInfo.TotalSize.cy != height) {
        BDD_LOG_ERROR("XENWDDM!%s mode ( %dx %d) doesn't match (%dx %d)\n", __FUNCTION__,
            pMonitorPreferredMode->VideoSignalInfo.TotalSize.cx, pMonitorPreferredMode->VideoSignalInfo.TotalSize.cy,
            width, height);
    }
    else {
        BDD_LOG_ERROR("XENWDDM!%s mode of (%d x %d) is fine! \n", __FUNCTION__, width, height);
    }
    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::AddRecommendedMonitorModes(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes, 
                                                          UINT32 preferred_width, UINT32 preferred_height)
{
    PAGED_CODE();
    NTSTATUS Status(STATUS_SUCCESS);

    HoldScopedMutex DisplayMutex(DisplayHelperMutex(), __FUNCTION__, MAX_CHILDREN);

    BDD_LOG_INFORMATION("XENWDDM!%s:%d preferred (%d x %d)\n", __FUNCTION__, pRecommendMonitorModes->VideoPresentTargetId, preferred_width, preferred_height);
    CurrentModes modes(m_CurrentModes[pRecommendMonitorModes->VideoPresentTargetId].pPVChild);

    for (UINT32 i = 0; i < modes.modes(); i++) {
        bool bPreferred = (modes.width(i) == preferred_width && modes.height(i) == preferred_height);

        Status = AddSingleMonitorMode(pRecommendMonitorModes, modes.width(i), modes.height(i), bPreferred);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    return Status;
}

NTSTATUS BASIC_DISPLAY_DRIVER::AddSingleMonitorMode(_In_ CONST DXGKARG_RECOMMENDMONITORMODES* CONST pRecommendMonitorModes, UINT32 width, UINT32 height, bool bPreferred)
{
    PAGED_CODE();

    D3DKMDT_MONITOR_SOURCE_MODE* pMonitorSourceMode = NULL;
    NTSTATUS Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnCreateNewModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet, &pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {
        // If failed to create a new mode info, mode doesn't need to be released since it was never created
        BDD_LOG_ERROR("pfnCreateNewModeInfo failed with Status = 0x%x, hMonitorSourceModeSet = 0x%p", Status, pRecommendMonitorModes->hMonitorSourceModeSet);
        return Status;
    }

    // Since we don't know the real monitor timing information, just use the current display mode (from the POST device) with unknown frequencies
    pMonitorSourceMode->VideoSignalInfo.VideoStandard = D3DKMDT_VSS_VESA_DMT;
    pMonitorSourceMode->VideoSignalInfo.TotalSize.cx = width;
    pMonitorSourceMode->VideoSignalInfo.TotalSize.cy = height;
    pMonitorSourceMode->VideoSignalInfo.ActiveSize = pMonitorSourceMode->VideoSignalInfo.TotalSize;
    pMonitorSourceMode->VideoSignalInfo.VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pMonitorSourceMode->VideoSignalInfo.ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;

    // We set the preference to PREFERRED since this is the only supported mode
    pMonitorSourceMode->Origin = D3DKMDT_MCO_DRIVER;
    pMonitorSourceMode->Preference = bPreferred?  D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
    pMonitorSourceMode->ColorBasis = D3DKMDT_CB_SRGB;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FirstChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.SecondChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    pMonitorSourceMode->ColorCoeffDynamicRanges.FourthChannel = 8;
    Status = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnAddMode(pRecommendMonitorModes->hMonitorSourceModeSet, pMonitorSourceMode);
    if (!NT_SUCCESS(Status))
    {

        BDD_LOG_ERROR("XENWDDM!%s failed here with 0x%x\n", __FUNCTION__, Status);
        if (Status != STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET)
        {
            BDD_LOG_EVENT("pfnAddMode failed with Status = 0x%x, hMonitorSourceModeSet = 0x%p, pMonitorSourceMode = 0x%p\n",
                            Status, pRecommendMonitorModes->hMonitorSourceModeSet, pMonitorSourceMode);
        }
        else
        {
            Status = STATUS_SUCCESS;
        }

        // If adding the mode failed, release the mode, if this doesn't work there is nothing that can be done, some memory will get leaked
        NTSTATUS TempStatus = pRecommendMonitorModes->pMonitorSourceModeSetInterface->pfnReleaseModeInfo(pRecommendMonitorModes->hMonitorSourceModeSet, pMonitorSourceMode);
        UNREFERENCED_PARAMETER(TempStatus);
        //NT_ASSERT(NT_SUCCESS(TempStatus));
    }
    else
    {
        // If AddMode succeeded with something other than STATUS_SUCCESS treat it as such anyway when propagating up
        Status = STATUS_SUCCESS;
    }
    BDD_LOG_INFORMATION("XENWDDM!%s setting  mode (%d x %d) for %d\n", __FUNCTION__, width, height,
        pRecommendMonitorModes->VideoPresentTargetId);
    return Status;
}

#pragma warning(pop)
