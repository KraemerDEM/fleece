//
// Encoder.cc
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

#include "Encoder.hh"
#include "FleeceImpl.hh"
#include "Pointer.hh"
#include "SharedKeys.hh"
#include "MutableDict.hh"
#include "Endian.hh"
#include "varint.hh"
#include "FleeceException.hh"
#include "ParseDate.hh"
#include "PlatformCompat.hh"
#include "TempArray.hh"
#include <algorithm>
#include <cmath>
#include <float.h>
#include <stdlib.h>
#include "betterassert.hh"


namespace fleece { namespace impl {
    using namespace internal;

    typedef uint8_t byte;

    Encoder::Encoder(size_t reserveSize)
    :_out(reserveSize),
     _stack(kInitialStackSize),
     _strings(10)
    {
        push(kSpecialTag, 1);                   // Top-level 'array' is just a single item
    }

    Encoder::Encoder(FILE *outputFile)
    :_out(outputFile),
     _stack(kInitialStackSize),
     _strings(10)
    {
        push(kSpecialTag, 1);                   // Top-level 'array' is just a single item
    }

    Encoder::~Encoder() {
    }

    void Encoder::setSharedKeys(SharedKeys *s) {
        _sharedKeys = s;
    }

    void Encoder::setBase(slice base, bool markExternPointers, size_t cutoff) {
        _base = base;
        _baseCutoff = nullptr;
        if (base && cutoff > 0 && cutoff < base.size) {
            assert(cutoff >= 8);
            _baseCutoff = (char*)base.end() - cutoff;
        }
        _baseMinUsed = _base.end();
        _markExternPtrs = markExternPointers;
    }

    void Encoder::end() {
        if (!_items)
            return;
        throwIf(_stackDepth > 1, EncodeError, "unclosed array/dict");
        throwIf(_items->size() > 1, EncodeError, "top level must have only one value");

        if (_trailer && !_items->empty()) {
            checkPointerWidths(_items, nextWritePos());
            fixPointers(_items);
            Value &root = (*_items)[0];
            if (_items->wide) {
                _out.write(&root, kWide);
                // Top level Value is 4 bytes, so append a 2-byte pointer to it, because the trailer
                // needs to be a 2-byte Value:
                Pointer ptr(4, kNarrow);
                _out.write(&ptr, kNarrow);
            } else {
                _out.write(&root, kNarrow);
            }
            _items->clear();
        }
        _items = nullptr;
        _stackDepth = 0;
    }

    size_t Encoder::finishItem() {
        throwIf(_stackDepth > 1, EncodeError, "unclosed array/dict");
        throwIf(!_items || _items->empty(), EncodeError, "No item to end");

        size_t itemPos;
        const Value *item = &(*_items)[0];
        if (item->isPointer()) {
            itemPos = item->_asPointer()->offset<true>() - _base.size;
        } else {
            itemPos = nextWritePos();
            _out.write(item, (_items->wide ? kWide : kNarrow));
        }
        _items->clear();
        _items = nullptr;
        _stackDepth = 0;
        push(kSpecialTag, 1);
        return itemPos;
    }

    alloc_slice Encoder::finish() {
        end();
        alloc_slice out = _out.finish();
        if (out.size == 0)
            out.reset();
        return out;
    }

    Retained<Doc> Encoder::finishDoc() {
        Retained<Doc> doc = new Doc(finish(),
                                    Doc::kTrusted,
                                    _sharedKeys,
                                    (_markExternPtrs ? _base : nullslice));
        return doc;
    }

    // Returns position in the stream of the next write. Pads stream to even pos if necessary.
    size_t Encoder::nextWritePos() {
        size_t pos = _out.length();
        if (pos & 1) {
            _out.write("\0"_sl);
            pos++;
        }
        return pos;
    }

    void Encoder::reset() {
        if (_items) {
            _items->clear();
            _items = nullptr;
        }
        _out.reset();
        _stackDepth = 0;
        push(kSpecialTag, 1);
        _strings.clear();
        _writingKey = _blockedOnKey = false;
    }


#pragma mark - WRITING:

