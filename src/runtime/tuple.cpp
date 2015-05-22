// Copyright (c) 2014-2015 Dropbox, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/tuple.h"

#include <algorithm>

#include "llvm/Support/raw_ostream.h"

#include "core/ast.h"
#include "core/common.h"
#include "core/stats.h"
#include "core/types.h"
#include "gc/collector.h"
#include "runtime/capi.h"
#include "runtime/objmodel.h"
#include "runtime/types.h"
#include "runtime/util.h"

namespace pyston {

extern "C" Box* createTuple(int64_t nelts, Box** elts) {
    return BoxedTuple::create(nelts, elts);
}

Box* _tupleSlice(BoxedTuple* self, i64 start, i64 stop, i64 step, i64 length) {

    i64 size = self->size();
    assert(step != 0);
    if (step > 0) {
        assert(0 <= start);
        assert(stop <= size);
    } else {
        assert(start < size);
        assert(-1 <= stop);
    }

    // FIXME: No need to initialize with NULL, since we're going to fill it in
    auto rtn = BoxedTuple::create(length);
    if (length > 0)
        copySlice(&rtn->elts[0], &self->elts[0], start, step, length);
    return rtn;
}

Box* tupleGetitemUnboxed(BoxedTuple* self, i64 n) {
    i64 size = self->size();

    if (n < 0)
        n = size + n;
    if (n < 0 || n >= size)
        raiseExcHelper(IndexError, "tuple index out of range");

    Box* rtn = self->elts[n];
    return rtn;
}

Box* tupleGetitemInt(BoxedTuple* self, BoxedInt* slice) {
    return tupleGetitemUnboxed(self, slice->n);
}

extern "C" PyObject** PyTuple_Items(PyObject* op) noexcept {
    RELEASE_ASSERT(PyTuple_Check(op), "");

    return &static_cast<BoxedTuple*>(op)->elts[0];
}

extern "C" PyObject* PyTuple_GetItem(PyObject* op, Py_ssize_t i) noexcept {
    RELEASE_ASSERT(PyTuple_Check(op), "");
    RELEASE_ASSERT(i >= 0, ""); // unlike tuple.__getitem__, PyTuple_GetItem doesn't do index wrapping
    try {
        return tupleGetitemUnboxed(static_cast<BoxedTuple*>(op), i);
    } catch (ExcInfo e) {
        abort();
    }
}

Box* tupleGetitemSlice(BoxedTuple* self, BoxedSlice* slice) {
    assert(isSubclass(self->cls, tuple_cls));
    assert(slice->cls == slice_cls);

    i64 start, stop, step, length;
    parseSlice(slice, self->size(), &start, &stop, &step, &length);
    return _tupleSlice(self, start, stop, step, length);
}

extern "C" PyObject* PyTuple_GetSlice(PyObject* p, Py_ssize_t low, Py_ssize_t high) noexcept {
    RELEASE_ASSERT(isSubclass(p->cls, tuple_cls), "");
    BoxedTuple* t = static_cast<BoxedTuple*>(p);

    Py_ssize_t n = t->size();
    if (low < 0)
        low = 0;
    if (high > n)
        high = n;
    if (high < low)
        high = low;

    if (low == 0 && high == n)
        return p;

    return BoxedTuple::create(high - low, &t->elts[low]);
}

extern "C" int _PyTuple_Resize(PyObject** pv, Py_ssize_t newsize) noexcept {
    // This is only allowed to be called when there is only one user of the tuple (ie a refcount of 1 in CPython)
    assert(pv);
    return BoxedTuple::Resize((BoxedTuple**)pv, newsize);
}

int BoxedTuple::Resize(BoxedTuple** pv, size_t newsize) noexcept {
    assert((*pv)->cls == tuple_cls);

    BoxedTuple* t = static_cast<BoxedTuple*>(*pv);

    if (newsize == t->size())
        return 0;

    if (newsize < t->size()) {
        // XXX resize the box (by reallocating) smaller if it makes sense
        t->ob_size = newsize;
        return 0;
    }

    BoxedTuple* resized = new (newsize)
        BoxedTuple(newsize); // we want an uninitialized tuple, but this will memset it with 0.
    memmove(resized->elts, t->elts, t->size());

    *pv = resized;
    return 0;
}

Box* tupleGetitem(BoxedTuple* self, Box* slice) {
    assert(self->cls == tuple_cls);

    if (PyIndex_Check(slice)) {
        Py_ssize_t i = PyNumber_AsSsize_t(slice, PyExc_IndexError);
        if (i == -1 && PyErr_Occurred())
            throwCAPIException();
        return tupleGetitemUnboxed(self, i);
    } else if (slice->cls == slice_cls)
        return tupleGetitemSlice(self, static_cast<BoxedSlice*>(slice));
    else
        raiseExcHelper(TypeError, "tuple indices must be integers, not %s", getTypeName(slice));
}

Box* tupleAdd(BoxedTuple* self, Box* rhs) {
    if (!isSubclass(rhs->cls, tuple_cls)) {
        return NotImplemented;
    }

    BoxedTuple* _rhs = static_cast<BoxedTuple*>(rhs);

    BoxedTuple* rtn = BoxedTuple::create(self->size() + _rhs->size());
    memmove(&rtn->elts[0], &self->elts[0], self->size() * sizeof(Box*));
    memmove(&rtn->elts[self->size()], &_rhs->elts[0], _rhs->size() * sizeof(Box*));
    return rtn;
}

Box* tupleMul(BoxedTuple* self, Box* rhs) {
    STAT_TIMER(t0, "us_timer_tupleMul");
    if (rhs->cls != int_cls) {
        raiseExcHelper(TypeError, "can't multiply sequence by non-int of type '%s'", getTypeName(rhs));
    }

    int n = static_cast<BoxedInt*>(rhs)->n;
    int s = self->size();

    if (n < 0)
        n = 0;

    if (s == 0 || n == 1) {
        return self;
    } else {
        BoxedTuple* rtn = BoxedTuple::create(n * s);
        int rtn_i = 0;
        for (int i = 0; i < n; ++i) {
            memmove(&rtn->elts[rtn_i], &self->elts[0], sizeof(Box*) * s);
            rtn_i += s;
        }
        return rtn;
    }
}

Box* tupleLen(BoxedTuple* t) {
    assert(isSubclass(t->cls, tuple_cls));
    return boxInt(t->size());
}

extern "C" Py_ssize_t PyTuple_Size(PyObject* op) noexcept {
    RELEASE_ASSERT(PyTuple_Check(op), "");
    return static_cast<BoxedTuple*>(op)->size();
}

Box* tupleRepr(BoxedTuple* t) {
    assert(isSubclass(t->cls, tuple_cls));

    std::string O("");
    llvm::raw_string_ostream os(O);
    os << "(";

    int n = t->size();
    for (int i = 0; i < n; i++) {
        if (i)
            os << ", ";

        BoxedString* elt_repr = static_cast<BoxedString*>(repr(t->elts[i]));
        os << elt_repr->s();
    }
    if (n == 1)
        os << ",";
    os << ")";

    return boxString(os.str());
}

Box* _tupleCmp(BoxedTuple* lhs, BoxedTuple* rhs, AST_TYPE::AST_TYPE op_type) {
    int lsz = lhs->size();
    int rsz = rhs->size();

    bool is_order
        = (op_type == AST_TYPE::Lt || op_type == AST_TYPE::LtE || op_type == AST_TYPE::Gt || op_type == AST_TYPE::GtE);

    int n = std::min(lsz, rsz);
    for (int i = 0; i < n; i++) {
        Box* is_eq = compareInternal(lhs->elts[i], rhs->elts[i], AST_TYPE::Eq, NULL);
        bool bis_eq = nonzero(is_eq);

        if (bis_eq)
            continue;

        if (op_type == AST_TYPE::Eq) {
            return boxBool(false);
        } else if (op_type == AST_TYPE::NotEq) {
            return boxBool(true);
        } else {
            Box* r = compareInternal(lhs->elts[i], rhs->elts[i], op_type, NULL);
            return r;
        }
    }

    if (op_type == AST_TYPE::Lt)
        return boxBool(lsz < rsz);
    else if (op_type == AST_TYPE::LtE)
        return boxBool(lsz <= rsz);
    else if (op_type == AST_TYPE::Gt)
        return boxBool(lsz > rsz);
    else if (op_type == AST_TYPE::GtE)
        return boxBool(lsz >= rsz);
    else if (op_type == AST_TYPE::Eq)
        return boxBool(lsz == rsz);
    else if (op_type == AST_TYPE::NotEq)
        return boxBool(lsz != rsz);

    RELEASE_ASSERT(0, "%d", op_type);
}

Box* tupleLt(BoxedTuple* self, Box* rhs) {
    if (!isSubclass(rhs->cls, tuple_cls)) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::Lt);
}

