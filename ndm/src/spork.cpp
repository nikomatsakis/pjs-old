/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sw=4 et tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is the Spork project.
 *
 * The Initial Developer of the Original Code is
 * Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2010
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Nicholas Matsakis <nmatsakis@mozilla.com>
 *   Donovan Preston <dpreston@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either of the GNU General Public License Version 2 or later (the "GPL"),
 * or the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include <string.h>
#include <pthread.h>
#include <nspr/prthread.h>
#include <nspr/prlock.h>
#include <nspr/prcvar.h>
#include <js/jsapi.h>
#include <js/jslock.h>

extern size_t gMaxStackSize;

using namespace js;

namespace spork {

/*************************************************************
 * Architecture
 *
 * There are a (currently) fixed number of worker threads, all managed
 * as part of a thread pool.  Each thread contains a Runtime and
 * Context which are reused.  There is a central ThreadSafeQueue which
 * is shared by all threads.  The queue stores "events", which are C++
 * objects not represented in JavaScript. All threads basically just
 * pull events off the queue.
 *
 * The kinds of events are:
 * - StartFromString: creates a new task executing the given string
 *
 * Example usage:
 *
 *   let taskHandle = fork(function() {
 *       return ...;
 *   });
 *   let tN = forkn(N, function(idx) {
 *       return ... idx ...;
 *   });
 *   oncompletion(function() {
 *       t.get();
 *   });
 *
 * The object Par is a pre-loaded global.  Calling Par.fork() creates
 * a subtask of the current task.  Calling Par.forkN() creates an
 * N-ary subtask (not yet implemented).  The result of both calls is a
 * Task object.  Calling Par.oncompletion() sets the handler; when the
 * turn ends, the forked children will execute and the current task
 * will be suspended until they complete, at which point the
 * oncompletion() thunk will execute.
 *
 * The `ctx` object is a TaskContext object.  It contains data
 * specific to that current task, such as its index (if applicable).
 * It is most commonly used to set the result of the task using
 * `set()`.  If `set()` is not invoked, then the task's result is
 * undefined.  A task is considered complete when there are no
 * registered callbacks.
 *
 * Both Tasks and Contexts have a C++ and a JavaScript component.
 *
 * Execution Model
 * ---------------
 *
 * The thread pool features a fixed number of threads called engine
 * runners (to distinguish them from web workers).  Each runner has
 * associated with it a runtime, context, and a set of executing
 * tasks. Once associated with a particular runner, a task never
 * moves: this is because the task has only a compartment, which is
 * bound to a particular runtime and context.
 *
 * For now, there is a central queue and each runner has an associated
 * queue as well.  The main loop of a runner first checks its local
 * queue for work: if none is found, then it blocks loading from the
 * main queue.  The runner may be reawoken if one of its associated
 * tasks is ready to execute; more on that later.
 * 
 * Most tasks are represented using two halves: the TaskHandle and the
 * TaskContext.  Both have JS counterparts.  The TaskHandle is the
 * result of a fork operation and represents the task "from the
 * outside".  The TaskContext is created when the task is about to
 * execute, and represents the task "from the inside."  It is where
 * the task compartment is stored along with other information.  The
 * root task is somewhat special: it has no TaskHandle, only a
 * TaskContext (there is no "outside" to the root task).
 *
 * To manage the parallel forking and joining, the TaskContext defines
 * a few central fields: a list of pending TaskHandles, a counter, and
 * a "oncompletion" callback (initially NULL when every turn begins).
 * A fork() call causes a new TaskHandle to be created and added to
 * the list of pending handles.  A call to "oncompletion()" will set
 * the oncompletion callback.  When the turn ends, the oncompletion
 * callback is checked: if it is non-NULL, then the counter is
 * incremented by 1 for each pending TaskHandle and the pending
 * TaskHandles are pushed onto the central queue.  The runner then
 * returns to the idle loop.  We will cover the case where the
 * oncallback handler is NULL below.
 *
 * At this point, whichever runners are idle will draw the child tasks
 * from the main queue.  They will then create an associated
 * TaskContext (which has a pointer to the TaskHandle, though this
 * pointer is not available to JS code) and begin executing the task
 * function (for now, this is passed in a string, but it will
 * eventually be a proxied closure).  At this point, we are back
 * at the beginning: these child task contexts may themselves create
 * new children, repeating the process described in the previous
 * paragraph.  But let us assume that they do not create new children,
 * or at least do not invoke oncallback().  In that case, at the end
 * of the child task's turn, the oncallback handler will be NULL.
 *
 * If the oncallback handler is NULL, the child task is assumed to be
 * completed (in the future we will consider other pending callbacks
 * as well, such as async I/O).  In that case, the counter on the
 * parent task context is atomically decremented. If the counter
 * reaches 0, then the parent task context is ready to be reawoken:
 * the parent task is thus pushed onto the runner's local queue and
 * the runner's lock is pulsed to reawaken if it is sleeping.
 *
 * Garbage collection
 * ------------------
 * 
 * The GC strategy is as follows: each active TaskHandle and
 * TaskContext is "self-rooted", which means that the C++ object adds
 * the corresponding JS object to the root set.  This is true up until
 * the TaskContext completes, at which point both C++ objects remove
 * themselves from the root set.  Either may still be live.
 *
 * Future improvements
 * -------------------
 * 
 * I would like to move to a work stealing model where each runner has
 * only a local deque and there is no shared queue.  This would also
 * facilitate the efficient implementation of forkN() primitive.  One
 * important piece of this is figuring out when to go idle, this will
 * be critical for real deployment.  I can't recall the right protocol
 * for this and don't want to implement it at the moment anyhow.
 *
 * Here is a proposal for the protocol:
 * - You have a shared 64bit counter on the thread pool, guarded by a lock (for
 *   now).  Counter initially 2.  If it is 0, that means that
 *   runners are idle and blocked.  If it is 1, that means to shutdown.
 * - Each runner executes in a loop:
 *   - Check for contexts to reawaken
 *   - Check for local work
 *   - Read value of counter (no lock needed on x86)
 *   - If counter is 1, end.
 *   - If counter is not 0, search for work to steal
 *   - If unsuccessful, acquire global and read value of counter:
 *     - if 0, become idle (no work)
 *     - if 1, exit
 *     - if still the same as before, set to 0 and become idle
 *
 * After producing new work or reawakening someone:
 *   - Acquire global lock and check value of counter
 *     - if MAX_ULONG-1, pulse the monitor and set counter to 0
 *     - else, increment counter, jump up to 2 if we roll over
 *
 * This has one potential (but I think exceedingly unlikely) failure
 * mode: if I read the counter, scan for work, and then sleep for a
 * long time, it's possible that while I am sleeping the counter is
 * incremented 2^64 times and it rolls over back to the same value I
 * read.  Then I will go to sleep.  If no more tasks are ever
 * produced, I might never wake up: but note that other runners (e.g.,
 * the one that produced the 2^64 work items) are still active, so all
 * that happens is we get less parallelism than we otherwise would.
 ************************************************************/

class ThreadPool;
class TaskHandle;
class TaskContext;
class Runner;

template<typename T> T* check_null(T* ptr) {
    if (ptr == NULL)
        throw "Fiddlesticks!";
    return ptr;
}

typedef Vector<TaskHandle*, 4, SystemAllocPolicy> TaskHandleVec;
typedef Vector<TaskContext*, 4, SystemAllocPolicy> TaskContextVec;

template<class T>
void delete_assoc(JSContext *cx, JSObject *obj) {
    T* t = (T*) JS_GetPrivate(cx, obj);
    delete t;
}

class CrossCompartment
{
private:
    JSCrossCompartmentCall *cc;

public:
    CrossCompartment(JSContext *cx, JSObject *obj) {
        cc = JS_EnterCrossCompartmentCall(cx, obj);
    }

