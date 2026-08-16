# Supermodel Libretro Core - Unified Makefile
# Follows libretro/skeletor pattern with all platforms consolidated

STATIC_LINKING := 0
DEBUG ?= 0
PSS_STYLE ?= 1
EXTERNAL_ZLIB ?= 0
HAVE_GRIFFIN ?= 1

# Core definitions
CORE_DIR := .
TARGET_NAME := supermodel

# ============================================================
# Platform Auto-Detection
# ============================================================
ifeq ($(platform),)
    platform = unix
    ifeq ($(shell uname -a),)
        platform = win
    else ifneq ($(findstring MINGW,$(shell uname -a)),)
        platform = win
    else ifneq ($(findstring Darwin,$(shell uname -a)),)
        platform = osx
    else ifneq ($(findstring win,$(shell uname -a)),)
        platform = win
    endif
endif

# System platform (for native builds)
system_platform = unix
ifeq ($(shell uname -a),)
    system_platform = win
else ifneq ($(findstring Darwin,$(shell uname -a)),)
    system_platform = osx
else ifneq ($(findstring MINGW,$(shell uname -a)),)
    system_platform = win
endif

# ============================================================
# Source Files (extracted from Makefile.common)
# ============================================================
LIBRETRO_COMM_DIR := $(CORE_DIR)/Src/OSD/libretro/libretro-common
DEPS_DIR := $(CORE_DIR)/deps
MUSASHI_DIR := $(CORE_DIR)/Src/CPU/68K/Musashi
MUSASHI_GEN_DIR := $(CORE_DIR)/build/libretro/musashi
MUSASHI_GENERATOR := $(MUSASHI_GEN_DIR)/m68kmake
MUSASHI_GENERATED := $(MUSASHI_GEN_DIR)/m68kops.h \
                     $(MUSASHI_GEN_DIR)/m68kops.c \
                     $(MUSASHI_GEN_DIR)/m68kopac.c \
                     $(MUSASHI_GEN_DIR)/m68kopdm.c \
                     $(MUSASHI_GEN_DIR)/m68kopnz.c

INCFLAGS := -I$(CORE_DIR) \
            -I$(DEPS_DIR)/ugui \
            -I$(LIBRETRO_COMM_DIR)/include \
            -I$(CORE_DIR)/Src/OSD/libretro/include \
            -I$(CORE_DIR)/Src/OSD/libretro \
            -I$(CORE_DIR)/Src/OSD \
            -I$(CORE_DIR)/Src \
            -I$(MUSASHI_DIR) \
            -I$(MUSASHI_GEN_DIR)

COREDEFINES := -D__LIBRETRO__ -DSUPERMODEL_OSD_LIBRETRO
COREDEFINES += -DPSS_STYLE=$(PSS_STYLE)

# C Source Files (28 files)
SOURCES_C := $(CORE_DIR)/Src/Pkgs/unzip.c \
             $(CORE_DIR)/Src/Pkgs/ioapi.c \
             $(MUSASHI_DIR)/m68kcpu.c \
             $(MUSASHI_GEN_DIR)/m68kops.c \
             $(MUSASHI_GEN_DIR)/m68kopac.c \
             $(MUSASHI_GEN_DIR)/m68kopdm.c \
             $(MUSASHI_GEN_DIR)/m68kopnz.c \
             $(DEPS_DIR)/ugui/ugui.c \
             $(CORE_DIR)/Src/ugui_tools.c \
             $(LIBRETRO_COMM_DIR)/streams/file_stream.c \
             $(LIBRETRO_COMM_DIR)/streams/file_stream_transforms.c \
             $(LIBRETRO_COMM_DIR)/file/file_path.c \
             $(LIBRETRO_COMM_DIR)/file/retro_dirent.c \
             $(LIBRETRO_COMM_DIR)/vfs/vfs_implementation.c \
             $(LIBRETRO_COMM_DIR)/lists/dir_list.c \
             $(LIBRETRO_COMM_DIR)/lists/string_list.c \
             $(LIBRETRO_COMM_DIR)/string/stdstring.c \
             $(LIBRETRO_COMM_DIR)/compat/compat_strl.c \
             $(LIBRETRO_COMM_DIR)/compat/fopen_utf8.c \
             $(LIBRETRO_COMM_DIR)/compat/compat_strcasestr.c \
             $(LIBRETRO_COMM_DIR)/compat/compat_posix_string.c \
             $(LIBRETRO_COMM_DIR)/encodings/encoding_utf.c \
             $(LIBRETRO_COMM_DIR)/memmap/memalign.c \
             $(LIBRETRO_COMM_DIR)/time/rtime.c \
             $(LIBRETRO_COMM_DIR)/hash/rhash.c

