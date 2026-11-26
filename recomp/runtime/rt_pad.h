/* shared gamepad state (rt_input.c fills it, rt_objc.c exposes it through the GameController emulation) */
#ifndef RT_PAD_H
#define RT_PAD_H
enum { PB_A = 1, PB_B = 2, PB_X = 4, PB_Y = 8, PB_LB = 16, PB_RB = 32, PB_BACK = 64, PB_START = 128, PB_LS = 256, PB_RS = 512,
       PB_UP = 1024, PB_DOWN = 2048, PB_LEFT = 4096, PB_RIGHT = 8192, PB_GUIDE = 16384 };
typedef struct { int connected; int api; /* 1 XInput 2 DirectInput */ float lx, ly, rx, ry, lt, rt; unsigned buttons; char name[64]; } PadState;
int rt_pad_poll(PadState *p);
#endif
