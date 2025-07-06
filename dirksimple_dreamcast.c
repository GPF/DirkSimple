// dirksimple_dreamcast.c - Dreamcast platform backend for DirkSimple

#include "dirksimple_platform.h"
#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/pvr.h>
// #define ZSTD_STATIC_LINKING_ONLY
// #include <zstd/zstd.h>
#include <lz4/lz4.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"
#include <png/png.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

// static uint16_t prev_buttons = 0;

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define DIRKSIMPLE_LUA_NAMESPACE "DirkSimple"
#define DIRKSIMPLE_FAKE_DISC_LAG_TICKS 800

// "included in all copies or substantial portions of the Software"
static const char *GLuaLicense =
"Lua:\n"
"\n"
"Copyright (C) 1994-2008 Lua.org, PUC-Rio.\n"
"\n"
"Permission is hereby granted, free of charge, to any person obtaining a copy\n"
"of this software and associated documentation files (the \"Software\"), to deal\n"
"in the Software without restriction, including without limitation the rights\n"
"to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
"copies of the Software, and to permit persons to whom the Software is\n"
"furnished to do so, subject to the following conditions:\n"
"\n"
"The above copyright notice and this permission notice shall be included in\n"
"all copies or substantial portions of the Software.\n"
"\n"
"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
"IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
"FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE\n"
"AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
"LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
"OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN\n"
"THE SOFTWARE.\n"
"\n";


static char *GGameName = NULL;
static char *GGamePath = NULL;
static char *GDataDir = NULL;
static char *GGameDir = NULL;
static uint64_t GPreviousInputBits = 0;
static int GHalted = 0;
static int GShowingSingleFrame = 0;
static unsigned int GSeekGeneration = 0;
static int GNeedInitialLuaTick = 1;
static uint64_t GClipStartTicks = 0;
static uint64_t GTicks = 0;
static uint64_t GTicksOffset = 0;
static int GDecoderActive = 0;
static int GSeeking = 0;
static int GSeekTargetFrame = -1;
static atomic_int seek_request = -1;
static int GRestartOnYPress = 0;
// static int GAudioChannels = 0;
// static int GAudioFreq = 0;
#define SCREEN_WIDTH 640
#define SCREEN_HEIGHT 480

#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240

#define VIDEO_SCALE_X ((float)SCREEN_WIDTH / (float)VIDEO_WIDTH)
#define VIDEO_SCALE_Y ((float)SCREEN_HEIGHT / (float)VIDEO_HEIGHT)

#define VIDEO_OFFSET_X ((SCREEN_WIDTH - VIDEO_WIDTH * VIDEO_SCALE_X) / 2)
#define VIDEO_OFFSET_Y ((SCREEN_HEIGHT - VIDEO_HEIGHT * VIDEO_SCALE_Y) / 2)

typedef enum RenderPrimitive {
    RENDPRIM_CLEAR,
    RENDPRIM_SPRITE,
    RENDPRIM_SOUND,
} RenderPrimitive;

typedef struct RenderCommand {
    RenderPrimitive prim;
    union {
        struct { uint8_t r, g, b; } clear;
        struct {
            char name[32];
            int32_t sx, sy, sw, sh, dx, dy, dw, dh;
            uint8_t r, g, b;
        } sprite;
        struct { char name[32]; } sound;
    } data;
} RenderCommand;

static DirkSimple_Sprite *GSprites = NULL;
static DirkSimple_Wave *GWaves = NULL;
static RenderCommand *GRenderCommands = NULL;
static int GNumRenderCommands = 0;
static int GNumAllocatedRenderCommands = 0;
void send_rendering_primitives(void);
#define DCMV_MAGIC "DCMV"
#define NUM_BUFFERS 16
#define RING_CAPACITY (NUM_BUFFERS + 1)
#define INVALID_FRAME -1

enum BufState {
    BUF_EMPTY = 0,
    BUF_LOADING = 1,
    BUF_READY = 2
};

typedef struct {
    int frame;
    int generation;
} PreloadJob;

static PreloadJob preload_ring[RING_CAPACITY];

// static ZSTD_DCtx *dctx;
static FILE *fp = NULL;
static FILE *audio_fp = NULL;
static uint8_t *compressed_buffer = NULL;
static uint8_t *frame_buffer[NUM_BUFFERS];
static uint32_t *frame_offsets = NULL;
static int frame_type,video_width, video_height, sample_rate, num_frames, video_frame_size, audio_channels, max_compressed_size, audio_offset;
static float fps, frame_duration;
static pvr_ptr_t pvr_txr;
static pvr_poly_hdr_t hdr;
static pvr_ptr_t sprite_txr;
// static pvr_poly_hdr_t sprite_hdr;
static pvr_vertex_t vert[4];
static pvr_vertex_t sprite_vert[4];
static snd_stream_hnd_t stream;

static atomic_int frame_index = 0;
static _Atomic double audio_start_time_ms = 0.0;
static _Atomic int audio_muted = 0;
static _Atomic size_t audio_bytes_fed = 0;
static double frame_timer_anchor = 0.0;
static _Atomic int buf_state[NUM_BUFFERS];
static atomic_int preload_ring_head = 0, preload_ring_tail = 0;
// static atomic_int preload_ring[RING_CAPACITY];
static volatile int audio_started = 0;
static lua_State *GLua = NULL;
static double psTimer(void) {
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + 0x021000);
    return jiffies / 4.410f;
}

static void out_of_memory(void)
{
    DirkSimple_panic("Out of memory!");
}


void *DirkSimple_xmalloc(size_t len)
{
    void *retval = DirkSimple_malloc(len);
    if (!retval) {
        out_of_memory();
    }
    return retval;
}

void *DirkSimple_xcalloc(size_t nmemb, size_t len)
{
    void *retval = DirkSimple_calloc(nmemb, len);
    if (!retval) {
        out_of_memory();
    }
    return retval;
}

void *DirkSimple_xrealloc(void *ptr, size_t len)
{
    void *retval = DirkSimple_realloc(ptr, len);
    if (!retval && (len > 0)) {
        out_of_memory();
    }
    return retval;
}

char *DirkSimple_xstrdup(const char *str)
{
    char *retval = DirkSimple_strdup(str);
    if (!retval) {
        out_of_memory();
    }
    return retval;
}


void DirkSimple_panic(const char *str) {
    printf("PANIC: %s\n", str);
    exit(1);
}

void DirkSimple_writelog(const char *str) {
    printf("[DirkSimple] %s\n", str);
}

void *DirkSimple_malloc(size_t len) { return malloc(len); }
void *DirkSimple_calloc(size_t n, size_t len) { return calloc(n, len); }
void *DirkSimple_realloc(void *ptr, size_t len) { return realloc(ptr, len); }
char *DirkSimple_strdup(const char *str) { return strdup(str); }
void DirkSimple_free(void *ptr) { free(ptr); }

static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    if (atomic_load(&audio_muted) == 1) {
        // printf("🔇 muted: %zu bytes\n", req);
        memset((void *)l, 0, req);
        if (audio_channels == 2)
            memset((void *)r, 0, req);
        return req;
    }

    if (!audio_fp) {
        printf("❌ audio_fp is NULL\n");
        return 0;
    }

    if (audio_channels == 2) {
        size_t lbytes = fread((void *)l, 1, req / 2, audio_fp);
        size_t rbytes = fread((void *)r, 1, req / 2, audio_fp);
        atomic_fetch_add(&audio_bytes_fed, lbytes + rbytes);
        return lbytes + rbytes;
    } else {
        size_t bytes = fread((void *)l, 1, req, audio_fp);
        atomic_fetch_add(&audio_bytes_fed, bytes);
        if (bytes < req) {
            printf("⚠️ Audio underflow: requested=%zu, got=%zu\n", req, bytes);
        }
        // printf("requested %zu bytes, provided %zu bytes\n", req, bytes);
        return bytes;
    }
}

void DirkSimple_audioformat(int channels, int rate) {
    audio_channels = channels;
    sample_rate = rate;
    snd_stream_init_ex(channels, 8192);
    stream = snd_stream_alloc(NULL, 8192);  // correct placeholder
    snd_stream_set_callback_direct(stream, audio_cb);  // sets real callback
    snd_stream_start_adpcm(stream, rate, channels == 2 ? 1 : 0);
    printf("[audio] started: %d ch, %d Hz\n", channels, rate);
}

void DirkSimple_videoformat(const char *title, uint32_t w, uint32_t h, double _fps) {
    (void)title;
    video_width = w;
    video_height = h;
    fps = _fps;
    frame_duration = 1000.0 / fps;
    pvr_init_defaults();
    pvr_txr = pvr_mem_malloc(video_width * video_height * 2);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY,
        PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
        video_width, video_height, pvr_txr, PVR_FILTER_BILINEAR);
    pvr_poly_compile(&hdr, &cxt);
    vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xffffffff};
    vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=640, .y=0, .z=1, .u=1, .v=0, .argb=0xffffffff};
    vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX, .x=0, .y=480, .z=1, .u=0, .v=1, .argb=0xffffffff};
    vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x=640, .y=480, .z=1, .u=1, .v=1, .argb=0xffffffff};
    // pvr_poly_cxt_t sprite_cxt;
    // sprite_txr = NULL; 
    // pvr_poly_cxt_txr(&sprite_cxt, PVR_LIST_TR_POLY,
    //     PVR_TXRFMT_ARGB4444, 512, 32, NULL, PVR_FILTER_BILINEAR);
    // sprite_cxt.gen.alpha = PVR_ALPHA_ENABLE;
    // sprite_cxt.gen.culling = PVR_CULLING_NONE;
    // pvr_poly_compile(&sprite_hdr, &sprite_cxt);
    printf("[video] %ldx%ld @ %.2f fps\n", w, h, _fps);
}

