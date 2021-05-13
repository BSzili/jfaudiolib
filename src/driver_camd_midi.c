/*
 Copyright (C) 2009 Jonathon Fowler <jf@jonof.id.au>

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

 See the GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

 */
#include "driver_camd_midi.h"

#include "music.h"

#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <exec/ports.h>
#include <midi/camd.h>
#include <midi/mididefs.h>
#include <clib/alib_protos.h>
#include <proto/camd.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/realtime.h>

#include <string.h>

struct Library *CamdBase = NULL;
struct RealTimeBase *RealTimeBase = NULL;

static inline struct ExecBase *getSysBase(void) { return SysBase; }
#define LOCAL_SYSBASE() struct ExecBase *const SysBase = getSysBase()

static inline struct Library *getCamdBase(void) { return CamdBase; }
#define LOCAL_CAMDBASE() struct Library *const CamdBase = getCamdBase()

static inline struct Library *getRealTimeBase(void) { return (struct Library *)RealTimeBase; }
#define LOCAL_REALTIMEBASE()                                                   \
  struct Library *const RealTimeBase = getRealTimeBase()

typedef enum {
  CamdDrv_Success = 0,
  CamdDrv_Error,
  CamdDrv_FailedOpenCamdLibrary,
  CamdDrv_FailedOpenRealTimeLibrary,
} CamdDrvErrors;

typedef enum { PC_CHANGETEMPO } PlayerCommand;

typedef struct {
  struct Message msg;
  PlayerCommand code;
  int data;
} PlayerMessage;

void (*g_service)() = NULL;
static struct Task *g_MainTask = NULL;
struct Task *g_serviceTask = NULL;
static struct Player *g_player = NULL;
static struct MidiNode *g_midiNode = NULL;
static struct MidiLink *g_midiLink = NULL;
static BYTE g_playerSignalBit = -1;
static volatile int s_CamdTicksPerMidiFrame = 12;

static CamdDrvErrors g_error = CamdDrv_Success;

static void /*__saveds*/ __stdargs ServiceTask(void);

static void Func_NoteOff(int channel, int key, int velocity) {
  MidiMsg mm = {{0, 0}};
  mm.mm_Status = MS_NoteOff | channel;
  mm.mm_Data1 = key & 0x7F;
  mm.mm_Data2 = velocity;

  PutMidiMsg(g_midiLink, &mm);
}

static void Func_NoteOn(int channel, int key, int velocity) {
  MidiMsg mm = {{0, 0}};
  mm.mm_Status = MS_NoteOn | channel;
  mm.mm_Data1 = key;
  mm.mm_Data2 = velocity;

  PutMidiMsg(g_midiLink, &mm);
}

static void Func_PolyAftertouch(int channel, int key, int pressure) {
  MidiMsg mm = {{0, 0}};
  mm.mm_Status = MS_PolyPress | channel;
  mm.mm_Data1 = key;
  mm.mm_Data2 = pressure;

  PutMidiMsg(g_midiLink, &mm);
}

static void Func_ControlChange(int channel, int number, int value) {
  MidiMsg mm = {{0, 0}};
  mm.mm_Status = MS_Ctrl | channel;
  mm.mm_Data1 = number;
  mm.mm_Data2 = value;

  PutMidiMsg(g_midiLink, &mm);
}

static void Func_ProgramChange(int channel, int program) {
  MidiMsg mm = {{0, 0}};
  mm.mm_Status = MS_Prog | channel;
  mm.mm_Data1 = program;

  PutMidiMsg(g_midiLink, &mm);
}

static void Func_ChannelAftertouch(int channel, int pressure) {
  MidiMsg mm = {{0, 0}};
  mm.mm_Status = MS_ChanPress | channel;
  mm.mm_Data1 = pressure;

  PutMidiMsg(g_midiLink, &mm);
}

static void Func_PitchBend(int channel, int lsb, int msb) {
  MidiMsg mm = {{0, 0}};
  mm.mm_Status = MS_PitchBend | channel;
  mm.mm_Data1 = lsb;
  mm.mm_Data2 = msb;

  PutMidiMsg(g_midiLink, &mm);
}

static void Func_SysEx(const unsigned char *data, int length) {
  PutSysEx(g_midiLink, (UBYTE *)data);
}

