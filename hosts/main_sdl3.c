/* Space Invaders - SDL3 frontend for the statically recompiled 8080 code.
 * Builds natively on Linux x64 and Windows x64 (MSVC, clang-cl or MinGW).
 *
 *   invaders [ROM] [options]
 *     --sounds DIR       directory with the WAV samples (default: <exe dir>/sounds)
 *     --scale N          initial window scale (default 3)
 *     --fullscreen       start fullscreen
 *     --ships 3|4|5|6    DIP switch: ships per game (default 3)
 *     --bonus 1000|1500  DIP switch: score for the extra ship (default 1500)
 *     --no-coin-info     DIP switch: hide coin info on the demo screen
 *     --frames N         quit after N frames (testing)
 *     --screenshot F     save the last frame as a BMP on exit (testing)
 *
 * Keys: C coin, 1/2 start, Left/Right/Space player 1, A/D/S player 2,
 *       P pause, T tilt, F3 reset, F11 fullscreen, Esc quit. */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu8080.h"
#include "machine.h"

#define W 224
#define H 256
#define AUDIO_RATE 44100

/* ------------------------------------------------------------------ sound */
/* Port 3 / port 5 bits -> samples (bit layout from the MAME driver notes). */
enum { SND_UFO, SND_SHOT, SND_PLAYER_DIE, SND_INVADER_DIE,
       SND_FLEET1, SND_FLEET2, SND_FLEET3, SND_FLEET4, SND_UFO_HIT, SND_COUNT };
static const char *snd_files[SND_COUNT] = {
    "ufo_lowpitch.wav", "shoot.wav", "explosion.wav", "invaderkilled.wav",
    "fastinvader1.wav", "fastinvader2.wav", "fastinvader3.wav", "fastinvader4.wav", "explosion.wav" };

typedef struct { Sint16 *data; int len; } sample_t;
typedef struct { int active, pos, loop; } voice_t;
static sample_t samples[SND_COUNT];
static voice_t voices[SND_COUNT];          /* guarded by the audio stream lock */
static SDL_AudioStream *audio;

/* Called by SDL (with the stream locked) whenever the device wants more data. */
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *stream, int additional, int total)
{
    static Sint16 buf[1024];
    (void)ud; (void)total;
    while (additional > 0) {
        int n = additional / 2;
        if (n > (int)SDL_arraysize(buf)) n = (int)SDL_arraysize(buf);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            int acc = 0;
            for (int s = 0; s < SND_COUNT; s++) {
                voice_t *v = &voices[s];
                if (!v->active || !samples[s].data) continue;
                acc += samples[s].data[v->pos++];
                if (v->pos >= samples[s].len) {
                    v->pos = 0;
                    if (!v->loop) v->active = 0;
                }
            }
            buf[i] = (Sint16)(acc > 32767 ? 32767 : acc < -32768 ? -32768 : acc);
        }
        SDL_PutAudioStreamData(stream, buf, n * 2);
        additional -= n * 2;
    }
}

static bool load_sample(const char *dir, const char *name, sample_t *dst, const SDL_AudioSpec *want)
{
    char path[2048];
    SDL_AudioSpec spec;
    Uint8 *wav = NULL, *pcm = NULL;
    Uint32 wav_len = 0;
    int pcm_len = 0;
    SDL_snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!SDL_LoadWAV(path, &spec, &wav, &wav_len)) return false;
    bool ok = SDL_ConvertAudioSamples(&spec, wav, (int)wav_len, want, &pcm, &pcm_len);
    SDL_free(wav);
    if (!ok) return false;
    dst->data = (Sint16 *)pcm;
    dst->len = pcm_len / 2;
    for (int i = 0; i < dst->len; i++) dst->data[i] = (Sint16)(dst->data[i] / 2); /* mix headroom */
    return true;
}