static int load_frame(int frame_num, int buf_index) {
    uint32_t offset = frame_offsets[frame_num];
    uint32_t next_offset = frame_offsets[frame_num + 1];
    uint32_t compressed_size = next_offset - offset;
    fseek(fp, offset, SEEK_SET);
    fread(compressed_buffer, 1, compressed_size, fp);
    // size_t decompressed = ZSTD_decompressDCtx(dctx, frame_buffer[buf_index], video_frame_size,
    //                                           compressed_buffer, compressed_size);
    // // printf("[frame %d] %u -> %zu bytes\n", frame_num, compressed_size, decompressed);
    // return ZSTD_isError(decompressed) ? -1 : 0;
    int decompressed = LZ4_decompress_fast(
    (const char *)compressed_buffer,
    (char *)frame_buffer[buf_index],
    video_frame_size);

    if (decompressed < 0) {
        printf("❌ LZ4 decompression failed on frame %d\n", frame_num);
        return -1;
    }
    return 0;

}

static void draw_ui_only_frame(void)
{
    pvr_scene_begin();

    // Skip video poly pass — no PVR texture loaded
    pvr_list_begin(PVR_LIST_TR_POLY);
    send_rendering_primitives();  // Includes text, HUD, sprite drawing
    pvr_list_finish();

    pvr_scene_finish();
}

static void draw_frame(int buf_index) {
    pvr_txr_load(frame_buffer[buf_index], pvr_txr, video_frame_size);

    pvr_scene_begin();

    // FMV background pass
    pvr_list_begin(PVR_LIST_OP_POLY);
    {
        pvr_dr_state_t dr;
        pvr_dr_init(&dr);
        sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &hdr, 1);
        sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), vert, 4);
        pvr_dr_finish();
    }
    pvr_list_finish();

    // Hint overlays and UI pass
    pvr_list_begin(PVR_LIST_TR_POLY);
    send_rendering_primitives();
    pvr_list_finish();

    pvr_scene_finish();
}
static void *worker_thread(void *arg) {
    (void)arg;
    while (1) {
        // ✅ Only poll audio if unmuted
        snd_stream_poll(stream);

        int tail = atomic_load(&preload_ring_tail);
        if (tail != atomic_load(&preload_ring_head)) {
            PreloadJob job = preload_ring[tail];

            if (job.generation != GSeekGeneration) {
                // 🧹 Stale job from old seek, skip it
                atomic_store(&preload_ring_tail, (tail + 1) % RING_CAPACITY);
                continue;
            }

            int frame = job.frame;
            int buf = frame % NUM_BUFFERS;

            if (atomic_load(&buf_state[buf]) == BUF_EMPTY) {
                atomic_store(&buf_state[buf], BUF_LOADING);
                if (load_frame(frame, buf) == 0)
                    atomic_store(&buf_state[buf], BUF_READY);
                else
                    atomic_store(&buf_state[buf], BUF_EMPTY);
            }

            atomic_store(&preload_ring_tail, (tail + 1) % RING_CAPACITY);
        }

        thd_sleep(1);
    }
    return NULL;
}

void DirkSimple_discvideo(const uint8_t *ignored) {
    int cur = atomic_load(&frame_index);
    int buf = cur % NUM_BUFFERS;
    if (atomic_load(&buf_state[buf]) == BUF_READY) {
        draw_frame(buf);
        atomic_store(&buf_state[buf], BUF_EMPTY);
        atomic_fetch_add(&frame_index, 1);
    }
}

static inline int snprintfcat(char **ptr, size_t *len, const char *fmt, ...)
{
    int bw = 0;
    va_list ap;
    va_start(ap, fmt);
    bw = vsnprintf(*ptr, *len, fmt, ap);
    va_end(ap);
    *ptr += bw;
    *len -= bw;
    return bw;
}


// Read data from a DirkSimple_Io when loading Lua code.
static const char *DirkSimple_lua_reader(lua_State *L, void *data, size_t *size)
{
    static char buffer[1024];
    DirkSimple_Io *in = (DirkSimple_Io *) data;
    const long br = in->read(in, buffer, sizeof (buffer));
    if (br <= 0) {  // eof or error? (lua doesn't care which?!)
        *size = 0;
        return NULL;
    }

    *size = (size_t) br;
    return buffer;
}

void DirkSimple_log(const char *fmt, ...)
{
    char *str = NULL;
    va_list ap;
    size_t len;

    va_start(ap, fmt);
    len = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    str = DirkSimple_xmalloc(len + 1);
    va_start(ap, fmt);
    vsnprintf(str, len + 1, fmt, ap);
    va_end(ap);

    DirkSimple_writelog(str);
    DirkSimple_free(str);
}
static int luahook_DirkSimple_stackwalk(lua_State *L)
{
    const char *errstr = lua_tostring(L, 1);
    lua_Debug ldbg;
    int i;

    if (errstr != NULL) {
        DirkSimple_log(errstr);
    }

    DirkSimple_log("Lua stack backtrace:");

    // start at 1 to skip this function.
    for (i = 1; lua_getstack(L, i, &ldbg); i++) {
        char scratchbuf[256];
        char *ptr = scratchbuf;
        size_t len = sizeof (scratchbuf);
        int bw = snprintfcat(&ptr, &len, "#%d", i-1);
        const int maxspacing = 4;
        int spacing = maxspacing - bw;
        while (spacing-- > 0) {
            snprintfcat(&ptr, &len, " ");
        }

        if (!lua_getinfo(L, "nSl", &ldbg)) {
            snprintfcat(&ptr, &len, "???");
            DirkSimple_log(scratchbuf);
            continue;
        }

        if (ldbg.namewhat[0]) {
            snprintfcat(&ptr, &len, "%s ", ldbg.namewhat);
        }

        if ((ldbg.name) && (ldbg.name[0])) {
            snprintfcat(&ptr, &len, "function %s ()", ldbg.name);
        } else {
            if (strcmp(ldbg.what, "main") == 0) {
                snprintfcat(&ptr, &len, "mainline of chunk");
            } else if (strcmp(ldbg.what, "tail") == 0) {
                snprintfcat(&ptr, &len, "tail call");
            } else {
                snprintfcat(&ptr, &len, "unidentifiable function");
            }
        }

        DirkSimple_log(scratchbuf);
        ptr = scratchbuf;
        len = sizeof (scratchbuf);

        for (spacing = 0; spacing < maxspacing; spacing++) {
            snprintfcat(&ptr, &len, " ");
        }

        if (strcmp(ldbg.what, "C") == 0) {
            snprintfcat(&ptr, &len, "in native code");
        } else if (strcmp(ldbg.what, "tail") == 0) {
            snprintfcat(&ptr, &len, "in Lua code");
        } else if ( (strcmp(ldbg.source, "=?") == 0) && (ldbg.currentline == 0) ) {
            snprintfcat(&ptr, &len, "in Lua code (debug info stripped)");
        } else {
            snprintfcat(&ptr, &len, "in Lua code at %s", ldbg.short_src);
            if (ldbg.currentline != -1)
                snprintfcat(&ptr, &len, ":%d", ldbg.currentline);
        }
        DirkSimple_log(scratchbuf);
    }

    lua_pushstring(L, errstr ? errstr : "");
    return 1;
}

static RenderCommand *new_render_command(RenderPrimitive prim) {
    if (GNumRenderCommands >= GNumAllocatedRenderCommands) {
        GNumAllocatedRenderCommands = (GNumAllocatedRenderCommands == 0) ? 32 : GNumAllocatedRenderCommands * 2;
        GRenderCommands = realloc(GRenderCommands, sizeof(RenderCommand) * GNumAllocatedRenderCommands);
    }

    RenderCommand *cmd = &GRenderCommands[GNumRenderCommands++];
    cmd->prim = prim;
    return cmd;
}

static int luahook_DirkSimple_play_sound(lua_State *L)
{
    RenderCommand *cmd = new_render_command(RENDPRIM_SOUND);
    snprintf(cmd->data.sound.name, sizeof (cmd->data.sound.name), "%s", lua_tostring(L, 1));
    return 0;
}

void DirkSimple_registercvar(const char *gamename, const char *name, const char *desc, const char *valid_values)
{
    // we don't care about this atm.
}

typedef struct {
    FILE *fp;
} FileIoUserdata;

static long file_read(DirkSimple_Io *io, void *buf, long len) {
    FileIoUserdata *ud = (FileIoUserdata *) io->userdata;
    return (long) fread(buf, 1, len, ud->fp);
}

static long file_streamlen(DirkSimple_Io *io) {
    FileIoUserdata *ud = (FileIoUserdata *) io->userdata;
    long cur = ftell(ud->fp);
    fseek(ud->fp, 0, SEEK_END);
    long size = ftell(ud->fp);
    fseek(ud->fp, cur, SEEK_SET);
    return size;
}

static int file_seek(DirkSimple_Io *io, long offset) {
    FileIoUserdata *ud = (FileIoUserdata *) io->userdata;
    return fseek(ud->fp, offset, SEEK_SET);
}

static void file_close(DirkSimple_Io *io) {
    FileIoUserdata *ud = (FileIoUserdata *) io->userdata;
    fclose(ud->fp);
    DirkSimple_free(ud);
    DirkSimple_free(io);
}