    void Encoder::addItem(Value v) {
        throwIf(_blockedOnKey, EncodeError, "need a key before this value");
        if (_writingKey) {
            _writingKey = false;
        } else {
            if (_items->tag == kDictTag)
                _blockedOnKey = _writingKey = true;
        }

        _items->push_back(v);
    }

    void Encoder::writeValue(tags tag, byte buf[], size_t size, bool canInline) {
        buf[0] |= tag << 4;
        writeRawValue(slice(buf, size), canInline);
        _out.padToEvenLength();
    }

    void Encoder::writeRawValue(slice rawValue, bool canInline) {
        if (canInline && rawValue.size <= 4) {
            if (rawValue.size < 4) {
                byte buf[4] = {0};      // zero the unused bytes
                memcpy(buf, rawValue.buf, rawValue.size);
                addItem(*(Value*)buf);
            } else {
                addItem(*(Value*)rawValue.buf);
            }
            if (rawValue.size > 2)
                _items->wide = true;
        } else {
            writePointer(nextWritePos());
            _out.write(rawValue.buf, rawValue.size);
        }
    }


#pragma mark - SCALARS:

    void Encoder::writeNull()              {addItem(Value(kSpecialTag, kSpecialValueNull));}
    void Encoder::writeBool(bool b)        {addItem(Value(kSpecialTag, b ? kSpecialValueTrue
                                                                         : kSpecialValueFalse));}
    void Encoder::writeInt(uint64_t i, bool isSmall, bool isUnsigned) {
        if (isSmall) {
            addItem(Value(kShortIntTag, (i >> 8) & 0x0F, i & 0xFF));
        } else {
            byte buf[10];
            size_t size = PutIntOfLength(&buf[1], i, isUnsigned);
            buf[0] = (byte)size - 1;
            if (isUnsigned)
                buf[0] |= 0x08;
            ++size;
            if (size & 1)
                buf[size++] = 0;  // pad to even size
            writeValue(kIntTag, buf, size);
        }
    }

    void Encoder::writeInt(int64_t i)   {writeInt(i, (i < 2048 && i >= -2048), false);}
    void Encoder::writeUInt(uint64_t i) {writeInt(i, (i < 2048),               true);}

    void Encoder::writeDouble(double n) {
        throwIf(std::isnan(n), InvalidData, "Can't write NaN");
        if (n == floor(n) && n <= INT64_MAX && n >= INT64_MIN) {
            return writeInt((int64_t)n);
        } else if (fabs(n) <= FLT_MAX && n == (float)n) {
            return _writeFloat((float)n);
        } else {
            littleEndianDouble swapped = n;
            uint8_t buf[2 + sizeof(swapped)];
            buf[0] = 0x08; // 'double' size flag
            buf[1] = 0;
            memcpy(&buf[2], &swapped, sizeof(swapped));
            writeValue(kFloatTag, buf, sizeof(buf));
        }
    }

    void Encoder::writeFloat(float n) {
        throwIf(std::isnan(n), InvalidData, "Can't write NaN");
        if (n == floorf(n) && n <= INT32_MAX && n >= INT32_MIN)
            writeInt((int32_t)n);
        else
            _writeFloat(n);
    }

    void Encoder::_writeFloat(float n) {
        littleEndianFloat swapped = n;
        uint8_t buf[2 + sizeof(swapped)];
        buf[0] = 0x00; // 'float' size flag
        buf[1] = 0;
        memcpy(&buf[2], &swapped, sizeof(swapped));
        writeValue(kFloatTag, buf, sizeof(buf));
    }


#pragma mark - STRINGS / DATA:

    // used for strings and binary data. Returns the location where s got written to, which
    // can be used until the enoding is over. (Unless it's inline, in which case s.buf is nullptr.)
    slice Encoder::writeData(tags tag, slice s) {
        uint8_t buf[4 + kMaxVarintLen64];
        buf[0] = (uint8_t)std::min(s.size, (size_t)0xF);
        const void *dst;
        if (s.size < kNarrow) {
            // Tiny data fits inline:
            memcpy(&buf[1], s.buf, s.size);
            writeValue(tag, buf, 1 + s.size);
            dst = nullptr;
        } else {
            // Large data doesn't:
            size_t bufLen = 1;
            if (s.size >= 0x0F)
                bufLen += PutUVarInt(&buf[1], s.size);
            if (s.size == 0)
                buf[bufLen++] = 0;
            buf[0] |= tag << 4;
            writeRawValue({buf, bufLen}, false);       // write header/count
            dst = _out.write(s.buf, s.size);
            _out.padToEvenLength();
        }
        return slice(dst, s.size);
    }

