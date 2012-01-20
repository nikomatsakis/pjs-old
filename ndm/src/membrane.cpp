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

#include "membrane.h"
#include <js/jsgc.h>
#include <vm/String.h>

namespace spork {

Membrane *Membrane::create(JSContext* cx, JSObject *gl) {
    Membrane *m = new Membrane(cx, gl);
    if (!m->_proxyMap.init()) {
        delete m;
        return NULL;
    }
    
    return m;
}

bool
Membrane::IsCrossThreadWrapper(const JSObject *wrapper)
{
    if (!IsWrapper(wrapper))
        return false;
    return !!(Wrapper::wrapperHandler(wrapper)->flags() & MEMBRANE);
}

static inline JSCompartment*
GetStringCompartment(JSString *obj) {
    return reinterpret_cast<js::gc::Cell *>(obj)->compartment();
}

bool Membrane::wrap(Value *vp) {
    JSContext *cx = _childCx;

    JS_CHECK_RECURSION(cx, return false);
    
    if (!vp->isMarkable())
        return true;

    if (vp->isString()) {
        JSString *str = vp->toString();

        /* If already in child compartment, done. */
        if (str->compartment() == _childCompartment)
            return true;

        /* If the string is an atom, we don't have to copy. */
        if (str->isAtom()) {
            return true;
        }
    }

    // This code to find the global object is taken from
    // jscompartment.cpp.  I do not fully understand what is
    // going on here.
    //JSObject *global;
    //if (cx->hasfp()) {
    //    global = &cx->fp()->scopeChain().global();
    //} else {
    //    global = JS_ObjectToInnerObject(cx, cx->globalObject);
    //    if (!global)
    //        return false;
    //}
    JSObject *global = _childGlobal;

    /* Unwrap incoming objects. */
    if (vp->isObject()) {
        JSObject *obj = &vp->toObject();

        /* If already in child compartment, done. */
        if (GetObjectCompartment(obj) == _childCompartment)
            return true;

        /* Translate StopIteration singleton. */
        if (obj->isStopIteration())
            return js_FindClassObject(cx, NULL, JSProto_StopIteration, vp);
    }


    /* If we already have a wrapper for this value, use it. */
    ProxyMap::Ptr p = _proxyMap.lookup(*vp);
    if (p.found()) {
        *vp = p->value;
        if (vp->isObject()) {
            JSObject *obj = &vp->toObject();
            JS_ASSERT(IsCrossThreadWrapper(obj));
            if (global->getClass() != &dummy_class &&
                obj->getParent() != global) {
                do {
                    if (!obj->setParent(cx, global))
                        return false;
                    obj = obj->getProto();
                } while (obj && IsCrossThreadWrapper(obj));
            }
        }
        return true;
    }

    /* Copy strings */
    if (vp->isString()) {
        Value orig = *vp;
        JSString *str = vp->toString();
        const jschar *chars = str->getChars(cx);
        if (!chars)
            return false;
        JSString *wrapped = js_NewStringCopyN(cx, chars, str->length());
        if (!wrapped)
            return false;
        vp->setString(wrapped);
        return _proxyMap.put(orig, *vp);
    }

    JSObject *obj = &vp->toObject();

    /*
     * Recurse to wrap the prototype. Long prototype chains will run out of
     * stack, causing an error in CHECK_RECURSE.
     *
     * Wrapping the proto before creating the new wrapper and adding it to the
     * cache helps avoid leaving a bad entry in the cache on OOM. But note that
     * if we wrapped both proto and parent, we would get infinite recursion
     * here (since Object.prototype->parent->proto leads to Object.prototype
     * itself).
     */
    JSObject *proto = obj->getProto();
    if (!wrap(&proto))
        return false;

    JSObject *wrapper = New(cx, obj, proto, global, this);
    vp->setObject(wrapper);

    if (!_proxyMap.put(GetProxyPrivate(wrapper), *vp))
        return false;

    return true;
}

}