DirkSimple_Io *DirkSimple_openfile_read(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    DirkSimple_Io *io = DirkSimple_xmalloc(sizeof(DirkSimple_Io));
    FileIoUserdata *ud = DirkSimple_xmalloc(sizeof(FileIoUserdata));
    ud->fp = fp;

    io->read = file_read;
    io->streamlen = file_streamlen;
    io->seek = file_seek;
    io->close = file_close;
    io->userdata = ud;

    return io;
}


static void load_lua_gamecode(lua_State *L, const char *gamedir)
{
    int rc;
    DirkSimple_Io *io;
    const size_t slen = strlen(gamedir) + 32;
    char *fname = (char *) DirkSimple_xmalloc(slen);
    snprintf(fname, slen, "@%sgame.luac", gamedir);
    io = DirkSimple_openfile_read(fname + 1);
    if (!io) {
        snprintf(fname, slen, "@%sgame.lua", gamedir);
        io = DirkSimple_openfile_read(fname + 1);
        if (!io) {
            char *err = (char *) DirkSimple_xmalloc(slen * 2);
            snprintf(err, slen * 2, "Failed to open Lua code at '%s'", fname + 1);
            DirkSimple_panic(err);
        }
    }

    lua_pushcfunction(L, luahook_DirkSimple_stackwalk);
    rc = lua_load(L, DirkSimple_lua_reader, io, fname, NULL);
    io->close(io);

    DirkSimple_free(fname);

    if (rc != 0) {
        lua_error(L);
    } else {
        // Call new chunk on top of the stack (lua_pcall will pop it off).
        if (lua_pcall(L, 0, 0, -2) != 0) {  // retvals are dumped.
            lua_error(L);   // error on stack has debug info.
        }
        // if this didn't panic, we succeeded.
    }
    lua_pop(L, 1);   // dump stackwalker.
}

static void do_cvar_registration(const char *name, const char *desc, const char *values)
{
    if (name && desc && values) {
        DirkSimple_log("Registering cvar '%s' (%s), valid values are '%s'", name, desc, values);
        DirkSimple_registercvar(GGameName, name, desc, values);
    }
}

static void register_cvars(lua_State *L)
{
    if (L) {
        lua_getglobal(L, DIRKSIMPLE_LUA_NAMESPACE);
        if (lua_istable(L, -1)) {  // namespace is sane?
            lua_getfield(L, -1, "cvars");
            if (lua_istable(L, -1)) {  // if not a table, maybe unsupported by this game
                lua_pushnil(L);  // first key for iteration...
                while (lua_next(L, -2)) { // replaces key, pushes value.
                    if (lua_istable(L, -1)) {
                        const char *cvar_name = NULL;
                        const char *cvar_desc = NULL;
                        const char *cvar_values = NULL;
                        lua_getfield(L, -1, "name");
                        cvar_name = lua_tostring(L, -1);
                        lua_getfield(L, -2, "desc");
                        cvar_desc = lua_tostring(L, -1);
                        lua_getfield(L, -3, "values");
                        cvar_values = lua_tostring(L, -1);
                        do_cvar_registration(cvar_name, cvar_desc, cvar_values);
                        lua_pop(L, 3);  // dump name, desc, values
                    }
                    lua_pop(L, 1);  // remove table, keep key for next iteration.
                }
            }
            lua_pop(L, 1);  // pop the cvars table
        }
        lua_pop(L, 1);  // pop the namespace
    }

    // !!! FIXME: register engine-level cvars here.
}

// setter lua function is on top of the Lua stack. This function will pop it.
static void set_lua_cvar(lua_State *L, const char *name, const char *valid_values, const char *newvalue)
{
    const size_t slenvalid = strlen(valid_values);
    const size_t slennew = strlen(newvalue);
    const char *ptr;

    if (slenvalid < slennew) {
        lua_pop(L, 1);  // pop the setter
        return;  // newvalue can't be listed in valid_values, it's bigger than the valid list string.
    }

    ptr = strstr(valid_values, newvalue);
    if (!ptr) {
        lua_pop(L, 1);  // pop the setter
        return;  // not listed in valid_values
    }

    if ( (slenvalid == slennew) ||  // matches entire string
         ((ptr == valid_values) && (valid_values[slennew] == '|')) ||   // matches start of string
         ((ptr > valid_values) && (ptr[-1] == '|') && ((ptr[slennew] == '|') || (ptr[slennew] == '\0'))) ) {  // matches middle or end of string
        // it's valid, pass it to Lua.
        lua_pushstring(L, name);
        lua_pushstring(L, newvalue);
        lua_call(L, 2, 0);
    }
}

void DirkSimple_setcvar(const char *name, const char *newvalue)
{
    lua_State *L = GLua;
    if (L) {
        lua_getglobal(L, DIRKSIMPLE_LUA_NAMESPACE);
        if (lua_istable(L, -1)) {  // namespace is sane?
            lua_getfield(L, -1, "cvars");
            if (lua_istable(L, -1)) {  // if not a table, maybe unsupported by this game
                lua_pushnil(L);  // first key for iteration...
                while (lua_next(L, -2)) { // replaces key, pushes value.
                    int endloop = 0;
                    if (lua_istable(L, -1)) {
                        const char *cvarname;
                        lua_getfield(L, -1, "name");
                        cvarname = lua_tostring(L, -1);
                        if (cvarname && (strcmp(cvarname, name) == 0)) {
                            const char *values;
                            lua_getfield(L, -2, "values");
                            values = lua_tostring(L, -1);
                            if (values) {
                                lua_getfield(L, -3, "setter");
                                if (lua_isfunction(L, -1)) {
                                    set_lua_cvar(L, name, values, newvalue);
                                } else {
                                    lua_pop(L, 1);  // pop the setter
                                }
                                endloop = 1;  // we're done.
                            }
                            lua_pop(L, 1);  // pop the values
                        }
                        lua_pop(L, 1);  // pop the name
                    }
                    lua_pop(L, 1);  // pop the table, keep key for next iteration.

                    if (endloop) {
                        lua_pop(L, 1);  // dump key, too.
                        break;
                    }
                }
            }
            lua_pop(L, 1);  // pop the cvars table
        }
        lua_pop(L, 1);  // pop the namespace
    }
}

// Sets t[sym]=f, where t is on the top of the Lua stack.
static void set_cfunc(lua_State *L, lua_CFunction f, const char *sym)
{
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, sym);
}

// Sets t[sym]=f, where t is on the top of the Lua stack.
static void set_integer(lua_State *L, const int x, const char *sym)
{
    lua_pushinteger(L, x);
    lua_setfield(L, -2, sym);
}

// Sets t[sym]=f, where t is on the top of the Lua stack.
static void set_string(lua_State *L, const char *str, const char *sym)
{
    lua_pushstring(L, str);
    lua_setfield(L, -2, sym);
}

// uint8_t *DirkSimple_loadbmp(const char *fname, int *_w, int *_h)
// {
//     // long flen = 0;
//     // uint8_t *fbuf = DirkSimple_loadmedia(fname, &flen);
//     // uint8_t *pixels = fbuf ? loadbmp_from_memory(fname, fbuf, flen, _w, _h) : NULL;
//     // DirkSimple_free(fbuf);
//     // return (uint8_t *) pixels;
// }

uint8_t *DirkSimple_loadpng(const char *fname, int *_w, int *_h)
{
    uint32 w = 0, h = 0;
    pvr_ptr_t tex = NULL;
    printf("[png] Loading width=%p, height=%p from '%s'\n",
           _w, _h, fname);
    if (png_load_texture(fname, &tex, PNG_NO_ALPHA, &w, &h) < 0) {
        DirkSimple_panic("Failed to load PNG with png_load_texture()");
    }

    if (_w) *_w = w;
    if (_h) *_h = h;

    printf("[png] Loaded '%s' as texture %p (%ux%u)\n",
           fname, tex, w, h);

    // sprite_txr = tex;
    return (uint8_t *)tex;
}


static DirkSimple_Sprite *get_cached_sprite(const char *name)
{
    // Lowercase the name
    char *loweredname = DirkSimple_xstrdup(name);
    for (int i = 0; loweredname[i]; i++) {
        if (loweredname[i] >= 'A' && loweredname[i] <= 'Z')
            loweredname[i] = loweredname[i] - ('A' - 'a');
    }

    // ✅ Search for existing sprite in cache
    for (DirkSimple_Sprite *sprite = GSprites; sprite != NULL; sprite = sprite->next) {
        if (strcmp(sprite->name, loweredname) == 0) {
            DirkSimple_free(loweredname);
            return sprite;
        }
    }

    // ❌ Not cached — load it
    const size_t slen = strlen(GGameDir) + strlen(loweredname) + 8;
    char *sprite_png = DirkSimple_xmalloc(slen);
    snprintf(sprite_png, slen, "%s%s.png", GGameDir, loweredname);

    int w, h;
    pvr_ptr_t tex = (pvr_ptr_t)DirkSimple_loadpng(sprite_png, &w, &h);
    DirkSimple_free(sprite_png);

    if (!tex) {
        DirkSimple_panic("Failed to load sprite PNG");
    }

    // Compile PVR sprite header
    pvr_poly_cxt_t sprite_cxt;
    pvr_poly_cxt_txr(&sprite_cxt, PVR_LIST_TR_POLY,
        PVR_TXRFMT_ARGB4444, w, h, tex, PVR_FILTER_BILINEAR);
    sprite_cxt.gen.alpha = PVR_ALPHA_ENABLE;
    sprite_cxt.gen.culling = PVR_CULLING_NONE;


    // 🔧 Allocate and add to linked list
    DirkSimple_Sprite *sprite = DirkSimple_xmalloc(sizeof (DirkSimple_Sprite));

    sprite->name = loweredname;
    sprite->width = w;
    sprite->height = h;
    sprite->rgba = (uint8_t *) tex;
    sprite->platform_handle = (void *) tex;
    sprite->next = GSprites;
    GSprites = sprite;
    
    pvr_poly_compile(&sprite->sprite_hdr, &sprite_cxt);
    return sprite;
}




