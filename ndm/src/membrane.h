#ifndef MEMBRANE_H
#define MEMBRANE_H

#include <js/jsapi.h>
#include <js/jsgc.h>
#include <js/jsfriendapi.h>
#include <js/HashTable.h>
#include <js/jswrapper.h>

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
};

}

#endif
