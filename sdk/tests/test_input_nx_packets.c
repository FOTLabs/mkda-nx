/* Exercise the actual Switch state branch with a native-pad sample stub. */
#define __SWITCH__ 1
#include "../src/input/xinput_device.c"
#include <assert.h>
static XBOX_GAMEPAD sample[4];
static int connected[4] = {1,1,0,0};
int xbox_nx_pad_read(unsigned port, uint16_t *digital, uint8_t analog[8], int16_t thumbs[4]) {
    if (!connected[port]) return 0;
    *digital = sample[port].wButtons;
    memcpy(analog,sample[port].bAnalogButtons,8);
    thumbs[0]=sample[port].sThumbLX;thumbs[1]=sample[port].sThumbLY;
    thumbs[2]=sample[port].sThumbRX;thumbs[3]=sample[port].sThumbRY;
    return 1;
}
int main(void) {
    XBOX_INPUT_STATE a,b; DWORD packet;
    assert(xbox_InputGetState(0,&a)==ERROR_SUCCESS);
    sample[0].wButtons=XBOX_GAMEPAD_START;
    assert(xbox_InputGetState(0,&a)==ERROR_SUCCESS);packet=a.dwPacketNumber;
    for(int i=0;i<1000;i++) {
        assert(xbox_InputGetState(0,&b)==ERROR_SUCCESS);
        assert(b.dwPacketNumber==packet && b.Gamepad.wButtons==XBOX_GAMEPAD_START);
    }
    sample[0].wButtons=0;xbox_InputGetState(0,&b);assert(b.dwPacketNumber==packet+1);
    sample[0].wButtons=XBOX_GAMEPAD_START;xbox_InputGetState(0,&b);assert(b.dwPacketNumber==packet+2);
    sample[0].sThumbLX=123;xbox_InputGetState(0,&b);assert(b.dwPacketNumber==packet+3);
    sample[0].bAnalogButtons[0]=255;xbox_InputGetState(0,&b);assert(b.dwPacketNumber==packet+4);
    xbox_InputGetState(1,&a);xbox_InputGetState(1,&b);assert(a.dwPacketNumber==b.dwPacketNumber);
    connected[0]=0;assert(xbox_InputGetState(0,&b)==ERROR_DEVICE_NOT_CONNECTED);
    connected[0]=1;xbox_InputGetState(0,&b);assert(b.dwPacketNumber==packet+5);
    puts("PASS: held Start preserves packet; release/repress, axes, analog buttons, port isolation and reconnect update packets");
}
