#ifndef MEMBRANE_H
#define MEMBRANE_H

#include <jsapi.h>
#include <jsgc.h>
#include <jsfriendapi.h>
#include <HashTable.h>
#include <jswrapper.h>

namespace spork {

using namespace js;

class Membrane : Wrapper
{
private:
    static const uintN MEMBRANE = Wrapper::LAST_USED_FLAG << 1;
    static const uintN LAST_USED_FLAG = MEMBRANE;

    static bool IsCrossThreadWrapper(const JSObject *wrapper);

    // Maps from objects in the parent space to wrapper object in
    // child space.
    WrapperMap _map;
    JSContext *_childCx;
    JSObject *_childGlobal;
    JSCompartment *_childCompartment;

    Membrane(JSContext* cx, JSObject *gl)
        : Wrapper(MEMBRANE)
        , _childCx(cx)
        , _childGlobal(gl)
    {
    }
public:
    static Membrane *create(JSContext* cx, JSObject *gl);

    virtual bool enter(JSContext *cx, JSObject *wrapper,
                       jsid id, Action act, bool *bp);

    // when invoked with a parent object, modifies vp to be a proxy in
    // child compartment that will permit read access to the parent
    // object.  returns true if successful.
    bool wrap(Value *vp);

    bool wrap(JSObject **objp);

#if 0
    // ______________________________________________________________________

    /* ES5 Harmony fundamental wrapper traps. */
    virtual bool getPropertyDescriptor(JSContext *cx, JSObject *wrapper, jsid id, bool set,
                                       PropertyDescriptor *desc) MOZ_OVERRIDE;
    virtual bool getOwnPropertyDescriptor(JSContext *cx, JSObject *wrapper, jsid id, bool set,
                                          PropertyDescriptor *desc) MOZ_OVERRIDE;
    virtual bool defineProperty(JSContext *cx, JSObject *wrapper, jsid id,
                                PropertyDescriptor *desc) MOZ_OVERRIDE;
    virtual bool getOwnPropertyNames(JSContext *cx, JSObject *wrapper, AutoIdVector &props) MOZ_OVERRIDE;
    virtual bool delete_(JSContext *cx, JSObject *wrapper, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool enumerate(JSContext *cx, JSObject *wrapper, AutoIdVector &props) MOZ_OVERRIDE;

    /* ES5 Harmony derived wrapper traps. */
    virtual bool has(JSContext *cx, JSObject *wrapper, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool hasOwn(JSContext *cx, JSObject *wrapper, jsid id, bool *bp) MOZ_OVERRIDE;
    virtual bool get(JSContext *cx, JSObject *wrapper, JSObject *receiver, jsid id, Value *vp) MOZ_OVERRIDE;
    virtual bool set(JSContext *cx, JSObject *wrapper, JSObject *receiver, jsid id, bool strict,
                     Value *vp) MOZ_OVERRIDE;
    virtual bool keys(JSContext *cx, JSObject *wrapper, AutoIdVector &props) MOZ_OVERRIDE;
    virtual bool iterate(JSContext *cx, JSObject *wrapper, uintN flags, Value *vp) MOZ_OVERRIDE;

    /* Spidermonkey extensions. */
    virtual bool call(JSContext *cx, JSObject *wrapper, uintN argc, Value *vp) MOZ_OVERRIDE;
    virtual bool construct(JSContext *cx, JSObject *wrapper, uintN argc, Value *argv, Value *rval) MOZ_OVERRIDE;
    virtual bool nativeCall(JSContext *cx, JSObject *wrapper, Class *clasp, Native native, CallArgs args) MOZ_OVERRIDE;
    virtual bool hasInstance(JSContext *cx, JSObject *wrapper, const Value *vp, bool *bp) MOZ_OVERRIDE;
    virtual JSString *obj_toString(JSContext *cx, JSObject *wrapper) MOZ_OVERRIDE;
    virtual JSString *fun_toString(JSContext *cx, JSObject *wrapper, uintN indent) MOZ_OVERRIDE;
    virtual bool defaultValue(JSContext *cx, JSObject *wrapper, JSType hint, Value *vp) MOZ_OVERRIDE;

    virtual void trace(JSTracer *trc, JSObject *wrapper) MOZ_OVERRIDE;
#endif
    
};

}

#endif