# Add libretro-common glsym for non-Android
ifeq (,$(findstring android,$(platform)))
    SOURCES_C += $(LIBRETRO_COMM_DIR)/glsym/glsym_gl.c \
                 $(LIBRETRO_COMM_DIR)/glsym/rglgen.c
endif

# C++ Source Files (80 files - verified to exist)
SOURCES_CXX := $(CORE_DIR)/Src/CPU/PowerPC/PPCDisasm.cpp \
               $(CORE_DIR)/Src/BlockFile.cpp \
               $(CORE_DIR)/Src/Model3/93C46.cpp \
               $(CORE_DIR)/Src/Model3/JTAG.cpp \
               $(CORE_DIR)/Src/Pkgs/imgui/imgui.cpp \
               $(CORE_DIR)/Src/Pkgs/imgui/imgui_draw.cpp \
               $(CORE_DIR)/Src/Pkgs/imgui/imgui_tables.cpp \
               $(CORE_DIR)/Src/Pkgs/imgui/imgui_widgets.cpp \
               $(CORE_DIR)/Src/Pkgs/imgui/imgui_impl_opengl3.cpp \
               $(CORE_DIR)/Src/Graphics/Legacy3D/Error.cpp \
               $(CORE_DIR)/Src/Graphics/Shader.cpp \
               $(CORE_DIR)/Src/Graphics/GLSLVersion.cpp \
               $(CORE_DIR)/Src/Model3/Real3D.cpp \
               $(CORE_DIR)/Src/Graphics/Legacy3D/Legacy3D.cpp \
               $(CORE_DIR)/Src/Graphics/Legacy3D/Models.cpp \
               $(CORE_DIR)/Src/Graphics/Legacy3D/TextureRefs.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/New3D.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/Mat4.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/Model.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/PolyHeader.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/TextureBank.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/VBO.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/Vec.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/R3DShader.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/R3DFloat.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/R3DScrollFog.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/R3DFrameBuffers.cpp \
               $(CORE_DIR)/Src/Graphics/New3D/GLSLShader.cpp \
               $(CORE_DIR)/Src/Graphics/FBO.cpp \
               $(CORE_DIR)/Src/Graphics/SuperAA.cpp \
               $(CORE_DIR)/Src/Graphics/Render2D.cpp \
               $(CORE_DIR)/Src/Model3/TileGen.cpp \
               $(CORE_DIR)/Src/Model3/Model3.cpp \
               $(CORE_DIR)/Src/CPU/PowerPC/ppc.cpp \
               $(if $(filter android aarch64 rpi64 osx,$(platform)),$(CORE_DIR)/Src/CPU/PowerPC/Jit/JitArm64.cpp,) \
               $(CORE_DIR)/Src/Model3/SoundBoard.cpp \
               $(CORE_DIR)/Src/Sound/SCSP.cpp \
               $(CORE_DIR)/Src/Sound/SCSPDSP.cpp \
               $(CORE_DIR)/Src/CPU/68K/68K.cpp \
               $(CORE_DIR)/Src/Model3/DSB.cpp \
               $(CORE_DIR)/Src/CPU/Z80/Z80.cpp \
               $(CORE_DIR)/Src/Model3/IRQ.cpp \
               $(CORE_DIR)/Src/Model3/53C810.cpp \
               $(CORE_DIR)/Src/Model3/PCI.cpp \
               $(CORE_DIR)/Src/Model3/RTC72421.cpp \
               $(CORE_DIR)/Src/Model3/DriveBoard/DriveBoard.cpp \
               $(CORE_DIR)/Src/Model3/DriveBoard/WheelBoard.cpp \
               $(CORE_DIR)/Src/Model3/DriveBoard/JoystickBoard.cpp \
               $(CORE_DIR)/Src/Model3/DriveBoard/SkiBoard.cpp \
               $(CORE_DIR)/Src/Model3/DriveBoard/BillBoard.cpp \
               $(CORE_DIR)/Src/Model3/DriveBoard/Z80CTC.cpp \
               $(CORE_DIR)/Src/Model3/MPC10x.cpp \
               $(CORE_DIR)/Src/Inputs/Input.cpp \
               $(CORE_DIR)/Src/Inputs/Inputs.cpp \
               $(CORE_DIR)/Src/Inputs/InputSource.cpp \
               $(CORE_DIR)/Src/Inputs/InputSystem.cpp \
               $(CORE_DIR)/Src/Inputs/InputTypes.cpp \
               $(CORE_DIR)/Src/Inputs/MultiInputSource.cpp \
               $(CORE_DIR)/Src/OSD/Outputs.cpp \
               $(CORE_DIR)/Src/Sound/MPEG/MpegAudio.cpp \
               $(CORE_DIR)/Src/Model3/Crypto.cpp \
               $(CORE_DIR)/Src/OSD/Logger.cpp \
               $(CORE_DIR)/Src/Util/Format.cpp \
               $(CORE_DIR)/Src/Util/NewConfig.cpp \
               $(CORE_DIR)/Src/Util/ByteSwap.cpp \
               $(CORE_DIR)/Src/Util/ConfigBuilders.cpp \
               $(CORE_DIR)/Src/GameLoader.cpp \
               $(CORE_DIR)/Src/Pkgs/tinyxml2.cpp \
               $(CORE_DIR)/Src/ROMSet.cpp \
               $(CORE_DIR)/Src/OSD/libretro/libretroAudio.cpp \
               $(CORE_DIR)/Src/OSD/libretro/libretroThread.cpp \
               $(CORE_DIR)/Src/OSD/libretro/libretroCrosshair.cpp \
               $(CORE_DIR)/Src/OSD/libretro/libretroGui.cpp \
               $(CORE_DIR)/Src/OSD/libretro/LibretroBlockFileMemory.cpp \
               $(CORE_DIR)/Src/OSD/libretro/CLibretroInputSystem.cpp \
               $(CORE_DIR)/Src/OSD/libretro/CLibretroOutputSystem.cpp \
               $(CORE_DIR)/Src/OSD/libretro/LibretroNetBoard.cpp \
               $(CORE_DIR)/Src/OSD/libretro/LibretroWrapper.cpp \
               $(CORE_DIR)/Src/OSD/libretro/vfs_ioapi.cpp \
               $(CORE_DIR)/Src/OSD/libretro/libretro.cpp

