#include "kirk.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace beta::crypto {

KirkEngine::KirkEngine(const uint32_t* deterministicPrngTime) {
    // Must be set before kirk_init() below: it seeds PRNG_DATA with real
    // time(0) right there, not just on a later real KIRK_CMD_PRNG call.
    if (deterministicPrngTime) {
        state_.use_deterministic_prng_time = 1;
        state_.deterministic_prng_time = *deterministicPrngTime;
    }
    if (kirk_init(&state_) != 0) {
        throw std::runtime_error("kirk_init failed");
    }
}

KirkEngine::KirkEngine(uint32_t fuseId90, uint32_t fuseId94, const uint32_t* deterministicPrngTime) {
    if (deterministicPrngTime) {
        state_.use_deterministic_prng_time = 1;
        state_.deterministic_prng_time = *deterministicPrngTime;
    }
    // Same lazy PRNG seed kirk_init() itself uses (kirk_engine.c's own
    // kirk_init() forwards to kirk_init2 with this exact string) -- only
    // the fuse values differ from the default constructor.
    const uint8_t seed[] = "Lazy Dev should have initialized!";
    if (kirk_init2(&state_, const_cast<uint8_t*>(seed), sizeof(seed) - 1, fuseId90, fuseId94) != 0) {
        throw std::runtime_error("kirk_init2 failed");
    }
}

std::vector<uint8_t> KirkEngine::encryptCbc(const std::vector<uint8_t>& plain, int keyId) {
    if (plain.size() % 16 != 0) {
        throw std::invalid_argument("KIRK AES-CBC data size must be a multiple of 16 bytes");
    }

    KIRK_AES128CBC_HEADER header{};
    header.mode = KIRK_MODE_ENCRYPT_CBC;
    header.keyseed = keyId;
    header.data_size = static_cast<int>(plain.size());

    std::vector<uint8_t> inbuf(sizeof(header) + plain.size());
    std::memcpy(inbuf.data(), &header, sizeof(header));
    std::memcpy(inbuf.data() + sizeof(header), plain.data(), plain.size());

    std::vector<uint8_t> outbuf(sizeof(header) + plain.size());
    const int result = kirk_CMD4(&state_, outbuf.data(), inbuf.data(), static_cast<int>(inbuf.size()));
    if (result != KIRK_OPERATION_SUCCESS) {
        throw std::runtime_error("kirk_CMD4 failed with code " + std::to_string(result));
    }

    return std::vector<uint8_t>(outbuf.begin() + sizeof(header), outbuf.end());
}

std::vector<uint8_t> KirkEngine::decryptCbc(const std::vector<uint8_t>& cipher, int keyId) {
    if (cipher.size() % 16 != 0) {
        throw std::invalid_argument("KIRK AES-CBC data size must be a multiple of 16 bytes");
    }

    KIRK_AES128CBC_HEADER header{};
    header.mode = KIRK_MODE_DECRYPT_CBC;
    header.keyseed = keyId;
    header.data_size = static_cast<int>(cipher.size());

    std::vector<uint8_t> inbuf(sizeof(header) + cipher.size());
    std::memcpy(inbuf.data(), &header, sizeof(header));
    std::memcpy(inbuf.data() + sizeof(header), cipher.data(), cipher.size());

    std::vector<uint8_t> outbuf(cipher.size());
    const int result = kirk_CMD7(&state_, outbuf.data(), inbuf.data(), static_cast<int>(inbuf.size()));
    if (result != KIRK_OPERATION_SUCCESS) {
        throw std::runtime_error("kirk_CMD7 failed with code " + std::to_string(result));
    }

    return outbuf;
}

std::vector<uint8_t> KirkEngine::sha1(const std::vector<uint8_t>& data) {
    KIRK_SHA1_HEADER header{};
    header.data_size = static_cast<int>(data.size());

    std::vector<uint8_t> inbuf(sizeof(header) + data.size());
    std::memcpy(inbuf.data(), &header, sizeof(header));
    std::memcpy(inbuf.data() + sizeof(header), data.data(), data.size());

    std::vector<uint8_t> digest(20);
    const int result = kirk_CMD11(&state_, digest.data(), inbuf.data(), static_cast<int>(inbuf.size()));
    if (result != KIRK_OPERATION_SUCCESS) {
        throw std::runtime_error("kirk_CMD11 failed with code " + std::to_string(result));
    }

    return digest;
}

int KirkEngine::command(uint8_t* outbuff, int outsize, uint8_t* inbuff, int insize, int cmd) {
    return kirk_sceUtilsBufferCopyWithRange(&state_, outbuff, outsize, inbuff, insize, cmd);
}

} // namespace beta::crypto
