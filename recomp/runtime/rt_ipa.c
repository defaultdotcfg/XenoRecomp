/* Extracts Payload/<Name>.app/** from an .ipa (zip) into <dest>\<Name>.app. Used by "--game <file.ipa>" / "--prepare" (RecompLauncher support).
 * Plain zip only (stored + deflate, no zip64): the game's IPA is ~100 MB. Prints "progress <0..1> <text>" lines to stdout. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <direct.h>

int rt_inflate_raw(const uint8_t *in, size_t n, uint8_t *out, size_t outn);   /* rt_zlib.c */

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void mkdirs(char *path) {
    for (char *s = path + 1; *s; s++) if (*s == '\\') { *s = 0; _mkdir(path); *s = '\\'; }
}

typedef struct { uint32_t method, csize, usize, off; char name[400]; } Ent;

/* returns 0 on success; app_out receives the full path of the extracted .app folder */
int rt_ipa_extract(const char *ipa, const char *dest, char *app_out, size_t cap) {
    FILE *f = fopen(ipa, "rb"); if (!f) { fprintf(stderr, "cannot open %s\n", ipa); return 1; }
    _fseeki64(f, 0, SEEK_END); long long fsz = _ftelli64(f);
    long long tail = fsz < 70000 ? fsz : 70000; uint8_t *tb = malloc((size_t)tail);
    _fseeki64(f, fsz - tail, SEEK_SET); if (fread(tb, 1, (size_t)tail, f) != (size_t)tail) { fclose(f); free(tb); return 1; }
    long e = -1; for (long i = (long)tail - 22; i >= 0; i--) if (rd32(tb + i) == 0x06054b50) { e = i; break; }
    if (e < 0) { fprintf(stderr, "not a zip file\n"); fclose(f); free(tb); return 2; }
    uint32_t nent = rd16(tb + e + 10), cdsz = rd32(tb + e + 12), cdoff = rd32(tb + e + 16); free(tb);
    if (nent == 0xFFFF || cdoff == 0xFFFFFFFFu) { fprintf(stderr, "zip64 not supported\n"); fclose(f); return 2; }
    uint8_t *cd = malloc(cdsz); _fseeki64(f, cdoff, SEEK_SET); if (fread(cd, 1, cdsz, f) != cdsz) { fclose(f); free(cd); return 1; }

    Ent *ents = calloc(nent, sizeof(Ent)); int ne = 0; char prefix[400] = ""; size_t pl = 0;
    uint32_t p = 0;
    for (uint32_t i = 0; i < nent && p + 46 <= cdsz && rd32(cd + p) == 0x02014b50; i++) {
        uint32_t nl = rd16(cd + p + 28), xl = rd16(cd + p + 30), cl = rd16(cd + p + 32);
        Ent *en = &ents[ne]; en->method = rd16(cd + p + 10); en->csize = rd32(cd + p + 20); en->usize = rd32(cd + p + 24); en->off = rd32(cd + p + 42);
        if (nl < sizeof en->name) { memcpy(en->name, cd + p + 46, nl); en->name[nl] = 0; ne++; }
        p += 46 + nl + xl + cl;
    }
    free(cd);
    /* the .app: first Payload/<X>.app/ that holds an Info.plist */
    for (int i = 0; i < ne && !pl; i++) {
        const char *n = ents[i].name; if (_strnicmp(n, "Payload/", 8) != 0) continue;
        const char *q = strchr(n + 8, '/'); if (!q || _stricmp(q + 1, "Info.plist") != 0 || q - n < 13 || _strnicmp(q - 4, ".app", 4) != 0) continue;
        pl = (size_t)(q - n) + 1; memcpy(prefix, n, pl); prefix[pl] = 0;
    }
    if (!pl) { fprintf(stderr, "no Payload/*.app found in the ipa\n"); fclose(f); free(ents); return 3; }
    char appname[260]; { size_t k = pl - 1; memcpy(appname, prefix + 8, k - 8); appname[k - 8] = 0; }
    char base[MAX_PATH]; snprintf(base, sizeof base, "%s\\%s", dest, appname);
    char root[MAX_PATH]; GetFullPathNameA(base, MAX_PATH, root, NULL); size_t rl = strlen(root);
    int total = 0, done = 0; for (int i = 0; i < ne; i++) if (strncmp(ents[i].name, prefix, pl) == 0 && ents[i].name[strlen(ents[i].name) - 1] != '/') total++;
    printf("progress 0 Extracting the game from the IPA\n"); fflush(stdout);
    int rc = 0;
    for (int i = 0; i < ne && !rc; i++) {
        Ent *en = &ents[i]; size_t nl = strlen(en->name);
        if (strncmp(en->name, prefix, pl) != 0 || en->name[nl - 1] == '/') continue;
        char rel[400]; strcpy(rel, en->name + pl); for (char *s = rel; *s; s++) if (*s == '/') *s = '\\';
        char tgt[MAX_PATH * 2], full[MAX_PATH]; snprintf(tgt, sizeof tgt, "%s\\%s", root, rel); GetFullPathNameA(tgt, MAX_PATH, full, NULL);
        if (_strnicmp(full, root, rl) != 0 || full[rl] != '\\') continue;                 /* never write outside the game folder */
        uint8_t lh[30]; _fseeki64(f, en->off, SEEK_SET);
        if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) { rc = 4; break; }
        _fseeki64(f, rd16(lh + 26) + rd16(lh + 28), SEEK_CUR);
        uint8_t *cb = malloc(en->csize ? en->csize : 1), *ub = malloc(en->usize ? en->usize : 1);
        if (fread(cb, 1, en->csize, f) != en->csize) rc = 4;
        else if (en->method == 0) { if (en->csize != en->usize) rc = 4; else memcpy(ub, cb, en->usize); }
        else if (en->method == 8) { if (rt_inflate_raw(cb, en->csize, ub, en->usize)) rc = 5; }
        else rc = 6;
        if (!rc) {
            char dir[MAX_PATH]; strcpy(dir, full); char *sl = strrchr(dir, '\\'); if (sl) { *sl = 0; strcat(dir, "\\x"); mkdirs(dir); }
            FILE *o = fopen(full, "wb"); if (!o) rc = 7; else { fwrite(ub, 1, en->usize, o); fclose(o); }
        }
        free(cb); free(ub);
        done++; if ((done & 15) == 0 || done == total) { printf("progress %.2f Extracting the game (%d of %d files)\n", (double)done / (total ? total : 1), done, total); fflush(stdout); }
    }
    fclose(f); free(ents);
    if (rc) { fprintf(stderr, "extraction failed (code %d)\n", rc); return 10 + rc; }
    snprintf(app_out, cap, "%s", root);
    return 0;
}
