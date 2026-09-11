#ifndef HOST_DSOUND_H
#define HOST_DSOUND_H

#include "host_win.h"

#define DSSCL_NORMAL 0x00000001
#define DSSCL_PRIORITY 0x00000002
#define DSBCAPS_CTRLVOLUME 0x00000080
#define DSBCAPS_GLOBALFOCUS 0x00008000

struct IDirectSound8;
struct IDirectSoundBuffer8;
typedef struct IDirectSound8 *LPDIRECTSOUND8;
typedef struct IDirectSoundBuffer8 *LPDIRECTSOUNDBUFFER8;

HRESULT DirectSoundCreate8(GUID *guid, LPDIRECTSOUND8 *ppv, LPVOID unk);

#endif
