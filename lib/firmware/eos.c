/*
 * This file is part of the KeepKey project.
 *
 * Copyright (C) 2018 KeepKey
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "keepkey/firmware/eos.h"

#include "keepkey/board/confirm_sm.h"
#include "keepkey/firmware/fsm.h"
#include "trezor/crypto/base58.h"
#include "trezor/crypto/bip32.h"
#include "trezor/crypto/hasher.h"
#include "trezor/crypto/memzero.h"
#include "trezor/crypto/secp256k1.h"

#include "messages-eos.pb.h"

static bool inited = false;
static CONFIDENTIAL Hasher hasher_preimage;
static CONFIDENTIAL HDNode node;
static EosTxHeader header;
static uint32_t actions_remaining = 0;

bool eos_formatAsset(const EosAsset *asset, char str[EOS_ASSET_STR_SIZE]) {
    memset(str, 0, EOS_ASSET_STR_SIZE);
    char *s = str;
    uint64_t v = (uint64_t)asset->amount;

    // Sign
    if (asset->amount < 0)                      { *s++ = '-'; v = ~v + 1; }

    // Value. Precision stored in low 8 bits
    uint8_t p = asset->symbol & 0xff;
    if (v > 10000000000000000000ULL || p >= 19) { *s++ = '0' + v / 10000000000000000000ULL % 10; }
    if (                               p == 19) { *s++ = '.'; }
    if (v > 1000000000000000000ULL  || p >= 18) { *s++ = '0' + v / 1000000000000000000ULL  % 10; }
    if (                               p == 18) { *s++ = '.'; }
    if (v > 100000000000000000ULL   || p >= 17) { *s++ = '0' + v / 100000000000000000ULL   % 10; }
    if (                               p == 17) { *s++ = '.'; }
    if (v > 10000000000000000ULL    || p >= 16) { *s++ = '0' + v / 10000000000000000ULL    % 10; }
    if (                               p == 16) { *s++ = '.'; }
    if (v > 1000000000000000ULL     || p >= 15) { *s++ = '0' + v / 1000000000000000ULL     % 10; }
    if (                               p == 15) { *s++ = '.'; }
    if (v > 100000000000000ULL      || p >= 14) { *s++ = '0' + v / 100000000000000ULL      % 10; }
    if (                               p == 14) { *s++ = '.'; }
    if (v > 10000000000000ULL       || p >= 13) { *s++ = '0' + v / 10000000000000ULL       % 10; }
    if (                               p == 13) { *s++ = '.'; }
    if (v > 1000000000000ULL        || p >= 12) { *s++ = '0' + v / 1000000000000ULL        % 10; }
    if (                               p == 12) { *s++ = '.'; }
    if (v > 100000000000ULL         || p >= 11) { *s++ = '0' + v / 100000000000ULL         % 10; }
    if (                               p == 11) { *s++ = '.'; }
    if (v > 10000000000ULL          || p >= 10) { *s++ = '0' + v / 10000000000ULL          % 10; }
    if (                               p == 10) { *s++ = '.'; }
    if (v > 1000000000ULL           || p >=  9) { *s++ = '0' + v / 1000000000ULL           % 10; }
    if (                               p ==  9) { *s++ = '.'; }
    if (v > 100000000ULL            || p >=  8) { *s++ = '0' + v / 100000000ULL            % 10; }
    if (                               p ==  8) { *s++ = '.'; }
    if (v > 10000000ULL             || p >=  7) { *s++ = '0' + v / 10000000ULL             % 10; }
    if (                               p ==  7) { *s++ = '.'; }
    if (v > 1000000ULL              || p >=  6) { *s++ = '0' + v / 1000000ULL              % 10; }
    if (                               p ==  6) { *s++ = '.'; }
    if (v > 100000ULL               || p >=  5) { *s++ = '0' + v / 100000ULL               % 10; }
    if (                               p ==  5) { *s++ = '.'; }
    if (v > 10000ULL                || p >=  4) { *s++ = '0' + v / 10000ULL                % 10; }
    if (                               p ==  4) { *s++ = '.'; }
    if (v > 1000ULL                 || p >=  3) { *s++ = '0' + v / 1000ULL                 % 10; }
    if (                               p ==  3) { *s++ = '.'; }
    if (v > 100ULL                  || p >=  2) { *s++ = '0' + v / 100ULL                  % 10; }
    if (                               p ==  2) { *s++ = '.'; }
    if (v > 10ULL                   || p >=  1) { *s++ = '0' + v / 10ULL                   % 10; }
    if (                               p ==  1) { *s++ = '.'; }
                                                  *s++ = '0' + v                           % 10;
    *s++ = ' ';

    // Symbol
    for (int i = 0; i < 7; i++) {
        char c = (char)((asset->symbol >> (i+1)*8) & 0xff);
        if (!('A' <= c && c <= 'Z') && c != 0) {
            memset(str, 0, EOS_ASSET_STR_SIZE);
            return false; // Invalid symbol
        }
        *s++ = c;
    }

    return true;
}

/// Ported from EOSIO libraries/chain/name.cpp
bool eos_formatName(uint64_t name, char str[EOS_NAME_STR_SIZE]) {
    memset(str, '.', EOS_NAME_STR_SIZE);
    static const char *charmap = ".12345abcdefghijklmnopqrstuvwxyz";

    uint64_t tmp = name;
    for (uint32_t i = 0; i <= 12; ++i) {
        char c = charmap[tmp & (i == 0 ? 0x0f : 0x1f)];
        str[12-i] = c;
        tmp >>= (i == 0 ? 4 : 5);
    }

    for (uint32_t i = EOS_NAME_STR_SIZE - 1; str[i] == '.'; --i) {
        str[i] = '\0';
    }

    return true;
}

bool eos_getPublicKey(const HDNode *n, const curve_info *curve, char *pubkey, size_t len) {
    const char *prefix = "EOS_K1_";
    const size_t prefix_len = strlen(prefix);
    strlcpy(pubkey, prefix, len);

    if (!base58_encode_check(n->public_key, 33, curve->hasher_base58,
                             pubkey + prefix_len,
                             len - prefix_len)) {
        return false;
    }

    return true;
}

// https://github.com/EOSIO/fc/blob/30eb81c1d995f9cd9834701e03b83ec7e6468a0f/include/fc/io/raw.hpp#L214
void eos_hashUInt(Hasher *hasher, uint64_t val) {
    do {
        uint8_t b = ((uint8_t)val) & 0x7f;
        val >>= 7;
        b |= ((val > 0) << 7);
        hasher_Update(hasher, &b, 1);
    } while (val);
}

void eos_signingInit(const uint8_t *chain_id, uint32_t num_actions,
                     const EosTxHeader *_header, const HDNode *_node) {
    hasher_Init(&hasher_preimage, HASHER_SHA2);

    memcpy(&header, _header, sizeof(header));
    memcpy(&node, _node, sizeof(node));

    hasher_Update(&hasher_preimage, chain_id, 32);
    hasher_Update(&hasher_preimage, (const uint8_t*)&header.expiration, 4);
    hasher_Update(&hasher_preimage, (const uint8_t*)&header.ref_block_num, 2);
    hasher_Update(&hasher_preimage, (const uint8_t*)&header.ref_block_prefix, 4);
    eos_hashUInt(&hasher_preimage, header.max_net_usage_words);
    hasher_Update(&hasher_preimage, (const uint8_t*)&header.max_cpu_usage_ms, 1);
    eos_hashUInt(&hasher_preimage, header.delay_sec);

    // context_free_actions. count, followed by each action
    eos_hashUInt(&hasher_preimage, 0);

    // actions. count, followed by each action
    eos_hashUInt(&hasher_preimage, num_actions);

    actions_remaining = num_actions;
    inited = true;
}

bool eos_signingIsInited(void) {
    return inited;
}

bool eos_signingIsFinished(void) {
    return inited && actions_remaining == 0;
}

void eos_signingAbort(void) {
    inited = false;
    memzero(&hasher_preimage, sizeof(hasher_preimage));
    memzero(&header, sizeof(header));
    memzero(&node, sizeof(node));
}

bool eos_compileActionCommon(const EosActionCommon *common) {
    if (!common->has_account)
        return false;

    if (!common->has_name)
        return false;

    if (!common->authorization_count)
        return false;

    hasher_Update(&hasher_preimage, (const uint8_t*)&common->account, 8);
    hasher_Update(&hasher_preimage, (const uint8_t*)&common->name, 8);

    for (size_t i = 0; i < common->authorization_count; i++) {
        if (!eos_compilePermissionLevel(&common->authorization[i]))
            return false;
    }

    return true;
}

bool eos_compilePermissionLevel(const EosPermissionLevel *auth) {
    if (!auth->has_actor)
        return false;

    if (!auth->has_permission)
        return false;

    hasher_Update(&hasher_preimage, (const uint8_t*)&auth->actor, 8);
    hasher_Update(&hasher_preimage, (const uint8_t*)&auth->permission, 8);

    return true;
}

bool eos_compileActionTransfer(const EosActionCommon *common,
                               const EosActionTransfer *transfer) {
    if (!(actions_remaining--))
        return false;

    (void)common;
    (void)transfer;

    uint8_t action_buf[4]; // FIXME: size
    size_t action_size = sizeof(action_buf);
    memzero(action_buf, sizeof(action_buf));

    hasher_Update(&hasher_preimage, action_buf, action_size);

    return true;
}

bool eos_signTx(EosSignedTx *tx) {
    if (!eos_signingIsInited()) {
        fsm_sendFailure(FailureType_Failure_Other, "Must call EosSignTx first");
        eos_signingAbort();
        return false;
    }

    // transaction_extensions. count, followed by data
    eos_hashUInt(&hasher_preimage, 0);

    // context_free_data. if nonempty, the sha256 digest of it. otherwise:
    hasher_Update(&hasher_preimage, (const uint8_t*)
                  "\x00\x00\x00\x00\x00\x00\x00\x00"
                  "\x00\x00\x00\x00\x00\x00\x00\x00"
                  "\x00\x00\x00\x00\x00\x00\x00\x00"
                  "\x00\x00\x00\x00\x00\x00\x00\x00", 32);

    // TODO: confirm max_usage_words

    // TODO: confirm max_cpu_usage_ms

    // TODO: confirm expiration

    // TODO: confirm delay_sec

    if (!confirm(ButtonRequestType_ButtonRequest_SignTx,
                 "Sign Transaction", "Do you really want to sign this EOS transaction?")) {
        fsm_sendFailure(FailureType_Failure_ActionCancelled, "Action Cancelled");
        eos_signingAbort();
        return false;
    }

    uint8_t hash[32];
    hasher_Final(&hasher_preimage, hash);

    uint8_t sig[64];
    uint8_t v;
    if (ecdsa_sign_digest(&secp256k1, node.private_key, hash, sig, &v, NULL) != 0) {
        fsm_sendFailure(FailureType_Failure_Other, "Signing failed");
        eos_signingAbort();
        return false;
    }
    memzero(&node, sizeof(node));

    tx->has_signature_v = true;
    tx->signature_v = v;

    tx->has_signature_r = true;
    tx->signature_r.size = 32;
    memcpy(tx->signature_r.bytes, sig, 32);

    tx->has_signature_s = true;
    tx->signature_s.size = 32;
    memcpy(tx->signature_s.bytes, sig + 32, 32);

    return true;
}