    ~CrossCompartment() {
        JS_LeaveCrossCompartmentCall(cc);
    }
};

// ____________________________________________________________
// TaskSpec

class ClonedObj {
private:
    uint64_t *_data;
    size_t _nbytes;
    ClonedObj(uint64_t *data, size_t nbytes)
        : _data(data)
        , _nbytes(nbytes)
    {}
public:
    ~ClonedObj();
    static JSBool pack(JSContext *cx, jsval val, ClonedObj **rval);
    JSBool unpack(JSContext *cx, jsval *rval);
};

class TaskSpec {
private:
    ClonedObj **_args;
    char *_fntext;
    
    TaskSpec(ClonedObj **args, char *fntext)
        : _args(args)
        , _fntext(fntext)
    {}
public:
    static JSBool create(JSContext *cx, jsval fnval,
                         jsval *argvals, int argcnt);
    ~TaskSpec();
};

// ____________________________________________________________
// Generation interface

class Generation {
private:
    enum { ReadySlot, MaxSlot };

    Generation(); // cannot be constructed

    static JSBool asGenObj(JSContext *cx, jsval genobj, JSObject **obj);

public:
    static JSBool create(JSContext *cx, jsval *rval);

    static JSBool getReady(JSContext *cx, jsval genobj, JSBool *rval);
    static JSBool setReady(JSContext *cx, jsval genobj, JSBool rval);

