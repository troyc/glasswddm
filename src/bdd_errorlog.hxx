/******************************Module*Header*******************************\
* Module Name: BDD_ErrorLog.hxx
*
* Basic Display Driver Logging Macros
*
*
* Copyright (c) 2010 Microsoft Corporation
*
\**************************************************************************/
#ifndef _BDD_ERRORLOG_HXX_
#define _BDD_ERRORLOG_HXX_

#define BDD_LOG_ASSERTION(...) NT_ASSERT(FALSE)
#define BDD_ASSERT(exp) {if (!(exp)) {BDD_LOG_ASSERTION(#exp);}}

#ifndef DBG

#define BDD_LOG_ERROR(...)
#define BDD_LOG_WARNING(...)
#define BDD_LOG_EVENT(...)
#define BDD_LOG_INFORMATION(...)
#define BDD_LOG_LOW_RESOURCE(...)

#endif

#if DBG
#define BDD_ASSERT_CHK(exp) BDD_ASSERT(exp)

#define BDD_LOG_ERROR(...) \
    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__))
#define BDD_LOG_WARNING(...) \
    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_WARNING_LEVEL, __VA_ARGS__))
#define BDD_LOG_EVENT(...) \
    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, __VA_ARGS__))
#define BDD_LOG_INFORMATION(...) \
    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL, __VA_ARGS__))
#define BDD_LOG_LOW_RESOURCE(...) \
    KdPrintEx((DPFLTR_IHVVIDEO_ID, DPFLTR_INFO_LEVEL, __VA_ARGS__))

#else
#define BDD_ASSERT_CHK(exp) {}
#endif

class BDD_TRACE {
private:
    static int g_call_level;
    static const char * in_levels [];
    static const char * out_levels [];
public:
    BDD_TRACE(const char * function, BOOLEAN *bVerbose) :_function(function), _bVerbose(*bVerbose)
    {
        if(!*bVerbose) return;
        call_level = g_call_level++;
        if(call_level > 4) call_level = 4;
        BDD_LOG_ERROR("%s ENTERING XENWDDM:%s\n", in_levels[call_level], _function);
    }
    BDD_TRACE(const char * function, BOOLEAN *bVerbose, PVChild * pChild)
        :_function(function)
        , _bVerbose(*bVerbose)
    {
        UNREFERENCED_PARAMETER(pChild);
        if(!*bVerbose) return;
        call_level = g_call_level++;
        if(call_level > 4) call_level = 4;
        BDD_LOG_ERROR("%s ENTERING XENWDDM:%s: source %d:%d\n", in_levels[call_level],
            _function, pChild->target_id(), pChild->key());
    }
    BDD_TRACE(const char * function, BOOLEAN *bVerbose, UINT32 Source, UINT32 key, UINT32 targetID)
        :_function(function)
        , _bVerbose(*bVerbose)
    {
        UNREFERENCED_PARAMETER(targetID);
        UNREFERENCED_PARAMETER(key);
        UNREFERENCED_PARAMETER(Source);
        call_level = g_call_level++;
        if(call_level > 4) call_level = 4;
        BDD_LOG_EVENT("%s ENTERING XENWDDM:%s source %d:%d->%d\n", in_levels[call_level],
            _function, Source, key, targetID);
    }
    BDD_TRACE(const char * function, BOOLEAN *bVerbose, UINT32 key)
        :_function(function)
        , _bVerbose(*bVerbose)
    {
        UNREFERENCED_PARAMETER(key);
        if(!*bVerbose) return;
        call_level = g_call_level++;
        if(call_level > 4) call_level = 4;
        BDD_LOG_ERROR("%s ENTERING XENWDDM:%s key:%d\n", in_levels[call_level],
            function, key);
    }
    ~BDD_TRACE()
    {
        if(!_bVerbose) return;
        g_call_level--;
        BDD_LOG_ERROR("%s LEAVING XENWDDM:%s\n", out_levels[call_level], _function);
    }
    const char * _function;
    BOOLEAN _bVerbose;
    int call_level;
};

#define BDD_TRACER \
    static BOOLEAN bVerbose(TRUE); \
    BDD_TRACE tracer(__FUNCTION__, &bVerbose)

//use SOURCE macros only in bdd members
#define BDD_TRACE_SOURCE_QUIET(Source) \
    static BOOLEAN bVerbose(FALSE); \
    BDD_TRACE(__FUNCTION__, &bVerbose, Source, GetCurrentMode(Source)->pPVChild->key(), GetCurrentMode(Source)->TargetId)

#define BDD_TRACE_SOURCE(Source) \
    static BOOLEAN bVerbose(TRUE); \
    BDD_TRACE(__FUNCTION__, &bVerbose, Source, GetCurrentMode(Source)->pPVChild->key(), GetCurrentMode(Source)->TargetId)

#define BDD_TRACE_CHILD(pChild) \
    static BOOLEAN bVerbose(TRUE);  \
    BDD_TRACE tracer(__FUNCTION__, &bVerbose, pChild)

#define BDD_TRACE_CHILD_QUIET(pChild) \
    static BOOLEAN bVerbose(FALSE);  \
    BDD_TRACE tracer(__FUNCTION__, &bVerbose, pChild)

#define BDD_TRACE_KEY(key) \
    static BOOLEAN bVerbose(TRUE);  \
    BDD_TRACE tracer(__FUNCTION__, &bVerbose, key)

#define BDD_TRACE_KEY_QUIET(key) \
    static BOOLEAN bVerbose(FALSE);  \
    BDD_TRACE tracer(__FUNCTION__, &bVerbose, key)


#endif  //_BDD_ERRORLOG_HXX_

