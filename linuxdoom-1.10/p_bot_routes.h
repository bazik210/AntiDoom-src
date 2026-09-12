/* Optional hints for the bundled Doom II maps. Match raw geometry, not just
   map numbers/counts: replacement maps and Doom I must use generic planning.
   Loaded savegame movers do not alter these raw lumps. */
#ifndef P_BOT_ROUTES_H
#define P_BOT_ROUTES_H
typedef struct { int map; unsigned int geometry; } bot_route_id_t;
static const bot_route_id_t bot_routes[] = {
    {2, 0x93ead084u}, {3, 0x97516f45u}, {4, 0x4c27f840u}, {5, 0xd6dd2008u}
};
static boolean map02_route;
static boolean map05_route;
enum { map04_yellow_entry = 532, map04_yellow_switch = 553,
       map04_yellow_return = 562 };
/* MAP03: the blue key opens the left-hand door pair before the switch in the
   corridor. Keep this pair as one story gate; otherwise the two split door
   linedefs compete with the switch and the planner bounces between them. */
enum { map03_blue_door_left = 367, map03_blue_door_right = 368,
       map03_blue_button = 431 };
enum { map02_key_house_x = 1168, map02_key_house_y = 528 };

static boolean Bot_MatchesRoute(const bot_route_id_t *route)
{
    static const int offsets[] = {ML_LINEDEFS, ML_SIDEDEFS, ML_VERTEXES, ML_SECTORS};
    char name[9];
    unsigned int hash = 2166136261u;
    int base, k, j;
    if (gamemode != commercial || gamemap != route->map) return false;
    sprintf(name,"MAP%02d",route->map);
    base = W_CheckNumForName(name);
    if (base < 0) return false;
    for (k=0; k<4; ++k) {
        int size = W_LumpLength(base+offsets[k]);
        unsigned char *data = malloc(size);
        if (!data) I_Error("Bot: map identity allocation failed");
        W_ReadLump(base+offsets[k],data);
        for (j=0; j<size; ++j) hash=(hash^data[j])*16777619u;
        free(data);
    }
    return hash == route->geometry;
}
#endif