    static JSClass jsClass;
};

// ____________________________________________________________
// Closure interface
// 
// class Closure
// {
// private:
//     char *_toExec;
//     jsval *_args;
//     int _argsCount;
// 
//     Closure(char *toExec, jsval *args, int argsCount)
//         : _toExec(toExec)
//         , _args(args)
//         , _argsCount(argsCount)
//     {}
// 
// public:
//     Closure *create(JSContext *cx, JSString *toExec,
//                     jsval *args, int argscnt);
// 
//     JSBool execute(JSContext *cx, jsval *rval);
// };

// ____________________________________________________________
// TaskHandle interface

class TaskHandle
{
private:
    TaskHandle(const TaskHandle &) MOZ_DELETE;
    TaskHandle & operator=(const TaskHandle &) MOZ_DELETE;

protected:
    TaskHandle()
    {}

public:
    virtual ~TaskHandle() {}

    virtual JSBool execute(JSContext *cx, JSObject *taskctx,
                           JSObject *global, jsval *rval) = 0;
    virtual void onCompleted(Runner *runner, jsval result) = 0;
};

class RootTaskHandle : public TaskHandle
{
    const char *scriptfn;

public:
    RootTaskHandle(const char *afn)
        : scriptfn(afn)
    {}

    virtual JSBool execute(JSContext *cx, JSObject *taskctx,
                           JSObject *global, jsval *rval);
    virtual void onCompleted(Runner *runner, jsval result);
};

class ChildTaskHandle : public TaskHandle
{
private:
    enum Reserved { ResultSlot, GenSlot, MaxSlot };

    static void jsFinalize(JSContext *cx, JSObject *obj) {
        delete_assoc<TaskHandle>(cx, obj);
    }

    TaskContext *_parent;
    JSObject *_object;

    char *_funcStr;

    ClonedObj *_result;

    JSBool addRoot(JSContext *cx);
    JSBool delRoot(JSContext *cx);

    explicit ChildTaskHandle(JSContext *cx, TaskContext *parent, jsval gen,
                             JSObject *object, char *toExec)
        : _parent(parent)
        , _object(object)
        , _funcStr(toExec)
        , _result(NULL)
    {
        JS_SetPrivate(cx, _object, this);
        JS_SetReservedSlot(cx, _object, ResultSlot, JSVAL_NULL);
        JS_SetReservedSlot(cx, _object, GenSlot, gen);
    }

    void clearResult();


    static JSClass jsClass;
    static JSFunctionSpec jsMethods[2];
    static JSBool jsGet(JSContext *cx, uintN argc, jsval *vp);

protected:
    virtual ~ChildTaskHandle() {
        clearResult();
        delete[] _funcStr;
    }

public:
    JSBool GetResult(JSContext *cx, jsval *rval);
    virtual JSBool execute(JSContext *cx, JSObject *taskctx,
                           JSObject *global, jsval *rval);
    virtual void onCompleted(Runner *runner, jsval result);

    JSObject *object() { return _object; }

    static ChildTaskHandle *create(JSContext *cx,
                                   TaskContext *parent,
                                   char *toExec);

    static JSBool initClass(JSContext *cx, JSObject *global);
};

// ______________________________________________________________________
// TaskContext interface

class TaskContext
{
public:
    enum TaskContextSlots { OnCompletionSlot, GenSlot, MaxSlot };

private:
    TaskHandle *_taskHandle;
    JSObject *_global;
    JSObject *_object;
    jsrefcount _outstandingChildren;
    TaskHandleVec _toFork;
    Runner *_runner;
    
    TaskContext(JSContext *cx, TaskHandle *aTask,
                Runner *aRunner, JSObject *aGlobal,
                JSObject *object)
      : _taskHandle(aTask)
      , _global(aGlobal)
      , _object(object)
      , _outstandingChildren(0)
      , _runner(aRunner)
    {
        setOncompletion(cx, JSVAL_NULL);
        JS_SetPrivate(cx, _object, this);
    }