Box* tupleLe(BoxedTuple* self, Box* rhs) {
    if (!isSubclass(rhs->cls, tuple_cls)) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::LtE);
}

Box* tupleGt(BoxedTuple* self, Box* rhs) {
    if (!isSubclass(rhs->cls, tuple_cls)) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::Gt);
}

Box* tupleGe(BoxedTuple* self, Box* rhs) {
    if (!isSubclass(rhs->cls, tuple_cls)) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::GtE);
}

Box* tupleEq(BoxedTuple* self, Box* rhs) {
    if (!isSubclass(rhs->cls, tuple_cls)) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::Eq);
}

Box* tupleNe(BoxedTuple* self, Box* rhs) {
    if (!isSubclass(rhs->cls, tuple_cls)) {
        return NotImplemented;
    }
    return _tupleCmp(self, static_cast<BoxedTuple*>(rhs), AST_TYPE::NotEq);
}

Box* tupleNonzero(BoxedTuple* self) {
    RELEASE_ASSERT(isSubclass(self->cls, tuple_cls), "");
    return boxBool(self->size() != 0);
}

Box* tupleContains(BoxedTuple* self, Box* elt) {
    int size = self->size();
    for (int i = 0; i < size; i++) {
        Box* e = self->elts[i];
        Box* cmp = compareInternal(e, elt, AST_TYPE::Eq, NULL);
        bool b = nonzero(cmp);
        if (b)
            return True;
    }
    return False;
}

