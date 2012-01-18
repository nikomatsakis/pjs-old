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

#include "mozilla/Attributes.h"

#include <string.h>
#include <pthread.h>
#include <deque>
#include <assert.h>
#include "prthread.h"
#include "prlock.h"
#include "prcvar.h"
#include "jsapi.h"
#include "jscntxt.h"
#include "jsdbgapi.h"
#include "jsstdint.h"
#include "jslock.h"

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
 *   let taskHandle = fork(function(taskCtx) {
 *       let foo = ...;
 *       taskCtx.setresult(foo);
 *   });
 *   let tN = forkn(N, function(ctx) {
 *       let foo = ... ctx.idx ...;
 *       ctx.setresult(foo);
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

template<typename T> T* getReservedSlot(JSContext *cx,
                                        JSObject *of_obj,
                                        int slot) {
    jsval val;
    if (!JS_GetReservedSlot(cx, of_obj, slot, &val))
        return NULL;
    JSObject *object = JSVAL_TO_OBJECT(val);
    return (T*) JS_GetPrivate(cx, object);
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
// TaskHandle interface

class TaskHandle MOZ_FINAL
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
                           JSObject *global) = 0;
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
                           JSObject *global);
    virtual void onCompleted(Runner *runner, jsval result);
};

class ChildTaskHandle : public TaskHandle
{
private:
    enum Reserved { ResultSlot, MaxSlot };

    static void jsFinalize(JSContext *cx, JSObject *obj) {
        delete_assoc<TaskHandle>(cx, obj);
    }

    TaskContext *_parent;
    int _generation;
    JSObject *_object;

    char *_funcStr;

    ClonedObj *_result;

    JSBool addRoot(JSContext *cx);
    JSBool delRoot(JSContext *cx);

    explicit ChildTaskHandle(JSContext *cx, TaskContext *parent, int gen,
                             JSObject *object, char *toExec)
        : _parent(parent)
        , _generation(gen)
        , _object(object)
        , _funcStr(toExec)
        , _result(NULL)
    {
        JS_SetPrivate(cx, _object, this);
        JS_SetReservedSlot(cx, _object, ResultSlot, JSVAL_NULL);
    }

    void clearResult();

protected:
    virtual ~ChildTaskHandle() {
        clearResult();
        delete[] _funcStr;
    }

public:
    JSBool GetResult(JSContext *cx, jsval *rval);
    virtual JSBool execute(JSContext *cx, JSObject *taskctx,
                           JSObject *global);
    virtual void onCompleted(Runner *runner, jsval result);

    JSObject *object() { return _object; }

    static ChildTaskHandle *create(JSContext *cx,
                                   TaskContext *parent,
                                   char *toExec);

    static JSClass jsClass;
};

// ______________________________________________________________________
// TaskContext interface

class TaskContext MOZ_FINAL
{
public:
    enum TaskContextSlots { OnCompletionSlot, ResultSlot, MaxSlot };

private:
    TaskHandle *_taskHandle;
    JSObject *_global;
    JSObject *_object;
    int _generation;
    jsrefcount _outstandingChildren;
    TaskHandleVec _toFork;
    Runner *_runner;
    
    TaskContext(JSContext *cx, TaskHandle *aTask,
                Runner *aRunner, JSObject *aGlobal,
                JSObject *object)
      : _taskHandle(aTask)
      , _global(aGlobal)
      , _object(object)
      , _generation(0)
      , _outstandingChildren(0)
      , _runner(aRunner)
    {
        setOncompletion(cx, JSVAL_NULL);
        setResult(cx, JSVAL_NULL);
        JS_SetPrivate(cx, _object, this);
    }

    JSBool addRoot(JSContext *cx);
    JSBool delRoot(JSContext *cx);

public:
    static TaskContext *create(JSContext *cx,
                               TaskHandle *aTask,
                               Runner *aRunner,
                               JSObject *aGlobal);

    void addTaskToFork(TaskHandle *th);

    void onChildCompleted();

    void resume(Runner *runner);

    int generation() { return _generation; }

    int generationReady(int gen) {
        return _generation > gen;
    }

    void setOncompletion(JSContext *cx, jsval val) {
        JS_SetReservedSlot(cx, _object, OnCompletionSlot, val);
    }

