// Copyright 2026 qiyinxi
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

#include "android/emulation/ModavdImePipe.h"

#include <string.h>
#include <algorithm>
#include <utility>

#include "aemu/base/memory/LazyInstance.h"
#include "android/utils/debug.h"

namespace android {
namespace emulation {

namespace {

constexpr uint32_t kProtocolVersion = 1;
constexpr size_t kMaxPayloadSize = 65536;
// Host -> guest bytes allowed to pile up for a guest that stopped reading.
constexpr size_t kMaxPendingOut = 1024 * 1024;

struct Globals {
    android::base::Lock lock;  // protects pipe, retired and state
    ModavdImePipe::Ptr pipe;   // the latest connection
    // Older connections, closed by the host and kept alive until the guest
    // closes them too.
    std::vector<ModavdImePipe::Ptr> retired;
    ModavdImePipe::State state;

    // Protects callbacks; held while they run.
    android::base::Lock callbackLock;
    std::vector<std::pair<int, ModavdImePipe::StateCallback>> callbacks;
    int nextCallbackId = 1;
};

android::base::LazyInstance<Globals> sGlobals = LAZY_INSTANCE_INIT;

void putU32(std::vector<uint8_t>* out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out->push_back(static_cast<uint8_t>(value >> (8 * i)));
    }
}

bool appendFrame(std::vector<uint8_t>* frames,
                  const std::vector<uint8_t>& payload, std::string* error) {
    if (payload.empty() || payload.size() > kMaxPayloadSize ||
        frames->size() + 4 + payload.size() > kMaxPendingOut) {
        if (error) *error = "input event too large";
        return false;
    }
    putU32(frames, static_cast<uint32_t>(payload.size()));
    frames->insert(frames->end(), payload.begin(), payload.end());
    return true;
}

bool appendCommit(std::vector<uint8_t>* frames, const std::string& text,
                   std::string* error) {
    if (text.size() > kMaxPendingOut) {
        if (error) *error = "input event too large";
        return false;
    }
    constexpr size_t kMaxText = kMaxPayloadSize - 1;
    size_t pos = 0;
    do {
        size_t n = std::min(kMaxText, text.size() - pos);
        if (pos + n < text.size()) {
            size_t cut = n;
            while (cut > 0 &&
                   (static_cast<uint8_t>(text[pos + cut]) & 0xC0) == 0x80) {
                --cut;
            }
            if (cut > 0) n = cut;
        }
        std::vector<uint8_t> payload{'C'};
        payload.insert(payload.end(), text.begin() + pos, text.begin() + pos + n);
        if (!appendFrame(frames, payload, error)) return false;
        pos += n;
    } while (pos < text.size());
    return true;
}

uint32_t getU32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}

// Forgets what the guest reported. textMode belongs to the window.
unsigned resetGuestStateLocked(ModavdImePipe::State* state) {
    unsigned changes = 0;
    if (state->connected) {
        changes |= ModavdImePipe::kConnectionChanged;
    }
    if (state->active || state->inputType || state->imeOptions) {
        changes |= ModavdImePipe::kFocusChanged;
    }
    if (state->hasCursor) {
        changes |= ModavdImePipe::kCursorChanged;
    }
    const bool textMode = state->textMode;
    *state = ModavdImePipe::State();
    state->textMode = textMode;
    return changes;
}

void notify(const ModavdImePipe::State& state, unsigned changes) {
    if (!changes) {
        return;
    }
    android::base::AutoLock lock(sGlobals->callbackLock);
    for (const auto& callback : sGlobals->callbacks) {
        callback.second(state, changes);
    }
}

}  // namespace

// What the frames of one onGuestSend() call reported.
struct ModavdImePipe::Update {
    bool connected = false;
    uint32_t version = 0;
    bool focus = false;
    bool active = false;
    uint32_t inputType = 0;
    uint32_t imeOptions = 0;
    bool cursor = false;
    int32_t rect[4] = {};
};

ModavdImePipe::ModavdImePipe(void* hwPipe, Service* svc)
    : AndroidPipe(hwPipe, svc) {}

