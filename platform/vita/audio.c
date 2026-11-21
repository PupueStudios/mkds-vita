#include "port.h"
#include "guest_if.h"
#include "vita.h"

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#define AUDIO_RATE 48000
#define AUDIO_GRAIN 512

static int s_port = -1;
static volatile bool s_paused;

static int audio_thread(SceSize args, void *argp) {
    (void) args, (void) argp;
    static int16_t buffers[2][AUDIO_GRAIN * 2];
    int which = 0;
    for (;;) {
        int16_t *buf = buffers[which];
        which ^= 1;
        if (s_paused) {
            memset(buf, 0, sizeof(buffers[0]));
        } else {
            guest_snd_render(buf, AUDIO_GRAIN, AUDIO_RATE);
        }
        sceAudioOutOutput(s_port, buf);
    }
    return 0;
}

bool vita_audio_start(void) {
    if (s_port >= 0) return true;
    s_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_GRAIN, AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (s_port < 0) {
        port_log("Sound: cannot open the audio output (%08x)", (unsigned) s_port);
        return false;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(s_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    SceUID t = sceKernelCreateThread("mkds_audio", audio_thread, 0x40, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
        port_log("Sound: cannot start");
        return false;
    }
    port_log("Sound ready");
    return true;
}

void vita_audio_pause(bool paused) { s_paused = paused; }
