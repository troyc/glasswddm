/******************************Module*Header*******************************\
* Module Name: bdd_util.cxx
*
* Basic Display Driver utility functions
*
* Created: 29-Mar-2011
* Author: Amos Eshel [amosesh]
*
* Copyright (c) 2011 Microsoft Corporation
\**************************************************************************/

#include "BDD.hxx"


#pragma code_seg("PAGE")

//
// EDID validation
//

BOOLEAN IsEdidHeaderValid(_In_reads_bytes_(EDID_V1_BLOCK_SIZE) const BYTE* pEdid)
{
    PAGED_CODE();

    static const UCHAR EDID1Header[8] = {0,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0};
    return memcmp(pEdid, EDID1Header, sizeof(EDID1Header)) == 0;
}

BOOLEAN IsEdidChecksumValid(_In_reads_bytes_(EDID_V1_BLOCK_SIZE) const BYTE* pEdid)
{
    PAGED_CODE();

    BYTE CheckSum = 0;
    for (const BYTE* pEdidStart = pEdid; pEdidStart < (pEdid + EDID_V1_BLOCK_SIZE); ++pEdidStart)
    {
        CheckSum += *pEdidStart;
    }

    return CheckSum == 0;
}
