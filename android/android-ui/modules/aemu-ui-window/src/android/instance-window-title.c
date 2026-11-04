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
#include "android/instance-window-title.h"

#include <string.h>

size_t instance_window_title(const char* display_name,
                             const char* avd_name,
                             char* buff,
                             size_t buff_len) {
    if (!buff || buff_len == 0) {
        return 0;
    }
    const char* name = avd_name ? avd_name : "";
    if (display_name) {
        const char* trimmed = display_name + strspn(display_name, " \t");
        if (*trimmed) {
            name = trimmed;
        }
    }
    size_t len = strlen(name);
    size_t max_len = buff_len - 1;
    if (max_len > INSTANCE_WINDOW_TITLE_MAX) {
        max_len = INSTANCE_WINDOW_TITLE_MAX;
    }
    if (len > max_len) {
        len = max_len;
        // Back up to the first byte of a UTF-8 sequence.
        while (len > 0 && ((unsigned char)name[len] & 0xC0) == 0x80) {
            --len;
        }
    }
    memcpy(buff, name, len);
    buff[len] = '\0';
    return len;
}
