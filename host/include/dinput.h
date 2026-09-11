#ifndef HOST_DINPUT_H
#define HOST_DINPUT_H

#include "host_win.h"

#define DISCL_EXCLUSIVE 0x00000001
#define DISCL_NONEXCLUSIVE 0x00000002
#define DISCL_FOREGROUND 0x00000004
#define DISCL_BACKGROUND 0x00000008
#define DIDFT_ALL 0x00000000

struct IDirectInput8A;
struct IDirectInputDevice8A;
typedef struct IDirectInput8A *LPDIRECTINPUT8;
typedef struct IDirectInputDevice8A *LPDIRECTINPUTDEVICE8;

HRESULT DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid,
			   LPVOID *ppv, LPVOID unk);

#endif
