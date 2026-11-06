#!/bin/sh
# build WINVBE.DRV / NVX.DRV / NVX2.DRV
set -e
cd "$(dirname "$0")"
. ../owenv.sh
RES=${RES:-../res}
python3 ../tools/mkres.py ../res >/dev/null

# ----------------- 1. Build Primary Driver (Head 0, 1080p, LFB 0xE0000000) -----------------
echo "==> Building Primary Driver (nvx.drv)..."
mkdir -p obj
CFLAGS="$XCF -q -ml -zc -zu -s -3 -ox -zl -zls -wx -we -wcd=303 -wcd=202 -bt=windows -fpi -zq -i=."
for f in util vbe color rop surf blt cursor enable robj output text dib dibeng ddhal; do
  wcc $CFLAGS -fo=obj/$f.obj $f.c
done
nasm -f obj -o obj/entry.obj entry.asm
cd obj
wlink @../display.lnk file entry.obj,util.obj,vbe.obj,color.obj,rop.obj,surf.obj,blt.obj,cursor.obj,enable.obj,robj.obj,output.obj,text.obj,dib.obj,dibeng.obj,ddhal.obj
cd ..

STOCK=../msfiles/stock
mv obj/winvbe.drv obj/winvbe.bin
for v in 30:w3:VGA30.DRV 31:w3:VGA31.DRV 9x:w9:-; do
  n=${v%%:*}; m=${v#*:}; m=${m%%:*}; f=${v##*:}
  out=obj/winvbe$n.drv; [ $n = 31 ] && out=obj/winvbe.drv
  python3 ../tools/stockres.py $m $STOCK/$f $RES obj/res$n
  [ $n = 30 ] || cat >> obj/res$n/winvbe.rc <<'VEOF'
1 VERSIONINFO
FILEVERSION 1,2,0,0
PRODUCTVERSION 1,2,0,0
FILEFLAGSMASK 0x3F
FILEFLAGS 0
FILEOS 0x1
FILETYPE 3
FILESUBTYPE 4
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904E4"
        BEGIN
            VALUE "CompanyName", "WINVBE\0"
            VALUE "FileDescription", "WINVBE Universal VESA/VGA/SVGA display driver\0"
            VALUE "FileVersion", "1.2\0"
            VALUE "InternalName", "DISPLAY\0"
            VALUE "LegalCopyright", "GNU General Public License v3\0"
            VALUE "OriginalFilename", "WINVBE.DRV\0"
            VALUE "ProductName", "WINVBE\0"
            VALUE "ProductVersion", "1.2\0"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1252
    END
END
VEOF
  wrc -q -r -bt=windows -30 -i=obj/res$n -fo=obj/winvbe$n.res obj/res$n/winvbe.rc
  cp obj/winvbe.bin $out
  wrc -q -bt=windows -30 obj/winvbe$n.res $out -fe=$out
done
mkdir -p ../../dist_win98
cp obj/winvbe9x.drv ../../dist_win98/nvx.drv

# ----------------- 2. Build Secondary Driver (Head 1, 1440p, LFB 0xE2000000) -----------------
echo "==> Building Secondary Driver (nvx2.drv)..."
mkdir -p obj_sec
CFLAGS_SEC="$CFLAGS -DSECONDARY_HEAD=1"
for f in util vbe color rop surf blt cursor enable robj output text dib dibeng ddhal; do
  wcc $CFLAGS_SEC -fo=obj_sec/$f.obj $f.c
done
nasm -f obj -o obj_sec/entry.obj entry.asm
cd obj_sec
wlink @../display_sec.lnk file entry.obj,util.obj,vbe.obj,color.obj,rop.obj,surf.obj,blt.obj,cursor.obj,enable.obj,robj.obj,output.obj,text.obj,dib.obj,dibeng.obj,ddhal.obj
cd ..

python3 ../tools/stockres.py w9 - $RES obj_sec/res9x
wrc -q -r -bt=windows -30 -i=obj_sec/res9x -fo=obj_sec/winvbe9x.res obj_sec/res9x/winvbe.rc
cp obj_sec/nvx2.drv obj_sec/winvbe9x.drv
wrc -q -bt=windows -30 obj_sec/winvbe9x.res obj_sec/winvbe9x.drv -fe=obj_sec/winvbe9x.drv
cp obj_sec/winvbe9x.drv ../../dist_win98/nvx2.drv

ls -l obj/winvbe9x.drv obj_sec/winvbe9x.drv ../../dist_win98/nvx.drv ../../dist_win98/nvx2.drv