    JSBool addRoot(JSContext *cx);
    JSBool delRoot(JSContext *cx);
    JSBool newGeneration(JSContext *cx, bool *initialGeneration);

public:
    static TaskContext *create(JSContext *cx,
                               TaskHandle *aTask,
                               Runner *aRunner,
                               JSObject *aGlobal);

    void addTaskToFork(TaskHandle *th);

    void onChildCompleted();

    void resume(Runner *runner);

    JSBool getGeneration(JSContext *cx, jsval *rval) {
        return JS_GetReservedSlot(cx, _object, GenSlot, rval);
    }

    void setOncompletion(JSContext *cx, jsval val) {
        JS_SetReservedSlot(cx, _object, OnCompletionSlot, val);
    }

    static JSClass jsClass;
};

// ____________________________________________________________
// Global interface

class Global
{
public:
    static JSClass jsClass;
};

// ____________________________________________________________
// Runner interface

class Runner
{
private:
    ThreadPool *_threadPool;
    int _index;
    TaskContextVec _toReawaken;
    JSLock *_runnerLock;
    PRCondVar *_runnerCondVar;
    JSRuntime *_rt;
    JSContext *_cx;
    
    bool getWork(TaskContext **reawaken, TaskHandle **create);

    Runner(ThreadPool *aThreadPool, int anIndex,
           JSRuntime *aRt, JSContext *aCx)
      : _threadPool(aThreadPool)
      , _index(anIndex)
      , _runnerLock(JS_NEW_LOCK())
      , _runnerCondVar(JS_NEW_CONDVAR(_runnerLock))
      , _rt(aRt)
      , _cx(aCx)
    {
    }

public:

    static Runner *create(ThreadPool *aThreadPool, int anIndex);

    ~Runner() {
        if (_runnerCondVar)
            JS_DESTROY_CONDVAR(_runnerCondVar);
        if (_runnerLock)
            JS_DESTROY_LOCK(_runnerLock);
    }
    
    JSRuntime *rt() { return _rt; }
    JSContext *cx() { return _cx; }

    void start();
    void reawaken(TaskContext *ctx);
    void enqueueTasks(TaskHandle **begin, TaskHandle **end);
    TaskContext *createTaskContext(TaskHandle *handle);
    void terminate();
};

class ThreadPool
{
private:
    int32_t _terminating;
    JSLock *_masterLock;
    PRCondVar *_masterCondVar;
    int _threadCount;
    PRThread **_threads;
    Runner **_runners;
    Vector<TaskHandle*, 4, SystemAllocPolicy> _toCreate;

    static void start(void* arg) {
        ((Runner*) arg)->start();
    }

    explicit ThreadPool(JSLock *aLock, PRCondVar *aCondVar,
                        int threadCount, PRThread **threads, Runner **runners)
      : _terminating(0)
      , _masterLock(aLock)
      , _masterCondVar(aCondVar)
      , _threadCount(threadCount)
      , _threads(threads)
      , _runners(runners)
    {
    }

public:
    ~ThreadPool() {
        PR_DestroyLock(_masterLock);
        PR_DestroyCondVar(_masterCondVar);
        delete[] _threads;
        for (int i = 0; i < _threadCount; i++)
            delete _runners[i];
        delete[] _runners;
    }

    JSLock *masterLock() { return _masterLock; }
    PRCondVar *masterCondVar() { return _masterCondVar; }
    TaskHandleVec *toCreate() { return &_toCreate; }

    void start(RootTaskHandle *rth);

    static ThreadPool *create();
    void terminate();
    void shutdown();
    int terminating() { return _terminating; }
};

// ______________________________________________________________________
// Global functions

/* The error reporter callback. */
void reportError(JSContext *cx, const char *message, JSErrorReport *report)
{
    fprintf(stderr, "%s:%u:%s\n",
            report->filename ? report->filename : "<no filename>",
            (unsigned int) report->lineno,
            message);
}

JSBool print(JSContext *cx, uintN argc, jsval *vp) {
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

JSBool fork(JSContext *cx, uintN argc, jsval *vp) {
    TaskContext *taskContext = (TaskContext*) JS_GetContextPrivate(cx);

    JSString *str;
    if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "S", &str))
        return JS_FALSE;