# Platform-specific OSD FileSystemPath implementation
ifeq ($(platform),win)
    SOURCES_CXX += $(CORE_DIR)/Src/OSD/Windows/FileSystemPath.cpp
else ifeq ($(platform),osx)
    SOURCES_CXX += $(CORE_DIR)/Src/OSD/OSX/FileSystemPath.cpp
else
    # osx, unix, android, rpi64, aarch64 all use Unix FileSystemPath
    SOURCES_CXX += $(CORE_DIR)/Src/OSD/Unix/FileSystemPath.cpp
endif

# Renderer selection: new3d (default) or legacy.
#   make platform=rpi64 RENDERER=legacy
# Legacy3D is the old desktop-GL renderer. It is being ported to run inside the
# GLES3 context (GLES2-style code: client vertex arrays, #version 100 shaders),
# so on GLES platforms it is only compiled in when explicitly asked for.
RENDERER ?= new3d

LEGACY3D_SOURCES := %/Legacy3D/Error.cpp %/Legacy3D/Legacy3D.cpp %/Legacy3D/Models.cpp %/Legacy3D/TextureRefs.cpp

ifeq ($(RENDERER),legacy)
    RENDERER_DEFINES := -DUSE_LEGACY3D
    DROP_LEGACY3D :=
else
    RENDERER_DEFINES :=
    DROP_LEGACY3D := 1
endif