    // Returns the location where s got written to, if possible, just like writeData above.
    slice Encoder::_writeString(slice s) {
        // Check whether this string's already been written:
        if (_usuallyTrue(_uniqueStrings && s.size >= kNarrow && s.size <= kMaxSharedStringSize)) {
            auto &entry = _strings.find(s);
            if (entry.first.buf != nullptr) {
                // Write pointer to existing string, as long as the offset's not too large
                ssize_t offset = entry.second.offset - _base.size;
                if (_items->wide || nextWritePos() - offset <= Pointer::kMaxNarrowOffset - 32) {
                    writePointer(offset);
                    if (offset < 0) {
                        const void *stringVal = &_base[_base.size + offset];
                        if (stringVal < _baseMinUsed)
                            _baseMinUsed = stringVal;
                    }
    #ifndef NDEBUG
                    _numSavedStrings++;
    #endif
                    return entry.first;
                }
            }

            auto offset = _base.size + nextWritePos();
            throwIf(offset > 1u<<31, MemoryError, "encoded data too large");
            slice sWritten = writeData(kStringTag, s);
#if !FL_EMBEDDED
            if (!sWritten.buf)
                sWritten = slice{_stringStorage.write(s), s.size};
#endif
            if (sWritten.buf) {
#if 0
                if (_strings.count() == 0)
                    fprintf(stderr, "---- new encoder ----\n");
                fprintf(stderr, "Caching `%.*s` --> %u\n", (int)sWritten.size, sWritten.buf, offset);
#endif
                if (entry.first.buf == nullptr) {
                    // insert string
                    StringTable::info i = {(uint32_t)offset};
                    _strings.addAt(entry, sWritten, i);
                } else {
                    // replace string
                    entry.second.offset = (uint32_t)offset;
                }
            }
            return sWritten;

        } else {
            return writeData(kStringTag, s);
        }
    }

    // Adds a preexisting string to the cache
    void Encoder::cacheString(slice s, size_t offsetInBase) {
        if (_usuallyTrue(_uniqueStrings && s.size >= kNarrow && s.size <= kMaxSharedStringSize)) {            auto &entry = _strings.find(s);
            if (entry.first.buf == nullptr) {
                StringTable::info i = {(unsigned)offsetInBase};
                _strings.addAt(entry, s, i);
            }
        }
    }

    void Encoder::writeString(const std::string &s) {
        _writeString(slice(s));
    }

    void Encoder::writeData(slice s) {
        writeData(kBinaryTag, s);
    }


    void Encoder::reuseBaseStrings() {
        reuseBaseStrings(Value::fromTrustedData(_base));
    }

    void Encoder::reuseBaseStrings(const Value *value) {
        if (value < _baseCutoff)
            return;
        switch (value->tag()) {
            case kStringTag:
                cacheString(value->asString(), (size_t)value - (ssize_t)_base.buf);
                break;
            case kArrayTag:
                for (Array::iterator iter(value->asArray()); iter; ++iter)
                    reuseBaseStrings(iter.value());
                break;
            case kDictTag:
                for (Dict::iterator iter(value->asDict()); iter; ++iter) {
                    reuseBaseStrings(iter.key());
                    reuseBaseStrings(iter.value());
                }
                break;
            default:
                break;
        }
    }


    void Encoder::writeDateString(int64_t timestamp, bool asUTC) {
        char str[kFormattedISO8601DateMaxSize];
        writeString(FormatISO8601Date(str, timestamp, asUTC));
    }


#pragma mark - WRITING VALUES:


    bool Encoder::isNarrowValue(const Value *value) {
        if (value->tag() >= kArrayTag)
            return value->countIsZero();
        else
            return value->dataSize() <= kNarrow;
    }


