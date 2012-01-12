
#include <pthread.h>
#include <math.h>
#include <js/jsapi.h>
#include <js/jscntxt.h>
#include <js/jslock.h>
#include "jsworkers.h"

// ************
// from js/src/shell/js.cpp
// ************

size_t gMaxStackSize = 500000;

static JSClass global_class = {
    "global", JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};


static JSBool nile_print(JSContext *cx, uintN argc, jsval *vp) {
    jsval *argv;
    uintN i;
    JSString *str;
    char *bytes;

	printf("%p ", pthread_self ());

    argv = JS_ARGV(cx, vp);
    for (i = 0; i < argc; i++) {
        str = JS_ValueToString(cx, argv[i]);
        if (!str)
            return JS_FALSE;
        bytes = JS_EncodeString(cx, str);
        if (!bytes)
            return JS_FALSE;
        printf("%s%s", i ? " " : "", bytes);
        JS_free(cx, bytes);
    }
    printf("\n");
    JS_SET_RVAL(cx, vp, JSVAL_VOID);
    return JS_TRUE;
}

static JSFunctionSpec nile_global_functions[] = {
    JS_FN("print", nile_print, 0, 0),
    JS_FS_END
};


using namespace js;

typedef enum JSShellExitCode {
    EXITCODE_RUNTIME_ERROR      = 3,
    EXITCODE_FILE_NOT_FOUND     = 4,
    EXITCODE_OUT_OF_MEMORY      = 5,
    EXITCODE_TIMEOUT            = 6
} JSShellExitCode;

enum CompartmentKind { SAME_COMPARTMENT, NEW_COMPARTMENT };

int gExitCode = 0;
static volatile bool gCanceled = false;
JSBool gQuitting = JS_FALSE;

static PRLock *gWatchdogLock = NULL;
static PRCondVar *gWatchdogWakeup = NULL;
static PRThread *gWatchdogThread = NULL;
static bool gWatchdogHasTimeout = false;
static PRIntervalTime gWatchdogTimeout = 0;

static PRCondVar *gSleepWakeup = NULL;

JSObject *gWorkers = NULL;
js::workers::ThreadPool *gWorkerThreadPool = NULL;

static bool
IsBefore(PRIntervalTime t1, PRIntervalTime t2)
{
    return int32_t(t1 - t2) < 0;
}

static bool
InitWatchdog(JSRuntime *rt)
{
    JS_ASSERT(!gWatchdogThread);
    gWatchdogLock = PR_NewLock();
    if (gWatchdogLock) {
        gWatchdogWakeup = PR_NewCondVar(gWatchdogLock);
        if (gWatchdogWakeup) {
            gSleepWakeup = PR_NewCondVar(gWatchdogLock);
            if (gSleepWakeup)
                return true;
            PR_DestroyCondVar(gWatchdogWakeup);
        }
        PR_DestroyLock(gWatchdogLock);
    }
    return false;
}

static void
KillWatchdog()
{
    PRThread *thread;

    PR_Lock(gWatchdogLock);
    thread = gWatchdogThread;
    if (thread) {
        /*
         * The watchdog thread is running, tell it to terminate waking it up
         * if necessary.
         */
        gWatchdogThread = NULL;
        PR_NotifyCondVar(gWatchdogWakeup);
    }
    PR_Unlock(gWatchdogLock);
    if (thread)
        PR_JoinThread(thread);
    PR_DestroyCondVar(gSleepWakeup);
    PR_DestroyCondVar(gWatchdogWakeup);
    PR_DestroyLock(gWatchdogLock);
}

static void
CancelExecution(JSRuntime *rt)
{
    gCanceled = true;
    if (gExitCode == 0)
        gExitCode = EXITCODE_TIMEOUT;
#ifdef JS_THREADSAFE
    if (gWorkerThreadPool)
        js::workers::terminateAll(gWorkerThreadPool);
#endif
    JS_TriggerAllOperationCallbacks(rt);

    static const char msg[] = "Script runs for too long, terminating.\n";
#if defined(XP_UNIX) && !defined(JS_THREADSAFE)
    /* It is not safe to call fputs from signals. */
    /* Dummy assignment avoids GCC warning on "attribute warn_unused_result" */
    ssize_t dummy = write(2, msg, sizeof(msg) - 1);
    (void)dummy;
#else
    fputs(msg, stderr);
#endif
}