# Platform-specific source filtering (MUST be before OBJECTS computation!)
# macOS and Android don't support Legacy3D (old fixed-pipeline OpenGL)
ifeq ($(platform),osx)
    SOURCES_CXX := $(filter-out $(LEGACY3D_SOURCES),$(SOURCES_CXX))
endif
ifeq ($(platform),android)
    SOURCES_CXX := $(filter-out $(LEGACY3D_SOURCES),$(SOURCES_CXX))
endif

# rpi64 and aarch64 use GLES3 (RetroArch on RPi5 provides only GLES context via EGL)
ifeq ($(platform),rpi64)
    SOURCES_C := $(filter-out %/glsym/glsym_gl.c,$(SOURCES_C))
    SOURCES_C += $(LIBRETRO_COMM_DIR)/glsym/glsym_es3.c
ifdef DROP_LEGACY3D
    SOURCES_CXX := $(filter-out $(LEGACY3D_SOURCES),$(SOURCES_CXX))
endif
endif
ifeq ($(platform),aarch64)
    SOURCES_C := $(filter-out %/glsym/glsym_gl.c,$(SOURCES_C))
    SOURCES_C += $(LIBRETRO_COMM_DIR)/glsym/glsym_es3.c
ifdef DROP_LEGACY3D
    SOURCES_CXX := $(filter-out $(LEGACY3D_SOURCES),$(SOURCES_CXX))
endif
endif

# Renderer capabilities exposed to the Libretro option layer. Keep these as
# feature defines rather than platform tests so future ports only need to
# enable the capability here.
ifneq ($(filter $(CORE_DIR)/Src/Graphics/Legacy3D/Legacy3D.cpp,$(SOURCES_CXX)),)
    RENDERER_DEFINES += -DHAVE_LEGACY3D
endif

ifneq ($(filter $(platform),osx android rpi64 aarch64),$(platform))
    RENDERER_DEFINES += -DHAVE_QUAD_RENDERING
endif

ifneq ($(filter $(platform),android rpi64 aarch64),$(platform))
    RENDERER_DEFINES += -DHAVE_CRT_COLOURS
    RENDERER_DEFINES += -DHAVE_SUPERSAMPLING
endif

# GIT version
GIT_VERSION := " $(shell git rev-parse --short HEAD || echo unknown)"
ifneq ($(GIT_VERSION)," unknown")
    COREDEFINES += -DGIT_VERSION=\"$(GIT_VERSION)\"
endif

OBJECTS := $(SOURCES_C:.c=.o) $(SOURCES_CXX:.cpp=.o)
DEPFILES := $(OBJECTS:.o=.d)

# Platform-specific defines (to be assembled with COREDEFINES into final DEFINES)
PLATFORM_DEFINES :=

# ============================================================
# Platform-Specific Configuration
# ============================================================

# ============ UNIX/LINUX (Default) ============
ifeq ($(platform),unix)
    TARGET := $(TARGET_NAME)_libretro.so
    LDFLAGS += -shared -fPIC
    CFLAGS += -fPIC
    CXXFLAGS += -fPIC
    # Legacy3D uses GLU's perspective helper. Unlike desktop OpenGL entry
    # points supplied by the Libretro frontend, GLU is not guaranteed to be
    # exported by the frontend process and must be an explicit dependency.
    LIBS += -ldl -lm -lz -lGLU
    INCFLAGS += -I/usr/include
endif

