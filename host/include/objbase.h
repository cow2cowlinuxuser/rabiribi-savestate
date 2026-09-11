#ifndef HOST_OBJBASE_H
#define HOST_OBJBASE_H

#include "host_win.h"

HRESULT CoInitialize(LPVOID reserved);
void CoUninitialize(void);
HRESULT CoCreateInstance(REFCLSID clsid, LPVOID unk, DWORD ctx, REFIID iid,
			 LPVOID *ppv);
void CoTaskMemFree(LPVOID p);

#endif
