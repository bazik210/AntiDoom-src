/* Links against the Win32 engine with -Dmain=antidoom_main. No WAD or window. */
#undef main
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doomstat.h"
#include "r_local.h"
#include "v_video.h"

extern int lookdir, mlook, uncapped_fps, detailLevel, smooth_scaling, screenblocks;
extern fixed_t planeheight;
extern lighttable_t **planezlight;
extern boolean setsizeneeded;
void R_SetupFrame(player_t *player);
void R_ExecuteSetViewSize(void);
void M_ChangeDetail(int choice);

static byte flat[64 * 64];
static byte lightmaps[NUMCOLORMAPS * 256];
static lighttable_t *plane_lights[MAXLIGHTZ];
static byte reference[MAX_SCREENWIDTH * SCREENHEIGHT];
static mobj_t camera;
static int failures, checks;

static void render_planes(void)
{
    int y;
    memset(screens[0], 0, SCREENWIDTH * SCREENHEIGHT);
    R_SetupFrame(&players[0]);
    R_ClearPlanes();
    ds_source = flat;
    planezlight = plane_lights;
    for (y = 0; y < viewheight; ++y) {
        /* Avoid the horizon; draw a ceiling and a floor at different heights. */
        if (abs(y - centery) < 12) continue;
        planeheight = (y < centery ? 86 : 42) * FRACUNIT;
        R_MapPlane(y, 0, viewwidth - 1);
    }
}

static void check_same_frame(const char *label)
{
    ++checks;
    render_planes();
    if (memcmp(reference, screens[0], SCREENWIDTH * SCREENHEIGHT)) {
        ++failures;
        fprintf(stderr, "FAIL %s: width=%d blocks=%d lookdir=%d detail=%d smooth=%d\n",
                label, SCREENWIDTH, screenblocks, lookdir, detailLevel, smooth_scaling);
    }
}

int main(void)
{
    const int widths[] = {640, 856, 1120};
    const int pitches[] = {0, 40, -60, 100, -110};
    int w, b, p, cycle, i;
    screens[0] = malloc(sizeof(reference));
    if (!screens[0]) return 2;
    for (i = 0; i < sizeof(flat); ++i)
        flat[i] = ((i & 63) * 17 + (i >> 6) * 31) & 255;
    for (i = 0; i < sizeof(lightmaps); ++i) lightmaps[i] = i & 255;
    for (i = 0; i < MAXLIGHTZ; ++i) plane_lights[i] = lightmaps;
    colormaps = lightmaps;
    players[0].mo = &camera;
    players[0].viewz = 42 * FRACUNIT;
    camera.x = 123 * FRACUNIT;
    camera.y = -37 * FRACUNIT;
    camera.angle = ANG45;
    mlook = 1;
    uncapped_fps = 0;

    for (w = 0; w < sizeof(widths) / sizeof(widths[0]); ++w) {
        SCREENWIDTH = widths[w];
        for (b = 3; b <= 11; ++b) {
            screenblocks = b;
            for (p = 0; p < sizeof(pitches) / sizeof(pitches[0]); ++p) {
                detailLevel = smooth_scaling = 0;
                lookdir = pitches[p];
                R_SetViewSize(b, detailLevel);
                R_ExecuteSetViewSize();
                /* Move once to establish a correct pitched reference frame. */
                ++lookdir;
                render_planes();
                --lookdir;
                render_planes();
                memcpy(reference, screens[0], SCREENWIDTH * SCREENHEIGHT);
                for (cycle = 0; cycle < 6; ++cycle) {
                    M_ChangeDetail(1);
                    if (setsizeneeded) R_ExecuteSetViewSize();
                    check_same_frame("detail switch");
                    check_same_frame("stationary next frame");
                }
                /* Resize away and back, compensating pitch to keep the same
                   screen horizon; the projection and row count still change. */
                i = centery;
                R_SetViewSize(b == 11 ? 10 : b + 1, detailLevel);
                R_ExecuteSetViewSize();
                lookdir = i - viewheight / 2;
                render_planes();
                R_SetViewSize(b, detailLevel);
                R_ExecuteSetViewSize();
                lookdir = pitches[p];
                check_same_frame("resize round trip");
            }
        }
    }
    free(screens[0]);
    printf("Plane detail regression: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