static void sound_init(const char *dir)
{
    const SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, AUDIO_RATE };
    int loaded = 0;
    for (int s = 0; s < SND_COUNT; s++) loaded += load_sample(dir, snd_files[s], &samples[s], &spec);
    if (!loaded) {
        SDL_Log("audio: no WAV files in '%s' - running silently", dir);
        return;
    }
    audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_cb, NULL);
    if (!audio) { SDL_Log("audio: %s - running silently", SDL_GetError()); return; }
    SDL_ResumeAudioStreamDevice(audio);
}

static void trigger(int s, int loop)
{
    voices[s].active = 1; voices[s].pos = 0; voices[s].loop = loop;
}

static void sound_update(void)
{
    static uint8_t prev3, prev5;
    uint8_t rise3 = (uint8_t)(si_out3 & ~prev3), rise5 = (uint8_t)(si_out5 & ~prev5);
    prev3 = si_out3; prev5 = si_out5;
    if (!audio) return;
    SDL_LockAudioStream(audio);
    if (si_out3 & 0x01) { if (!voices[SND_UFO].active) trigger(SND_UFO, 1); }  /* UFO drone repeats */
    else voices[SND_UFO].active = 0;
    for (int b = 1; b <= 3; b++) if (rise3 & (1 << b)) trigger(b, 0);
    for (int b = 0; b <= 4; b++) if (rise5 & (1 << b)) trigger(SND_FLEET1 + b, 0);
    SDL_UnlockAudioStream(audio);
}

/* ------------------------------------------------------------------ video */
/* The monitor is rotated 90 degrees counter-clockwise; video RAM 2400-3FFF holds
 * 224 columns of 32 bytes, bit 0 of the first byte being the bottom-left pixel.
 * Coloured cellophane on the cabinet glass tints the top band red and the bottom
 * band (player area, plus the lives/credit row left of centre) green. */
static void render(Uint32 *px)
{
    for (int y = 0; y < H; y++) {
        Uint32 tint = 0xFFFFFFFF;
        if (y >= 32 && y < 64) tint = 0xFFFF3030;
        for (int x = 0; x < W; x++) {
            int vy = 255 - y;
            int bit = (si_mem[0x2400 + x * 32 + (vy >> 3)] >> (vy & 7)) & 1;
            Uint32 c = tint;
            if ((y >= 184 && y < 240) || (y >= 240 && x >= 16 && x < 134)) c = 0xFF30FF30;
            px[y * W + x] = bit ? c : 0xFF000000;
        }
    }
}

/* ------------------------------------------------------------------ input */
static uint8_t dip_in2;

static void read_input(int tilt)
{
    const bool *k = SDL_GetKeyboardState(NULL);
    uint8_t in1 = 0x08, in2 = dip_in2;
    if (k[SDL_SCANCODE_C]) in1 |= 0x01;
    if (k[SDL_SCANCODE_2]) in1 |= 0x02;
    if (k[SDL_SCANCODE_1]) in1 |= 0x04;
    if (k[SDL_SCANCODE_SPACE]) in1 |= 0x10;
    if (k[SDL_SCANCODE_LEFT]) in1 |= 0x20;
    if (k[SDL_SCANCODE_RIGHT]) in1 |= 0x40;
    if (tilt) in2 |= 0x04;
    if (k[SDL_SCANCODE_S]) in2 |= 0x10;
    if (k[SDL_SCANCODE_A]) in2 |= 0x20;
    if (k[SDL_SCANCODE_D]) in2 |= 0x40;
    si_in1 = in1; si_in2 = in2;
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s [ROM] [--sounds DIR] [--scale N] [--fullscreen] [--ships 3-6]\n"
                    "          [--bonus 1000|1500] [--no-coin-info] [--frames N] [--screenshot F.bmp]\n", argv0);
}