static void
WatchdogMain(void *arg)
{
    JSRuntime *rt = (JSRuntime *) arg;

    PR_Lock(gWatchdogLock);
    while (gWatchdogThread) {
        PRIntervalTime now = PR_IntervalNow();
         if (gWatchdogHasTimeout && !IsBefore(now, gWatchdogTimeout)) {
            /*
             * The timeout has just expired. Trigger the operation callback
             * outside the lock.
             */
            gWatchdogHasTimeout = false;
            PR_Unlock(gWatchdogLock);
            CancelExecution(rt);
            PR_Lock(gWatchdogLock);

            /* Wake up any threads doing sleep. */
            PR_NotifyAllCondVar(gSleepWakeup);
        } else {
            PRIntervalTime sleepDuration = gWatchdogHasTimeout
                                           ? gWatchdogTimeout - now
                                           : PR_INTERVAL_NO_TIMEOUT;
            DebugOnly<PRStatus> status =
                PR_WaitCondVar(gWatchdogWakeup, sleepDuration);
            JS_ASSERT(status == PR_SUCCESS);
        }
    }
    PR_Unlock(gWatchdogLock);
}

static bool
ScheduleWatchdog(JSRuntime *rt, jsdouble t)
{
    if (t <= 0) {
        PR_Lock(gWatchdogLock);
        gWatchdogHasTimeout = false;
        PR_Unlock(gWatchdogLock);
        return true;
    }

    PRIntervalTime interval = PRIntervalTime(ceil(t * PR_TicksPerSecond()));
    PRIntervalTime timeout = PR_IntervalNow() + interval;
    PR_Lock(gWatchdogLock);
    if (!gWatchdogThread) {
        JS_ASSERT(!gWatchdogHasTimeout);
        gWatchdogThread = PR_CreateThread(PR_USER_THREAD,
                                          WatchdogMain,
                                          rt,
                                          PR_PRIORITY_NORMAL,
                                          PR_LOCAL_THREAD,
                                          PR_JOINABLE_THREAD,
                                          0);
        if (!gWatchdogThread) {
            PR_Unlock(gWatchdogLock);
            return false;
        }
    } else if (!gWatchdogHasTimeout || IsBefore(timeout, gWatchdogTimeout)) {
         PR_NotifyCondVar(gWatchdogWakeup);
    }
    gWatchdogHasTimeout = true;
    gWatchdogTimeout = timeout;
    PR_Unlock(gWatchdogLock);
    return true;
}


static JSObject *
NewGlobalObject(JSContext *cx, CompartmentKind compartment)
{
    RootedVarObject glob(cx);

    glob = (compartment == NEW_COMPARTMENT)
           ? JS_NewCompartmentAndGlobalObject(cx, &global_class, NULL)
           : JS_NewGlobalObject(cx, &global_class);
    if (!glob)
        return NULL;

    {
        JSAutoEnterCompartment ac;
        if (!ac.enter(cx, glob))
            return NULL;

#if 1
//ifdef LAZY_STANDARD_CLASSES
        if (!JS_InitStandardClasses(cx, glob))
            return NULL;
#endif

#if 0
//ifdef JS_HAS_CTYPES
        if (!JS_InitCTypesClass(cx, glob))
            return NULL;
#endif
        if (!JS_InitReflect(cx, glob))
            return NULL;
        if (!JS_DefineFunctions(cx, glob, nile_global_functions)) {
            return NULL;
        }

    }

    if (compartment == NEW_COMPARTMENT && !JS_WrapObject(cx, glob.address()))
        return NULL;

    return glob;
}

// ************
// end from js.cpp
// ************

static JSClass Result_class = {
    "Result",
    JSCLASS_HAS_PRIVATE,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

JSBool Result_construct(JSContext *cx, uintN argc, jsval *vp) {
    jsval constructor = JS_THIS(cx, vp);

    JSObject *self = JS_NewObject(cx, &Result_class, NULL, JSVAL_TO_OBJECT(constructor));

    jsval rval;
    int ok = JS_EvaluateScript(cx, self, "this.get = function get() { throw new Error('get before execute'); };", 69, "main", 0, &rval);

    JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(self));
    return JS_TRUE;
}