Box* tupleIndex(BoxedTuple* self, Box* elt) {
    int size = self->size();
    for (int i = 0; i < size; i++) {
        Box* e = self->elts[i];
        Box* cmp = compareInternal(e, elt, AST_TYPE::Eq, NULL);
        bool b = nonzero(cmp);
        if (b)
            return boxInt(i);
    }

    raiseExcHelper(ValueError, "tuple.index(x): x not in tuple");
}

Box* tupleHash(BoxedTuple* self) {
    STAT_TIMER(t0, "us_timer_tupleHash");
    assert(isSubclass(self->cls, tuple_cls));

    int64_t rtn = 3527539;
    for (auto e : *self) {
        BoxedInt* h = hash(e);
        assert(isSubclass(h->cls, int_cls));
        rtn ^= h->n + 0x9e3779b9 + (rtn << 6) + (rtn >> 2);
    }

    return boxInt(rtn);
}

extern "C" Box* tupleNew(Box* _cls, BoxedTuple* args, BoxedDict* kwargs) {
    if (!isSubclass(_cls->cls, type_cls))
        raiseExcHelper(TypeError, "tuple.__new__(X): X is not a type object (%s)", getTypeName(_cls));

    BoxedClass* cls = static_cast<BoxedClass*>(_cls);
    if (!isSubclass(cls, tuple_cls))
        raiseExcHelper(TypeError, "tuple.__new__(%s): %s is not a subtype of tuple", getNameOfClass(cls),
                       getNameOfClass(cls));

    int args_sz = args->size();
    int kwargs_sz = kwargs->d.size();

    if (args_sz + kwargs_sz > 1)
        raiseExcHelper(TypeError, "tuple() takes at most 1 argument (%d given)", args_sz + kwargs_sz);

    if (args_sz || kwargs_sz) {
        Box* elements;
        // if initializing from iterable argument, check common case positional args first
        if (args_sz) {
            elements = args->elts[0];
        } else {
            assert(kwargs_sz);
            auto const seq = *(kwargs->d.begin());
            auto const kw = static_cast<BoxedString*>(seq.first);

            if (kw->s() == "sequence")
                elements = seq.second;
            else
                raiseExcHelper(TypeError, "'%s' is an invalid keyword argument for this function", kw->data());
        }

        std::vector<Box*, StlCompatAllocator<Box*>> elts;
        for (auto e : elements->pyElements())
            elts.push_back(e);

        return BoxedTuple::create(elts.size(), &elts[0], cls);
    } else {
        return BoxedTuple::create(0, cls);
    }
}

extern "C" int PyTuple_SetItem(PyObject* op, Py_ssize_t i, PyObject* newitem) noexcept {
    RELEASE_ASSERT(PyTuple_Check(op), "");

    BoxedTuple* t = static_cast<BoxedTuple*>(op);
    RELEASE_ASSERT(i >= 0 && i < t->size(), "");
    t->elts[i] = newitem;
    return 0;
}

extern "C" PyObject* PyTuple_Pack(Py_ssize_t n, ...) noexcept {
    va_list vargs;

    va_start(vargs, n);
    PyObject* result = PyTuple_New(n);

    if (result == NULL) {
        va_end(vargs);
        return NULL;
    }

    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject* o = va_arg(vargs, PyObject*);
        PyTuple_SetItem(result, i, o);
    }
    va_end(vargs);
    return result;
}