void ShutDownCamd(void) {
  if (g_midiNode) {
    // FIXME: reset sysex to device to get clean slate
    FlushMidi(g_midiNode);
    if (g_midiLink) {
      RemoveMidiLink(g_midiLink);
      g_midiLink = NULL;
    }
    DeleteMidi(g_midiNode);
    g_midiNode = NULL;
  }
  if (RealTimeBase) {
    CloseLibrary((struct Library *)RealTimeBase);
    RealTimeBase = NULL;
  }
  if (CamdBase) {
    CloseLibrary(CamdBase);
    CamdBase = NULL;
  }
}

static LONG GetVarQuiet(CONST_STRPTR name, STRPTR buffer, LONG size, LONG flags) {
  LONG ret;
  struct Process *me;
  APTR oldwindow;

  // Just like GetVar, but it won't open any requesters
  me = (struct Process *)FindTask(NULL);
  oldwindow = me->pr_WindowPtr;
  ret = GetVar(name, buffer, size, flags);
  me->pr_WindowPtr = oldwindow;

  return ret;
}

const char *FindMidiDevice(void) {
  LOCAL_CAMDBASE();

  static char _outport[128] = "";
  char *retname = NULL;

  APTR key = LockCAMD(CD_Linkages);
  if (key != NULL) {
    struct MidiCluster *cluster = NextCluster(NULL);

    while (cluster && !retname) {
      // Get the current cluster name
      char *dev = cluster->mcl_Node.ln_Name;

      if (strstr(dev, "out") != NULL) {
        // This is an output device, return this
        strncpy(_outport, dev, sizeof(_outport));
        retname = _outport;
      } else {
        // Search the next one
        cluster = NextCluster(cluster);
      }
    }

    // If the user has a preference outport set, use this instead
    if (GetVarQuiet("DefMidiOut", _outport, sizeof(_outport), GVF_GLOBAL_ONLY)) {
      retname = _outport;
    }

    UnlockCAMD(key);
  }

  return retname;
}

static int InitCamd(void) {

  if (!(CamdBase = OpenLibrary("camd.library", 0))) {
    g_error = CamdDrv_FailedOpenCamdLibrary;
    goto failure;
  }
  if (!(RealTimeBase = (struct RealTimeBase *)OpenLibrary("realtime.library", 0))) {
    g_error = CamdDrv_FailedOpenRealTimeLibrary;
    goto failure;
  }

  g_midiNode = CreateMidi(MIDI_MsgQueue, 0L, MIDI_SysExSize, 0, MIDI_Name,
                          (Tag) "JFAudioLib Midi Out", TAG_END);
  if (!g_midiNode) {
    g_error = CamdDrv_Error;
    goto failure;
  }

  const char *deviceName = FindMidiDevice();
  if (!deviceName) {
    g_error = CamdDrv_Error;
    goto failure;
  }

  g_midiLink = AddMidiLink(g_midiNode, MLTYPE_Sender, MLINK_Location,
                           (Tag)deviceName /*"out.0"*/, TAG_END);
  if (!g_midiLink) {
    g_error = CamdDrv_Error;
    goto failure;
  }

  return CamdDrv_Success;

failure:
  ShutDownCamd();
  return g_error;
}
int CamdDrv_GetError() { return g_error; }

const char *CamdDrv_ErrorString(int ErrorNumber) {
  switch (ErrorNumber) {
  case CamdDrv_Success:
    return "CamdDrv_Success";
  case CamdDrv_Error:
    return "CamdDrv_Error";
  case CamdDrv_FailedOpenCamdLibrary:
    return "CamdDrv: failed to open camd.library";
  case CamdDrv_FailedOpenRealTimeLibrary:
    return "CamdDrv: failed to open realtime.library";
  default:
    return "unknown error";
  }
}

int CamdDrv_MIDI_Init(midifuncs *funcs, const char *params) {

  g_error = CamdDrv_Success;

  if (InitCamd() != CamdDrv_Success) {
    return MUSIC_Error;
  }

  memset(funcs, 0, sizeof(midifuncs));

  funcs->NoteOff = Func_NoteOff;
  funcs->NoteOn = Func_NoteOn;
  funcs->PolyAftertouch = Func_PolyAftertouch;
  funcs->ControlChange = Func_ControlChange;
  funcs->ProgramChange = Func_ProgramChange;
  funcs->ChannelAftertouch = Func_ChannelAftertouch;
  funcs->PitchBend = Func_PitchBend;
  funcs->SysEx = Func_SysEx;

  // FIXME: what about these? None of the other midi driver implements them
  //  void ( *ReleasePatches )( void );
  //  void ( *LoadPatch )( int number );
  //  void ( *SetVolume )( int volume );
  //  int  ( *GetVolume )( void );

  return MUSIC_Ok;
}

void CamdDrv_MIDI_Shutdown() { ShutDownCamd(); }

