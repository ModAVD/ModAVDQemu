# ModAVD#58: the window title and the lean window's frame arithmetic, without a
# windowing system, so that they can be tested on their own.
android_add_library(
  TARGET aemu-ui-lean-window
  LICENSE Apache-2.0
  SOURCE_DIR src
  INCLUDES include
  SRC android/instance-window-title.c android/lean-window-frame.cpp)

android_add_test(
  TARGET aemu-ui-lean-window_unittests
  SOURCE_DIR test
  SRC android/instance-window-title_unittest.cpp
      android/lean-window-frame_unittest.cpp
  DEPS aemu-ui-lean-window gmock_main)

android_qt_lib(
  TARGET aemu-ui-window
  # SHARED
  ALIAS ui::window
  SRC android/emulator-environment.cpp android/emulator-window.c android/main-common-ui.c android/resource.c
      android/skin/ui.c android/skin/user-config.c android/skin/window.c
  DEPS aemu-gl-init
       aemu-ui-lean-window
       aemu-gl-init
       aemu-recording
       android-emu-agents
       android-emu-avd
       android-emu-base
       android-emu-base-headers
       android-emu-cmdline
       android-emu-files
       android-emu-metrics
       android-emu-sockets
       android-emu-utils
       virtual_environment_renderer
       qemu-host-common-headers
       png
       gfxstream_glm_headers
       webrtc-yuv
       ui::common)

target_compile_definitions(aemu-ui-window PRIVATE "-D_LIBCPP_VERSION=__GLIBCPP__")

target_include_directories(
  aemu-ui-window
  PRIVATE
    src
    # Fix this dependency once those libs have been sliced.
    ${ANDROID_QEMU2_TOP_DIR}/android/android-emu
    # use ui::gl-bridge once the cycles have been broken.
    # ../aemu-gl-bridge/include
    # TODO(jansene): We actually have a hard dependency on qemu-glue as there
    # are a lot of externs that are actually defined in qemu2-glue. this has to
    # be sorted out,
    ${ANDROID_QEMU2_TOP_DIR}/android-qemu2-glue/config/${ANDROID_TARGET_TAG}
  PUBLIC include)