void ModavdImePipe::onGuestClose(PipeCloseReason reason) {
    // Holds the last reference; declared first so it goes away last.
    Ptr self;
    {
        android::base::AutoLock lock(mLock);
        mClosed = true;
        mOut.clear();
        mOutPos = 0;
    }
    // A host thread may have queued a wake before mClosed was set; the hw
    // pipe is gone once this returns.
    abortPendingOperation();

    State snapshot;
    unsigned changes = 0;
    {
        android::base::AutoLock lock(sGlobals->lock);
        if (sGlobals->pipe.get() == this) {
            self = std::move(sGlobals->pipe);
            changes = resetGuestStateLocked(&sGlobals->state);
            snapshot = sGlobals->state;
        } else {
            auto& retired = sGlobals->retired;
            auto it = std::find_if(retired.begin(), retired.end(),
                                   [this](const Ptr& p) { return p.get() == this; });
            if (it != retired.end()) {
                self = std::move(*it);
                retired.erase(it);
            }
        }
    }
    if (changes & kConnectionChanged) {
        dinfo("modavd_ime: guest input method disconnected (reason %d)",
              static_cast<int>(reason));
    }
    notify(snapshot, changes);
}

unsigned ModavdImePipe::onGuestPoll() const {
    android::base::AutoLock lock(mLock);
    if (mClosed) {
        return PIPE_POLL_HUP;
    }
    unsigned result = PIPE_POLL_OUT;
    if (mOutPos < mOut.size()) {
        result |= PIPE_POLL_IN;
    }
    return result;
}

int ModavdImePipe::onGuestRecv(AndroidPipeBuffer* buffers, int numBuffers) {
    android::base::AutoLock lock(mLock);
    if (mClosed) {
        return PIPE_ERROR_IO;
    }
    int total = 0;
    for (int i = 0; i < numBuffers && mOutPos < mOut.size(); ++i) {
        const size_t n = std::min(buffers[i].size, mOut.size() - mOutPos);
        memcpy(buffers[i].data, mOut.data() + mOutPos, n);
        mOutPos += n;
        total += static_cast<int>(n);
    }
    if (mOutPos == mOut.size()) {
        mOut.clear();
        mOutPos = 0;
    }
    return total > 0 ? total : PIPE_ERROR_AGAIN;
}

int ModavdImePipe::onGuestSend(const AndroidPipeBuffer* buffers,
                               int numBuffers,
                               void** newPipePtr) {
    Update update;
    bool bad = false;
    int total = 0;
    {
        android::base::AutoLock lock(mLock);
        if (mClosed) {
            return PIPE_ERROR_IO;
        }
        for (int i = 0; i < numBuffers && !bad; ++i) {
            const uint8_t* data = buffers[i].data;
            const size_t size = buffers[i].size;
            size_t pos = 0;
            while (pos < size) {
                if (mHeaderSize < sizeof(mHeader)) {
                    const size_t n =
                            std::min(sizeof(mHeader) - mHeaderSize, size - pos);
                    memcpy(mHeader + mHeaderSize, data + pos, n);
                    mHeaderSize += n;
                    pos += n;
                    if (mHeaderSize == sizeof(mHeader)) {
                        const uint32_t length = getU32(mHeader);
                        if (length < 1 || length > kMaxPayloadSize) {
                            bad = true;
                            break;
                        }
                        mPayload.resize(length);
                        mPayloadSize = 0;
                    }
                    continue;
                }
                const size_t n =
                        std::min(mPayload.size() - mPayloadSize, size - pos);
                memcpy(mPayload.data() + mPayloadSize, data + pos, n);
                mPayloadSize += n;
                pos += n;
                if (mPayloadSize == mPayload.size()) {
                    if (!handleFrameLocked(&update)) {
                        bad = true;
                        break;
                    }
                    mHeaderSize = 0;
                    mPayloadSize = 0;
                    mPayload.clear();
                }
            }
            total += static_cast<int>(pos);
        }
        if (bad) {
            closeFromHostLocked();
        }
    }

    if (bad) {
        dwarning("modavd_ime: malformed frame from the guest, closing the "
                 "connection");
        detach();
        return PIPE_ERROR_IO;
    }

    State snapshot;
    unsigned changes = 0;
    {
        android::base::AutoLock lock(sGlobals->lock);
        if (sGlobals->pipe.get() == this) {
            State& state = sGlobals->state;
            if (update.connected) {
                state.connected = true;
                state.version = update.version;
                changes |= kConnectionChanged;
            }
            if (update.focus) {
                state.active = update.active;
                state.inputType = update.inputType;
                state.imeOptions = update.imeOptions;
                changes |= kFocusChanged;
            }
            if (update.cursor) {
                if (!state.hasCursor || state.cursorLeft != update.rect[0] ||
                    state.cursorTop != update.rect[1] ||
                    state.cursorRight != update.rect[2] ||
                    state.cursorBottom != update.rect[3]) {
                    changes |= kCursorChanged;
                }
                state.hasCursor = true;
                state.cursorLeft = update.rect[0];
                state.cursorTop = update.rect[1];
                state.cursorRight = update.rect[2];
                state.cursorBottom = update.rect[3];
            }
            snapshot = state;
        }
    }
    if (update.connected) {
        dinfo("modavd_ime: guest input method connected, protocol %u",
              update.version);
    }
    notify(snapshot, changes);
    return total;
}