float *DirkSimple_loadwav(const char *fname, int *_numframes, const int wantchannels, int wantfreq)
{
    // long flen = 0;
    // int havechannels = 0;
    // int havefreq = 0;
    // int frames = 0;
    // uint8_t *fbuf = DirkSimple_loadmedia(fname, &flen);
    // float *pcm = loadwav_from_memory(fname, fbuf, flen, &frames, &havechannels, &havefreq);
    // DirkSimple_free(fbuf);
    // if (pcm) {
    //     float *cvtpcm = convertwav(fname, pcm, &frames, havechannels, havefreq, wantchannels, wantfreq);
    //     if (cvtpcm != pcm) {
    //         DirkSimple_free(pcm);
    //         pcm = cvtpcm;
    //     }
    // }
    // *_numframes = frames;
    // return pcm;
    return NULL;
}

static DirkSimple_Wave *get_cached_wave(const char *name)
{
    // Lowercase copy of the name
    char *loweredname = DirkSimple_xstrdup(name);
    for (int i = 0; name[i]; i++) {
        char ch = name[i];
        if (ch >= 'A' && ch <= 'Z') {
            loweredname[i] = ch - ('A' - 'a');
        }
    }

    // Check if already cached
    for (DirkSimple_Wave *wave = GWaves; wave != NULL; wave = wave->next) {
        if (strcmp(wave->name, loweredname) == 0) {
            DirkSimple_free(loweredname);
            return wave;
        }
    }

    const size_t slen = strlen(GGameDir) + strlen(loweredname) + 8;
    char *fname = (char *) DirkSimple_xmalloc(slen);
    snprintf(fname, slen, "%s%s.wav", GGameDir, loweredname);
    // printf("[audio] Loading wave '%s' from '%s'", loweredname, fname);
    // Load via KOS SFX manager
    sfxhnd_t sfx = snd_sfx_load(fname);
    DirkSimple_free(fname);

    if (sfx < 0) {
        char errmsg[128];
        snprintf(errmsg, sizeof(errmsg),
                 "Failed to load wave '%s'. Check your installation?", loweredname);
        DirkSimple_panic(errmsg);
    }

    DirkSimple_Wave *wave = (DirkSimple_Wave *) DirkSimple_xmalloc(sizeof(DirkSimple_Wave));
    wave->name = loweredname;
    wave->numframes = 0;
    wave->pcm = NULL;
    wave->duration_ticks = 0;
    wave->ticks_when_available = 0;
    wave->platform_handle = (void *)(intptr_t)sfx;  // safe cast
    wave->next = GWaves;
    GWaves = wave;

    return wave;
}


void send_rendering_primitives(void) {
    for (int i = 0; i < GNumRenderCommands; ++i) {
        RenderCommand *cmd = &GRenderCommands[i];
        switch (cmd->prim) {
            case RENDPRIM_CLEAR:
                DirkSimple_clearscreen(cmd->data.clear.r, cmd->data.clear.g, cmd->data.clear.b);
                break;            
            case RENDPRIM_SPRITE:
            {
                int dx = (int)(cmd->data.sprite.dx * VIDEO_SCALE_X + VIDEO_OFFSET_X);
                int dy = (int)(cmd->data.sprite.dy * VIDEO_SCALE_Y + VIDEO_OFFSET_Y);
                int dw = (int)(cmd->data.sprite.dw * VIDEO_SCALE_X);
                int dh = (int)(cmd->data.sprite.dh * VIDEO_SCALE_Y);
                DirkSimple_drawsprite(
                    get_cached_sprite(cmd->data.sprite.name),
                    cmd->data.sprite.sx, cmd->data.sprite.sy, cmd->data.sprite.sw, cmd->data.sprite.sh,
                    dx, dy, dw, dh,
                    cmd->data.sprite.r,  cmd->data.sprite.g,  cmd->data.sprite.b
                );
                break;
            }
            case RENDPRIM_SOUND:
                DirkSimple_playwave(get_cached_wave(cmd->data.sound.name));
                break;
            default: break;
        }
    }
    GNumRenderCommands = 0;
}


// void DirkSimple_beginframe(void)
// {
//     pvr_scene_begin();
//     pvr_list_begin(PVR_LIST_OP_POLY);  // FMV / video layer
    
// }

// void DirkSimple_endframe(void)
// {
//     pvr_list_finish();                 // End FMV poly list
//     pvr_list_begin(PVR_LIST_TR_POLY); // Begin transparent overlay list
//     send_rendering_primitives();      // Draw hints, overlays, etc
//     pvr_list_finish();
//     pvr_scene_finish();
// }



static int luahook_DirkSimple_clear_screen(lua_State *L)
{
    // DirkSimple_log("Clearing screen with color (%d, %d, %d)",
    //               (int) lua_tonumber(L, 1),
    //               (int) lua_tonumber(L, 2),
    //               (int) lua_tonumber(L, 3));
    RenderCommand *cmd = new_render_command(RENDPRIM_CLEAR);
    cmd->data.clear.r = (uint8_t) lua_tonumber(L, 1);
    cmd->data.clear.g = (uint8_t) lua_tonumber(L, 2);
    cmd->data.clear.b = (uint8_t) lua_tonumber(L, 3);
    return 0;
}

static int luahook_DirkSimple_draw_sprite(lua_State *L)
{
// printf("🖼️ draw_sprite: name=%s, sx=%d sy=%d sw=%d sh=%d dx=%d dy=%d dw=%d dh=%d\n",
//     lua_tostring(L, 1),
//     (int)lua_tonumber(L, 2), (int)lua_tonumber(L, 3),
//     (int)lua_tonumber(L, 4), (int)lua_tonumber(L, 5),
//     (int)lua_tonumber(L, 6), (int)lua_tonumber(L, 7),
//     (int)lua_tonumber(L, 8), (int)lua_tonumber(L, 9));
    RenderCommand *cmd = new_render_command(RENDPRIM_SPRITE);
    snprintf(cmd->data.sprite.name, sizeof (cmd->data.sprite.name), "%s", lua_tostring(L, 1));
    cmd->data.sprite.sx = (int32_t) lua_tonumber(L, 2);
    cmd->data.sprite.sy = (int32_t) lua_tonumber(L, 3);
    cmd->data.sprite.sw = (int32_t) lua_tonumber(L, 4);
    cmd->data.sprite.sh = (int32_t) lua_tonumber(L, 5);
    cmd->data.sprite.dx = (int32_t) lua_tonumber(L, 6);
    cmd->data.sprite.dy = (int32_t) lua_tonumber(L, 7);
    cmd->data.sprite.dw = (int32_t) lua_tonumber(L, 8);
    cmd->data.sprite.dh = (int32_t) lua_tonumber(L, 9);
    cmd->data.sprite.r = (uint8_t) lua_tonumber(L, 10);
    cmd->data.sprite.g = (uint8_t) lua_tonumber(L, 11);
    cmd->data.sprite.b = (uint8_t) lua_tonumber(L, 12);
    return 0;
}


static int luahook_DirkSimple_log(lua_State *L)
{
    const char *str = lua_tostring(L, 1);
    DirkSimple_log("%s", str);
    return 0;
}


static void collect_lua_garbage(lua_State *L)
{
    lua_gc(L, LUA_GCCOLLECT, 0);
}


// Allocator interface for internal Lua use.
static void *DirkSimple_lua_allocator(void *ud, void *ptr, size_t osize, size_t nsize)
{
    if (nsize == 0) {
        DirkSimple_free(ptr);
        return NULL;
    }
    return DirkSimple_xrealloc(ptr, nsize);
}


static int luahook_panic(lua_State *L)
{
    const char *errstr;

    luahook_DirkSimple_stackwalk(L);

    errstr = lua_tostring(L, -1);
    if (errstr == NULL) {
        errstr = "Something disastrous happened, aborting.";  // doesn't actually return.
    }

    DirkSimple_panic(errstr);
    return 1;  // doesn't actually return, but stackwalk pushed a return value, so return 1 for hygiene purposes.
}

static void register_lua_libs(lua_State *L)
{
    // We always need the string and base libraries (although base has a
    //  few we could trim). The rest you can compile in if you want/need them.
    int i;
    static const luaL_Reg lualibs[] = {
        {"_G", luaopen_base},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table},
        //{LUA_LOADLIBNAME, luaopen_package},
        //{LUA_IOLIBNAME, luaopen_io},
        //{LUA_OSLIBNAME, luaopen_os},
        //{LUA_MATHLIBNAME, luaopen_math},
        //{LUA_DBLIBNAME, luaopen_debug},
        //{LUA_BITLIBNAME, luaopen_bit32},
        //{LUA_COLIBNAME, luaopen_coroutine},
    };

    for (i = 0; i < (sizeof (lualibs) / sizeof (lualibs[0])); i++) {
        luaL_requiref(L, lualibs[i].name, lualibs[i].func, 1);
        lua_pop(L, 1);  // remove lib
    }
}

