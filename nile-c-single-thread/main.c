
#include "js/jsapi.h"
#include <pthread.h>


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

static JSClass global_class = {
    "global", JSCLASS_GLOBAL_FLAGS,
    JS_PropertyStub, JS_PropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
    JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub, JS_FinalizeStub,
    JSCLASS_NO_OPTIONAL_MEMBERS
};

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
    JSRuntime *rt = JS_NewRuntime(8 * 1024 * 1024);
    if (rt == NULL)
        return 1;

    JSContext *cx = make_context(rt);
    if (cx == NULL)
        return 1;

    JSObject *global = JS_GetGlobalObject(cx);

    jsval rval;
    JSBool ok;

    JSScript *nile = JS_CompileUTF8File(cx, global, "nile.js");
    if (nile == NULL)
        return 1;

    ok = JS_ExecuteScript(cx, global, nile, &rval);
    if (!ok || JSVAL_IS_NULL(rval))
        return 1;

    JS_DestroyContext(cx);
    JS_DestroyRuntime(rt);
    JS_ShutDown();
    return 0;
}
