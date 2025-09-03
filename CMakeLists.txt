cmake_minimum_required(VERSION 3.16)
project(puredata C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PD_VERSION_MAJOR "0")
set(PD_VERSION_MINOR "56")
set(PD_VERSION_BUG "1")


# Libraries
option(ENABLE_FFTW "Enable FFTW backend" ON)

# Midi backend
option(ENABLE_MIDI_DUMMY "Enable dummy MIDI backend" OFF)
option(ENALBE_MIDI_PORTMIDI "Enable PortMidi  MIDI backend" OFF)

# Audio backend
option(ENABLE_AUDIO_JACK "Enable JACK audio backend" OFF)
option(ENABLE_AUDIO_ALSA "Enable ALSA audio backend" OFF)
option(ENABLE_AUDIO_PIPEWIRE "Enable PIPEWIRE audio backend" ON)

set(CMAKE_C_STANDARD 99)

# Include paths
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)

# Core compile definitions (mirroring the compile_commands.json)
add_compile_definitions(
  PACKAGE_NAME="pd"
  PACKAGE_TARNAME="pd"
  PACKAGE_VERSION="${PD_VERSION_MAJOR}.${PD_VERSION_MINOR}-${PD_VERSION_BUG}"
  PACKAGE_STRING="pd ${PD_VERSION_MAJOR}.${PD_VERSION_MINOR}-${PD_VERSION_BUG}"
  PACKAGE_BUGREPORT=""
  PACKAGE_URL=""
  PACKAGE="pd"
  VERSION="${PD_VERSION_MAJOR}.${PD_VERSION_MINOR}-${PD_VERSION_BUG}"
  LT_OBJDIR=".libs/"
  # PD flags
  USEAPI_MIDIDUMMY
  PD
  PD_INTERNAL
  #
  INSTALL_PREFIX="/usr/local"
  PD_WATCHDOG=1
  EXTERNAL_EXTENSION='.pd_linux')

add_compile_options(-ffast-math -fno-finite-math-only -funroll-loops
                    -fomit-frame-pointer)

set(PD_SOURCES
    src/d_ctl.c
    src/d_fft.c
    src/s_entry.c
    src/d_arithmetic.c
    src/d_delay.c
    src/d_filter.c
    src/d_dac.c
    src/d_array.c
    src/d_global.c
    src/d_math.c
    src/d_misc.c
    src/d_osc.c
    src/d_resample.c
    src/d_soundfile.c
    src/d_soundfile_aiff.c
    src/d_soundfile_caf.c
    src/d_soundfile_next.c
    src/d_soundfile_wave.c
    src/d_ugen.c
    src/g_all_guis.c
    src/g_canvas.c
    src/g_array.c
    src/g_bang.c
    src/g_clone.c
    src/g_editor.c
    src/g_editor_extras.c
    src/g_graph.c
    src/g_guiconnect.c
    src/g_io.c
    src/g_mycanvas.c
    src/g_numbox.c
    src/g_radio.c
    src/g_readwrite.c
    src/g_rtext.c
    src/g_scalar.c
    src/g_slider.c
    src/g_template.c
    src/g_text.c
    src/g_toggle.c
    src/g_traversal.c
    src/g_undo.c
    src/g_vumeter.c
    src/m_binbuf.c
    src/m_atom.c
    src/m_class.c
    src/m_conf.c
    src/m_glob.c
    src/m_memory.c
    src/m_obj.c
    src/m_pd.c
    src/m_sched.c
    src/s_audio.c
    src/s_inter.c
    src/s_inter_gui.c
    src/s_main.c
    src/s_loader.c
    src/s_net.c
    src/s_path.c
    src/s_print.c
    src/s_utf8.c
    src/x_acoustics.c
    src/x_arithmetic.c
    src/x_array.c
    src/x_connective.c
    src/x_file.c
    src/x_gui.c
    src/x_list.c
    src/x_interface.c
    src/x_midi.c
    src/x_misc.c
    src/x_net.c
    src/x_scalar.c
    src/x_text.c
    src/x_time.c
    src/x_vexp.c
    src/x_vexp_fun.c
    src/x_vexp_if.c
    # File and MIDI backends present in commands
    src/s_file.c
    src/s_midi.c
    # dummy backends
    src/s_midi_dummy.c
    src/s_audio_dummy.c
    src/s_audio_paring.c)

# fft
if(ENABLE_FFTW)
  list(APPEND PD_SOURCES src/d_fft_fftw.c)
endif()

# audio sources
if(ENABLE_AUDIO_ALSA)
  list(APPEND PD_SOURCES src/s_audio_alsa.c)
endif()

if(ENABLE_AUDIO_JACK)
  list(APPEND PD_SOURCES src/s_audio_jack.c)
endif()

if(ENABLE_AUDIO_PIPEWIRE)
  list(APPEND PD_SOURCES src/s_audio_pipewire.c)
endif()

# ╭──────────────────────────────────────╮
# │               pd exec                │
# ╰──────────────────────────────────────╯
add_executable(pd "${PD_SOURCES}")
target_compile_definitions(pd PRIVATE HAVE_UNISTD_H=1 HAVE_SYS_TYPES_H=1
                                      HAVE_FCNTL_H=1)
target_link_libraries(pd PRIVATE m dl pthread)

# fft
if(ENABLE_FFTW)
  find_library(FFTW3F_LIB fftw3f)
  list(APPEND PRIVATE_LIBS ${FFTW3F_LIB})
  if(FFTW3F_LIB)
    target_link_libraries(pd PRIVATE fftw3f)
  else()
    message(FATAL_ERROR "FFTW requested but not found")
  endif()
endif()

# audio backend
if(ENABLE_AUDIO_ALSA)
  # TODO: replace by pkg_check_modules
  target_link_libraries(pd PRIVATE asound)
  target_compile_definitions(pd PRIVATE USEAPI_ALSA)
endif()

if(ENABLE_AUDIO_JACK)
  # TODO: replace by pkg_check_modules
  target_link_libraries(pd PRIVATE jack)
  target_compile_definitions(pd PRIVATE USEAPI_JACK)
endif()

if(ENABLE_AUDIO_PIPEWIRE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
  target_link_libraries(pd PUBLIC ${PIPEWIRE_LIBRARIES})
  target_include_directories(pd PUBLIC ${PIPEWIRE_INCLUDE_DIRS})
  target_compile_definitions(pd PRIVATE USEAPI_PIPEWIRE)
endif()

# pdsend exec
add_executable(pdsend src/u_pdsend.c src/s_net.c)
target_compile_definitions(pdsend PRIVATE NDEBUG)
target_link_libraries(pdsend PRIVATE m dl pthread)

# pdreceive
add_executable(pdreceive src/u_pdreceive.c src/s_net.c)
target_compile_definitions(pdreceive PRIVATE NDEBUG)
target_link_libraries(pdreceive PRIVATE m dl pthread)

# watchdog helper
add_executable(pd_watchdog src/s_watchdog.c)
target_compile_definitions(pd_watchdog PRIVATE NDEBUG)
target_link_libraries(pd_watchdog PRIVATE m dl pthread)