uint64_t poll_controller_input(void) {
    uint64_t inputbits = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (dev && dev->status_valid) {
        cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
        if (state) {
            // if (state->buttons != 0) {
            //     printf("🎮 poll_controller_input(): buttons=0x%04lX\n", state->buttons);
            // }


            if (state->buttons & CONT_START) {
                printf("⏹️ CONT_START pressed — exiting via arch_exit()\n");
                arch_exit();
            }            
            if (state->buttons & CONT_A) {
                inputbits |= DIRKSIMPLE_INPUT_ACTION1;
                // printf("🅰️ CONT_A detected\n");
            }
            if (state->buttons & CONT_X) {
                inputbits |= DIRKSIMPLE_INPUT_ACTION2;
                // printf("❌ CONT_X detected\n");
            }
            if (state->buttons & CONT_B) {
                inputbits |= DIRKSIMPLE_INPUT_COINSLOT;
                // printf("🅱️ CONT_B (Coin Slot) detected\n");
                vid_screen_shot("/pc/screenshot.ppm");                
            }
            if (state->buttons & CONT_Y) {
                inputbits |= DIRKSIMPLE_INPUT_START;
                printf("🟡 CONT_Y (Start) detected\n");
            }
            if (state->buttons & CONT_DPAD_UP) {
                inputbits |= DIRKSIMPLE_INPUT_UP;
                // printf("⬆️ D-Pad UP detected\n");
            }
            if (state->buttons & CONT_DPAD_DOWN) {
                inputbits |= DIRKSIMPLE_INPUT_DOWN;
                // printf("⬇️ D-Pad DOWN detected\n");
            }
            if (state->buttons & CONT_DPAD_LEFT) {
                inputbits |= DIRKSIMPLE_INPUT_LEFT;
                // printf("⬅️ D-Pad LEFT detected\n");
            }
            if (state->buttons & CONT_DPAD_RIGHT) {
                inputbits |= DIRKSIMPLE_INPUT_RIGHT;
                // printf("➡️ D-Pad RIGHT detected\n");
            }
        }
    } else {
        printf("⚠️ No valid controller detected!\n");
    }

    return inputbits;
}

void DirkSimple_show_single_frame(uint32_t startms) {
    if (!GDecoderActive) return;
    printf("[api] show_single_frame at %lu ms\n", startms);

    // // 🚫 Stop current audio stream immediately
    // if (audio_fp) {
    //     fclose(audio_fp);
    //     audio_fp = NULL;
    // }

    if (startms >= 5830 && startms <= 6200) {
        GRestartOnYPress = 1;  // ✅ Enable restart trigger mode
    }
    // ✅ Immediately mute audio output
    atomic_store(&audio_muted, 1);
    // snd_stream_stop();

    // 🧹 Clear any buffered audio
    // if (stream) {
    //     snd_stream_poll(stream);  // Flush any remaining audio block
    // }

    int frame = (int)((startms / 1000.0) * fps);
    if (frame < 0) frame = 0;
    if (frame >= num_frames) frame = num_frames - 1;

    int buf = frame % NUM_BUFFERS;
    if (load_frame(frame, buf) == 0) {
        draw_frame(buf);
        atomic_store(&frame_index, frame);
    }

    GClipStartTicks = 0;
    GShowingSingleFrame = 1;
    GHalted = 0;

    // while (GShowingSingleFrame) {
    //     uint64_t now_ms = (uint64_t)(psTimer());
    //     uint64_t inputbits = poll_controller_input();
    //     DirkSimple_tick(now_ms, inputbits);
    //     thd_sleep(1);  // prevent CPU spin
    // }

    // ✅ Unmute audio again after frame ends
    // atomic_store(&audio_muted, 0);
}



static int luahook_DirkSimple_show_single_frame(lua_State *L)
{
    const uint32_t startms = (uint32_t) lua_tonumber(L, 1);
    DirkSimple_show_single_frame(startms);
    return 0;
}

bool schedule_frame_preload(int frame) {
    int buf = frame % NUM_BUFFERS;

    int current_state = atomic_load(&buf_state[buf]);
    if (current_state != BUF_EMPTY)
        return false;

    int head = atomic_load(&preload_ring_head);
    int tail = atomic_load(&preload_ring_tail);
    int next_head = (head + 1) % RING_CAPACITY;

    if (next_head == tail)
        return false; // Ring full

    // Check for duplicates
    for (int i = tail; i != head; i = (i + 1) % RING_CAPACITY) {
        if (preload_ring[i].frame == frame || (preload_ring[i].frame % NUM_BUFFERS) == buf) {
            printf("🔧 Buffer conflict: frame %d conflicts with queued frame %d (both use buf %d)\n",
                   frame, preload_ring[i].frame, buf);
            return false;
        }
    }

    preload_ring[head].frame = frame;
    preload_ring[head].generation = GSeekGeneration;
    atomic_store(&preload_ring_head, next_head);
    return true;
}

void seek_to_frame(int new_frame) {
    if (new_frame < 0) new_frame = 0;
    if (new_frame >= num_frames) new_frame = num_frames - 1;

    int old_frame = atomic_load(&frame_index);
    double old_audio_time = atomic_load(&audio_start_time_ms);

    printf("🔄 Seeking from frame %d to frame %d\n", old_frame, new_frame);

    // Stop audio and mute
    fclose(audio_fp);
    atomic_store(&audio_muted, 1);

    // Reset preload buffers
    for (int i = 0; i < NUM_BUFFERS; i++) {
        atomic_store(&buf_state[i], BUF_EMPTY);
        // printf("🔄 Cleared buffer %d\n", i);
    }

    atomic_store(&preload_ring_head, 0);
    atomic_store(&preload_ring_tail, 0);
    // printf("🔄 Ring buffer cleared\n");

    // Reopen and seek audio file
    audio_fp = fopen(GGamePath, "rb");
    int samples_per_frame = (int)(sample_rate / fps);
    int bytes_to_skip = ((new_frame * samples_per_frame) / 2 + 15) & ~0xF;
    bytes_to_skip += audio_offset;
    fseek(audio_fp, bytes_to_skip, SEEK_SET);

    // Update frame index
    atomic_store(&frame_index, new_frame);

    // Preload upcoming frames
    // printf("🔄 Rescheduling initial frames starting from %d\n", new_frame);
    for (int i = 0; i < MIN(NUM_BUFFERS, 4) && (new_frame + i) < num_frames; i++) {
        // bool scheduled = 
        schedule_frame_preload(new_frame + i);
        // printf("🔄 Frame %d scheduled: %s\n", new_frame + i, scheduled ? "YES" : "NO");
    }

    // Wait for some frames to be ready
    // printf("🔄 Waiting for initial frames to load...\n");
    for (int i = 0; i < MIN(3, NUM_BUFFERS) && (new_frame + i) < num_frames; i++) {
        int frame_to_wait = new_frame + i;
        int buf_to_wait = frame_to_wait % NUM_BUFFERS;
        int wait_count = 0;
        while (atomic_load(&buf_state[buf_to_wait]) != BUF_READY && wait_count < 200) {
            thd_sleep(1);
            wait_count++;
        }
        // if (atomic_load(&buf_state[buf_to_wait]) == BUF_READY) {
        //     printf("🔄 Frame %d ready in buffer %d\n", frame_to_wait, buf_to_wait);
        // } else {
        //     printf("🔄 ⚠️ Frame %d not ready after waiting\n", frame_to_wait);
        // }
    }

    // ✅ Reset sync BEFORE preload
    double new_audio_time = (double)(new_frame * samples_per_frame) * 1000.0 / sample_rate;
    frame_timer_anchor = psTimer();
    atomic_store(&audio_start_time_ms, new_audio_time);
    atomic_store(&audio_bytes_fed, 0);  // optional
    GHalted = 0;  // Reset halted state
    // printf("🔄 Seek complete: frame %d → %d | audio %.2fms → %.2fms | byte offset: %d\n",
    //     old_frame, new_frame, old_audio_time, new_audio_time,
    //     bytes_to_skip - audio_offset);

    // Unmute after sync reset
    // atomic_store(&audio_muted, 0);
}

void DirkSimple_start_clip(uint32_t startms) {
    if (!GDecoderActive) return;
    int frame = (int)((startms / 1000.0f) * fps);
    if (frame < 0) frame = 0;
    if (frame >= num_frames) frame = num_frames - 1;

    // Reset single-frame and halted state
    GShowingSingleFrame = 0;
    GHalted = 0;

    // Optional: log and simulate disc lag if needed
    DirkSimple_log("START CLIP: GTicks %llu, startms %u → frame %d", GTicks, startms, frame);

    atomic_store(&seek_request, frame);
    
    collect_lua_garbage(GLua);
}

int luahook_DirkSimple_start_clip(lua_State *L)
{
    const uint32_t startms = (uint32_t) lua_tonumber(L, 1);
    DirkSimple_log("Lua requested start_clip at %u ms", startms);
    DirkSimple_start_clip(startms);  // ✅ Use the real function
    return 0;
}

static void DirkSimple_halt_video(void)
{
    if (!GDecoderActive) return;

    DirkSimple_log("HALT VIDEO");

    GHalted = 1;
    GShowingSingleFrame = 0;
    GClipStartTicks = GTicks;
    GSeekGeneration--;  // invalidate preload queue
    DirkSimple_cleardiscaudio();
    
    // force a frame draw to "freeze" screen (optional)
    DirkSimple_discvideo(NULL);
}


static int luahook_DirkSimple_halt_video(lua_State *L)
{
    DirkSimple_halt_video();
    return 0;
}

