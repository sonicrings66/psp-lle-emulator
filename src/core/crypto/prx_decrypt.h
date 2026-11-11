#pragma once

#include <cstdint>
#include <vector>

#include "kirk.h"

namespace beta::crypto {

// Decrypts a real PSP module (a "~PSP"-tagged block, 0x150-byte header
// format) using whichever of PPSSPP's six known encode "types" its tag
// (at inbuf+0xD0) maps to -- ported from PPSSPP's Core/ELF/PrxDecrypter.cpp
// pspDecryptType0/1/2/5/6/9 and its pspDecryptPRX dispatcher, which tries
// each type in that order and returns the first that both matches the tag
// and passes its own SHA1 self-check.
//
// The individual decryptPrxTypeN functions below are exposed for testing
// and for cases where the type is already known; decryptPrx() is the
// general entry point most callers want. `size` must be at least 0x150
// (a full PSP module header). All of them return the number of decrypted
// payload bytes (the block's own "decryptSize" field, at inbuf+0xB0), or
// throw on failure (unsupported tag, or the block's own SHA1 self-check
// not matching).
//
// Only the tag/key table entries this project has actually encountered in
// real firmware (661.PBP) are populated (see prx_tags.h) -- a module using
// a tag outside that set throws rather than silently producing garbage.
int32_t decryptPrxType0(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size);
int32_t decryptPrxType1(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size);
int32_t decryptPrxType2(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size);
int32_t decryptPrxType5(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size);
int32_t decryptPrxType6(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size);
int32_t decryptPrxType9(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size);

// Tries each type in turn (0, 1, 2, 5, 6, 9) and returns the first that
// succeeds. Throws only if every type fails.
int32_t decryptPrx(KirkEngine& kirk, const uint8_t* inbuf, uint8_t* outbuf, uint32_t size);

// Decrypts a "~PSP" module (decryptPrx) and, if its header says the result
// is compressed, decompresses it too -- returning a plain ELF either way.
// Real PSP modules use one of a few compression schemes depending on
// firmware era; GZIP (util/inflate.h) is the one this project has actually
// encountered. KL4E/KL3E are Sony's own undocumented formats -- detected
// but not implemented (PPSSPP doesn't implement KL4E either).
std::vector<uint8_t> decryptModule(KirkEngine& kirk, const std::vector<uint8_t>& rawModule);

} // namespace beta::crypto
