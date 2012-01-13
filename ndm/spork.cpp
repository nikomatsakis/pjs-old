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
 * - You have a shared counter on the thread pool, guarded by a lock (for
 *   now).
 * - The counter initially 0,
 *   but set to MAX_INT when there are idle 
 * - Whenever a a new task is pushed, the thread examines
 *   the counter: if it is < MAX_INT, 
 *   increments the counter.
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
// TaskHandle interface

class TaskHandle MOZ_FINAL
{
public:
    enum Slots { ResultSlot, MaxSlot };

private:
    TaskHandle(const TaskHandle &) MOZ_DELETE;
    TaskHandle & operator=(const TaskHandle &) MOZ_DELETE;

    static void jsFinalize(JSContext *cx, JSObject *obj) {
        delete_assoc<TaskHandle>(cx, obj);
    }

protected:
    TaskHandle()
    {}

public:
    virtual ~TaskHandle() {}

    virtual JSBool execute(JSContext *cx, JSObject *global) = 0;
    virtual void onCompleted(Runner *runner, jsval result) = 0;

    static JSClass jsClass;
};

class RootTaskHandle : public TaskHandle
{
    const char *scriptfn;

public:
    RootTaskHandle(const char *afn)
        : scriptfn(afn)
    {}

    virtual JSBool execute(JSContext *cx, JSObject *global);
    virtual void onCompleted(Runner *runner, jsval result);
};

class ChildTaskHandle : public TaskHandle
{
    enum Reserved { Result };

private:
    TaskContext *_parent;
    int _generation;
    JSObject *_object;
    char *_toExec;
    uint64_t *_result;
    size_t _nbytes;

    explicit ChildTaskHandle(JSContext *cx, TaskContext *parent, int gen,
                             JSObject *object, char *toExec)
        : _parent(parent)
        , _generation(gen)
        , _object(object)
        , _toExec(toExec)
        , _result(NULL)
        , _nbytes(0)
    {
        JS_SetPrivate(cx, _object, this);
    }

    void clearResult();

protected:
    virtual ~ChildTaskHandle() {
        clearResult();
        delete[] _toExec;
    }

public:
    JSBool GetResult(JSContext *cx, jsval *result);
    virtual JSBool execute(JSContext *cx, JSObject *global);
    virtual void onCompleted(Runner *runner, jsval result);

    JSObject *object() { return _object; }

    static ChildTaskHandle *create(JSContext *cx,
                                   TaskContext *parent,
                                   char *toExec);
};

// ______________________________________________________________________
// TaskContext interface

class TaskContext MOZ_FINAL
{
public:
    enum TaskContextSlots { ResultSlot, MaxSlot };

private:
    TaskHandle *_taskHandle;
    JSObject *_global;
    JSObject *_object;
    JSObject *_oncompletion;
    int _generation;
    jsrefcount _outstandingChildren;
    TaskHandleVec _toFork;
    Runner *_runner;
    
    static void jsFinalize(JSContext *cx, JSObject *obj) {
        delete_assoc<TaskContext>(cx, obj);
    }

    TaskContext(JSContext *cx, TaskHandle *aTask,
                Runner *aRunner, JSObject *aGlobal,
                JSObject *object)
      : _taskHandle(aTask)
      , _global(aGlobal)
      , _object(object)
      , _oncompletion(NULL)
      , _generation(0)
      , _outstandingChildren(0)
      , _runner(aRunner)
    {
        JS_SetPrivate(cx, _object, this);
    }

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

    void setOncompletion(JSObject *obj) {
        _oncompletion = obj;
    }

    static JSClass jsClass;
};

// ____________________________________________________________
// Global interface

class Global MOZ_FINAL
{
private:
    JSObject *_object;

    static void jsFinalize(JSContext *cx, JSObject *obj) {
        delete_assoc<Global>(cx, obj);
    }

