#include "port.h"
#include "vita.h"
#include "menu.h"
#include "mods.h"
#define XZ_USE_CRC64
#include "../../vita/xz/xz.h"

#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "hostapi.h"

static const char *const s_exts[] = {".xdelta", ".vcdiff", ".delta", ".nds"};

static int mod_ext(const char *file) {
    size_t n = strlen(file);
    for (int i = 0; i < 4; i++) {
        size_t e = strlen(s_exts[i]);
        if (n > e && strcasecmp(file + n - e, s_exts[i]) == 0) return i;
    }
    return -1;
}

static bool is_dir(const SceIoDirent *e) { return SCE_S_ISDIR(e->d_stat.st_mode); }

static void sort_names(char names[][VITA_MOD_NAME], int count) {
    for (int i = 1; i < count; i++) {
        for (int j = i; j > 0 && strcasecmp(names[j - 1], names[j]) > 0; j--) {
            char t[VITA_MOD_NAME];
            strcpy(t, names[j]);
            strcpy(names[j], names[j - 1]);
            strcpy(names[j - 1], t);
        }
    }
}

bool vita_mod_paths(const char *name, char *rom, char *save, size_t size) {
    SceIoStat st;
    snprintf(rom, size, "%s/%s/game.nds", VITA_MODS_DIR, name);
    snprintf(save, size, "%s/%s/save.sav", VITA_MODS_DIR, name);
    return name[0] && sceIoGetstat(rom, &st) >= 0;
}

int vita_mods_list(char names[][VITA_MOD_NAME], int max) {
    int count = 0;
    SceUID dir = sceIoDopen(VITA_MODS_DIR);
    if (dir < 0) {
        sceIoMkdir(VITA_MODS_DIR, 0777);
        return 0;
    }
    SceIoDirent ent;
    while (count < max && sceIoDread(dir, &ent) > 0) {
        char rom[256], save[256];
        if (!is_dir(&ent) || ent.d_name[0] == '.' || strlen(ent.d_name) >= VITA_MOD_NAME) continue;
        if (vita_mod_paths(ent.d_name, rom, save, sizeof(rom))) strcpy(names[count++], ent.d_name);
    }
    sceIoDclose(dir);
    sort_names(names, count);
    return count;
}

static int loose_files(char files[][VITA_MOD_NAME + 8], int max) {
    int count = 0;
    SceUID dir = sceIoDopen(VITA_MODS_DIR);
    if (dir < 0) return 0;
    SceIoDirent ent;
    while (count < max && sceIoDread(dir, &ent) > 0) {
        int ext = mod_ext(ent.d_name);
        if (is_dir(&ent) || ext < 0 || strlen(ent.d_name) - strlen(s_exts[ext]) == 0 ||
            strlen(ent.d_name) - strlen(s_exts[ext]) >= VITA_MOD_NAME) {
            continue;
        }
        strcpy(files[count++], ent.d_name);
    }
    sceIoDclose(dir);
    return count;
}

bool vita_mods_pending(void) {
    char files[1][VITA_MOD_NAME + 8];
    return loose_files(files, 1) > 0;
}

static SceUID s_fb_block = -1;
static uint32_t *s_fb;
static int s_shown_percent = -1;
static char s_title[96];

