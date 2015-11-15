//
//  Encoder.cc
//  Fleece
//
//  Created by Jens Alfke on 1/26/15.
//  Copyright (c) 2015 Couchbase. All rights reserved.
//

#include "Encoder.hh"
#include "Endian.h"
#include "varint.hh"
#include <algorithm>
#include <assert.h>


namespace fleece {

    static const size_t kMaxSharedStringSize = 100;

    // root encoder
    encoder::encoder(Writer& out)
    :_parent(NULL),
     _offset(0),
     _keyOffset(0),
     _count(1),
     _out(out),
     _strings(new stringTable),
     _writingKey(false),
     _blockedOnKey(false)
    { }

    encoder::encoder(encoder *parent, size_t offset, size_t keyOffset, size_t count)
    :_parent(parent),
     _offset(offset),
     _keyOffset(keyOffset),
     _count(count),
     _out(parent->_out),
     _strings(parent->_strings),
     _writingKey(keyOffset > 0),
     _blockedOnKey(_writingKey)
    { }

    encoder::~encoder() {
        if (!_parent)
            delete _strings;
    }

    void encoder::reset() {
        if (_parent)
            throw "can only reset root encoder";
        _count = 1;
        _out = Writer();
        delete _strings;
        _strings = new stringTable;
    }

    // primitive to write a value
    void encoder::writeValue(value::tags tag, uint8_t *buf, size_t size, bool canInline) {
        if (_count == 0)
            throw "no more space in collection";
        if (_blockedOnKey)
            throw "need a key before this value";

        if (tag < value::kPointerTagFirst) {
            assert((buf[0] & 0xF0) == 0);
            buf[0] |= tag<<4;
        }

        if (_parent) {
            size_t &offset = _writingKey ? _keyOffset : _offset;
            if (size <= 2 && canInline) {
                // Add directly to parent collection at offset:
                _out.rewrite(offset, slice(buf,size));
            } else {
                // Write to output, then add a pointer in the parent collection:
                uint8_t ptr[2];
                if (!makePointer(_out.length(), ptr))
                    throw "delta too large to write value";
                _out.rewrite(offset, slice(ptr,2));
                _out.write(buf, size);
            }
            offset += 2;
        } else {
            // Root element: just write it
            _out.write(buf, size);
        }

        if (_writingKey) {
            _writingKey = false;
        } else {
            --_count;
            if (_keyOffset > 0)
                _blockedOnKey = _writingKey = true;
        }
    }

    bool encoder::writePointerTo(uint64_t dstOffset) {
        uint8_t buf[2];
        if (!makePointer(dstOffset, buf))
            return false;
        writeValue(value::kPointerTagFirst, buf, 2);
        return true;
    }

    bool encoder::makePointer(uint64_t toOffset, uint8_t buf[2]) {
        size_t fromOffset = _writingKey ? _keyOffset : _offset;
        ssize_t delta = (toOffset - fromOffset) / 2;
        if (delta < -0x4000 || delta >= 0x4000)
            return false;
        int16_t ptr = _enc16(delta);
        memcpy(buf, &ptr, 2);
        buf[0] |= 0x80;  // tag it
        return true;
    }

    inline void encoder::writeSpecial(uint8_t special) {
        uint8_t buf[2] = {0, special};
        writeValue(value::kSpecialTag, buf, 2);
    }

    void encoder::writeNull() {
        writeSpecial(value::kSpecialValueNull);
    }

    void encoder::writeBool(bool b) {
        writeSpecial(b ? value::kSpecialValueTrue : value::kSpecialValueFalse);
    }

    void encoder::writeInt(int64_t i, bool isUnsigned) {
        if (i >= -2048 && i < 2048) {
            uint8_t buf[2] = {(uint8_t)((i >> 8) & 0x0F),
                              (uint8_t)(i & 0xFF)};
            writeValue(value::kShortIntTag, buf, 2);
        } else {
            uint8_t buf[10];
            size_t size = PutIntOfLength(&buf[1], i);
            buf[0] = size - 1;
            if (isUnsigned)
                buf[0] |= 0x08;
            ++size;
            if (size & 1)
                buf[size++] = 0;  // pad to even size
            writeValue(value::kIntTag, buf, size);
        }
    }