// This just lets you punch in one-liners and Lua will run them as individual
//  chunks, but you can completely access all Lua state, including calling C
//  functions and altering tables. At this time, it's more of a "console"
//  than a debugger. You can do "p DirkSimple_lua_debugger()" from gdb to launch this
//  from a breakpoint in native code, or call DirkSimple.debugger() to launch
//  it from Lua code (with stacktrace intact, too: type 'bt' to see it).
static int luahook_DirkSimple_debugger(lua_State *L)
{
    int origtop;

    lua_pushcfunction(L, luahook_DirkSimple_stackwalk);
    origtop = lua_gettop(L);

    printf("Quick and dirty Lua debugger. Type 'exit' to quit.\n");

    while (1) {
        char buf[256];
        int len = 0;
        printf("> ");
        fflush(stdout);
        if (fgets(buf, sizeof (buf), stdin) == NULL) {
            printf("\n\n  fgets() on stdin failed: ");
            break;
        }

        len = (int) (strlen(buf) - 1);
        while ( (len >= 0) && ((buf[len] == '\n') || (buf[len] == '\r')) ) {
            buf[len--] = '\0';
        }

        if (strcmp(buf, "q") == 0) {
            break;
        } else if (strcmp(buf, "quit") == 0) {
            break;
        } else if (strcmp(buf, "exit") == 0) {
            break;
        } else if (strcmp(buf, "bt") == 0) {
            strcpy(buf, "DirkSimple.stackwalk()");
        }

        if ( (luaL_loadstring(L, buf) != 0) ||
             (lua_pcall(L, 0, LUA_MULTRET, -2) != 0) ) {
            printf("%s\n", lua_tostring(L, -1));
            lua_pop(L, 1);
        } else {
            printf("Returned %d values.\n", lua_gettop(L) - origtop);
            while (lua_gettop(L) != origtop) {
                // !!! FIXME: dump details of values to stdout here.
                lua_pop(L, 1);
            }
            printf("\n");
        }
    }

    lua_pop(L, 1);
    printf("exiting debugger...\n");

    return 0;
}

static int luahook_DirkSimple_truncate(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer) lua_tonumber(L, 1));
    return 1;
}


static int luahook_DirkSimple_to_int(lua_State *L)
{
    if (lua_isstring(L, 1)) {
        lua_pushinteger(L, atoi(lua_tostring(L, 1)));
    } else if (lua_isboolean(L, 1)) {
        lua_pushinteger(L, lua_toboolean(L, 1) ? 1 : 0);
    } else if (lua_isnumber(L, 1)) {
        lua_pushinteger(L, (lua_Integer) lua_tonumber(L, 1));
    } else {
        lua_pushinteger(L, 0);
    }
    return 1;
}

static int luahook_DirkSimple_to_bool(lua_State *L)
{
    if (lua_isstring(L, 1)) {
        lua_pushboolean(L, (strcmp(lua_tostring(L, 1), "true") == 0));
    } else if (lua_isboolean(L, 1)) {
        lua_pushboolean(L, lua_toboolean(L, 1));
    } else if (lua_isnumber(L, 1)) {
        lua_pushboolean(L, (lua_tonumber(L, 1) != 0));
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}


static void setup_lua(void)
{
    GLua = lua_newstate(DirkSimple_lua_allocator, NULL);  // calls DirkSimple_panic() on failure.
    lua_atpanic(GLua, luahook_panic);
    register_lua_libs(GLua);

    // Build DirkSimple namespace for Lua to access and fill in C bridges...
    lua_newtable(GLua);
        set_cfunc(GLua, luahook_DirkSimple_show_single_frame, "show_single_frame");
        set_cfunc(GLua, luahook_DirkSimple_start_clip, "start_clip");
        set_cfunc(GLua, luahook_DirkSimple_halt_video, "halt_video");
        set_cfunc(GLua, luahook_DirkSimple_log, "log");
        set_cfunc(GLua, luahook_panic, "panic");
        set_cfunc(GLua, luahook_DirkSimple_stackwalk, "stackwalk");
        set_cfunc(GLua, luahook_DirkSimple_debugger, "debugger");
        set_cfunc(GLua, luahook_DirkSimple_truncate, "truncate");
        set_cfunc(GLua, luahook_DirkSimple_to_int, "to_int");
        set_cfunc(GLua, luahook_DirkSimple_to_bool, "to_bool");
        set_cfunc(GLua, luahook_DirkSimple_clear_screen, "clear_screen");
        set_cfunc(GLua, luahook_DirkSimple_draw_sprite, "draw_sprite");
        set_cfunc(GLua, luahook_DirkSimple_play_sound, "play_sound");
        set_string(GLua, "", "gametitle");
        set_string(GLua, GLuaLicense, "lua_license");  // just so deadcode elimination can't remove this string from the binary.
                // This is the dimensions of the video, which might not actually match what the arcade used.
        set_integer(GLua, (int) 320, "video_width");
        set_integer(GLua, (int) 240, "video_height");
    lua_setglobal(GLua, DIRKSIMPLE_LUA_NAMESPACE);

    load_lua_gamecode(GLua, GGameDir);

    register_cvars(GLua);

    collect_lua_garbage(GLua);  // get rid of old init crap we don't need.
}


static void setup_game_strings(const char *basedir, const char *gamepath, const char *gamename)
{
    char *ptr;
    size_t slen;

    GGamePath = DirkSimple_xstrdup(gamepath);

    if (gamename) {
        GGameName = DirkSimple_xstrdup(gamename);
    } else {
        for (ptr = GGamePath + strlen(GGamePath); ptr >= GGamePath; ptr--) {
            #if defined(_WIN32) || defined(__OS2__)
            if (*ptr == '\\') {
                ptr++;
                break;
            }
            #endif
            if (*ptr == '/') {
                ptr++;
                break;
            }
        }

        if (ptr < GGamePath) {
            ptr = GGamePath;
        }

        GGameName = DirkSimple_xstrdup(ptr);

        for (ptr = GGameName + strlen(GGameName); ptr >= GGameName; ptr--) {
            if (*ptr == '.') {
                *ptr = '\0';
                break;
            }
        }
    }

    for (ptr = GGameName; *ptr; ptr++) {
        const char ch = *ptr;
        if ((ch >= 'A') && (ch <= 'Z')) {
            *ptr = ch - ('A' - 'a');
        }
    }

    // Fix: extract grandparent directory to avoid duplicating "games"
    char *lastsep = strrchr(basedir, '/');
    if (lastsep) {
        size_t len = lastsep - basedir;
        char *temp = DirkSimple_xmalloc(len + 1);
        strncpy(temp, basedir, len);
        temp[len] = '\0';
        GDataDir = temp;
    } else {
        GDataDir = DirkSimple_xstrdup(basedir);
    }

    slen = strlen(GDataDir) + strlen(GGameName) + 32;
    GGameDir = (char *) DirkSimple_xmalloc(slen);
    snprintf(GGameDir, slen, "%s/%s/", GDataDir, GGameName);
    printf("Game directory: %s, gamename: %s\n", GGameDir, GGameName);
}


void DirkSimple_debugger(void)
{
    luahook_DirkSimple_debugger(GLua);
}


void DirkSimple_discaudio(const float *pcm, int numframes) { (void)pcm; (void)numframes; }
void DirkSimple_cleardiscaudio(void) {
    // 🛑 Mute the stream immediately.
    atomic_store(&audio_muted, 1);

    // 🔁 Rewind audio_fp if open (optional but good to prevent garbage)
    if (audio_fp) {
        fseek(audio_fp, 0, SEEK_SET);
    }

    // 🧹 Clear state for sync tracking
    atomic_store(&audio_bytes_fed, 0);
    atomic_store(&frame_index, 0);
    atomic_store(&seek_request, -1);
    audio_start_time_ms = 0;
}
void DirkSimple_clearscreen(uint8_t r, uint8_t g, uint8_t b)
{
    // Start scene if not already begun (safe: draw_ui_only_frame handles this)
    uint32_t color = (0xFF << 24) | (r << 16) | (g << 8) | b;

    // Submit a full-screen quad to clear background
    pvr_vertex_t vtx[4] = {
        {.flags = PVR_CMD_VERTEX,     .x = 0,   .y = 0,   .z = 0.5f, .argb = color, .oargb = 0},
        {.flags = PVR_CMD_VERTEX,     .x = 640, .y = 0,   .z = 0.5f, .argb = color, .oargb = 0},
        {.flags = PVR_CMD_VERTEX,     .x = 0,   .y = 480, .z = 0.5f, .argb = color, .oargb = 0},
        {.flags = PVR_CMD_VERTEX_EOL, .x = 640, .y = 480, .z = 0.5f, .argb = color, .oargb = 0},
    };

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);  // Use TR to ensure it blends correctly
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &hdr, 1);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), vtx, 4);
}

