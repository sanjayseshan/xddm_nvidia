@echo off
if exist C:\SLOG.COM C:\SLOG.COM [WIN98-BOOT] Applying hardware configuration (NVX.DRV + NVX.VXD)...
if exist C:\NVX_REG.REG C:\WINDOWS\regedit.exe /L:C:\WINDOWS\SYSTEM.DAT /R:C:\WINDOWS\USER.DAT C:\NVX_REG.REG
if exist C:\NVX_REG.REG ren C:\NVX_REG.REG NVX_REG.BAK
if exist C:\WINDOWS\FAKEKBD.COM C:\WINDOWS\FAKEKBD.COM
if exist C:\SLOG.COM C:\SLOG.COM [WIN98-BOOT] Launching Windows 98 GUI...
win /b
if exist C:\SLOG.COM C:\SLOG.COM [WIN98-BOOT] Windows session terminated.