static void screen_show(const char *what, int percent, bool error) {
    if (!s_fb) {
        s_fb_block = sceKernelAllocMemBlock("mkds_mods_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x200000, NULL);
        if (s_fb_block < 0) return;
        sceKernelGetMemBlockBase(s_fb_block, (void **) &s_fb);
    }
    int pw, ph;
    const uint32_t *panel = vita_menu_progress_panel(s_title, what, percent, error, &pw, &ph);
    int x0 = (VITA_SCREEN_W - pw) / 2, y0 = (VITA_SCREEN_H - ph) / 2;
    for (int y = 0; y < VITA_SCREEN_H; y++) {
        for (int x = 0; x < VITA_SCREEN_W; x++) {
            uint32_t c = 0xff1a1414;
            int px = x - x0, py = y - y0;
            if (px >= 0 && px < pw && py >= 0 && py < ph && (panel[py * pw + px] >> 24)) c = panel[py * pw + px];
            s_fb[y * VITA_SCREEN_W + x] = c;
        }
    }
    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = s_fb;
    fb.pitch = VITA_SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VITA_SCREEN_W;
    fb.height = VITA_SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static void progress(const char *what, int percent) {
    if (percent == s_shown_percent) return;
    s_shown_percent = percent;
    screen_show(what, percent, false);
}

static void screen_close(void) {
    if (s_fb_block >= 0) {
        sceDisplayWaitVblankStart();
        sceKernelFreeMemBlock(s_fb_block);
    }
    s_fb_block = -1;
    s_fb = NULL;
}

static const char *s_error;

static bool fail(const char *why) {
    if (!s_error) s_error = why;
    return false;
}

static bool read_at(SceUID fd, uint64_t at, void *buf, uint32_t n) {
    uint8_t *p = buf;
    if (sceIoLseek(fd, (SceOff) at, SCE_SEEK_SET) != (SceOff) at) return false;
    while (n) {
        int k = sceIoRead(fd, p, n > 0x100000 ? 0x100000 : n);
        if (k <= 0) return false;
        p += k;
        n -= (uint32_t) k;
    }
    return true;
}

static bool write_all(SceUID fd, const void *buf, uint32_t n) {
    const uint8_t *p = buf;
    while (n) {
        int k = sceIoWrite(fd, p, n > 0x100000 ? 0x100000 : n);
        if (k <= 0) return false;
        p += k;
        n -= (uint32_t) k;
    }
    return true;
}

typedef struct Reader {
    SceUID fd;
    uint8_t buf[0x10000];
    uint32_t pos, len;
    uint64_t done, total;
} Reader;

static bool rd_bytes(Reader *r, uint8_t *out, uint32_t n);

static bool rd_byte(Reader *r, uint8_t *out) { return rd_bytes(r, out, 1); }

static bool rd_bytes(Reader *r, uint8_t *out, uint32_t n) {
    while (n) {
        if (r->pos == r->len) {
            int k = sceIoRead(r->fd, r->buf, sizeof(r->buf));
            if (k <= 0) return false;
            r->pos = 0;
            r->len = (uint32_t) k;
        }
        uint32_t k = r->len - r->pos < n ? r->len - r->pos : n;
        memcpy(out, r->buf + r->pos, k);
        r->pos += k;
        r->done += k;
        out += k;
        n -= k;
    }
    return true;
}

static bool rd_varint(Reader *r, uint64_t *v) {
    uint8_t c;
    *v = 0;
    for (int i = 0; i < 10; i++) {
        if (!rd_byte(r, &c)) return false;
        *v = *v << 7 | (c & 0x7f);
        if (!(c & 0x80)) return true;
    }
    return false;
}

static bool mem_varint(const uint8_t *b, uint32_t len, uint32_t *p, uint32_t *v) {
    uint64_t x = 0;
    for (int i = 0; i < 6; i++) {
        if (*p >= len) return false;
        uint8_t c = b[(*p)++];
        x = x << 7 | (c & 0x7f);
        if (!(c & 0x80)) {
            if (x > 0xffffffffu) return false;
            *v = (uint32_t) x;
            return true;
        }
    }
    return false;
}

static uint8_t s_type[2][256], s_size[2][256], s_mode[2][256];

static void code_set(int code, int t1, int s1, int m1, int t2, int s2, int m2) {
    s_type[0][code] = (uint8_t) t1, s_size[0][code] = (uint8_t) s1, s_mode[0][code] = (uint8_t) m1;
    s_type[1][code] = (uint8_t) t2, s_size[1][code] = (uint8_t) s2, s_mode[1][code] = (uint8_t) m2;
}

static void code_table(void) {
    int i = 0;
    code_set(i++, 3, 0, 0, 0, 0, 0);
    for (int sz = 0; sz < 18; sz++) code_set(i++, 1, sz, 0, 0, 0, 0);
    for (int m = 0; m < 9; m++) {
        code_set(i++, 2, 0, m, 0, 0, 0);
        for (int sz = 4; sz <= 18; sz++) code_set(i++, 2, sz, m, 0, 0, 0);
    }
    for (int m = 0; m < 6; m++) {
        for (int asz = 1; asz <= 4; asz++) {
            for (int csz = 4; csz <= 6; csz++) code_set(i++, 1, asz, 0, 2, csz, m);
        }
    }
    for (int m = 6; m < 9; m++) {
        for (int asz = 1; asz <= 4; asz++) code_set(i++, 1, asz, 0, 2, 4, m);
    }
    for (int m = 0; m < 9; m++) code_set(i++, 2, 4, m, 1, 1, 0);
}

static struct xz_dec *s_xz[3];

static uint8_t *section(Reader *r, int kind, uint32_t len, bool compressed, int secondary, uint32_t *out_len) {
    uint8_t *raw = malloc(len ? len : 1);
    if (!raw) return fail("Not enough memory for the patch"), NULL;
    if (!rd_bytes(r, raw, len)) {
        free(raw);
        return fail("The patch is incomplete"), NULL;
    }
    *out_len = len;
    if (!compressed) return raw;
    if (secondary != 2) {
        free(raw);
        return fail("This patch uses a compression that is not supported"), NULL;
    }
    uint32_t p = 0, size = 0;
    uint8_t *out = NULL;
    if (mem_varint(raw, len, &p, &size) && (out = malloc(size ? size : 1))) {
        if (!s_xz[kind]) s_xz[kind] = xz_dec_init(XZ_DYNALLOC, 1u << 26);
        struct xz_buf b = {raw + p, 0, len - p, out, 0, size};
        enum xz_ret ret = s_xz[kind] ? XZ_OK : XZ_MEM_ERROR;
        while (ret == XZ_OK && (b.in_pos < b.in_size || b.out_pos < b.out_size)) {
            size_t in_before = b.in_pos, out_before = b.out_pos;
            ret = xz_dec_run(s_xz[kind], &b);
            if (b.in_pos == in_before && b.out_pos == out_before) break;
        }
        if ((ret != XZ_STREAM_END && ret != XZ_OK) || b.out_pos != size) {
            free(out);
            out = NULL;
        }
    }
    free(raw);
    if (!out) return fail("The patch is damaged"), NULL;
    *out_len = size;
    return out;
}

static bool apply_patch(const char *patch, const char *target) {
    uint32_t base_size;
    const uint8_t *base = hapi_rom(&base_size);
    Reader *r = calloc(1, sizeof(Reader));
    if (!r) return fail("Not enough memory for the patch");
    r->fd = sceIoOpen(patch, SCE_O_RDONLY, 0);
    if (r->fd < 0) {
        free(r);
        return fail("The patch cannot be opened");
    }
    r->total = (uint64_t) sceIoLseek(r->fd, 0, SCE_SEEK_END);
    sceIoLseek(r->fd, 0, SCE_SEEK_SET);
    SceUID out = sceIoOpen(target, SCE_O_RDWR | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    bool ok = out >= 0;
    if (!ok) fail("The mod cannot be written to the memory card");
    uint8_t magic[5];
    int secondary = 0;
    if (ok && (!rd_bytes(r, magic, 5) || magic[0] != 0xd6 || magic[1] != 0xc3 || magic[2] != 0xc4)) {
        ok = fail("This is not an xdelta patch");
    }
    uint8_t c;
    uint64_t v;
    if (ok && (magic[4] & 1)) {
        ok = rd_byte(r, &c);
        secondary = c;
    }
    if (ok && (magic[4] & 2)) ok = fail("Patches with their own code table are not supported");
    if (ok && (magic[4] & 4)) {
        ok = rd_varint(r, &v);
        uint8_t skip[256];
        while (ok && v) {
            uint32_t k = v > sizeof(skip) ? sizeof(skip) : (uint32_t) v;
            ok = rd_bytes(r, skip, k);
            v -= k;
        }
    }
    code_table();
    xz_crc32_init();
    xz_crc64_init();
    uint8_t *t = NULL, *seg_buf = NULL;
    uint32_t t_cap = 0;
    uint64_t written = 0;
    while (ok) {
        uint8_t wi;
        if (!rd_byte(r, &wi)) break;
        uint64_t ssz = 0, spos = 0, tlen64, dlen, ilen, alen;
        uint8_t di;
        if (wi & 3) ok = rd_varint(r, &ssz) && rd_varint(r, &spos);
        ok = ok && rd_varint(r, &v) && rd_varint(r, &tlen64) && rd_byte(r, &di) && rd_varint(r, &dlen) &&
             rd_varint(r, &ilen) && rd_varint(r, &alen);
        if (ok && (wi & 4)) ok = rd_bytes(r, magic, 4);
        if (!ok || tlen64 > 0x4000000 || ssz > 0x10000000 || dlen > 0x4000000 || ilen > 0x4000000 ||
            alen > 0x4000000) {
            ok = fail("The patch is damaged");
            break;
        }
        uint32_t tlen = (uint32_t) tlen64, dl, il, al;
        uint8_t *data = section(r, 0, (uint32_t) dlen, di & 1, secondary, &dl);
        uint8_t *inst = data ? section(r, 1, (uint32_t) ilen, di & 2, secondary, &il) : NULL;
        uint8_t *addr = inst ? section(r, 2, (uint32_t) alen, di & 4, secondary, &al) : NULL;
        const uint8_t *seg = NULL;
        if (!addr) ok = false;
        else if ((wi & 1) && ssz) {
            if (spos + ssz > base_size) ok = fail("This patch is not for this ROM");
            else seg = base + spos;
        } else if ((wi & 2) && ssz) {
            free(seg_buf);
            seg_buf = spos + ssz <= written ? malloc((size_t) ssz) : NULL;
            if (!seg_buf || !read_at(out, spos, seg_buf, (uint32_t) ssz)) ok = fail("The patch is damaged");
            sceIoLseek(out, (SceOff) written, SCE_SEEK_SET);
            seg = seg_buf;
        }
        if (ok && t_cap < tlen) {
            free(t);
            t_cap = tlen;
            t = malloc(t_cap ? t_cap : 1);
            if (!t) ok = fail("Not enough memory for the patch");
        }
        uint32_t tp = 0, dp = 0, ip = 0, ap = 0, ni = 0, near[4] = {0}, same[768];
        memset(same, 0, sizeof(same));
        uint32_t here0 = (uint32_t) ssz;
        while (ok && ip < il) {
            int code = inst[ip++];
            for (int half = 0; half < 2 && ok; half++) {
                int typ = s_type[half][code], mode = s_mode[half][code];
                uint32_t size = s_size[half][code];
                if (!typ) continue;
                if (!size && !mem_varint(inst, il, &ip, &size)) ok = false;
                if (!ok || size > tlen - tp) {
                    ok = fail("The patch is damaged");
                    break;
                }
                if (typ == 1) {
                    if (size > dl - dp) ok = fail("The patch is damaged");
                    else memcpy(t + tp, data + dp, size);
                    dp += size, tp += size;
                } else if (typ == 3) {
                    if (dp >= dl) ok = fail("The patch is damaged");
                    else memset(t + tp, data[dp++], size);
                    tp += size;
                } else {
                    uint32_t here = here0 + tp, a = 0, x = 0;
                    if (mode < 6) {
                        if (!mem_varint(addr, al, &ap, &x)) ok = false;
                        a = mode == 0 ? x : mode == 1 ? here - x : near[mode - 2] + x;
                    } else {
                        if (ap >= al) ok = false;
                        else a = same[(mode - 6) * 256 + addr[ap++]];
                    }
                    if (!ok || a >= here) {
                        ok = fail("The patch is damaged");
                        break;
                    }
                    near[ni] = a;
                    ni = (ni + 1) & 3;
                    same[a % 768] = a;
                    for (uint32_t k = 0; k < size; k++) {
                        uint32_t q = a + k;
                        t[tp++] = q < here0 ? seg[q] : t[q - here0];
                    }
                }
            }
        }
        free(data);
        free(inst);
        free(addr);
        if (ok && tp != tlen) ok = fail("The patch is damaged");
        if (ok && !write_all(out, t, tlen)) ok = fail("The memory card is full");
        written += tlen;
        if (r->total) progress("Applying the patch", (int) (r->done * 100 / r->total));
    }
    free(t);
    free(seg_buf);
    for (int i = 0; i < 3; i++) {
        if (s_xz[i]) xz_dec_end(s_xz[i]);
        s_xz[i] = NULL;
    }
    sceIoClose(r->fd);
    free(r);
    if (out >= 0) sceIoClose(out);
    if (ok && written == 0) ok = fail("The patch is damaged");
    return ok;
}

typedef struct FsEntry {
    char path[96];
    uint16_t id;
} FsEntry;

typedef struct Fs {
    FsEntry *files;
    int count, cap;
    const uint8_t *fnt;
    uint32_t fnt_size;
} Fs;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t) (p[0] | p[1] << 8); }

static void fs_walk(Fs *fs, int dir, const char *prefix, int depth) {
    uint32_t entry = (uint32_t) (dir & 0xfff) * 8;
    if (depth > 16 || entry + 8 > fs->fnt_size) return;
    uint32_t p = le32(fs->fnt + entry);
    uint16_t id = le16(fs->fnt + entry + 4);
    while (p < fs->fnt_size) {
        int len = fs->fnt[p++];
        if (!len) break;
        int n = len & 0x7f;
        if (p + n > fs->fnt_size) break;
        char path[96];
        snprintf(path, sizeof(path), "%s%.*s", prefix, n, (const char *) fs->fnt + p);
        p += n;
        if (len & 0x80) {
            if (p + 2 > fs->fnt_size) break;
            int sub = le16(fs->fnt + p);
            p += 2;
            char sub_prefix[96];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s/", path);
            fs_walk(fs, sub, sub_prefix, depth + 1);
        } else {
            if (fs->count == fs->cap) {
                fs->cap = fs->cap ? fs->cap * 2 : 1024;
                FsEntry *f = realloc(fs->files, sizeof(FsEntry) * fs->cap);
                if (!f) return;
                fs->files = f;
            }
            snprintf(fs->files[fs->count].path, sizeof(fs->files[0].path), "%s", path);
            fs->files[fs->count++].id = id++;
        }
    }
}

static int fs_cmp(const void *a, const void *b) { return strcmp(((const FsEntry *) a)->path, ((const FsEntry *) b)->path); }

static void fs_load(Fs *fs, const uint8_t *fnt, uint32_t size) {
    memset(fs, 0, sizeof(*fs));
    fs->fnt = fnt;
    fs->fnt_size = size;
    fs_walk(fs, 0xf000, "", 0);
    if (fs->count) qsort(fs->files, (size_t) fs->count, sizeof(FsEntry), fs_cmp);
}

typedef struct Change {
    uint16_t id;
    uint32_t start, size;
} Change;

static bool merge(const char *mod_path, const char *out_path) {
    uint32_t base_size;
    const uint8_t *base = hapi_rom(&base_size);
    SceUID fd = sceIoOpen(mod_path, SCE_O_RDONLY, 0);
    if (fd < 0) return fail("The mod cannot be opened");
    uint64_t mod_size = (uint64_t) sceIoLseek(fd, 0, SCE_SEEK_END);
    uint8_t head[0x200];
    bool ok = mod_size > 0x200 && read_at(fd, 0, head, sizeof(head));
    if (!ok || memcmp(head + 0x0c, "AMCP", 4) != 0) {
        sceIoClose(fd);
        return fail(ok ? "This mod is not for the European game" : "The mod is not a DS game");
    }
    uint32_t fnt_off = le32(head + 0x40), fnt_size = le32(head + 0x44), fat_off = le32(head + 0x48),
             fat_size = le32(head + 0x4c);
    uint32_t bfnt = le32(base + 0x40), bfnt_size = le32(base + 0x44), bfat = le32(base + 0x48);
    uint8_t *fnt = fnt_size < 0x100000 ? malloc(fnt_size) : NULL;
    uint8_t *fat = fat_size < 0x100000 ? malloc(fat_size) : NULL;
    if (!fnt || !fat || !read_at(fd, fnt_off, fnt, fnt_size) || !read_at(fd, fat_off, fat, fat_size)) {
        free(fnt);
        free(fat);
        sceIoClose(fd);
        return fail("The mod is damaged");
    }
    bool code = le32(head + 0x2c) != le32(base + 0x2c) || le32(head + 0x54) != le32(base + 0x54);
    uint32_t arm9 = le32(base + 0x2c);
    uint8_t *chunk = malloc(0x10000);
    for (uint32_t done = 0; !code && chunk && done < arm9; done += 0x10000) {
        uint32_t n = arm9 - done < 0x10000 ? arm9 - done : 0x10000;
        if (le32(base + 0x20) + done + n > base_size || !read_at(fd, le32(head + 0x20) + done, chunk, n) ||
            memcmp(chunk, base + le32(base + 0x20) + done, n) != 0) {
            code = true;
        }
    }
    Fs b, m;
    fs_load(&b, base + bfnt, bfnt_size);
    fs_load(&m, fnt, fnt_size);
    Change *changes = calloc((size_t) (b.count ? b.count : 1), sizeof(Change));
    int count = 0;
    uint64_t extra = 0;
    for (int i = 0; ok && chunk && changes && i < b.count; i++) {
        progress("Reading the mod", i * 100 / b.count);
        FsEntry key;
        strcpy(key.path, b.files[i].path);
        FsEntry *hit = bsearch(&key, m.files, (size_t) m.count, sizeof(FsEntry), fs_cmp);
        if (!hit || (uint32_t) hit->id * 8 + 8 > fat_size) continue;
        size_t plen = strlen(key.path);
        if (code && plen > 5 && strcasecmp(key.path + plen - 5, ".sdat") == 0) continue;
        uint32_t ms = le32(fat + hit->id * 8), me = le32(fat + hit->id * 8 + 4);
        uint32_t bs = le32(base + bfat + b.files[i].id * 8), be = le32(base + bfat + b.files[i].id * 8 + 4);
        if (me < ms || me > mod_size || be < bs || be > base_size) continue;
        bool same = me - ms == be - bs;
        for (uint32_t done = 0; same && done < me - ms; done += 0x10000) {
            uint32_t n = me - ms - done < 0x10000 ? me - ms - done : 0x10000;
            if (!read_at(fd, ms + done, chunk, n)) ok = fail("The mod cannot be read");
            same = ok && memcmp(chunk, base + bs + done, n) == 0;
        }
        if (!ok || same) continue;
        changes[count].id = b.files[i].id;
        changes[count].start = ms;
        changes[count].size = me - ms;
        count++;
        extra += ((me - ms) + 0x1ff) & ~0x1ffu;
    }
    free(chunk);
    free(m.files);
    free(b.files);
    free(fnt);
    uint64_t total = ((base_size + 0x1ffu) & ~0x1ffu) + extra;
    uint8_t *rom = ok && changes && total < 0x10000000 ? malloc((size_t) total) : NULL;
    if (ok && !rom) ok = fail("Not enough memory for this mod");
    if (ok) {
        memset(rom, 0, (size_t) total);
        memcpy(rom, base, base_size);
        uint32_t end = (base_size + 0x1ffu) & ~0x1ffu;
        for (int i = 0; ok && i < count; i++) {
            progress("Building the game", i * 100 / (count ? count : 1));
            if (!read_at(fd, changes[i].start, rom + end, changes[i].size)) ok = fail("The mod cannot be read");
            uint8_t *e = rom + bfat + changes[i].id * 8;
            uint32_t stop = end + changes[i].size;
            e[0] = (uint8_t) end, e[1] = (uint8_t) (end >> 8), e[2] = (uint8_t) (end >> 16), e[3] = (uint8_t) (end >> 24);
            e[4] = (uint8_t) stop, e[5] = (uint8_t) (stop >> 8), e[6] = (uint8_t) (stop >> 16), e[7] = (uint8_t) (stop >> 24);
            end = (stop + 0x1ffu) & ~0x1ffu;
        }
    }
    free(fat);
    free(changes);
    sceIoClose(fd);
    if (!ok) {
        free(rom);
        return false;
    }
    port_log("Mod: %d files of the game changed%s", count, code ? ", its own code and sounds are not used" : "");
    progress("Saving", 100);
    SceUID out = sceIoOpen(out_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    ok = out >= 0 && write_all(out, rom, (uint32_t) total);
    if (out >= 0) sceIoClose(out);
    free(rom);
    if (!ok) {
        sceIoRemove(out_path);
        return fail("The memory card is full");
    }
    return true;
}

static bool install(const char *file) {
    char src[256], dir[256], rom[256], tmp[256], patched[256], name[VITA_MOD_NAME];
    int ext = mod_ext(file);
    size_t len = strlen(file) - strlen(s_exts[ext]);
    memcpy(name, file, len);
    name[len] = 0;
    snprintf(src, sizeof(src), "%s/%s", VITA_MODS_DIR, file);
    snprintf(dir, sizeof(dir), "%s/%s", VITA_MODS_DIR, name);
    snprintf(rom, sizeof(rom), "%s/game.nds", dir);
    snprintf(tmp, sizeof(tmp), "%s/game.tmp", dir);
    snprintf(patched, sizeof(patched), "%s/patched.tmp", dir);
    s_error = NULL;
    s_shown_percent = -1;
    snprintf(s_title, sizeof(s_title), "Adding %s", name);
    sceIoMkdir(dir, 0777);
    bool ok;
    if (ext != 3) {
        progress("Applying the patch", 0);
        ok = apply_patch(src, patched) && merge(patched, tmp);
        sceIoRemove(patched);
    } else {
        progress("Reading the mod", 0);
        ok = merge(src, tmp);
    }
    if (ok) {
        sceIoRemove(rom);
        ok = sceIoRename(tmp, rom) >= 0;
        if (!ok) fail("The mod cannot be written to the memory card");
    }
    if (ok) {
        sceIoRemove(src);
        port_log("Mod %s added", name);
        return true;
    }
    sceIoRemove(tmp);
    sceIoRmdir(dir);
    char failed[300];
    snprintf(failed, sizeof(failed), "%s.failed", src);
    sceIoRename(src, failed);
    port_log("Mod %s: %s", name, s_error ? s_error : "it could not be added");
    screen_show(s_error ? s_error : "The mod could not be added", -1, true);
    sceKernelDelayThread(5000000);
    return false;
}

void vita_mods_install(void) {
    char files[16][VITA_MOD_NAME + 8];
    int count = loose_files(files, 16);
    for (int i = 0; i < count; i++) install(files[i]);
    screen_close();
}