int main(int argc, char *argv[])
{
    const char *rom = NULL, *snd_dir = NULL, *shot = NULL;
    int scale = 3, fullscreen = 0, ships = 3, bonus = 1500, coin_info = 1;
    long max_frames = -1;
    char rom_buf[2048], snd_buf[2048];

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--sounds") && v) { snd_dir = v; i++; }
        else if (!strcmp(a, "--scale") && v) { scale = atoi(v); i++; }
        else if (!strcmp(a, "--ships") && v) { ships = atoi(v); i++; }
        else if (!strcmp(a, "--bonus") && v) { bonus = atoi(v); i++; }
        else if (!strcmp(a, "--frames") && v) { max_frames = atol(v); i++; }
        else if (!strcmp(a, "--screenshot") && v) { shot = v; i++; }
        else if (!strcmp(a, "--fullscreen")) fullscreen = 1;
        else if (!strcmp(a, "--no-coin-info")) coin_info = 0;
        else if (a[0] != '-') rom = a;
        else { usage(argv[0]); return 2; }
    }
    if (scale < 1) scale = 1;
    if (ships < 3 || ships > 6) ships = 3;
    /* Port 2 DIP switches: bits 0-1 ships (00=3 .. 11=6), bit 3 bonus at 1000,
     * bit 7 set hides the coin info on the demo screen. */
    dip_in2 = (uint8_t)((ships - 3) | (bonus == 1000 ? 0x08 : 0) | (coin_info ? 0 : 0x80));

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const char *base = SDL_GetBasePath();
    if (!base) base = "./";
    SDL_snprintf(rom_buf, sizeof rom_buf, "%sspace-invaders.rom", base);
    if (!(rom && si_load_rom(rom)) && !si_load_rom(rom_buf) && !si_load_rom("space-invaders.rom")) {
        const char *msg = "Could not load the Space Invaders ROM (8 KB).\n"
                          "Put space-invaders.rom next to the executable or pass its path.";
        fprintf(stderr, "%s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Invaders", msg, NULL);
        SDL_Quit();
        return 1;
    }
    if (!snd_dir) { SDL_snprintf(snd_buf, sizeof snd_buf, "%ssounds", base); snd_dir = snd_buf; }
    sound_init(snd_dir);

    SDL_Window *win;
    SDL_Renderer *ren;
    if (!SDL_CreateWindowAndRenderer("Space Invaders - static recompilation", W * scale, H * scale,
                                     SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0), &win, &ren)) {
        fprintf(stderr, "SDL_CreateWindowAndRenderer: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderLogicalPresentation(ren, W, H, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);
    static Uint32 pixels[W * H];

    si_reset();
    const Uint64 frame_ns = SDL_NS_PER_SECOND / 60;
    Uint64 next = SDL_GetTicksNS();
    int running = 1, paused = 0, tilt = 0;
    long frame = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = 0;
            else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                switch (ev.key.key) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_P: paused = !paused; break;
                case SDLK_T: tilt = 4; break;
                case SDLK_F3: si_reset(); break;
                case SDLK_F11:
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(win, fullscreen != 0);
                    break;
                default: paused = 0; break;
                }
            }
        }
        if (!paused) {
            read_input(tilt > 0);
            if (tilt > 0) tilt--;
            si_run_frame();
            sound_update();
            frame++;
        }
        render(pixels);
        SDL_UpdateTexture(tex, NULL, pixels, W * (int)sizeof(Uint32));
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);
        if (shot && max_frames >= 0 && frame >= max_frames) {
            SDL_Surface *s = SDL_RenderReadPixels(ren, NULL);
            if (s) { SDL_SaveBMP(s, shot); SDL_DestroySurface(s); }
        }
        SDL_RenderPresent(ren);
        if (max_frames >= 0 && frame >= max_frames) break;

        /* Pace to 60 Hz regardless of the monitor's refresh rate. */
        next += frame_ns;
        Uint64 now = SDL_GetTicksNS();
        if (now < next) SDL_DelayPrecise(next - now);
        else if (now - next > SDL_NS_PER_SECOND / 4) next = now;   /* fell far behind: resync */
    }

    if (audio) SDL_DestroyAudioStream(audio);
    for (int s = 0; s < SND_COUNT; s++) SDL_free(samples[s].data);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
