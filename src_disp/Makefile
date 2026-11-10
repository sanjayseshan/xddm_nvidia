CC = x86_64-w64-mingw32-gcc
CC32 = i686-w64-mingw32-gcc
DLLTOOL = x86_64-w64-mingw32-dlltool
CFLAGS = -Wall -Wextra -O2 -nostdlib -I. -I./include -D_WIN32_WINNT=0x0502
CFLAGS32 = -fno-builtin -Wall -Wextra -O2 -nostdlib -I. -I./include -D_WIN32_WINNT=0x0501 -m32
LDFLAGS = -shared -nostdlib -Wl,-e,DrvEnableDriver -Wl,--subsystem,native:5.2 -L. -lwin32k -lntoskrnl
LDFLAGS32 = -shared -nostdlib -Wl,-e,_DrvEnableDriver@12 -Wl,--subsystem,native:5.1 /usr/i686-w64-mingw32/lib/libwin32k.a -lgcc
HOSTCC ?= cc
NVX_ENABLE_EXPERIMENTAL_OPENGL_DDI ?= 0

ifeq ($(NVX_ENABLE_EXPERIMENTAL_OPENGL_DDI),1)
CFLAGS += -DNVX_ENABLE_EXPERIMENTAL_OPENGL_DDI=1
endif

OBJS = enable.o screen.o blt.o text.o diagnostic.o opengl.o
OBJS32 = runtime32.o enable32.o screen32.o blt32.o text32.o diagnostic32.o opengl32.o

all: nvxdisp.dll nvxdisp32.dll

libwin32k.a: win32k.def
	$(DLLTOOL) -d $< -l $@

libntoskrnl.a: ntoskrnl.def
	$(DLLTOOL) -d $< -l $@

nvxdisp.dll: $(OBJS) libwin32k.a libntoskrnl.a
	$(CC) $(OBJS) $(LDFLAGS) -o $@

nvxdisp32.dll: $(OBJS32) nvxdisp32.def tests/check_xp32_imports.py
	$(CC32) $(OBJS32) nvxdisp32.def -Wl,--kill-at $(LDFLAGS32) -o $@
	python3 tests/check_xp32_imports.py $@

%.o: %.c driver.h color.h ../src_driver/nvx_3d_abi.h
	$(CC) $(CFLAGS) -c $< -o $@

%32.o: %.c driver.h color.h ../src_driver/nvx_3d_abi.h
	$(CC32) $(CFLAGS32) -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS32) nvxdisp.dll nvxdisp32.dll libwin32k.a libntoskrnl.a

check:
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -O2 -fno-builtin -DNVX_RUNTIME_HOSTED tests/runtime_test.c runtime.c -o /tmp/nvx-runtime-host-test
	/tmp/nvx-runtime-host-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -I. tests/color_test.c -o /tmp/nvx-color-test
	/tmp/nvx-color-test
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -I. tests/opengl_icd_test.c -o /tmp/nvx-opengl-icd-test
	/tmp/nvx-opengl-icd-test

.PHONY: all clean check