    Global(JSObject *anObject)
      : _object(anObject)
    {}

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

class ThreadPool MOZ_FINAL
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
    char *encoded = check_null(new char[length+5]);
    JS_EncodeStringToBuffer(str, encoded+1, length);
    encoded[0] = '(';
    encoded[length+1] = ')';
    encoded[length+2] = '(';
    encoded[length+3] = ')';
    encoded[length+4] = 0;
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
    if (JS_GetClass(cx, taskObj) != &TaskHandle::jsClass) {
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
    taskContext->setOncompletion(func);
    return JS_TRUE;
}

static JSFunctionSpec sporkGlobalFunctions[] = {
    JS_FN("print", print, 0, 0),
    JS_FN("fork", fork, 1, 0),
    JS_FN("gettaskresult", gettaskresult, 1, 0),
    JS_FN("oncompletion", oncompletion, 1, 0),
    JS_FS_END
};

// ______________________________________________________________________
// Global impl

JSClass Global::jsClass = {
    "Global", JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, Global::jsFinalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

// ______________________________________________________________________
// TaskHandle impl

JSClass TaskHandle::jsClass = {
    "TaskHandle", JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(MaxSlot),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, TaskHandle::jsFinalize,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

void RootTaskHandle::onCompleted(Runner *runner, jsval result) {
    runner->terminate();
}

JSBool RootTaskHandle::execute(JSContext *cx, JSObject *global) {
    JSScript *scr = JS_CompileUTF8File(cx, global, scriptfn);
    if (scr == NULL)
        return 0;
    
    jsval rval;
    if (!JS_ExecuteScript(cx, global, scr, &rval))
        return  0;

    if (JSVAL_IS_NULL(rval))
        return 0;

    return 1;
}

void ChildTaskHandle::onCompleted(Runner *runner, jsval result) {
    // Clone the result:
    JS_WriteStructuredClone(runner->cx(), result, &_result, &_nbytes,
                            NULL, NULL);
    _parent->onChildCompleted();
}

JSBool ChildTaskHandle::execute(JSContext *cx, JSObject *global) {
    jsval rval;
    if (!JS_EvaluateScript(cx, global, _toExec, strlen(_toExec),
                           "fork", 1, &rval))
        return  0;

    if (JSVAL_IS_NULL(rval))
        return 0;

    return 1;
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
    return th;
}

void ChildTaskHandle::clearResult() {
    if (_result) {
        js::Foreground::free_(_result);
        _result = 0;
        _nbytes = 0;
    }
}

JSBool ChildTaskHandle::GetResult(JSContext *cx,
                                  jsval *result)
{
    if (_result != NULL) {
        jsval decloned;
        if (!JS_ReadStructuredClone(cx, _result, _nbytes, 
                                    JS_STRUCTURED_CLONE_VERSION, &decloned,
                                    NULL, NULL))
            return false;
        clearResult();

        JS_SetReservedSlot(cx, _object, ResultSlot, decloned);
    }

    if (!_parent->generationReady(_generation)) {
        JS_ReportError(cx, "all child tasks not yet completed");
        return false;
    }

    return JS_GetReservedSlot(cx, _object, ResultSlot, result);
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
    return new TaskContext(cx, aTask, aRunner, aGlobal, object);
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

    JS_SetContextPrivate(cx, this);
    while (true) {
        _generation++;

        JSBool ok;
        if (!_oncompletion) {
            ok = _taskHandle->execute(cx, _global);
        } else {
            jsval fn = OBJECT_TO_JSVAL(_oncompletion);
            jsval rval;
            _oncompletion = NULL;
            ok = JS_CallFunctionValue(cx, _global, fn, 0, NULL, &rval);
        }
        JS_SetContextPrivate(cx, NULL);

        if (_oncompletion) {
            // fork off outstanding children then block till they're done:
            if (!_toFork.empty()) {
                JS_ATOMIC_ADD(&_outstandingChildren, _toFork.length());
                runner->enqueueTasks(_toFork.begin(), _toFork.end());
                _toFork.clear();
                return;
            }
        } else {
            // we are done, notify our parent:
            jsval result = JSVAL_NULL;
            JS_GetReservedSlot(cx, _object, ResultSlot, &result);
            _taskHandle->onCompleted(runner, result);
            JS_SetReservedSlot(cx, _object, ResultSlot, JSVAL_NULL);
            return;
        }
    }
}

JSClass TaskContext::jsClass = {
    "TaskContext", JSCLASS_HAS_PRIVATE | JSCLASS_HAS_RESERVED_SLOTS(MaxSlot),
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, TaskContext::jsFinalize,
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