    int length = JS_GetStringEncodingLength(cx, str);
    char *encoded = check_null(new char[length+3]);
    JS_EncodeStringToBuffer(str, encoded+1, length);
    encoded[0] = '(';
    encoded[length+1] = ')';
    encoded[length+2] = 0;

    ChildTaskHandle *th = ChildTaskHandle::create(cx, taskContext, encoded);
    JS_SET_RVAL(cx, vp, OBJECT_TO_JSVAL(th->object()));

    taskContext->addTaskToFork(th);
    return JS_TRUE;
}

JSBool oncompletion(JSContext *cx, uintN argc, jsval *vp) {
    TaskContext *taskContext = (TaskContext*) JS_GetContextPrivate(cx);
    JSObject *func;
    if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "o", &func))
        return JS_FALSE;
    if (!JS_ObjectIsFunction(cx, func)) {
        JS_ReportError(cx, "expected function as argument");
        return JS_FALSE;
    }
    taskContext->setOncompletion(cx, OBJECT_TO_JSVAL(func));
    return JS_TRUE;
}

static JSFunctionSpec sporkGlobalFunctions[] = {
    JS_FN("print", print, 0, 0),
    JS_FN("fork", fork, 1, 0),
    JS_FN("oncompletion", oncompletion, 1, 0),
    JS_FS_END
};

// ______________________________________________________________________
// Global impl