    void setResult(JSContext *cx, jsval val) {
        JS_SetReservedSlot(cx, _object, ResultSlot, val);
    }

    static JSClass jsClass;
};

// ____________________________________________________________
// Global interface

class Global MOZ_FINAL
{
public:
    static JSClass jsClass;
};

// ____________________________________________________________
// Runner interface

class Runner MOZ_FINAL
{
private:
    ThreadPool *_threadPool;
    int _index;
    TaskContextVec _toReawaken;
    std::deque<TaskHandle*> _toCreate;
    JSLock *_runnerLock;
    JSRuntime *_rt;
    JSContext *_cx;
    
    bool getWork(TaskContext **reawaken, TaskHandle **create);
    bool getLocalWork(TaskContext **reawaken, TaskHandle **create);
    bool getStolenWork(TaskHandle **create);
    bool steal(TaskHandle **create);

    Runner(ThreadPool *aThreadPool, int anIndex,
           JSLock *runnerLock, JSRuntime *aRt, JSContext *aCx)
      : _threadPool(aThreadPool)
      , _index(anIndex)
      , _runnerLock(runnerLock)
      , _rt(aRt)
      , _cx(aCx)
    {
    }

public:

    static Runner *create(ThreadPool *aThreadPool, int anIndex);

    ~Runner() {
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

// ______________________________________________________________________
// ThreadPool inter

typedef unsigned long wc_t;

class ThreadPool MOZ_FINAL
{
private:
    const static wc_t wc_terminate = 0;
    const static wc_t wc_idle = 1;
    const static wc_t wc_rollover = 2;

    volatile wc_t _workCounter; 
    JSLock *_masterLock;
    PRCondVar *_masterCondVar;
    int _threadCount;
    PRThread **_threads;
    Runner **_runners;

    static void start(void* arg) {
        ((Runner*) arg)->start();
    }

    explicit ThreadPool(JSLock *aLock, PRCondVar *aCondVar,
                        int threadCount, PRThread **threads, Runner **runners)
      : _workCounter(wc_rollover)
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

    void start(RootTaskHandle *rth);

    wc_t readWorkCounter();
    bool waitTillAllRunnersAreCreated(int runnerId);
    bool waitTillWorkIsProduced(int runnerId, wc_t sinceWorkCounter);
    void notifyWorkProduced();

    Runner **getRunners(int *runner_count);

    static ThreadPool *create();
    void terminate();
    void shutdown();
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

JSBool gettaskresult(JSContext *cx, uintN argc, jsval *vp) {
    // FIXME--method on task
    TaskContext *taskContext = (TaskContext*) JS_GetContextPrivate(cx);
    JSObject *taskObj;
    if (!JS_ConvertArguments(cx, argc, JS_ARGV(cx, vp), "o", &taskObj))
        return JS_FALSE;
    if (JS_GetClass(cx, taskObj) != &ChildTaskHandle::jsClass) {
        JS_ReportError(cx, "expected task as argument");
        return JS_FALSE;
    }
    ChildTaskHandle *task = (ChildTaskHandle*) JS_GetPrivate(cx, taskObj);
    jsval result;
    if (!task->GetResult(cx, &result))
        return JS_FALSE;
    JS_SET_RVAL(cx, vp, result);
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

JSBool setresult(JSContext *cx, uintN argc, jsval *vp) {
    TaskContext *taskContext = (TaskContext*) JS_GetContextPrivate(cx);
    taskContext->setResult(cx, JS_ARGV(cx, vp)[0]);
    return JS_TRUE;
}

static JSFunctionSpec sporkGlobalFunctions[] = {
    JS_FN("print", print, 0, 0),
    JS_FN("fork", fork, 1, 0),
    JS_FN("gettaskresult", gettaskresult, 1, 0),
    JS_FN("setresult", setresult, 1, 0),
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

JSClass ChildTaskHandle::jsClass = {
    "TaskHandle", JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(MaxSlot),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, jsFinalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

void RootTaskHandle::onCompleted(Runner *runner, jsval result) {
    runner->terminate();
}

JSBool RootTaskHandle::execute(JSContext *cx, JSObject *taskctx,
                               JSObject *global) {
    JSScript *scr = JS_CompileUTF8File(cx, global, scriptfn);
    if (scr == NULL)
        return 0;

    jsval rval;
    return JS_ExecuteScript(cx, global, scr, &rval);
}

void ChildTaskHandle::onCompleted(Runner *runner, jsval result) {
    ClonedObj::pack(runner->cx(), result, &_result);
    _parent->onChildCompleted();
    delRoot(runner->cx());
}

JSBool ChildTaskHandle::execute(JSContext *cx, JSObject *taskctx,
                                JSObject *global) {
    jsval fnval;
    if (!JS_EvaluateScript(cx, global, _funcStr, strlen(_funcStr),
                           "fork", 1, &fnval))
        return  0;

    jsval rval;
    JSBool result = JS_CallFunctionValue(cx, global, fnval, 0, NULL, &rval);
    return result;
}

ChildTaskHandle *ChildTaskHandle::create(JSContext *cx,
                                         TaskContext *parent,
                                         char *toExec) {
    // To start, create the JS object representative:
    JSObject *object = JS_NewObject(cx, &jsClass, NULL, NULL);
    if (!object) {
        return NULL;
    }
    
    // Create C++ object, which will be linked via Private:
    ChildTaskHandle *th = new ChildTaskHandle(cx, parent, parent->generation(),
                                              object, toExec);
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
    if (!_parent->generationReady(_generation)) {
        JS_ReportError(cx, "all child tasks not yet completed");
        return false;
    }

    if (_result != NULL) {
        jsval decloned;
        if (!_result->unpack(cx, &decloned))
            return false;
        clearResult();
        JS_SetReservedSlot(cx, _object, ResultSlot, decloned);
    }

    return JS_GetReservedSlot(cx, _object, ResultSlot, rval);
}

JSBool ChildTaskHandle::addRoot(JSContext *cx) {
    return JS_AddNamedObjectRoot(cx, &_object, "ChildTaskHandle::addRoot()");
}

JSBool ChildTaskHandle::delRoot(JSContext *cx) {
    return JS_RemoveObjectRoot(cx, &_object);
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

void TaskContext::resume(Runner *runner) {
    JSContext *cx = runner->cx();
    CrossCompartment cc(cx, _global);
    jsval rval = JSVAL_NULL;

    // If we break from this loop, this task context has completed,
    // either in error or successfully:
    while (true) {
        int gen = _generation++;

        JS_SetContextPrivate(cx, this);
        if (gen == 0) {
            // First round when task was just started
            if (!_taskHandle->execute(cx, _object, _global))
                break;
        } else {
            jsval fn;
            if (JS_GetReservedSlot(cx, _object, OnCompletionSlot, &fn)) {
                setOncompletion(cx, JSVAL_NULL);
                jsval ignored;
                if (!JS_CallFunctionValue(cx, _global, fn, 0, NULL, &ignored))
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
        
        // no _oncompletion handler is set, so we are done.  load the
        // final result.
        JS_GetReservedSlot(cx, _object, ResultSlot, &rval);
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
    JSLock *lock = check_null(JS_NEW_LOCK());
    JS_SetOptions(cx, JSOPTION_VAROBJFIX | JSOPTION_METHODJIT);
    JS_SetVersion(cx, JSVERSION_LATEST);
    JS_SetErrorReporter(cx, reportError);
    JS_ClearContextThread(cx);
    JS_ClearRuntimeThread(rt);
    if(getenv("SPORK_ZEAL") != NULL)
        JS_SetGCZeal(cx, 2, 1, false);
    return new Runner(aThreadPool, anIndex, lock, rt, cx);
}

bool Runner::getLocalWork(TaskContext **reawaken, TaskHandle **create) {
    AutoLock hold(_runnerLock);

    // Executed only from the thread of this runner

    fprintf(stderr, "runner %d getLocalWork toReawaken=%d toCreate=%d\n",
            _index, _toReawaken.empty(), _toCreate.empty());

    if (!_toReawaken.empty()) {
        *reawaken = _toReawaken.popCopy();
        return true;
    }

    if (!_toCreate.empty()) {
        *create = _toCreate.front();
        _toCreate.pop_front();
        return true;
    }

    return false;
}

bool Runner::steal(TaskHandle **create) {
    AutoLock hold(_runnerLock);
    
    // Executed only from the thread of another runner

    if (!_toCreate.empty()) {
        *create = _toCreate.back();
        _toCreate.pop_back();
        return true;
    }

    return false;
}

bool Runner::getStolenWork(TaskHandle **create) {
    int c;
    Runner **runners = _threadPool->getRunners(&c);
    for (int i = _index + 1; i < c; i++)
        if (runners[i]->steal(create))
            return true;
    for (int i = 0; i < _index; i++)
        if (runners[i]->steal(create))
            return true;
    return false;
}

bool Runner::getWork(TaskContext **reawaken, TaskHandle **create) {
    wc_t workCounter;
    *reawaken = NULL;
    *create = NULL;
    do {
        fprintf(stderr, "runner %d searching for work\n", _index);
        workCounter = _threadPool->readWorkCounter();
        if (getLocalWork(reawaken, create))
            return true;
        if (getStolenWork(create))
            return true;
    } while (_threadPool->waitTillWorkIsProduced(_index, workCounter));
    return false;
}

void Runner::reawaken(TaskContext *ctx) {
    fprintf(stderr, "runner %d reawaken:%p\n", _index, ctx);

    {
        AutoLock hold(_runnerLock);
        _toReawaken.append(ctx);
    }

    _threadPool->notifyWorkProduced(); // FIXME---only necc. if this is idle
}

void Runner::enqueueTasks(TaskHandle **begin, TaskHandle **end) {
    {
        AutoLock hold(_runnerLock);
        for (TaskHandle **p = begin; p != end; p++)
            _toCreate.push_front(*p);
    }

    _threadPool->notifyWorkProduced();
}

void Runner::start() {
    TaskContext *reawaken = NULL;
    TaskHandle *create = NULL;
    JS_SetRuntimeThread(_rt);
    JS_SetContextThread(_cx);

    // first thing we do is block for the first item of "work"
    _threadPool->waitTillAllRunnersAreCreated(_index);

    while (getWork(&reawaken, &create)) {
        JS_BeginRequest(_cx);
        fprintf(stderr, "runner %d got work reawaken=%p create=%p\n", 
                _index, reawaken, create);
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

    // this will cause the workers to actually enter the main loop
    tp->notifyWorkProduced();

    return tp;
}

void ThreadPool::start(RootTaskHandle *rth) {
    TaskHandle *th = rth;
    TaskHandle **first = &th;
    _runners[0]->enqueueTasks(first, first+1);
}

void ThreadPool::terminate() {
    AutoLock hold(_masterLock);
    fprintf(stderr, "ThreadPool::terminate()\n");
    _workCounter = wc_terminate;
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

wc_t ThreadPool::readWorkCounter() {
    return _workCounter;
}

Runner **ThreadPool::getRunners(int *runner_count) {
    *runner_count = _threadCount;
    return _runners;
}

void ThreadPool::notifyWorkProduced() {
    AutoLock hold(_masterLock);
    wc_t wc = _workCounter;
    assert (wc != wc_terminate);
    if (wc == wc_idle) {
        _workCounter = wc_rollover;
        JS_NOTIFY_ALL_CONDVAR(_masterCondVar);
    } else {
        wc_t next_wc = wc + 1;
        if (next_wc == 0)
            _workCounter = wc_rollover;
        else
            _workCounter = next_wc;
    }
    fprintf(stderr, "notifyWorkProduced() wc=%u _workCounter=%u\n",
            wc, _workCounter);
}

bool ThreadPool::waitTillAllRunnersAreCreated(int runnerId) {
    waitTillWorkIsProduced(runnerId, wc_rollover);
}

bool ThreadPool::waitTillWorkIsProduced(int runnerId, wc_t sinceWorkCounter) {
    AutoLock hold(_masterLock);

    fprintf(stderr, "waitTillWorkIsProduced(%d, %lu) _workCounter=%lu\n",
            runnerId, sinceWorkCounter, _workCounter);

    if (_workCounter > sinceWorkCounter)
        return true;

    if (sinceWorkCounter == wc_terminate)
        return false;

    if (_workCounter == sinceWorkCounter)
        _workCounter = wc_idle;

    while (_workCounter == wc_idle) {
        fprintf(stderr, "  runner %d going idle\n", runnerId);
        JS_WAIT_CONDVAR(_masterCondVar, JS_NO_TIMEOUT);
    }

    return (_workCounter != wc_terminate);
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
