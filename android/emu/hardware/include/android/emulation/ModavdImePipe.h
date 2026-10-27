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
#pragma once

#include <stddef.h>  // for size_t
#include <cstdint>   // for uint8_t, uint32_t, int32_t
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "aemu/base/synchronization/Lock.h"
#include "host-common/AndroidPipe.h"
#include "host-common/android_pipe_common.h"

namespace android {
namespace emulation {

// Host side of the ModAVD host input method channel, protocol v1. The guest
// input method (org.modavd.ime) connects to the "modavd_ime" pipe service and
// reports the focused editor; the host sends it committed text, composing
// text, key presses and deletions.
//
// A frame is a little-endian u32 payload length N (1 <= N <= 65536) followed
// by N payload bytes. The first payload byte is the frame type:
//   guest -> host: 'V' u32 version (must come first), 'F' u8 active +
//                  u32 inputType + u32 imeOptions, 'R' i32 left, top, right,
//                  bottom (guest display 0 pixels)
//   host -> guest: 'V' u32 version, 'C' text, 'P' i32 cursor + text, 'E',
//                  'K' u32 keycode + u32 metaState + u8 action,
//                  'D' i32 before + i32 after
// Text is UTF-8 without a terminating NUL. Only the latest connection is
// used; a frame with a bad length, an unknown type or missing fields closes
// the connection.
//
// The static methods below are thread-safe.
class ModavdImePipe final : public AndroidPipe {
public:
    using Ptr = std::shared_ptr<ModavdImePipe>;

    struct State {
        bool connected = false;  // the guest finished the handshake
        uint32_t version = 0;    // protocol version announced by the guest
        bool active = false;     // the focus is an editable text field
        uint32_t inputType = 0;  // EditorInfo.inputType of the last 'F'
        uint32_t imeOptions = 0;
        bool hasCursor = false;  // an 'R' arrived on this connection
        int32_t cursorLeft = 0;
        int32_t cursorTop = 0;
        int32_t cursorRight = 0;
        int32_t cursorBottom = 0;
        bool textMode = false;  // the emulator window is in text mode
    };

    // Bits of the |changes| argument of a StateCallback.
    enum Change : unsigned {
        kConnectionChanged = 1 << 0,
        kFocusChanged = 1 << 1,
        kCursorChanged = 1 << 2,
    };

    // Called from the thread that holds the VM lock, without any pipe lock
    // held. Must not block; hand the work over to another thread instead.
    using StateCallback = std::function<void(const State&, unsigned changes)>;

    // Values of the 'K' action field.
    enum KeyAction : uint8_t { kKeyDown = 0, kKeyUp = 1, kKeyDownUp = 2 };

    class Service final : public AndroidPipe::Service {
    public:
        Service();
        AndroidPipe* create(void* hwPipe,
                            const char* args,
                            enum AndroidPipeFlags flags) override;
    };

    ModavdImePipe(void* hwPipe, Service* svc);

    void onGuestClose(PipeCloseReason reason) override;
    unsigned onGuestPoll() const override;
    int onGuestRecv(AndroidPipeBuffer* buffers, int numBuffers) override;
    int onGuestSend(const AndroidPipeBuffer* buffers,
                    int numBuffers,
                    void** newPipePtr) override;
    void onGuestWantWakeOn(int flags) override;

    // Host -> guest. Each returns false and fills |error| (if not null) when
    // no guest is connected or the frame can't be sent.
    static bool sendCommit(const std::string& text,
                           std::string* error = nullptr);
    static bool sendPreedit(int32_t cursor,
                            const std::string& text,
                            std::string* error = nullptr);
    static bool sendFinishComposing(std::string* error = nullptr);
    static bool sendKey(uint32_t keycode,
                        uint32_t metaState,
                        uint8_t action,
                        std::string* error = nullptr);
    static bool sendDelete(int32_t before,
                           int32_t after,
                           std::string* error = nullptr);

    static State getState();
    // Records whether the emulator window is in text mode, for "ime status".
    static void setTextMode(bool textMode);

    // Returns an id for removeStateCallback(). After removeStateCallback()
    // returns, the callback is not running and won't be called again.
    static int addStateCallback(StateCallback cb);
    static void removeStateCallback(int id);

private:
    struct Update;

    static bool sendFrame(const std::vector<uint8_t>& payload,
                          std::string* error);
    bool queueFrameLocked(const uint8_t* payload, size_t size);
    bool queueFrame(const std::vector<uint8_t>& payload, std::string* error);
    bool handleFrameLocked(Update* update);
    void closeFromHostLocked();
    void detach();

    mutable android::base::Lock mLock;  // protects the members below
    bool mClosed = false;      // closed by the guest or the host
    bool mHandshaked = false;  // the guest's 'V' arrived

    // Guest -> host frame being received.
    uint8_t mHeader[4] = {};
    size_t mHeaderSize = 0;
    std::vector<uint8_t> mPayload;
    size_t mPayloadSize = 0;

    // Host -> guest bytes not read by the guest yet.
    std::vector<uint8_t> mOut;
    size_t mOutPos = 0;
};

void registerModavdImePipeService();

}  // namespace emulation
}  // namespace android