JSClass Global::jsClass = {
    "Global", JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

// ______________________________________________________________________
// Generation impl

JSBool Generation::create(JSContext *cx, jsval *rval) {
    JSObject *object = JS_NewObject(cx, &jsClass, NULL, NULL);
    if (!object)
        return NULL;

    *rval = OBJECT_TO_JSVAL(object);
    return setReady(cx, *rval, false);
}

JSBool Generation::asGenObj(JSContext *cx, jsval gen, JSObject **genobj) {
    if (!JSVAL_IS_OBJECT(gen)) {
        JS_ReportError(cx, "invalid generation object");
        return false; // should not happen
    }
    *genobj = JSVAL_TO_OBJECT(gen);
    if (JS_GET_CLASS(cx, *genobj) != &jsClass) {
        JS_ReportError(cx, "invalid generation object");
        return false; // should not happen
    }
    return true;
}

JSBool Generation::getReady(JSContext *cx, jsval gen, JSBool *rval) {
    JSObject *genobj;
    if (!asGenObj(cx, gen, &genobj))
        return false;
    jsval ready;
    if (!JS_GetReservedSlot(cx, genobj, ReadySlot, &ready))
        return false;
    *rval = JSVAL_TO_BOOLEAN(ready);
}

JSBool Generation::setReady(JSContext *cx, jsval gen, JSBool rval) {
    JSObject *genobj;
    if (!asGenObj(cx, gen, &genobj))
        return false;
    return JS_SetReservedSlot(cx, genobj, ReadySlot, BOOLEAN_TO_JSVAL(rval));
}

JSClass Generation::jsClass = {
    "Generation", JSCLASS_HAS_RESERVED_SLOTS(MaxSlot),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

// ______________________________________________________________________
// 

ClonedObj::~ClonedObj() {
    js::Foreground::free_(_data);
}

JSBool ClonedObj::pack(JSContext *cx, jsval val, ClonedObj **rval) {
    uint64_t *data;
    size_t nbytes;
    if (!JS_WriteStructuredClone(cx, val, &data, &nbytes, NULL, NULL)) {
        *rval = NULL;
        return false;
    }
    *rval = new ClonedObj(data, nbytes);
    return true;
}

JSBool ClonedObj::unpack(JSContext *cx, jsval *rval) {
    return JS_ReadStructuredClone(cx, _data, _nbytes, 
                                  JS_STRUCTURED_CLONE_VERSION, rval,
                                  NULL, NULL);
}

// ______________________________________________________________________
// TaskHandle impl

void RootTaskHandle::onCompleted(Runner *runner, jsval result) {
    runner->terminate();
}

JSBool RootTaskHandle::execute(JSContext *cx, JSObject *taskctx,
                               JSObject *global, jsval *rval) {
    JSScript *scr = JS_CompileUTF8File(cx, global, scriptfn);
    if (scr == NULL)
        return 0;

    return JS_ExecuteScript(cx, global, scr, rval);
}

void ChildTaskHandle::onCompleted(Runner *runner, jsval result) {
    ClonedObj::pack(runner->cx(), result, &_result);
    _parent->onChildCompleted();
    delRoot(runner->cx());
}

JSBool ChildTaskHandle::execute(JSContext *cx, JSObject *taskctx,
                                JSObject *global, jsval *rval) {
    jsval fnval;
    if (!JS_EvaluateScript(cx, global, _funcStr, strlen(_funcStr),
                           "fork", 1, &fnval))
        return  0;

    return JS_CallFunctionValue(cx, global, fnval, 0, NULL, rval);
}

ChildTaskHandle *ChildTaskHandle::create(JSContext *cx,
                                         TaskContext *parent,
                                         char *toExec) {
    jsval gen;
    if (!parent->getGeneration(cx, &gen))
        return NULL;
    
    // To start, create the JS object representative:
    JSObject *object = JS_NewObject(cx, &jsClass, NULL, NULL);
    if (!object)
        return NULL;

    // Create C++ object, which will be linked via Private:
    ChildTaskHandle *th = new ChildTaskHandle(cx, parent, gen, object, toExec);
    th->addRoot(cx);
    return th;
}

void ChildTaskHandle::clearResult() {
    if (_result) {
        delete _result;
        _result = 0;
    }
}

JSBool ChildTaskHandle::GetResult(JSContext *cx,
                                  jsval *rval)
{
    // Check if all tasks within generation have completely executed:
    jsval gen;
    if (!JS_GetReservedSlot(cx, _object, GenSlot, &gen))
        return false;
    JSBool ready;
    if (!Generation::getReady(cx, gen, &ready))
        return false;
    if (!ready) {
        JS_ReportError(cx, "all child tasks not yet completed");
        return false;
    }

    // Declone the result, if that is not already done:
    if (_result != NULL) {
        jsval decloned;
        if (!_result->unpack(cx, &decloned))
            return false;
        clearResult();
        JS_SetReservedSlot(cx, _object, ResultSlot, decloned);
    }

    // Load the result:
    return JS_GetReservedSlot(cx, _object, ResultSlot, rval);
}

JSBool ChildTaskHandle::addRoot(JSContext *cx) {
    return JS_AddNamedObjectRoot(cx, &_object, "ChildTaskHandle::addRoot()");
}

JSBool ChildTaskHandle::delRoot(JSContext *cx) {
    return JS_RemoveObjectRoot(cx, &_object);
}

JSBool ChildTaskHandle::initClass(JSContext *cx, JSObject *global) {
    return !!JS_InitClass(cx, 
                          global, NULL, // obj, parent_proto
                          &jsClass,     // clasp
                          NULL, 0,      // constructor, nargs
                          NULL,         // JSPropertySpec *ps
                          jsMethods,    // JSFunctionSpec *fs
                          NULL,         // JSPropertySpec *static_ps
                          NULL);        // JSFunctionSpec *static_fs
}

JSClass ChildTaskHandle::jsClass = {
    "TaskHandle", JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(MaxSlot),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, jsFinalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

JSFunctionSpec ChildTaskHandle::jsMethods[2] = {
    { "get", &jsGet, 0, 0 },
    JS_FS_END
};

JSBool ChildTaskHandle::jsGet(JSContext *cx, uintN argc, jsval *vp) {
    TaskContext *taskContext = static_cast<TaskContext*>(
        JS_GetContextPrivate(cx));
    JSObject *self = JS_THIS_OBJECT(cx, vp);
    if (!JS_InstanceOf(cx, self, &jsClass, NULL)) {
        JS_ReportError(cx, "expected TaskHandle as this");
        return JS_FALSE;
    }
    ChildTaskHandle *task = static_cast<ChildTaskHandle*>(
        JS_GetPrivate(cx, self));
    jsval rval;
    if (!task->GetResult(cx, &rval))
        return JS_FALSE;
    JS_SET_RVAL(cx, vp, rval);
    return JS_TRUE;
}

// ______________________________________________________________________
// TaskContext impl

TaskContext *TaskContext::create(JSContext *cx,
                                 TaskHandle *aTask,
                                 Runner *aRunner,
                                 JSObject *aGlobal) {
    // To start, create the JS object representative:
    JSObject *object = JS_NewObject(cx, &jsClass, NULL, NULL);
    if (!object) {
        return NULL;
    }

    // Create C++ object, which will be linked via Private:
    TaskContext *tc = new TaskContext(cx, aTask, aRunner, aGlobal, object);
    if (!tc->addRoot(cx)) {
        delete tc;
    }
    return tc;
}

JSBool TaskContext::addRoot(JSContext *cx) {
    if (!JS_AddNamedObjectRoot(cx, &_object, "TaskContext::_object"))
        return false;
    if (!JS_AddNamedObjectRoot(cx, &_global, "TaskContext::_global")) {
        JS_RemoveObjectRoot(cx, &_object);
        return false;
    }
    return true;
}

JSBool TaskContext::delRoot(JSContext *cx) {
    JS_RemoveObjectRoot(cx, &_object);
    JS_RemoveObjectRoot(cx, &_global);
}

void TaskContext::addTaskToFork(TaskHandle *th) {
    _toFork.append(th);
}

void TaskContext::onChildCompleted() {
    jsrefcount v = JS_ATOMIC_DECREMENT(&_outstandingChildren);
    if (v == 0) {
        _runner->reawaken(this);
    }
}

JSBool TaskContext::newGeneration(JSContext *cx, bool *initialGeneration) {
    jsval gen;

    // Set the current generation (if any) to be ready:
    if (!JS_GetReservedSlot(cx, _object, GenSlot, &gen))
        return false;
    if (!JSVAL_IS_VOID(gen)) {
        Generation::setReady(cx, gen, true);
        *initialGeneration = false;
    } else {
        *initialGeneration = true;
    }

    // Create a fresh (non-ready) generation:
    if (!Generation::create(cx, &gen))
        return false;
    return JS_SetReservedSlot(cx, _object, GenSlot, gen);
}

void TaskContext::resume(Runner *runner) {
    JSContext *cx = runner->cx();
    CrossCompartment cc(cx, _global);
    jsval rval;

    // If we break from this loop, this task context has completed,
    // either in error or successfully:
    while (true) {
        rval = JSVAL_NULL;

        bool initialGeneration;
        if (!newGeneration(cx, &initialGeneration))
            break;

        JS_SetContextPrivate(cx, this);
        if (initialGeneration) {
            if (!_taskHandle->execute(cx, _object, _global, &rval))
                break;
        } else {
            jsval fn;
            if (JS_GetReservedSlot(cx, _object, OnCompletionSlot, &fn)) {
                setOncompletion(cx, JSVAL_NULL);
                if (!JS_CallFunctionValue(cx, _global, fn, 0, NULL, &rval))
                    break;
            } else {
                break;
            }
        }
        JS_SetContextPrivate(cx, NULL);

        // If the _oncompletion handler is set, fork off any tasks
        // and block until they complete:
        jsval fn;
        if (JS_GetReservedSlot(cx, _object, OnCompletionSlot, &fn) &&
            !JSVAL_IS_NULL(fn)) {
            if (!_toFork.empty()) {
                JS_ATOMIC_ADD(&_outstandingChildren, _toFork.length());
                runner->enqueueTasks(_toFork.begin(), _toFork.end());
                _toFork.clear();
                return;
            }
            continue; // degenerate case: no tasks, just loop around
        }
        
        // no _oncompletion handler is set, so we are done.
        break;
    }

    // we have finished, notify parent.
    _taskHandle->onCompleted(runner, rval);
    delRoot(cx);
    delete this;
    return;
}

JSClass TaskContext::jsClass = {
    "TaskContext", JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(MaxSlot),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

// ______________________________________________________________________
// Runner impl

Runner *Runner::create(ThreadPool *aThreadPool, int anIndex) {
    JSRuntime *rt = check_null(JS_NewRuntime(1L * 1024L * 1024L));
    JSContext *cx = check_null(JS_NewContext(rt, 8192));
    JS_SetOptions(cx, JSOPTION_VAROBJFIX | JSOPTION_METHODJIT);
    JS_SetVersion(cx, JSVERSION_LATEST);
    JS_SetErrorReporter(cx, reportError);
    JS_ClearContextThread(cx);
    JS_ClearRuntimeThread(rt);
    if(getenv("SPORK_ZEAL") != NULL)
        JS_SetGCZeal(cx, 2, 1, false);
    return new Runner(aThreadPool, anIndex, rt, cx);
}

bool Runner::getWork(TaskContext **reawaken, TaskHandle **create) {
    // FIXME---this is very coarse locking indeed!
    *reawaken = NULL;
    *create = NULL;
    AutoLock holdM(_threadPool->masterLock());
    while (true) {
        if (!_toReawaken.empty()) {
            *reawaken = _toReawaken.popCopy();
            return true;
        }
            
        if (_threadPool->terminating())
            return false;

        if (!_threadPool->toCreate()->empty()) {
            *create = _threadPool->toCreate()->popCopy();
            return true;
        }

        JS_WAIT_CONDVAR(_threadPool->masterCondVar(), JS_NO_TIMEOUT);
    }
}

void Runner::reawaken(TaskContext *ctx) {
    AutoLock holdM(_threadPool->masterLock());
    _toReawaken.append(ctx);
    JS_NOTIFY_ALL_CONDVAR(_threadPool->masterCondVar());
}

void Runner::enqueueTasks(TaskHandle **begin, TaskHandle **end) {
    AutoLock holdM(_threadPool->masterLock());
    if (!_threadPool->toCreate()->append(begin, end)) {
        check_null((void*)NULL);
    }
    JS_NOTIFY_ALL_CONDVAR(_threadPool->masterCondVar());
}

void Runner::start() {
    TaskContext *reawaken = NULL;
    TaskHandle *create = NULL;
    JS_SetRuntimeThread(_rt);
    JS_SetContextThread(_cx);
    while (getWork(&reawaken, &create)) {
        JS_BeginRequest(_cx);
        if (reawaken) {
            reawaken->resume(this);
        }
        
        if (create) {
            TaskContext *ctx = createTaskContext(create);
            ctx->resume(this);
        }
        JS_EndRequest(_cx);
    }
}

TaskContext *Runner::createTaskContext(TaskHandle *handle) {
    JSObject *global = JS_NewCompartmentAndGlobalObject(
        /*JSContext *cx: */ _cx, 
        /*JSClass *clasp: */ &Global::jsClass,
        /*JSPrincipals*/ NULL);

    CrossCompartment cc(_cx, global);

    if (!JS_InitStandardClasses(_cx, global))
        return NULL;
        
    if (!JS_DefineFunctions(_cx, global, sporkGlobalFunctions))
        return NULL;

    if (!ChildTaskHandle::initClass(_cx, global))
        return NULL;

    return TaskContext::create(_cx, handle, this, global);
}

void Runner::terminate() {
    _threadPool->terminate();
}

// ______________________________________________________________________
// ThreadPool impl

ThreadPool *ThreadPool::create() {
    JSLock *lock = JS_NEW_LOCK();
    if (!lock) {
        throw "FIXME";
    }

    PRCondVar *condVar = JS_NEW_CONDVAR(lock);
    if (!condVar) {
        throw "FIXME";
    }

    const int threadCount = 4; // for now

    PRThread **threads = check_null(new PRThread*[threadCount]);
    memset(threads, 0, sizeof(PRThread*) * threadCount);

    Runner **runners = check_null(new Runner*[threadCount]);
    memset(threads, 0, sizeof(Runner*) * threadCount);

    ThreadPool *tp = check_null(
        new ThreadPool(lock, condVar, threadCount, threads, runners));

    for (int i = 0; i < threadCount; i++) {
        runners[i] = check_null(Runner::create(tp, i));
        threads[i] = PR_CreateThread(PR_USER_THREAD, 
                                     start, runners[i], 
                                     PR_PRIORITY_NORMAL,
                                     PR_LOCAL_THREAD, 
                                     PR_JOINABLE_THREAD, 
                                     0);
        check_null(threads[i]);
    }

    return tp;
}

void ThreadPool::start(RootTaskHandle *rth) {
    AutoLock hold(_masterLock);
    if (!_toCreate.append(rth)) {
        check_null((void*)NULL);
    }
    JS_NOTIFY_ALL_CONDVAR(_masterCondVar);
}

void ThreadPool::terminate() {
    AutoLock hold(_masterLock);
    _terminating = 1;
    JS_NOTIFY_ALL_CONDVAR(_masterCondVar);
}

void ThreadPool::shutdown() {
    for (int i = 0; i < _threadCount; i++) {
        if (_threads[i]) {
            PR_JoinThread(_threads[i]);
            _threads[i] = NULL;
        }
    }
}

// ______________________________________________________________________
// Init

ThreadPool *init(const char *scriptfn) {
    ThreadPool *tp = check_null(ThreadPool::create());
    RootTaskHandle *rth = new RootTaskHandle(scriptfn);
    tp->start(rth);
    tp->shutdown();
}

}