void ModavdImePipe::onGuestWantWakeOn(int flags) {
    android::base::AutoLock lock(mLock);
    if (mClosed) {
        return;
    }
    int wake = 0;
    if ((flags & PIPE_WAKE_READ) && mOutPos < mOut.size()) {
        wake |= PIPE_WAKE_READ;
    }
    if (flags & PIPE_WAKE_WRITE) {
        wake |= PIPE_WAKE_WRITE;  // writes never block
    }
    if (wake) {
        signalWake(wake);
    }
}

bool ModavdImePipe::handleFrameLocked(Update* update) {
    const uint8_t* p = mPayload.data();
    const size_t size = mPayload.size();
    const uint8_t type = p[0];
    VERBOSE_INFO(keys, "modavd_ime: guest frame '%c', %zu bytes",
                 (type >= 0x20 && type < 0x7f) ? type : '?', size);

    if (!mHandshaked) {
        if (type != 'V' || size < 5) {
            return false;
        }
        const uint32_t version = getU32(p + 1);
        if (version < 1) {
            return false;
        }
        mHandshaked = true;
        std::vector<uint8_t> reply{'V'};
        putU32(&reply, kProtocolVersion);
        queueFrameLocked(reply.data(), reply.size());
        update->connected = true;
        update->version = version;
        return true;
    }

    switch (type) {
        case 'V':
            // A repeated handshake changes nothing.
            return size >= 5;
        case 'F':
            if (size < 10 || p[1] > 1) {
                return false;
            }
            update->focus = true;
            update->active = p[1] == 1;
            update->inputType = getU32(p + 2);
            update->imeOptions = getU32(p + 6);
            return true;
        case 'R':
            if (size < 17) {
                return false;
            }
            update->cursor = true;
            for (int i = 0; i < 4; ++i) {
                update->rect[i] = static_cast<int32_t>(getU32(p + 1 + 4 * i));
            }
            return true;
        default:
            return false;
    }
}

bool ModavdImePipe::queueFrameLocked(const uint8_t* payload, size_t size) {
    if (mClosed || size < 1 || size > kMaxPayloadSize ||
        mOut.size() - mOutPos + 4 + size > kMaxPendingOut) {
        return false;
    }
    // Drop what the guest has read, so the buffer stays near the cap even
    // when the guest never quite catches up.
    if (mOutPos == mOut.size()) {
        mOut.clear();
        mOutPos = 0;
    } else if (mOutPos >= kMaxPendingOut / 4) {
        mOut.erase(mOut.begin(), mOut.begin() + mOutPos);
        mOutPos = 0;
    }
    putU32(&mOut, static_cast<uint32_t>(size));
    mOut.insert(mOut.end(), payload, payload + size);
    signalWake(PIPE_WAKE_READ);
    return true;
}