    // Returns the minimum address used by the given Value (transitively).
    // If that minimum address comes before _baseCutoff, immediately returns null.
    const Value* Encoder::minUsed(const Value *value) {
        if (value < _baseCutoff)
            return nullptr;
        switch (value->type()) {
        case kArray: {
            const Value *minVal = value;
            for (Array::iterator i((const Array*)value); i; ++i) {
                minVal = std::min(minVal, minUsed(i.value()));
                if (minVal == nullptr)
                    break;
            }
            return minVal;
        }
        case kDict: {
            const Value *minVal = value;
            for (Dict::iterator i((const Dict*)value, false); i; ++i) {
                minVal = std::min(minVal, minUsed(i.key()));
                minVal = std::min(minVal, minUsed(i.value()));
                if (minVal == nullptr)
                    break;
            }
            return minVal;
        }
        default:
            return value;
        }
    }


    void Encoder::writeValue(const Value *value,
                             const SharedKeys* &sk,
                             const WriteValueFunc *writeNestedValue)
    {
        if (valueIsInBase(value) && !isNarrowValue(value)) {
            auto minVal = minUsed(value);
            if (minVal >= _baseCutoff) {
                // Value is in the base data, and close enough; I can just emit a pointer to it:
                writePointer( (ssize_t)value - (ssize_t)_base.end() );
                if (minVal && minVal < _baseMinUsed)
                    _baseMinUsed = minVal;
                return;
            }
        }
        switch (value->tag()) {
            case kShortIntTag:
            case kIntTag:
            case kFloatTag:
            case kSpecialTag:
                writeRawValue(slice(value, value->dataSize()));
                _out.padToEvenLength();
                break;
            case kStringTag:
                writeString(value->asString());
                break;
            case kBinaryTag:
                writeData(value->asData());
                break;
            case kArrayTag: {
                ++_copyingCollection;
                auto iter = value->asArray()->begin();
                beginArray(iter.count());
                for (; iter; ++iter) {
                    if (!writeNestedValue || !(*writeNestedValue)(nullptr, iter.value()))
                        writeValue(iter.value(), sk, writeNestedValue);
                }
                endArray();
                --_copyingCollection;
                break;
            }
            case kDictTag: {
                ++_copyingCollection;
                auto dict = (const Dict*)value;
                if (dict->isMutable()) {
                    dict->heapDict()->writeTo(*this/*, writeNestedValue*/);
                } else {
                    auto iter = dict->begin();
                    beginDictionary(iter.count());
                    for (; iter; ++iter) {
                        if (!writeNestedValue || !(*writeNestedValue)(iter.key(), iter.value())) {
                            if (!sk && iter.key()->isInteger())
                                sk = value->sharedKeys();
                            writeKey(iter.key(), sk);
                            writeValue(iter.value(), sk, writeNestedValue);
                        }
                    }
                    endDictionary();
                }
                --_copyingCollection;
                break;
            }
            default:
                FleeceException::_throw(UnknownValue, "illegal tag in Value; corrupt data?");
        }
    }


    void Encoder::writeValue(const Value* NONNULL value, const WriteValueFunc *fn) {
        const SharedKeys *sk = nullptr;
        writeValue(value, sk, fn);
    }



#pragma mark - POINTERS:


    // Pointers are added here as absolute positions in the stream, then fixed up before they're
    // written to the output. This is because we don't know yet what the position of the pointer
    // itself will be.
    // When there's a base (i.e. we're writing a delta), we calculate the absolute position as
    // being from the start of the base data. That way we can represent pointers back into the base
    // without having to write negative numbers as positions.

    bool Encoder::valueIsInBase(const Value *value) const {
        return _base && value >= _base.buf && value < _base.end();
    }

    // Parameter p is an offset into the current stream, not taking into account the base.
    void Encoder::writePointer(ssize_t p)   {
        addItem(Pointer(_base.size + p, kWide));
    }

    // Check whether any pointers in _items can't fit in a narrow Value:
    void Encoder::checkPointerWidths(valueArray *items, size_t pointerOrigin) {
        if (!items->wide) {
            for (Value &v : *items) {
                if (v.isPointer()) {
                    ssize_t pos = v._asPointer()->offset<true>() - _base.size;
                    if (pointerOrigin - pos > Pointer::kMaxNarrowOffset) {
                        items->wide = true;
                        break;
                    }
                }
                pointerOrigin += kNarrow;
            }
        }
    }

