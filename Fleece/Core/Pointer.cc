//
// Pointer.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "Pointer.hh"
#include "Doc.hh"
#include <tuple>
#include <stdio.h>
#include "betterassert.hh"

using namespace std;

namespace fleece { namespace impl { namespace internal {

    Pointer::Pointer(size_t offset, int width, bool external)
    :Value(kPointerTagFirst, 0)
    {
        assert((offset & 1) == 0);
        offset >>= 1;
        if (width < internal::kWide) {
            throwIf(offset >= 0x4000, InternalError, "offset too large");
            if (external)
                offset |= 0x4000;
            setNarrowBytes((uint16_t)_enc16(offset | 0x8000)); // big-endian, high bit set
        } else {
            if (offset >= 0x40000000)
                FleeceException::_throw(OutOfRange, "data too large");
            if (external)
                offset |= 0x40000000;
            setWideBytes((uint32_t)_enc32(offset | 0x80000000));
        }
    }


    const Value* Pointer::_deref(uint32_t offset) const {
        assert(offset > 0);
        const Value *dst = offsetby(this, -(ptrdiff_t)offset);
        if (_usuallyFalse(isExternal())) {
            dst = Doc::resolvePointerFrom(this, dst);
            if (!dst)
                fprintf(stderr, "FATAL: Fleece extern pointer at %p, offset -%u,"
                        " did not resolve to any address\n", this, offset);
        }
        return dst;
    }


    const Value* Pointer::carefulDeref(bool wide,
                                       const void* &dataStart,
                                       const void* &dataEnd) const noexcept
    {
        uint32_t off = wide ? offset<true>() : offset<false>();
        if (off == 0)
            return nullptr;
        const Value *target = offsetby(this, -(ptrdiff_t)off);

        if (_usuallyFalse(isExternal())) {
            slice destination;
            tie(target, destination) = Doc::resolvePointerFromWithRange(this, target);
            if (_usuallyFalse(!target))
                return nullptr;
            assert_always((size_t(target) & 1) == 0);
            dataStart = destination.buf;
            dataEnd = destination.end();
        } else {
            if (_usuallyFalse(target < dataStart) || _usuallyFalse(target >= dataEnd))
                return nullptr;
            dataEnd = this;
        }

        if (_usuallyFalse(target->isPointer()))
            return target->_asPointer()->carefulDeref(true, dataStart, dataEnd);
        else
            return target;
    }


    bool Pointer::validate(bool wide, const void *dataStart) const noexcept{
        const void *dataEnd = this;
        const Value *target = carefulDeref(wide, dataStart, dataEnd);
        return target && target->validate(dataStart, dataEnd);
    }


} } }