bool ModavdImePipe::queueFrame(const std::vector<uint8_t>& payload,
                               std::string* error) {
    android::base::AutoLock lock(mLock);
    if (mClosed || !mHandshaked) {
        if (error) {
            *error = "guest input method not connected";
        }
        return false;
    }
    if (payload.size() > kMaxPayloadSize) {
        if (error) {
            *error = "text too long";
        }
        return false;
    }
    if (!queueFrameLocked(payload.data(), payload.size())) {
        if (error) {
            *error = "guest input method is not reading";
        }
        return false;
    }
    VERBOSE_INFO(keys, "modavd_ime: host frame '%c', %zu bytes",
                 static_cast<char>(payload[0]), payload.size());
    return true;
}

bool ModavdImePipe::queueBatch(const std::vector<uint8_t>& frames,
                               std::string* error) {
    android::base::AutoLock lock(mLock);
    if (mClosed || !mHandshaked) {
        if (error) *error = "guest input method not connected";
        return false;
    }
    if (frames.size() > kMaxPendingOut - (mOut.size() - mOutPos)) {
        if (error) *error = "guest input method is not reading";
        return false;
    }
    // Reserve before modifying the queue; no partial commit on backpressure.
    mOut.reserve(mOut.size() + frames.size());
    if (mOutPos) {
        mOut.erase(mOut.begin(), mOut.begin() + mOutPos);
        mOutPos = 0;
    }
    mOut.insert(mOut.end(), frames.begin(), frames.end());
    signalWake(PIPE_WAKE_READ);
    return true;
}

void ModavdImePipe::closeFromHostLocked() {
    if (mClosed) {
        return;
    }
    mClosed = true;
    mOut.clear();
    mOutPos = 0;
    closeFromHost();
}

void ModavdImePipe::detach() {
    State snapshot;
    unsigned changes = 0;
    {
        android::base::AutoLock lock(sGlobals->lock);
        if (sGlobals->pipe.get() != this) {
            return;
        }
        sGlobals->retired.push_back(std::move(sGlobals->pipe));
        changes = resetGuestStateLocked(&sGlobals->state);
        snapshot = sGlobals->state;
    }
    notify(snapshot, changes);
}

// static
bool ModavdImePipe::sendFrame(const std::vector<uint8_t>& payload,
                              std::string* error) {
    Ptr pipe;
    {
        android::base::AutoLock lock(sGlobals->lock);
        if (sGlobals->state.connected) {
            pipe = sGlobals->pipe;
        }
    }
    if (!pipe) {
        if (error) {
            *error = "guest input method not connected";
        }
        return false;
    }
    return pipe->queueFrame(payload, error);
}

// static
bool ModavdImePipe::sendCommit(const std::string& text, std::string* error) {
    InputEvent input;
    input.hasCommit = true;
    input.commit = text;
    return sendInputEvent(input, error);
}

// static
bool ModavdImePipe::sendInputEvent(const InputEvent& input,
                                   std::string* error) {
    if (input.deleteBefore < 0 || input.deleteAfter < 0 ||
        (input.setPreedit && input.preedit.size() > kMaxPayloadSize - 5)) {
        if (error) *error = "invalid input event or preedit too long";
        return false;
    }
    std::vector<uint8_t> frames;
    if (input.deleteBefore || input.deleteAfter) {
        std::vector<uint8_t> payload{'D'};
        putU32(&payload, static_cast<uint32_t>(input.deleteBefore));
        putU32(&payload, static_cast<uint32_t>(input.deleteAfter));
        if (!appendFrame(&frames, payload, error)) return false;
    }
    if (input.hasCommit && !appendCommit(&frames, input.commit, error)) return false;
    if (input.setPreedit) {
        std::vector<uint8_t> payload{'P'};
        putU32(&payload, static_cast<uint32_t>(input.cursor));
        payload.insert(payload.end(), input.preedit.begin(), input.preedit.end());
        if (!appendFrame(&frames, payload, error)) return false;
    }
    if (input.finishComposing && !appendFrame(&frames, {'E'}, error)) return false;
    if (frames.empty()) return true;
    Ptr pipe;
    {
        android::base::AutoLock lock(sGlobals->lock);
        if (sGlobals->state.connected) pipe = sGlobals->pipe;
    }
    if (!pipe) {
        if (error) *error = "guest input method not connected";
        return false;
    }
    return pipe->queueBatch(frames, error);
}

