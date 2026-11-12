/* gens64: route MessageBox through Gens64_MessageBox (hashlog.cpp). Without -batch it is a plain
   MessageBoxA; with -batch it prints the text to stderr and returns the default button, so batch
   playback never blocks on a modal dialog. Force-included by CMake for the Gens sources that use it. */
#ifndef GENS64_MSGBOX_H
#define GENS64_MSGBOX_H
#include <windows.h>
#ifdef __cplusplus
extern "C"
#endif
int WINAPI Gens64_MessageBox(HWND hWnd, LPCSTR text, LPCSTR caption, UINT type);
#undef MessageBox
#define MessageBox Gens64_MessageBox
#endif
