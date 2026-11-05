CC = i686-w64-mingw32-gcc
CFLAGS = -m32 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-pointer-to-int-cast -D__VXDKERNEL__ -I../src_driver -I../src_driver/tests -Ishim -nostdlib -fno-builtin

SRCS = shim/videoprt_shim.c \
       ../src_driver/nvx_fw.c \
       ../src_driver/nvx_rpc.c \
       ../src_driver/nvx_seq.c \
       ../src_driver/nvx_mmu.c \
       ../src_driver/nvx_vbios.c

OBJS = $(SRCS:.c=.o)

all: libnvx_core32.a

libnvx_core32.a: $(OBJS)
	i686-w64-mingw32-ar rcs $@ $^

clean:
	rm -f $(OBJS) libnvx_core32.a