// static
bool ModavdImePipe::sendPreedit(int32_t cursor,
                                const std::string& text,
                                std::string* error) {
    if (text.size() > kMaxPayloadSize - 5) {
        if (error) {
            *error = "text too long";
        }
        return false;
    }
    std::vector<uint8_t> payload{'P'};
    putU32(&payload, static_cast<uint32_t>(cursor));
    payload.insert(payload.end(), text.begin(), text.end());
    return sendFrame(payload, error);
}

// static
bool ModavdImePipe::sendFinishComposing(std::string* error) {
    return sendFrame({'E'}, error);
}

// static
bool ModavdImePipe::sendKey(uint32_t keycode,
                            uint32_t metaState,
                            uint8_t action,
                            std::string* error) {
    if (action > kKeyDownUp) {
        if (error) {
            *error = "bad key action";
        }
        return false;
    }
    std::vector<uint8_t> payload{'K'};
    putU32(&payload, keycode);
    putU32(&payload, metaState);
    payload.push_back(action);
    return sendFrame(payload, error);
}

// static
bool ModavdImePipe::sendDelete(int32_t before,
                               int32_t after,
                               std::string* error) {
    if (before < 0 || after < 0) {
        if (error) {
            *error = "lengths must not be negative";
        }
        return false;
    }
    std::vector<uint8_t> payload{'D'};
    putU32(&payload, static_cast<uint32_t>(before));
    putU32(&payload, static_cast<uint32_t>(after));
    return sendFrame(payload, error);
}

// static
ModavdImePipe::State ModavdImePipe::getState() {
    android::base::AutoLock lock(sGlobals->lock);
    return sGlobals->state;
}

// static
void ModavdImePipe::setTextMode(bool textMode) {
    android::base::AutoLock lock(sGlobals->lock);
    sGlobals->state.textMode = textMode;
}

// static
int ModavdImePipe::addStateCallback(StateCallback cb) {
    if (!cb) {
        return 0;
    }
    android::base::AutoLock lock(sGlobals->callbackLock);
    const int id = sGlobals->nextCallbackId++;
    sGlobals->callbacks.emplace_back(id, std::move(cb));
    return id;
}

// static
void ModavdImePipe::removeStateCallback(int id) {
    android::base::AutoLock lock(sGlobals->callbackLock);
    auto& callbacks = sGlobals->callbacks;
    callbacks.erase(std::remove_if(callbacks.begin(), callbacks.end(),
                                   [id](const std::pair<int, StateCallback>& c) {
                                       return c.first == id;
                                   }),
                    callbacks.end());
}

////////////////////////////////////////////////////////////////////////////////

ModavdImePipe::Service::Service() : AndroidPipe::Service("modavd_ime") {}

AndroidPipe* ModavdImePipe::Service::create(void* hwPipe,
                                            const char* args,
                                            enum AndroidPipeFlags flags) {
    const auto pipe = std::make_shared<ModavdImePipe>(hwPipe, this);
    Ptr old;
    State snapshot;
    unsigned changes = 0;
    {
        android::base::AutoLock lock(sGlobals->lock);
        old = std::move(sGlobals->pipe);
        if (old) {
            sGlobals->retired.push_back(old);
        }
        sGlobals->pipe = pipe;
        changes = resetGuestStateLocked(&sGlobals->state);
        snapshot = sGlobals->state;
    }
    if (old) {
        // Only the latest connection counts.
        android::base::AutoLock lock(old->mLock);
        old->closeFromHostLocked();
    }
    notify(snapshot, changes);
    return pipe.get();
}

void registerModavdImePipeService() {
    android::AndroidPipe::Service::add(
            std::make_unique<ModavdImePipe::Service>());
    dinfo("modavd_ime: host input method pipe service registered");
}

}  // namespace emulation
}  // namespace android