void Parallel_finalize(JSContext *cx, JSObject *obj) {

}

static JSClass Parallel_class = {
    "Parallel",
    JSCLASS_HAS_PRIVATE,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, Parallel_finalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

JSBool Parallel_construct(JSContext *cx, uintN argc, jsval *vp) {
    jsval constructor = JS_THIS(cx, vp);

    JSObject *self = JS_NewObject(cx, &Parallel_class, NULL, JSVAL_TO_OBJECT(constructor));

    jsval rval;
    JS_EvaluateScript(cx, self, "(function(self) { var q = [], p = 0; self.fork = function fork(func) { var a = [], r = new Result(); for (var i = 0, l = arguments.length; i < l; i++) { a[i] = arguments[i]; } q.push([func, a, r]); p++; return r; }; self.execute = function execute(cb) { var x = q; q = []; self.cb = cb; for (var i in x) { var f = x[i][0], a = x[i][1], r = x[i][2]; r.r = f.apply(null, a); p--; if (!p) { self.cb() } r.get = function get() { if (p) { throw new Error('get before execute');  } return this.r; }; } }; })(this)", 507, "main", 0, &rval);

    JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(self));
    return JS_TRUE;
}


void report_error(JSContext *cx, const char *message, JSErrorReport *report)
{
    fprintf(stderr, "%s:%u:%s\n",
        report->filename ? report->filename : "<no filename>",
        (unsigned int) report->lineno,
        message);
}

JSContext * make_context(JSRuntime *rt) {
    JSContext *cx = JS_NewContext(rt, 8192);
    if (cx == NULL)
        return NULL;

    JS_SetOptions(cx, JSOPTION_VAROBJFIX | JSOPTION_METHODJIT);
    JS_SetVersion(cx, JSVERSION_LATEST);
    JS_SetErrorReporter(cx, report_error);

    JSObject * global = JS_NewCompartmentAndGlobalObject(cx, &global_class, NULL);
    if (global == NULL)
        return NULL;

    if (!JS_InitStandardClasses(cx, global))
        return NULL;

    if (!JS_DefineFunctions(cx, global, nile_global_functions))
        return NULL;

    JSObject *result = JS_InitClass(
        cx, global, NULL,
        &Result_class,
        Result_construct,
        0, // 0 args
        NULL, // no properties
        NULL, // no functions
        NULL, NULL);

    JSObject *par = JS_InitClass(
        cx, global, NULL,
        &Parallel_class,
        Parallel_construct,
        0, // 0 args
        NULL, // no properties
        NULL, // no functions
        NULL, NULL);

    JS_SetGlobalObject(cx, global);
    return cx;
}

int main(int argc, const char *argv[])
{
    int result;

    JSRuntime *rt = JS_NewRuntime(8 * 1024 * 1024);
    if (rt == NULL)
        return 1;

    JSContext *cx = make_context(rt);
    if (cx == NULL)
        return 1;

    RootedVarObject glob(cx);
    glob = NewGlobalObject(cx, NEW_COMPARTMENT);

    JSObject *global = JS_GetGlobalObject(cx);

    jsval rval;
    JSBool ok;

    JSScript *nile = JS_CompileUTF8File(cx, global, "spork.js");
    if (nile == NULL)
        return 1;

    class ShellWorkerHooks : public js::workers::WorkerHooks {
    public:
        JSObject *newGlobalObject(JSContext *cx) {
            return NewGlobalObject(cx, NEW_COMPARTMENT);
        }
    };
    ShellWorkerHooks hooks;
    if (!JS_AddNamedObjectRoot(cx, &gWorkers, "Workers") ||
        (gWorkerThreadPool = js::workers::init(cx, &hooks, glob, &gWorkers)) == NULL) {
        return 1;
    }

    ok = JS_ExecuteScript(cx, global, nile, &rval);
    if (!ok || JSVAL_IS_NULL(rval))
        return 1;

    js::workers::finish(cx, gWorkerThreadPool);
    JS_RemoveObjectRoot(cx, &gWorkers);
    if (result == 0)
        result = gExitCode;

    JS_DestroyContext(cx);
    JS_DestroyRuntime(rt);
    JS_ShutDown();
    return result;
}
