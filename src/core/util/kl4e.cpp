#include "kl4e.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace beta::util {

namespace {

// Reads one bit using arithmetic coding, with a given (updated)
// probability and its associated decay/bonus. Direct port of
// KL4E.java's read_bit.
int readBit(uint32_t& inputVal, uint32_t& range, uint8_t* probPtr, int probPtrOffset,
            const std::vector<uint8_t>& inBuf, uint32_t& inBufOffset, int decay, int bonus) {
    uint32_t prob = probPtr[probPtrOffset];
    uint32_t bound;
    if ((range >> 24) == 0) {
        inputVal = (inputVal << 8) + inBuf.at(inBufOffset++);
        bound = range * prob;
        range <<= 8;
    } else {
        bound = (range >> 8) * prob;
    }
    prob -= (prob >> decay);
    if (inputVal >= bound) {
        inputVal -= bound;
        range -= bound;
        probPtr[probPtrOffset] = static_cast<uint8_t>(prob);
        return 0;
    }
    range = bound;
    probPtr[probPtrOffset] = static_cast<uint8_t>(prob + bonus);
    return 1;
}

bool readBoolean(uint32_t& inputVal, uint32_t& range, uint8_t* probPtr, int probPtrOffset,
                  const std::vector<uint8_t>& inBuf, uint32_t& inBufOffset, int decay, int bonus) {
    return readBit(inputVal, range, probPtr, probPtrOffset, inBuf, inBufOffset, decay, bonus) != 0;
}

// Same as readBit but with a fixed, balanced 1/2 probability.
int readBitUniform(uint32_t& inputVal, uint32_t& range, const std::vector<uint8_t>& inBuf, uint32_t& inBufOffset) {
    if ((range >> 24) == 0) {
        inputVal = (inputVal << 8) + inBuf.at(inBufOffset++);
        range = range << 7;
    } else {
        range = range >> 1;
    }
    if (inputVal >= range) {
        inputVal -= range;
        return 0;
    }
    return 1;
}

// Same as readBitUniform, but without renormalizing the range.
int readBitUniformNoNormal(uint32_t& inputVal, uint32_t& range) {
    range >>= 1;
    if (inputVal >= range) {
        inputVal -= range;
        return 0;
    }
    return 1;
}

// Outputs one raw byte by reading 8 bits using arithmetic coding, using a
// per-bit-position probability context selected by `shift` and the
// previous output byte (`curByte`).
void outputRaw(uint32_t& inputVal, uint32_t& range, uint8_t* probs, int probsOffset,
               const std::vector<uint8_t>& inBuf, uint32_t& inBufOffset, uint32_t& curByte,
               std::vector<uint8_t>& curOut, uint32_t curOutOffset, int shift) {
    const int mask = (static_cast<int>(curOutOffset & 7) << 8) | (static_cast<int>(curByte) & 0xFF);
    const int curProbsOffset = probsOffset + (((mask >> shift) & 7) * 255) - 1;
    curByte = 1;
    while (curByte < 0x100) {
        const int curProbOffset = curProbsOffset + static_cast<int>(curByte);
        curByte = (curByte << 1) | static_cast<uint32_t>(readBit(inputVal, range, probs, curProbOffset, inBuf, inBufOffset, 3, 31));
    }
    curOut.at(curOutOffset) = static_cast<uint8_t>(curByte & 0xFF);
}

} // namespace