    // Convert absolute offsets to relative in _items:
    void Encoder::fixPointers(valueArray *items) {
        size_t pointerOrigin = nextWritePos();
        int width = items->wide ? kWide : kNarrow;
        for (Value &v : *items) {
            if (v.isPointer()) {
                ssize_t pos = v._asPointer()->offset<true>() - _base.size;
                assert(pos < (ssize_t)pointerOrigin);
                bool isExternal = (pos < 0);
                v = Pointer(pointerOrigin - pos, width, isExternal && _markExternPtrs);
            }
            pointerOrigin += width;
        }
    }

#pragma mark - ARRAYS / DICTIONARIES:

    void Encoder::addingKey() {
        if (_usuallyFalse(!_blockedOnKey)) {
            if (_items->tag == kDictTag)
                FleeceException::_throw(EncodeError, "need a value after a key");
            else
                FleeceException::_throw(EncodeError, "not writing a dictionary");
        }
        _blockedOnKey = false;
    }

    void Encoder::writeKey(const std::string &s) {
        writeKey(slice(s));
    }

    void Encoder::writeKey(slice s) {
        int encoded;
        if (_sharedKeys && _sharedKeys->encodeAndAdd(s, encoded)) {
            writeKey(encoded);
            return;
        }
        addingKey();
        slice writtenKey = _writeString(s);
        if (!writtenKey.buf && _copyingCollection)
            writtenKey = s;         // Workaround for written strings not being kept in memory by the Writer if it's writing to a file
        addedKey(writtenKey);
    }

    void Encoder::writeKey(int n) {
        throwIf(!_sharedKeys && n != Dict::kMagicParentKey
#ifndef NDEBUG
                             && !gDisableNecessarySharedKeysCheck
#endif
                ,EncodeError, "Can't add numeric key without SharedKeys");
        addingKey();
        writeInt(n);
        addedKey(nullslice);
    }

    void Encoder::writeKey(const Value *key) {
        slice str = key->asString();
        if (str) {
            int encoded;
            if (_sharedKeys && _sharedKeys->encodeAndAdd(str, encoded)) {
                writeKey(encoded);
                return;
            }
            addingKey();
            writeValue(key, nullptr);
            addedKey(str);
        } else {
            throwIf(!key->isInteger(), InvalidData, "Key must be a string or integer");
            writeKey((int)key->asInt());
        }
    }

    void Encoder::writeKey(const Value *key, const SharedKeys *sk) {
        if (key->isInteger()) {
            throwIf(!sk, EncodeError, "Numeric key given without SharedKeys");
            int intKey = (int)key->asInt();
            if (sk != _sharedKeys) {
                slice keySlice = sk->decode(intKey);
                throwIf(!keySlice, InvalidData, "Unrecognized integer key");
                writeKey(keySlice);
            } else {
                writeKey(intKey);
            }
        } else {
            writeKey(key);
        }
    }

    void Encoder::addedKey(slice str) {
        // Note: str will be nullslice iff the key is numeric
        _items->keys.push_back(str);
    }

    void Encoder::push(tags tag, size_t reserve) {
        if (_usuallyFalse(_stackDepth >= _stack.size()))
            _stack.resize(2*_stackDepth);
        _items = &_stack[_stackDepth++];
        _items->reset(tag);
        if (reserve > 0) {
            _items->reserve(reserve);
            if (_usuallyTrue(tag == kDictTag)) {
                _items->reserve(2 * reserve);
                _items->keys.reserve(reserve);
            } else {
                _items->reserve(reserve);
            }
        }
    }

    void Encoder::pop() {
        --_stackDepth;
        _items = &_stack[_stackDepth - 1];
    }

    void Encoder::beginArray(size_t reserve) {
        push(kArrayTag, reserve);
    }

    void Encoder::beginDictionary(size_t reserve) {
        push(kDictTag, 2*reserve);
        _writingKey = _blockedOnKey = true;
    }

    void Encoder::beginDictionary(const Dict *parent, size_t reserve) {
        throwIf(!valueIsInBase(parent), EncodeError, "parent is not in base");
        beginDictionary(1 + reserve);
        writeKey(Dict::kMagicParentKey);
        writeValue(parent);
    }