# ============ macOS / osxcross ============
ifeq ($(platform),osx)
    TARGET := $(TARGET_NAME)_libretro.dylib
    LDFLAGS += -dynamiclib -fPIC
    CFLAGS += -fPIC
    CXXFLAGS += -fPIC
    LIBS += -lm -framework OpenGL -framework CoreFoundation
    PLATFORM_DEFINES += -DGL_SILENCE_DEPRECATION

    ifeq ($(system_platform),osx)
        # Native macOS build (CI macOS runner or local Mac developer build).
        # Build for the host arch so the CI x64 job → x86_64, arm64 job → arm64.
        NATIVE_ARCH := $(shell uname -m)
        ARCHFLAGS := -arch $(NATIVE_ARCH)
        CFLAGS += $(ARCHFLAGS)
        CXXFLAGS += $(ARCHFLAGS)
        LDFLAGS += $(ARCHFLAGS)

        # The imported ARM64 backend uses Apple's MAP_JIT/W^X API on native
        # Apple Silicon. Keep Intel and osxcross universal builds on the
        # interpreter unless their JIT slices are configured independently.
        ifeq ($(NATIVE_ARCH),arm64)
            PLATFORM_DEFINES += -DHAVE_PPC_JIT
        endif
    else
        # Cross-compile from Linux via osxcross (universal x86_64 + arm64 dylib).
        OSXCROSS_ROOT ?= /opt/osxcross
        OSXCROSS_PATH := $(OSXCROSS_ROOT)/target/bin

        ifeq ($(wildcard $(OSXCROSS_PATH)/o64-clang++),)
            $(error osxcross not found at $(OSXCROSS_ROOT). Install: cd /tmp && git clone https://github.com/tpoechtrager/osxcross.git && cd osxcross && wget -nc https://github.com/rtrussell/osxcross-build/releases/download/12.0/MacOSX12.0.sdk.tar.xz -O tarballs/MacOSX12.0.sdk.tar.xz && ./build.sh)
        endif

        export PATH := $(OSXCROSS_PATH):$(PATH)
        override CC  := o64-clang
        override CXX := o64-clang++
        override LD  := o64-clang++

        ARCHFLAGS := -arch x86_64 -arch arm64
        CFLAGS  += $(ARCHFLAGS)
        CXXFLAGS += $(ARCHFLAGS)
        LDFLAGS += $(ARCHFLAGS)

        MACOSX_SDK ?= /opt/osxcross/target/SDK/MacOSX12.0.sdk
        CFLAGS   += -isysroot $(MACOSX_SDK)
        CXXFLAGS += -isysroot $(MACOSX_SDK)
        LDFLAGS  += -isysroot $(MACOSX_SDK)

        LDFLAGS += -static-libstdc++
    endif
endif

# ============ ANDROID ============
ifeq ($(platform),android)
    TARGET := $(TARGET_NAME)_libretro_android.so
    fpic := -fPIC
    SHARED := -shared
    PLATFORM_DEFINES += -DANDROID -D__LIBRETRO__ -DPSS_STYLE=1 -D_FILE_OFFSET_BITS=64
    PLATFORM_DEFINES += -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
    PLATFORM_DEFINES += -DEGL_EGLEXT_PROTOTYPES
    LIBS += -lGLESv3 -llog -lz
    
    # Prepend Android shim headers to resolve <GL/glew.h> to our shim
    INCFLAGS := -I$(CORE_DIR)/Src/OSD/Android/include $(INCFLAGS)
    
    # --- NDK Detection ---
    # Check common locations if NDK_ROOT is not already set
    ifeq ($(NDK_ROOT),)
        NDK_ROOT := $(shell ls -d ~/Android/Sdk/ndk/28.2.13676358 2>/dev/null)
    endif
    
    # Performance Flags from Mupen64Plus
    HAVE_NEON := 0
    ifeq ($(arch),arm)
        HAVE_NEON := 1
    endif
    ifeq ($(arch),arm64)
        HAVE_NEON := 1
    endif
    
    # LTO Control - Disable for Android due to PIC relocation issues with NDK clang
    HAVE_LTCG := 0
    
    # If NDK is found, use its Clang toolchain
    ifneq ($(NDK_ROOT),)
        # Default to aarch64 (64-bit ARM) if no 'arch' is specified
        ifeq ($(arch),arm)
            ARCH_TRIPLE := arm-linux-androideabi
            CLANG_TRIPLE := armv7a-linux-androideabi24
            PLATFORM_DEFINES += -march=armv7-a -mfloat-abi=softfp -mfpu=neon
        else ifeq ($(arch),x86)
            ARCH_TRIPLE := i686-linux-android
            CLANG_TRIPLE := i686-linux-android24
        else ifeq ($(arch),x86_64)
            ARCH_TRIPLE := x86_64-linux-android
            CLANG_TRIPLE := x86_64-linux-android24
        else
            # Default to arm64 (aarch64)
            ARCH_TRIPLE := aarch64-linux-android
            CLANG_TRIPLE := aarch64-linux-android24
            PLATFORM_DEFINES += -DHAVE_PPC_JIT
        endif
        
        TOOLCHAIN := $(NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin
        CC  := $(TOOLCHAIN)/$(CLANG_TRIPLE)-clang
        CXX := $(TOOLCHAIN)/$(CLANG_TRIPLE)-clang++
        LD  := $(CXX)
        
        # Android Clang usually needs some extra flags
        CFLAGS   += -ffunction-sections -fdata-sections
        CXXFLAGS += -ffunction-sections -fdata-sections
        LDFLAGS  += $(SHARED) -Wl,-soname,$(TARGET) -Wl,--no-undefined
        
        # Link C++ library statically to avoid dlopen issues on device
        LDFLAGS  += -static-libstdc++
    else
        # Fallback to generic names if NDK not found
        CC  = arm-linux-androideabi-gcc
        CXX = arm-linux-androideabi-g++
        LD  := $(CXX)
    endif
    
    ifeq ($(HAVE_NEON),1)
        PLATFORM_DEFINES += -DHAVE_NEON -D__ARM_NEON__ -ftree-vectorize
    endif
    
    OBJOUT  = -o
    LINKOUT = -o
    LIBM    := -lm
    WARNING_DEFINES := -Wno-write-strings
    
    # Ensure CXXFLAGS is appended
    CXXFLAGS += -std=c++17
    
    ifeq ($(DEBUG),1)
        CFLAGS   += -O0 -g -DDEBUG
        CXXFLAGS += -O0 -g -DDEBUG
    else
        CFLAGS   += -O3 -DNDEBUG
        CXXFLAGS += -O3 -DNDEBUG
        
        # General Performance Optimizations
        CFLAGS   += -ffast-math -funsafe-math-optimizations -fomit-frame-pointer
        CXXFLAGS += -ffast-math -funsafe-math-optimizations -fomit-frame-pointer
        
        # Visibility Optimizations
        CFLAGS   += -fvisibility=hidden
        CXXFLAGS += -fvisibility=hidden -fvisibility-inlines-hidden
        
        # LTO (Link Time Optimization)
        ifeq ($(HAVE_LTCG),1)
            CFLAGS   += -flto
            CXXFLAGS += -flto
            LDFLAGS  += -flto
        endif
    endif
endif

# ============ WINDOWS / MinGW ============
ifeq ($(platform),win)
    TARGET := $(TARGET_NAME)_libretro.dll
    LDFLAGS += -shared
    CFLAGS += -D_WIN32 -DWIN32
    CXXFLAGS += -D_WIN32 -DWIN32
    
    ifeq ($(system_platform),win)
        # Native Windows build with MinGW
        override CC := gcc
        override CXX := g++
    else
        # Cross-compile to Windows (from Linux)
        override CC := x86_64-w64-mingw32-gcc
        override CXX := x86_64-w64-mingw32-g++
        override AR := x86_64-w64-mingw32-ar
    endif
    
    # Static-link MinGW runtimes and zlib so the DLL is self-contained — no
    # libgcc_s_seh-1.dll / libstdc++-6.dll / zlib1.dll required on the user's system.
    LDFLAGS += -static-libgcc -static-libstdc++
    LIBS += -lm -Wl,-Bstatic -lz -Wl,-Bdynamic -lopengl32 -lglu32 -lgdi32
endif

# ============ Raspberry Pi 64-bit ============
ifeq ($(platform),rpi64)
    CC  = aarch64-linux-gnu-gcc
    CXX = aarch64-linux-gnu-g++
    LD  = aarch64-linux-gnu-g++
    
    TARGET := $(TARGET_NAME)_libretro_aarch64.so
    fpic   := -fPIC
    SHARED := -shared -Wl,--version-script=$(CORE_DIR)/link.T -Wl,-no-undefined
    
    # 1. ARCHITECTURE & FEATURE FLAGS
    PLATFORM_DEFINES += -DARM -D__aarch64__ -DLSB_FIRST -DGL_GLEXT_PROTOTYPES -DHAVE_PPC_JIT
    PLATFORM_DEFINES += -fomit-frame-pointer -ffast-math -funsafe-math-optimizations
    # Prevent system header redeclaration
    PLATFORM_DEFINES += -DGLES -Dgles -DHAVE_OPENGLES=1 -DHAVE_OPENGLES3=1 -DCORE_GLES -D__glext_h_ -D__GLEXT_H_

    # 2. CPU TUNING
    # platform=rpi64 always matches above, so the specific board must be
    # selected via board=rpi5 / board=rpi4 (e.g. make platform=rpi64 board=rpi5)
    ifeq ($(board), rpi5)
        PLATFORM_DEFINES += -mcpu=cortex-a76
    else ifeq ($(board), rpi4)
        PLATFORM_DEFINES += -mcpu=cortex-a72
    else
        PLATFORM_DEFINES += -mcpu=cortex-a53
    endif

    # 3. LIBRARY & PATHS
    LDFLAGS += $(SHARED) -L/usr/lib/aarch64-linux-gnu
    LIBS := -lGLESv2 -lz -lm

    # 4. CXXFLAGS
    CXXFLAGS += -std=c++17
endif

# ============ aarch64 (Generic ARM64) ============
ifeq ($(platform),aarch64)
    CC  = aarch64-linux-gnu-gcc
    CXX = aarch64-linux-gnu-g++
    LD  = aarch64-linux-gnu-g++
    
    TARGET := $(TARGET_NAME)_libretro.so
    fpic   := -fPIC
    SHARED := -shared -Wl,--version-script=$(CORE_DIR)/link.T -Wl,-no-undefined
    
    # 1. ARCHITECTURE & FEATURE FLAGS
    PLATFORM_DEFINES += -DARM -D__aarch64__ -DLSB_FIRST -DGL_GLEXT_PROTOTYPES -DHAVE_PPC_JIT
    PLATFORM_DEFINES += -fomit-frame-pointer -ffast-math -funsafe-math-optimizations
    # Prevent system header redeclaration
    PLATFORM_DEFINES += -DGLES -Dgles -DHAVE_OPENGLES=1 -DHAVE_OPENGLES3=1 -DCORE_GLES -D__glext_h_ -D__GLEXT_H_

    # 2. LIBRARY & PATHS
    LDFLAGS += $(SHARED) -L/usr/lib/aarch64-linux-gnu
    LIBS := -lGLESv2 -lz -lm
    
    # 3. CXXFLAGS
    CXXFLAGS += -std=c++17
endif

# ============ COMMON COMPILER FLAGS ============

# C compiler defaults
CC ?= gcc
CXX ?= g++
AR ?= ar

# Assemble final DEFINES from COREDEFINES and PLATFORM_DEFINES (mirrors old build system)
DEFINES := $(COREDEFINES) $(PLATFORM_DEFINES) $(RENDERER_DEFINES)
OBJOUT  ?= -o
LINKOUT ?= -o
LIBM    ?= -lm
WARNING_DEFINES ?= -Wno-write-strings

# Base optimization and warning flags
CFLAGS := $(CFLAGS) -Wall -Wextra -O3 -ffast-math -DHAVE_ZLIB $(fpic)
CXXFLAGS := $(CXXFLAGS) -Wall -Wextra -std=c++17 -O3 -ffast-math -DHAVE_ZLIB $(fpic)

# Include path flags and final defines assembly
CFLAGS += $(INCFLAGS) $(DEFINES)
CXXFLAGS += $(INCFLAGS) $(DEFINES)

# Debug settings
ifeq ($(DEBUG),1)
    CFLAGS := $(filter-out -O3,$(CFLAGS)) -g -O0 -DDEBUG
    CXXFLAGS := $(filter-out -O3,$(CXXFLAGS)) -g -O0 -DDEBUG
    LDFLAGS += -g
endif

# External zlib
ifeq ($(EXTERNAL_ZLIB),1)
    INCFLAGS += $(shell pkg-config --cflags zlib)
    LIBS += $(shell pkg-config --libs zlib)
else
    LIBS += -lz
endif

# ============================================================
# Build Rules
# ============================================================

.PHONY: all clean info
$(info PLATFORM_DEFINES ARE: $(PLATFORM_DEFINES))
all: $(TARGET)

# Track included headers so incremental builds rebuild every object whose ABI
# may have changed. This is especially important for shared core option types.
-include $(DEPFILES)

$(MUSASHI_GEN_DIR):
	mkdir -p $@

$(MUSASHI_GENERATOR): $(MUSASHI_DIR)/m68kmake.c $(MUSASHI_DIR)/m68k_in.c | $(MUSASHI_GEN_DIR)
	$(CC) $(CFLAGS) $(MUSASHI_DIR)/m68kmake.c -o $@

$(MUSASHI_GEN_DIR)/.generated: $(MUSASHI_GENERATOR) $(MUSASHI_DIR)/m68k_in.c $(MUSASHI_DIR)/m68k.h $(MUSASHI_DIR)/m68kconf.h
	$(MUSASHI_GENERATOR) $(MUSASHI_GEN_DIR) $(MUSASHI_DIR)/m68k_in.c
	touch $@

$(MUSASHI_GENERATED): $(MUSASHI_GEN_DIR)/.generated

$(MUSASHI_DIR)/m68kcpu.o: $(MUSASHI_GEN_DIR)/m68kops.h

$(TARGET): $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $@
	@echo "Build complete: $@"

# Special handling for ppc.o: strip -ffast-math to avoid FENV_ACCESS pragma conflicts
# ppc_ops.c contains #pragma STDC FENV_ACCESS ON which requires precise FP semantics
# For Android DEBUG builds, also ensure -fPIC is applied to prevent relocation errors
$(CORE_DIR)/Src/CPU/PowerPC/ppc.o: CXXFLAGS := $(filter-out -ffast-math -funsafe-math-optimizations,$(CXXFLAGS))
$(CORE_DIR)/Src/CPU/PowerPC/ppc.o: CXXFLAGS += -Wno-unused-parameter

# Both 3D renderers explicitly sanitize NaN/Inf fog values used by Star Wars
# Trilogy. Fast-math assumes these values cannot exist and removes the checks.
IEEE_FP_RENDERER_OBJECTS := $(CORE_DIR)/Src/Graphics/New3D/New3D.o \
                            $(CORE_DIR)/Src/Graphics/Legacy3D/Legacy3D.o
$(IEEE_FP_RENDERER_OBJECTS): CXXFLAGS := $(filter-out -ffast-math -funsafe-math-optimizations,$(CXXFLAGS))

ifeq ($(platform),android)
  ifeq ($(DEBUG),1)
    $(CORE_DIR)/Src/CPU/PowerPC/ppc.o: CXXFLAGS += -fPIC
  endif
endif

# These vendored/generated sources intentionally expose platform-dependent
# parameters and legacy implementation details that are unused by this core.
# Keep their known warning noise local instead of weakening warnings globally.
MUSASHI_OBJECTS := $(MUSASHI_DIR)/m68kcpu.o \
                   $(MUSASHI_GEN_DIR)/m68kops.o \
                   $(MUSASHI_GEN_DIR)/m68kopac.o \
                   $(MUSASHI_GEN_DIR)/m68kopdm.o \
                   $(MUSASHI_GEN_DIR)/m68kopnz.o
LIBRETRO_COMMON_WARNING_OBJECTS := $(LIBRETRO_COMM_DIR)/file/file_path.o \
                                   $(LIBRETRO_COMM_DIR)/file/retro_dirent.o \
                                   $(LIBRETRO_COMM_DIR)/vfs/vfs_implementation.o \
                                   $(LIBRETRO_COMM_DIR)/string/stdstring.o

$(MUSASHI_OBJECTS): CFLAGS += -Wno-unused-parameter
$(DEPS_DIR)/ugui/ugui.o: CFLAGS += -Wno-sign-compare -Wno-bitwise-op-parentheses -Wno-unused-but-set-variable
$(LIBRETRO_COMMON_WARNING_OBJECTS): CFLAGS += -Wno-unused-parameter -Wno-sign-compare

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

clean:
	@echo "Cleaning..."
	@rm -f $(OBJECTS) $(DEPFILES) $(TARGET)
	@rm -rf $(MUSASHI_GEN_DIR)
	@echo "Clean complete"

info:
	@echo "Platform: $(platform)"
	@echo "Target: $(TARGET)"
	@echo "CC: $(CC)"
	@echo "CXX: $(CXX)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "LIBS: $(LIBS)"