void DirkSimple_drawsprite(DirkSimple_Sprite *sprite, int sx, int sy, int sw, int sh,
                           int dx, int dy, int dw, int dh,
                           uint8_t rmod, uint8_t gmod, uint8_t bmod)
{
    if (!sprite || !sprite->rgba) return;
    // int pot_w = 1 << (32 - __builtin_clz(dw - 1));
    // int pot_h = 1 << (32 - __builtin_clz(dh - 1));
    // DirkSimple_log("Drawing sprite '%s' at (%d, %d) with size (%d, %d) to (%d, %d) with size (%d, %d)",
    //               sprite->name, sx, sy, sw, sh, dx, dy, dw, dh);
    float u0 = (float)sx / sprite->width;
    float v0 = (float)sy / sprite->height;
    float u1 = (float)(sx + sw) / sprite->width;
    float v1 = (float)(sy + sh) / sprite->height;

    uint32_t color = (0xFF << 24) | (rmod << 16) | (gmod << 8) | bmod;

    sprite_vert[0] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX,     .x = dx,       .y = dy,       .z = 1.0f, .u = u0, .v = v0, .argb = color, .oargb = 0};
    sprite_vert[1] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX,     .x = dx + dw,  .y = dy,       .z = 1.0f, .u = u1, .v = v0, .argb = color, .oargb = 0};
    sprite_vert[2] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX,     .x = dx,       .y = dy + dh,  .z = 1.0f, .u = u0, .v = v1, .argb = color, .oargb = 0};
    sprite_vert[3] = (pvr_vertex_t){.flags = PVR_CMD_VERTEX_EOL, .x = dx + dw,  .y = dy + dh,  .z = 1.0f, .u = u1, .v = v1, .argb = color, .oargb = 0};

    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &sprite->sprite_hdr, 1);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), sprite_vert, 4);
}



void DirkSimple_destroysprite(DirkSimple_Sprite *sprite)
{
    // SDL_Texture *texture = (SDL_Texture *) sprite->platform_handle;
    // if (texture) {
    //     SDL_DestroyTexture(texture);
    // }
}


void DirkSimple_playwave(DirkSimple_Wave *wave)
{
    // printf("playwave: %s\n", wave ? wave->name : "NULL");
    if (!wave || !wave->platform_handle) return;
    sfxhnd_t sfx = (sfxhnd_t)(intptr_t)wave->platform_handle;  // cast back
    if (sfx < 0) return;

    const uint8_t volume = 255;
    const uint8_t pan = 128;  // CENTER
    snd_sfx_play(sfx, volume, pan);
}


void DirkSimple_startup(const char *basedir, const char *gamepath, const char *gamename, DirkSimple_PixFmt pixfmt) {
    (void)basedir; (void)gamepath; (void)gamename; (void)pixfmt;
        // dctx = ZSTD_createDCtx();
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, 15);
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_forceIgnoreChecksum, 1);
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_refMultipleDDicts, ZSTD_rmd_refSingleDDict);
        // ZSTD_DCtx_setParameter(dctx, ZSTD_d_maxBlockSize, 65536);
        // ZSTD_DCtx_refDDict(dctx, NULL);
        // ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);    
        atomic_store(&audio_muted, 1);
    fp = fopen(gamepath, "rb");
    if (!fp) {
        printf("[error] failed to open video: %s\n", gamepath);
        return;
    }
        char magic[4];
        fread(magic, 1, 4, fp);
        uint32_t version;
        fread(&version, 4, 1, fp);
        fread(&frame_type, 1, 1, fp);
        fread(&video_width, 2, 1, fp);
        fread(&video_height, 2, 1, fp);
        fread(&fps, sizeof(float), 1, fp); 
        fread(&sample_rate, 2, 1, fp);
        fread(&audio_channels, 2, 1, fp);
        fread(&num_frames, 4, 1, fp);
        fread(&video_frame_size, 4, 1, fp);
        fread(&max_compressed_size, 4, 1, fp);
        fread(&audio_offset, 4, 1, fp);

    frame_offsets = malloc((num_frames + 1) * sizeof(uint32_t));
    fread(frame_offsets, sizeof(uint32_t), num_frames + 1, fp);
    compressed_buffer = memalign(32, max_compressed_size);
        printf("📦 Header: %s %dx%d @ %ffps, %dHz, %dch, %d frames, frame_size=%d, max_compressed_size=%d, audio_offset=0x%X\n",
            frame_type == 1 ? "YUV420P" : "RGB565", video_width, video_height, fps, sample_rate, audio_channels, num_frames, video_frame_size, max_compressed_size, audio_offset);    
    for (int i = 0; i < NUM_BUFFERS; i++) frame_buffer[i] = memalign(32, video_frame_size);
    for (int i = 0; i < NUM_BUFFERS; i++) atomic_store(&buf_state[i], BUF_EMPTY);
    DirkSimple_videoformat("DC", video_width, video_height, fps);
    DirkSimple_audioformat(audio_channels, sample_rate);
    // audio_fp = fp;
    // fseek(audio_fp, audio_offset, SEEK_SET);
    frame_timer_anchor = psTimer();
    atomic_store(&audio_start_time_ms, 0.0);
    thd_create(0, worker_thread, NULL);
        // DirkSimple_shutdown();  // safe to call even if not started up at the moment.
    printf("[startup] DirkSimple started with gamepath: %s and gamename: %s\n", gamepath, gamename);
    setup_game_strings(basedir, gamepath, gamename);
    setup_lua();
    GDecoderActive = 1;
    printf("[startup] video frames: %d\n", num_frames);
}

void DirkSimple_shutdown(void) {
    // Free game sprites
    // DirkSimple_Sprite *sprite, *spritenext;
    // for (sprite = GSprites; sprite != NULL; sprite = spritenext) {
    //     spritenext = sprite->next;
    //     DirkSimple_destroysprite(sprite);
    //     DirkSimple_free(sprite->name);
    //     DirkSimple_free(sprite->rgba);
    //     DirkSimple_free(sprite);
    // }
    // GSprites = NULL;

    // Free game sounds
    // DirkSimple_Wave *wave, *wavenext;
    // for (wave = GWaves; wave != NULL; wave = wavenext) {
    //     wavenext = wave->next;
    //     DirkSimple_destroywave(wave);
    //     DirkSimple_free(wave->name);
    //     DirkSimple_free(wave->pcm);
    //     DirkSimple_free(wave);
    // }
    // GWaves = NULL;

    // Reset render command buffer
    // DirkSimple_free(GRenderCommands);
    // GRenderCommands = NULL;
    // GNumRenderCommands = 0;
    // GNumAllocatedRenderCommands = 0;

    // Reset video/audio state (lightweight reset)
    GClipStartTicks = 0;
    GTicks = 0;
    GTicksOffset = 0;
    GSeekGeneration = 0;
    GNeedInitialLuaTick = 1;
    GPreviousInputBits = 0;
    GHalted = 0;
    GShowingSingleFrame = 0;

    // Clear audio sync tracking
    audio_start_time_ms = 0;
    atomic_store(&audio_bytes_fed, 0);
    atomic_store(&seek_request, -1);
    atomic_store(&audio_muted, 1);

    // Optional: rewind audio_fp
    if (audio_fp) {
        fseek(audio_fp, audio_offset, SEEK_SET);
    }

    // Free Lua state
    if (GLua) {
        lua_close(GLua);
        GLua = NULL;
    }

    // DO NOT free:
    // - dctx
    // - compressed_buffer
    // - frame_offsets
    // - frame_buffer[]
    // These are initialized once in startup() and reused!
}

// Sets t[sym]=f, where t is on the top of the Lua stack.
static void set_boolean(lua_State *L, int x, const char *sym)
{
    lua_pushboolean(L, x);
    lua_setfield(L, -2, sym);
}

typedef int (*inputbit_testfn)(const uint64_t curbits, const uint64_t prevbits, const uint64_t flag);

static int inputbit_is_pressed(const uint64_t curbits, const uint64_t prevbits, const uint64_t flag)
{
    return ((curbits & flag) != 0) && ((prevbits & flag) == 0);
}

static int inputbit_is_held(const uint64_t curbits, const uint64_t prevbits, const uint64_t flag)
{
    return ((curbits & prevbits & flag) != 0);
}

static int inputbit_is_released(const uint64_t curbits, const uint64_t prevbits, const uint64_t flag)
{
    return ((curbits & flag) == 0) && ((prevbits & flag) != 0);
}

static int inputbit_is_untouched(const uint64_t curbits, const uint64_t prevbits, const uint64_t flag)
{
    return ((curbits & prevbits & flag) == 0);
}


static void set_input_subtable(lua_State *L, const char *tablename, inputbit_testfn testfn, uint64_t curbits, uint64_t prevbits)
{
    lua_newtable(L);  // the subtable
        set_boolean(L, testfn(curbits, prevbits, DIRKSIMPLE_INPUT_UP), "up");
        set_boolean(L, testfn(curbits, prevbits, DIRKSIMPLE_INPUT_DOWN), "down");
        set_boolean(L, testfn(curbits, prevbits, DIRKSIMPLE_INPUT_LEFT), "left");
        set_boolean(L, testfn(curbits, prevbits, DIRKSIMPLE_INPUT_RIGHT), "right");
        set_boolean(L, testfn(curbits, prevbits, DIRKSIMPLE_INPUT_ACTION1), "action");
        set_boolean(L, testfn(curbits, prevbits, DIRKSIMPLE_INPUT_ACTION2), "action2");
        set_boolean(L, testfn(curbits, prevbits, DIRKSIMPLE_INPUT_COINSLOT), "coinslot");
        set_boolean(L, testfn(curbits, prevbits, DIRKSIMPLE_INPUT_START), "start");
    lua_setfield(L, -2, tablename);
}


static void push_inputs_table(lua_State *L, const uint64_t curbits)
{
    const uint64_t prevbits = GPreviousInputBits;
    lua_newtable(L);  // the inputs table
    set_input_subtable(L, "pressed", inputbit_is_pressed, curbits, prevbits);
    set_input_subtable(L, "held", inputbit_is_held, curbits, prevbits);
    set_input_subtable(L, "released", inputbit_is_released, curbits, prevbits);
    set_input_subtable(L, "untouched", inputbit_is_untouched, curbits, prevbits);
    // inputs table is ready, on top of Lua stack.
}

