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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The longest title, in bytes: skin_ui_update_title copies the title into 128
// bytes.
#define INSTANCE_WINDOW_TITLE_MAX 127

// Writes the window title of an instance into |buff|: the instance's name
// alone, as the manager shows it and keeps it in avd.ini.displayname
// (|display_name|, leading spaces and tabs dropped). Without one, or with an
// empty one, the AVD name |avd_name| stands in. The title is cut to
// |buff_len| - 1 and at most INSTANCE_WINDOW_TITLE_MAX bytes, between UTF-8
// characters. Either name may be NULL. Returns the length written.
size_t instance_window_title(const char* display_name,
                             const char* avd_name,
                             char* buff,
                             size_t buff_len);

#ifdef __cplusplus
}
#endif