    void Encoder::endArray() {
        endCollection(internal::kArrayTag);
    }

    void Encoder::endDictionary() {
        throwIf(!_writingKey, EncodeError, "need a value");
        endCollection(internal::kDictTag);
    }

    void Encoder::endCollection(tags tag) {
        if (_usuallyFalse(_items->tag != tag)) {
            if (_items->tag == kSpecialTag)
                FleeceException::_throw(EncodeError, "endCollection: not in a collection");
            else
                FleeceException::_throw(EncodeError, "ending wrong type of collection");
        }

        // Pop _items off the stack:
        valueArray *items = _items;
        pop();
        _writingKey = _blockedOnKey = false;

        if (tag == kDictTag)
            sortDict(*items);

        auto nValues = items->size();    // includes keys if this is a dict!
        auto count = (uint32_t)nValues;
        if (items->tag == kDictTag)
            count /= 2;

        // Write the array header to the outer Value:
        uint8_t buf[2 + kMaxVarintLen32];
        uint32_t inlineCount = std::min(count, (uint32_t)kLongArrayCount);
        buf[0] = (uint8_t)(inlineCount >> 8);
        buf[1] = (uint8_t)(inlineCount & 0xFF);
        size_t bufLen = 2;
        if (count >= kLongArrayCount) {
            bufLen += PutUVarInt(&buf[2], count - kLongArrayCount);
            if (bufLen & 1)
                buf[bufLen++] = 0;
        }

        checkPointerWidths(items, nextWritePos() + bufLen);

        if (items->wide)
            buf[0] |= 0x08;     // "wide" flag
        writeValue(items->tag, buf, bufLen, (count==0));          // can inline only if empty

        fixPointers(items);

        // Write the values:
        if (nValues > 0) {
            if (items->wide) {
                _out.write(&(*items)[0], kWide*nValues);
            } else {
                TempArray(narrow, uint16_t, nValues);
                size_t i = 0;
                for (auto &v : *items) {
                    ::memcpy(&narrow[i++], &v, kNarrow);
                }
                _out.write(narrow, kNarrow*nValues);
            }
        }

#ifndef NDEBUG
        if (items->wide) {
            _numWide++;
            _wideCount += count;
        } else {
            _numNarrow++;
            _narrowCount += count;
        }
#endif

        items->clear();
    }

    // compares dictionary keys as slices. If a slice has a null `buf`, it represents an integer
    // key, whose value is in the `size` field.
    static inline int compareKeysByIndex(const slice *sa, const slice *sb) {
        if (sa->buf) {
            if (sb->buf)
                return sa->compare(*sb) < 0;                // string key comparison
            else
                return false;
        } else {
            if (sb->buf)
                return true;
            else
                return (int)sa->size < (int)sb->size;       // integer key comparison
        }
    }

    void Encoder::sortDict(valueArray &items) {
        auto &keys = items.keys;
        size_t n = keys.size();
        if (n < 2)
            return;

        // Fill in the pointers of any keys that refer to inline strings:
        for (unsigned i = 0; i < n; i++) {
            if (keys[i].buf == nullptr) {
                const Value *item = &items[2*i];
                if (item->tag() == kStringTag) {
                    keys[i].setBuf(offsetby(item, 1));                      // inline string
                } else {
                    assert(item->tag() == kShortIntTag);
                    keys[i] = slice(nullptr, (size_t)item->asUnsigned());   // integer
                }
            }
        }

        // Construct an array that describes the permutation of item indices:
        TempArray(indices, const slice*, n);
        const slice* base = &keys[0];
        for (unsigned i = 0; i < n; i++)
            indices[i] = base + i;
        std::sort(&indices[0], &indices[n], &compareKeysByIndex);
        // indices[i] is now a pointer to the Value that should go at index i

        // Now rewrite items according to the permutation in indices:
        TempArray(oldBuf, char, 2*n * sizeof(Value));
        auto old = (Value*)oldBuf;
        memcpy(old, &items[0], 2*n * sizeof(Value));
        for (size_t i = 0; i < n; i++) {
            auto j = indices[i] - base;
            if ((ssize_t)i != j) {
                items[2*i]   = old[2*j];
                items[2*i+1] = old[2*j+1];
            }
        }
    }

} }