int CamdDrv_MIDI_StartPlayback(void (*service)()) {

  g_service = service;

  if (!g_serviceTask) {
    g_MainTask = FindTask(NULL);

    BPTR file = 0;
    //    Open("PROGDIR:log.txt", MODE_READWRITE);
    //    Seek(file, 0, OFFSET_END);
    //    FPrintf(file, "Creating new Task  -------------------\n");

    // Set priority to 21, so it is just a bit higher than input and mouse
    // movements won't slow down playback
    if (!(g_serviceTask = (struct Task *)CreateNewProcTags(
              NP_Name, (Tag) "JFAudioLib MidiService", NP_Priority, 21, NP_Entry,
              (Tag)ServiceTask, NP_StackSize, 64000, NP_Output, (Tag)file,
              TAG_END)) == NULL) {
      g_error = CamdDrv_Error;
      return MUSIC_Error;
    }

    ULONG signal = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E);
    if (!(signal & SIGBREAKF_CTRL_E)) {
      // Task failed to start or allocate resources. It'll signal
      // SIGBREAKF_CTRL_C and exit
      g_serviceTask = 0;

      g_error = CamdDrv_Error;

      return MUSIC_Error;
    }
  }

  return MUSIC_Ok;
}

void CamdDrv_MIDI_HaltPlayback() {
  if (g_serviceTask) {
    Signal(g_serviceTask, SIGBREAKF_CTRL_C);
    Wait(SIGBREAKF_CTRL_E);
    g_serviceTask = NULL;
  }
}

void CamdDrv_MIDI_SetTempo(int tempo, int division) {

  int secondspertick = (60 << 16) / (tempo * division);
  s_CamdTicksPerMidiFrame = (TICK_FREQ * secondspertick) >> 16;
}

void CamdDrv_MIDI_Lock() {}

void CamdDrv_MIDI_Unlock() {}

static void /*__saveds*/ __stdargs ServiceTask(void) {
  LOCAL_SYSBASE();
  LOCAL_REALTIMEBASE();

  BOOL initSuccess = FALSE;

  struct Task *thisTask = FindTask(NULL);

  if ((g_playerSignalBit = AllocSignal(-1)) == -1) {
    g_error = CamdDrv_Error;
    goto failure;
  }

  ULONG err = 0;
  g_player = CreatePlayer(
      PLAYER_Name, (Tag) "JFAudioLib Player", PLAYER_Conductor, (Tag) "JFAudioLib Conductor",
      PLAYER_AlarmSigTask, (Tag)thisTask, PLAYER_AlarmSigBit,
      (Tag)g_playerSignalBit, PLAYER_ErrorCode, (Tag)&err, TAG_END);

  if (!g_player) {
    g_error = CamdDrv_Error;
    goto failure;
  }

  SetConductorState(g_player, CONDSTATE_RUNNING, 0);

  const ULONG playerSignalBitMask = (1UL << g_playerSignalBit);
  const ULONG signalMask = playerSignalBitMask | SIGBREAKF_CTRL_C;
  struct Player *player = g_player;

  /*LONG res =*/ SetPlayerAttrs(player, PLAYER_AlarmTime,
                            player->pl_MetricTime + s_CamdTicksPerMidiFrame,
                            PLAYER_Ready, TRUE, TAG_END);

  // Let main thread know we're alive
  Signal(g_MainTask, SIGBREAKF_CTRL_E);

  initSuccess = TRUE;

  while (TRUE) {
    ULONG signals = Wait(signalMask);
    if ((signals & playerSignalBitMask)) {

      LONG nextAlarm = player->pl_MetricTime + s_CamdTicksPerMidiFrame;
      // Service the timer function, this is actually parsing the midi song and
      // causing calls into oiur various FUNC_ midi functions.
      g_service();

      /*LONG res =*/ SetPlayerAttrs(player, PLAYER_AlarmTime, nextAlarm,
                                PLAYER_Ready, TRUE, TAG_END);
    } else if (signals & SIGBREAKF_CTRL_C) {
      break;
    }
  };

failure:

  if (!initSuccess) {
    Signal(g_MainTask, SIGBREAKF_CTRL_C);
  }

  if (g_player) {
    SetConductorState(g_player, CONDSTATE_STOPPED, 0);
    DeletePlayer(g_player);
    g_player = NULL;
  }
  if (g_playerSignalBit != -1) {
    FreeSignal(g_playerSignalBit);
  }

  Forbid();
  Signal(g_MainTask, SIGBREAKF_CTRL_E);
}