std::vector<uint8_t> decompressKl4e(const std::vector<uint8_t>& inBuf, std::size_t expectedSize,
                                     bool isKl4e, std::size_t* consumedOut) {
    std::vector<uint8_t> outBuf(expectedSize);
    std::array<uint8_t, 2040> litProbs{};
    std::array<uint8_t, 304> copyDistBitsProbs{};
    std::array<uint8_t, 144> copyDistProbs{};
    std::array<uint8_t, 64> copyCountBitsProbs{};
    std::array<uint8_t, 256> copyCountProbs{};

    const uint32_t outEndOffset = static_cast<uint32_t>(expectedSize);
    uint32_t curOutOffset = 0;
    uint32_t curByte = 0;
    uint32_t range = 0xFFFFFFFFu;
    int copyDist = 0;
    int copyCount;
    int curCopyDistBitsProbsOffset = 0;
    uint32_t inBufOffset = 0;

    uint32_t inputVal = (static_cast<uint32_t>(inBuf.at(1)) << 24) | (static_cast<uint32_t>(inBuf.at(2)) << 16) |
                         (static_cast<uint32_t>(inBuf.at(3)) << 8) | static_cast<uint32_t>(inBuf.at(4));

    // Handle the direct-copy case (the data isn't actually compressed).
    if ((inBuf.at(0) & 0x80) != 0) {
        inBufOffset += 5;
        const uint32_t dataEndOffset = inputVal;
        if (dataEndOffset >= outEndOffset) {
            throw std::runtime_error("decompressKl4e: invalid direct-copy size");
        }
        while (curOutOffset < dataEndOffset) {
            outBuf.at(curOutOffset++) = inBuf.at(inBufOffset++);
        }
        if (consumedOut) *consumedOut = inBufOffset;
        return outBuf;
    }

    // Initialize probabilities from the header value.
    const uint8_t initProb = static_cast<uint8_t>(128 - (((inBuf.at(0) >> 3) & 3) << 4));
    litProbs.fill(initProb);
    copyCountBitsProbs.fill(initProb);
    copyDistBitsProbs.fill(initProb);
    copyCountProbs.fill(initProb);
    copyDistProbs.fill(initProb);

    uint8_t* curCopyCountBitsProbs = copyCountBitsProbs.data();
    int curCopyCountBitsProbsOffset = 0;

    // Shift used to determine if probabilities lean more on the output's
    // byte alignment or on the previous byte.
    const int shift = inBuf.at(0) & 0x7;
    inBufOffset += 5;

    // Read the first literal directly.
    outputRaw(inputVal, range, litProbs.data(), 0, inBuf, inBufOffset, curByte, outBuf, curOutOffset, shift);

    while (true) {
        curOutOffset++;

        // A 0 bit means "read another literal".
        if (!readBoolean(inputVal, range, curCopyCountBitsProbs, curCopyCountBitsProbsOffset, inBuf, inBufOffset, 4, 15)) {
            curCopyCountBitsProbsOffset = std::max(curCopyCountBitsProbsOffset - 1, 0);
            if (curOutOffset == outEndOffset) {
                throw std::runtime_error("decompressKl4e: invalid size (ran out of output space on a literal)");
            }
            outputRaw(inputVal, range, litProbs.data(), 0, inBuf, inBufOffset, curByte, outBuf, curOutOffset, shift);
            continue;
        }

        // Otherwise, find the number of bits used in the "length" code.
        copyCount = 1;
        int copyCountBits = -1;
        while (copyCountBits < 6) {
            curCopyCountBitsProbsOffset += 8;
            if (!readBoolean(inputVal, range, curCopyCountBitsProbs, curCopyCountBitsProbsOffset, inBuf, inBufOffset, 4, 15)) {
                break;
            }
            copyCountBits++;
        }

        // Determine the length itself; which distance-code probabilities
        // apply depends on it (and on whether this is KL3E or KL4E).
        int powLimit = 0;
        if (copyCountBits >= 0) {
            uint8_t* probs = copyCountProbs.data();
            const int probsOffset = (copyCountBits << 5) | (((curOutOffset & 3) << (copyCountBits + 3)) & 0x18) | (curCopyCountBitsProbsOffset & 7);
            if (copyCountBits < 3) {
                copyCount = 1;
            } else {
                copyCount = 2 + readBit(inputVal, range, probs, probsOffset + 24, inBuf, inBufOffset, 3, 31);
                if (copyCountBits > 3) {
                    copyCount = (copyCount << 1) | readBit(inputVal, range, probs, probsOffset + 24, inBuf, inBufOffset, 3, 31);
                    if (copyCountBits > 4) {
                        copyCount = (copyCount << 1) | readBitUniform(inputVal, range, inBuf, inBufOffset);
                    }
                    for (int i = 5; i < copyCountBits; i++) {
                        copyCount = (copyCount << 1) | readBitUniformNoNormal(inputVal, range);
                    }
                }
            }
            copyCount = copyCount << 1;
            if (readBoolean(inputVal, range, probs, probsOffset, inBuf, inBufOffset, 3, 31)) {
                copyCount |= 1;
                if (copyCountBits <= 0) {
                    powLimit = isKl4e ? 256 : 128;
                    curCopyDistBitsProbsOffset = 56 + copyCountBits;
                }
            } else {
                if (copyCountBits <= 0) {
                    powLimit = 64;
                    curCopyDistBitsProbsOffset = copyCountBits;
                }
            }
            if (copyCountBits > 0) {
                copyCount = (copyCount << 1) | readBit(inputVal, range, probs, probsOffset + 8, inBuf, inBufOffset, 3, 31);
                if (copyCountBits != 1) {
                    copyCount = copyCount << 1;
                    if (readBoolean(inputVal, range, probs, probsOffset + 16, inBuf, inBufOffset, 3, 31)) {
                        copyCount = copyCount + 1;
                        if (copyCount == 0xFF) {
                            // End-of-stream marker: the real produced size is
                            // curOutOffset, which can be smaller than the
                            // caller's expectedSize (a safe upper bound, not
                            // a required exact target -- this format is
                            // self-terminating).
                            outBuf.resize(curOutOffset);
                            if (consumedOut) *consumedOut = inBufOffset;
                            return outBuf;
                        }
                    }
                }
                curCopyDistBitsProbsOffset = 56 + copyCountBits;
                powLimit = isKl4e ? 256 : 128;
            }
        } else {
            powLimit = 64;
            curCopyDistBitsProbsOffset = copyCountBits;
        }

        // Find the number of bits used for the distance code.
        int curPow = 8;
        bool skip = false;
        int copyDistBits;
        while (true) {
            const int curProbOffset = curCopyDistBitsProbsOffset + (curPow - 7);
            curPow <<= 1;
            copyDistBits = curPow - powLimit;
            if (!readBoolean(inputVal, range, copyDistBitsProbs.data(), curProbOffset, inBuf, inBufOffset, 3, 31)) {
                if (copyDistBits >= 0) {
                    if (copyDistBits != 0) {
                        copyDistBits -= 8;
                        break;
                    }
                    copyDist = 0;
                    skip = true; // just copy with a zero distance
                    break;
                }
            } else {
                curPow += 8;
                if (copyDistBits >= 0) {
                    break;
                }
            }
        }

        if (!skip) {
            // Find the distance itself.
            uint8_t* curProbs = copyDistProbs.data();
            const int curProbsOffset = copyDistBits;
            int readBits = copyDistBits / 8;
            if (readBits < 3) {
                copyDist = 1;
            } else {
                copyDist = 2 + readBit(inputVal, range, curProbs, curProbsOffset + 3, inBuf, inBufOffset, 3, 31);
                if (readBits > 3) {
                    copyDist = (copyDist << 1) | readBit(inputVal, range, curProbs, curProbsOffset + 3, inBuf, inBufOffset, 3, 31);
                    if (readBits > 4) {
                        copyDist = (copyDist << 1) | readBitUniform(inputVal, range, inBuf, inBufOffset);
                        readBits--;
                    }
                    while (readBits > 4) {
                        copyDist = copyDist << 1;
                        copyDist += readBitUniformNoNormal(inputVal, range);
                        readBits--;
                    }
                }
            }
            copyDist = copyDist << 1;
            if (readBoolean(inputVal, range, curProbs, curProbsOffset, inBuf, inBufOffset, 3, 31)) {
                if (readBits > 0) {
                    copyDist = copyDist + 1;
                }
            } else {
                if (readBits <= 0) {
                    copyDist = copyDist - 1;
                }
            }
            if (readBits > 0) {
                copyDist = copyDist << 1;
                if (readBoolean(inputVal, range, curProbs, curProbsOffset + 1, inBuf, inBufOffset, 3, 31)) {
                    if (readBits != 1) {
                        copyDist = copyDist + 1;
                    }
                } else {
                    if (readBits == 1) {
                        copyDist = copyDist - 1;
                    }
                }
                if (readBits != 1) {
                    copyDist = copyDist << 1;
                    if (!readBoolean(inputVal, range, curProbs, curProbsOffset + 2, inBuf, inBufOffset, 3, 31)) {
                        copyDist = copyDist - 1;
                    }
                }
            }

            if (static_cast<uint32_t>(copyDist) >= curOutOffset) {
                throw std::runtime_error("decompressKl4e: invalid format (copy distance out of range)");
            }
        }

        // Copy the bytes with the given count and distance.
        for (int i = 0; i < copyCount + 1; i++) {
            outBuf.at(curOutOffset + i) = outBuf.at(curOutOffset - copyDist - 1 + i);
        }
        curByte = outBuf.at(curOutOffset + copyCount);
        curOutOffset += copyCount;
        curCopyCountBitsProbs = copyCountBitsProbs.data();
        curCopyCountBitsProbsOffset = 6 + (curOutOffset & 1);
    }
}

} // namespace beta::util