static void call_lua_tick(lua_State *L, uint64_t ticks, uint64_t clipstartticks, uint64_t inputbits)
{
    lua_getglobal(L, DIRKSIMPLE_LUA_NAMESPACE);
    if (!lua_istable(L, -1)) {  // namespace is sane?
        DirkSimple_panic("DirkSimple Lua namespace is not a table!");
    }
    lua_getfield(L, -1, "tick");
    if (!lua_isfunction(L, -1)) {
        DirkSimple_panic("DirkSimple.tick is not a function!");
    }
    lua_pushnumber(L, (lua_Number) ticks);
    lua_pushnumber(L, (lua_Number) clipstartticks);
    push_inputs_table(L, inputbits);
    lua_call(L, 3, 0);  // this will pop the function and args
    lua_pop(L, 1);  // pop the namespace

    // Clean up any Lua tick waste.
    // collect_lua_garbage(L);  // we can move this to start_clip if it turns out to be too heavy.
}

static void fmv_tick(uint64_t now_ms) {

    static double accumulated_frame_debt = 0.0;
    static int frames_dropped = 0;
    static int stall_count = 0;
    static double max_frame_time = 0.0;
    static double avg_frame_time = 0.0;
    static double frame_time_samples = 0.0;

    if (GShowingSingleFrame) {
        // 🔄 Let Lua tick get actual timing, like normal playback
        call_lua_tick(GLua, GTicks, (GClipStartTicks ? (GTicks - GClipStartTicks) : 0), poll_controller_input());
        return;
    }
    int req = atomic_exchange(&seek_request, -1);
    if (req >= 0) {
        seek_to_frame(req);
        GSeekGeneration++;
        GClipStartTicks = GTicks;
        atomic_store(&audio_muted, 0);  // ✅ Move unmute here
    }
    int current_frame = atomic_load(&frame_index);
    static int muted_after_seek = 0;
    if (req >= 0) {
        muted_after_seek = 1;
    } else if (muted_after_seek) {
        atomic_store(&audio_muted, 0);
        muted_after_seek = 0;
    }    
    double current_time = psTimer();
    double elapsed_ms = (current_time - frame_timer_anchor); 
    double current_audio_time_ms = atomic_load(&audio_start_time_ms) + elapsed_ms;
    static int printed = 0;
    if (!printed) {
        printf("🎯 psTimer()=%.2fms, anchor=%.2fms, elapsed=%.2fms\n", current_time, frame_timer_anchor, elapsed_ms);
        printed = 1;
    }    

    double expected_video_time = current_frame * frame_duration;

    // Adjust target time with frame debt
    double target_time_ms = expected_video_time;
    if (accumulated_frame_debt > 0.0)
        target_time_ms += MIN(accumulated_frame_debt, frame_duration * 0.5);
    else if (accumulated_frame_debt < 0.0)
        target_time_ms += MAX(accumulated_frame_debt, -frame_duration * 0.5);

    // Frame skipping logic
    int frames_to_skip = 0;
    int temp_frame = current_frame;
    const double max_lag_ms = 52.0; 
    while ((temp_frame < num_frames) && (temp_frame * frame_duration + max_lag_ms < current_audio_time_ms)) {
        temp_frame++;
        frames_to_skip++;
        accumulated_frame_debt *= 0.5;
    }

    if (frames_to_skip > 0) {
        printf("⚠️ Skipping %d frame(s): %d → %d (audio ahead by %.1fms)\n",
               frames_to_skip, current_frame, temp_frame,
               current_audio_time_ms - expected_video_time);
        atomic_fetch_add(&frame_index, frames_to_skip);
        frames_dropped += frames_to_skip;
        current_frame = temp_frame;
    }

    double frame_render_start = psTimer();
    if (current_audio_time_ms >= target_time_ms) {
        int buf = current_frame % NUM_BUFFERS;

        if (atomic_load(&buf_state[buf]) == BUF_READY) {
            draw_frame(buf);
            atomic_store(&buf_state[buf], BUF_EMPTY);
            atomic_fetch_add(&frame_index, 1);
            stall_count = 0;

            // Schedule next few frames
            for (int i = 1; i <= 3; i++) {
                int next = current_frame + i;
                if (next >= num_frames) break;
                int b = next % NUM_BUFFERS;
                if (atomic_load(&buf_state[b]) == BUF_EMPTY)
                    schedule_frame_preload(next);
            }
        } else {
            stall_count++;
            if (atomic_load(&buf_state[buf]) == BUF_EMPTY)
                schedule_frame_preload(current_frame);

            if (stall_count > 10) {
                printf("⚠️ Emergency advancing past stalled frame %d\n", current_frame);
                atomic_store(&buf_state[buf], BUF_EMPTY);
                atomic_fetch_add(&frame_index, 1);
                stall_count = 0;
                for (int i = 0; i < 3; i++) {
                    int recover = current_frame + i;
                    if (recover < num_frames)
                        schedule_frame_preload(recover);
                }
            }
        }
    }

    // Timing stats
    double frame_render_end = psTimer();
    double this_frame_time = frame_render_end - frame_render_start;

    if (this_frame_time > max_frame_time)
        max_frame_time = this_frame_time;

    avg_frame_time = (avg_frame_time * frame_time_samples + this_frame_time) / (frame_time_samples + 1);
    frame_time_samples++;

    // Adjust sync debt
    double frame_overrun = this_frame_time - frame_duration;
    if (frame_overrun > 0.0)
        accumulated_frame_debt -= frame_overrun;
    else
        accumulated_frame_debt += (-frame_overrun * 0.1);
    accumulated_frame_debt *= 0.95;

    // Optional wait logic
    double wait_ms = target_time_ms - current_audio_time_ms;
    if (accumulated_frame_debt < -10.0)
        wait_ms = MAX(0.0, wait_ms + accumulated_frame_debt * 0.1);

    if (wait_ms > 8.0) {
        int sleep_ms = (int)(wait_ms - 3.0);
        if (sleep_ms > 0)
            thd_sleep(sleep_ms);
    } else if (wait_ms > 1.0) {
        thd_pass();
    }
}




void DirkSimple_restart(void)  // DO NOT CALL THIS FROM LUA CODE
{
    if (GLua) {
        lua_close(GLua);
        GLua = NULL;
    }
    setup_lua();
}
static int last_seen_seek_generation = -1;

void DirkSimple_tick(uint64_t monotonic_ms, uint64_t inputbits)
{
    if (GRestartOnYPress && inputbit_is_pressed(inputbits, GPreviousInputBits, DIRKSIMPLE_INPUT_START)) {
        printf("🔁 Restart triggered via Y button at %llu ms frame\n", monotonic_ms);
        DirkSimple_restart();
        GRestartOnYPress = 0;
        return;
    }

    if (GTicksOffset == 0) {
        if (monotonic_ms < 2) return;
        GTicksOffset = monotonic_ms - 1;
    } else if (GTicksOffset > monotonic_ms) {
        DirkSimple_panic("Time ran backwards! Aborting!");
    }

    GTicks = monotonic_ms - GTicksOffset;

    // 🔄 Delay Lua tick() and FMV logic until frame seek completes
    if (GSeeking) {
        int buf = GSeekTargetFrame % NUM_BUFFERS;
        if (atomic_load(&buf_state[buf]) == BUF_READY) {
            draw_frame(buf);
            atomic_store(&buf_state[buf], BUF_EMPTY);
            atomic_store(&frame_index, GSeekTargetFrame + 1);

            frame_timer_anchor = psTimer();
            atomic_store(&audio_start_time_ms, GSeekTargetFrame * frame_duration);
            atomic_store(&audio_bytes_fed, 0);
            atomic_store(&audio_muted, 0);

            GClipStartTicks = GTicks;
            GSeekTargetFrame = -1;
            GSeeking = 0;

            DirkSimple_log("✅ Seek complete at GTicks=%llu, anchor=%.2fms", GTicks, frame_timer_anchor);
        } else {
            draw_ui_only_frame();  // just show UI while waiting
            GPreviousInputBits = inputbits;
            return;
        }
    }

    if (GShowingSingleFrame) {
        call_lua_tick(GLua, GTicks, (GClipStartTicks ? (GTicks - GClipStartTicks) : 0), inputbits);
        GPreviousInputBits = inputbits;
        return;
    }

    const unsigned int expected_seek_generation = GSeekGeneration;

    if (GNeedInitialLuaTick) {
        GNeedInitialLuaTick = 0;
        call_lua_tick(GLua, 0, 0, inputbits);
    } else if (GClipStartTicks) {
        call_lua_tick(GLua, GTicks, (GTicks - GClipStartTicks), inputbits);
    }

    if (GHalted || expected_seek_generation != GSeekGeneration) {
        draw_ui_only_frame();
        GPreviousInputBits = inputbits;
        return;
    }

    fmv_tick(monotonic_ms);  // continue FMV sync logic

    GPreviousInputBits = inputbits;
}




int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("💡 MAIN STARTED\n");

    // DirkSimple_startup("/pc/data/games/", "/pc/data/games/lair/lair.dcmv", "lair", DIRKSIMPLE_PIXFMT_RGB565);
    DirkSimple_startup("/pc/data/games/", "/pc/data/games/cliff/cliff.dcmv", "cliff", DIRKSIMPLE_PIXFMT_RGB565);
    printf("[main] DirkSimple running...\n");

    while (1) {
        uint64_t now_ms = (uint64_t)(psTimer());
        uint64_t inputbits = poll_controller_input();
        // DirkSimple_beginframe();
        DirkSimple_tick(now_ms, inputbits);

        // DirkSimple_endframe();
        thd_sleep(1);
    }

    return 0;
}