extern "C" PyObject* PyTuple_New(Py_ssize_t size) noexcept {
    RELEASE_ASSERT(size >= 0, "");

    return BoxedTuple::create(size);
}


BoxedClass* tuple_iterator_cls = NULL;
extern "C" void tupleIteratorGCHandler(GCVisitor* v, Box* b) {
    boxGCHandler(v, b);
    BoxedTupleIterator* it = (BoxedTupleIterator*)b;
    v->visit(it->t);
}


void setupTuple() {
    tuple_iterator_cls = BoxedHeapClass::create(type_cls, object_cls, &tupleIteratorGCHandler, 0, 0,
                                                sizeof(BoxedTupleIterator), false, "tuple");

    tuple_cls->giveAttr("__new__", new BoxedFunction(boxRTFunction((void*)tupleNew, UNKNOWN, 1, 0, true, true)));
    CLFunction* getitem = createRTFunction(2, 0, 0, 0);
    addRTFunction(getitem, (void*)tupleGetitemInt, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, BOXED_INT });
    addRTFunction(getitem, (void*)tupleGetitemSlice, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, SLICE });
    addRTFunction(getitem, (void*)tupleGetitem, UNKNOWN, std::vector<ConcreteCompilerType*>{ UNKNOWN, UNKNOWN });
    tuple_cls->giveAttr("__getitem__", new BoxedFunction(getitem));

    tuple_cls->giveAttr("__contains__", new BoxedFunction(boxRTFunction((void*)tupleContains, BOXED_BOOL, 2)));
    tuple_cls->giveAttr("index", new BoxedFunction(boxRTFunction((void*)tupleIndex, BOXED_INT, 2)));

    tuple_cls->giveAttr("__iter__",
                        new BoxedFunction(boxRTFunction((void*)tupleIter, typeFromClass(tuple_iterator_cls), 1)));


    tuple_cls->giveAttr("__lt__", new BoxedFunction(boxRTFunction((void*)tupleLt, UNKNOWN, 2)));
    tuple_cls->giveAttr("__le__", new BoxedFunction(boxRTFunction((void*)tupleLe, UNKNOWN, 2)));
    tuple_cls->giveAttr("__gt__", new BoxedFunction(boxRTFunction((void*)tupleGt, UNKNOWN, 2)));
    tuple_cls->giveAttr("__ge__", new BoxedFunction(boxRTFunction((void*)tupleGe, UNKNOWN, 2)));
    tuple_cls->giveAttr("__eq__", new BoxedFunction(boxRTFunction((void*)tupleEq, UNKNOWN, 2)));
    tuple_cls->giveAttr("__ne__", new BoxedFunction(boxRTFunction((void*)tupleNe, UNKNOWN, 2)));

    tuple_cls->giveAttr("__nonzero__", new BoxedFunction(boxRTFunction((void*)tupleNonzero, BOXED_BOOL, 1)));

    tuple_cls->giveAttr("__hash__", new BoxedFunction(boxRTFunction((void*)tupleHash, BOXED_INT, 1)));
    tuple_cls->giveAttr("__len__", new BoxedFunction(boxRTFunction((void*)tupleLen, BOXED_INT, 1)));
    tuple_cls->giveAttr("__repr__", new BoxedFunction(boxRTFunction((void*)tupleRepr, STR, 1)));
    tuple_cls->giveAttr("__add__", new BoxedFunction(boxRTFunction((void*)tupleAdd, BOXED_TUPLE, 2)));
    tuple_cls->giveAttr("__mul__", new BoxedFunction(boxRTFunction((void*)tupleMul, BOXED_TUPLE, 2)));
    tuple_cls->giveAttr("__rmul__", new BoxedFunction(boxRTFunction((void*)tupleMul, BOXED_TUPLE, 2)));

    tuple_cls->freeze();

    CLFunction* hasnext = boxRTFunction((void*)tupleiterHasnextUnboxed, BOOL, 1);
    addRTFunction(hasnext, (void*)tupleiterHasnext, BOXED_BOOL);
    tuple_iterator_cls->giveAttr("__hasnext__", new BoxedFunction(hasnext));
    tuple_iterator_cls->giveAttr(
        "__iter__", new BoxedFunction(boxRTFunction((void*)tupleIterIter, typeFromClass(tuple_iterator_cls), 1)));
    tuple_iterator_cls->giveAttr("next", new BoxedFunction(boxRTFunction((void*)tupleiterNext, UNKNOWN, 1)));

    tuple_iterator_cls->freeze();
    tuple_iterator_cls->tpp_hasnext = tupleiterHasnextUnboxed;
}

void teardownTuple() {
    // TODO do clearattrs?
    // decref(tuple_iterator_cls);
}
}