    void encoder::writeDouble(double n) {
        if (isnan(n))
            throw "Can't write NaN";
        if (n == (int64_t)n) {
            return writeInt((int64_t)n);
        } else {
            littleEndianDouble swapped = n;
            uint8_t buf[2 + sizeof(swapped)];
            buf[0] = sizeof(swapped);
            buf[1] = 0;
            memcpy(&buf[2], &swapped, sizeof(swapped));
            writeValue(value::kFloatTag, buf, sizeof(buf));
        }
    }

    void encoder::writeFloat(float n) {
        if (isnan(n))
            throw "Can't write NaN";
        if (n == (int32_t)n) {
            return writeInt((int32_t)n);
        } else {
            littleEndianFloat swapped = n;
            uint8_t buf[2 + sizeof(swapped)];
            buf[0] = sizeof(swapped);
            buf[1] = 0;
            memcpy(&buf[2], &swapped, sizeof(swapped));
            writeValue(value::kFloatTag, buf, sizeof(buf));
        }
    }

    // used for strings and binary data
    void encoder::writeData(value::tags tag, slice s) {
        uint8_t buf[2 + kMaxVarintLen64];
        buf[0] = std::min(s.size, (size_t)0xF);
        if (s.size <= 1) {
            // Tiny data fits inline:
            if (s.size == 1)
                buf[1] = s[0];
            writeValue(tag, buf, 2, true);
        } else {
            // Large data doesn't:
            size_t bufLen = 1;
            if (s.size >= 0x0F)
                bufLen += PutUVarInt(&buf[1], s.size);
            writeValue(tag, buf, bufLen, false);
            _out << s;
        }
    }

    void encoder::writeString(std::string s) {
        // Check whether this string's already been written:
        if (s.size() > 1 && s.size() < kMaxSharedStringSize) {
            auto entry = _strings->find(s);
            if (entry != _strings->end()) {
                uint64_t offset = entry->second;
                if (writePointerTo(offset))
                    return;
                entry->second = _out.length();      // update with offset of new instance of string
            } else {
                _strings->insert({s, _out.length()});
            }
        }
        writeData(value::kStringTag, slice(s));
    }

    void encoder::writeString(slice s)      {writeString((std::string)s);}

    void encoder::writeData(slice s)        {writeData(value::kBinaryTag, s);}

    encoder encoder::writeArrayOrDict(value::tags tag, uint32_t count) {
        // Write the array/dict header (2 bytes):
        uint8_t buf[2 + kMaxVarintLen32];
        uint32_t inlineCount = std::min(count, (uint32_t)0x0FFF);
        buf[0] = inlineCount >> 8;
        buf[1] = inlineCount & 0xFF;
        size_t bufLen = 2;
        if (count >= 0x0FFF) {
            bufLen += PutUVarInt(&buf[2], count);
            if (bufLen & 1)
                buf[bufLen++] = 0;
        }
        writeValue(tag, buf, bufLen, (count==0));          // can inline only if empty

        // Reserve space for the values (and keys, for dicts):
        size_t offset = _out.length();
        size_t keyOffset = 0;
        size_t space = 2*count;
        if (tag == value::kDictTag) {
            keyOffset = offset;
            offset += 2*count;
            space *= 2;
        }
        _out.reserveSpace(space);

        return encoder(this, offset, keyOffset, count);
    }

    void encoder::writeKey(std::string s)   {writeKey(slice(s));}

    void encoder::writeKey(slice s) {
        if (!_blockedOnKey)
            throw _keyOffset>0 ? "need a value after a key" : "not a dictionary";
        _blockedOnKey = false;
        writeString(s);
    }

    void encoder::end() {
        if (_count > 0)
            throw "not all items were written";
    }

}