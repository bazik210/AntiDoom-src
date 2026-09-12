/* Optional hints for the bundled Doom II maps. Match raw geometry, not just
   map numbers/counts: replacement maps and Doom I must use generic planning.
   Loaded savegame movers do not alter these raw lumps. */
#ifndef P_BOT_ROUTES_H
#define P_BOT_ROUTES_H
typedef struct { int map; unsigned int geometry; } bot_route_id_t;
static const bot_route_id_t bot_routes[] = {
    {2, 0x93ead084u}, {3, 0x97516f45u}, {4, 0x4c27f840u}, {5, 0xd6dd2008u}
};
typedef struct {
    int map;
    unsigned int geometry;
    signed char keys[3];
    unsigned char flags;
} bot_campaign_hint_t;
enum { BOT_HINT_MOMENTUM = 1, BOT_HINT_CLEAR = 2, BOT_HINT_ICON = 4 };

/* Coarse milestones from the Doom II walkthrough. Fine routing remains
   geometry-driven: these hints choose key order and identify maps where an
   unreachable milestone should trigger run-up discovery. */
static const bot_campaign_hint_t bot_campaign_hints[] = {
    { 6,0xed3e3be1u,{it_bluecard,it_redcard,it_yellowcard},BOT_HINT_MOMENTUM},
    { 7,0xb397bf8eu,{-1,-1,-1},BOT_HINT_CLEAR},
    { 8,0xc257fcf8u,{it_yellowcard,it_redcard,-1},0},
    { 9,0xa0e216a2u,{it_bluecard,it_yellowcard,-1},BOT_HINT_MOMENTUM},
    {10,0x00136599u,{it_bluecard,it_yellowcard,-1},0},
    {11,0xfdea8f06u,{it_bluecard,it_redcard,-1},BOT_HINT_MOMENTUM},
    {12,0x537c22d4u,{it_bluecard,it_yellowcard,-1},0},
    {13,0xf8a37112u,{it_bluecard,it_redcard,it_yellowcard},BOT_HINT_MOMENTUM},
    {14,0xcb4c99bdu,{it_redcard,it_bluecard,-1},BOT_HINT_MOMENTUM},
    {15,0xd1d6e088u,{it_yellowcard,it_bluecard,-1},BOT_HINT_MOMENTUM},
    {16,0x4b8ea82au,{it_bluecard,it_redcard,-1},0},
    {17,0xb1638345u,{it_redcard,it_bluecard,it_yellowcard},BOT_HINT_MOMENTUM},
    {18,0x13d1826fu,{it_yellowcard,it_bluecard,-1},0},
    {19,0xddb9ed8fu,{it_yellowcard,it_redcard,-1},BOT_HINT_MOMENTUM},
    {20,0x0c351566u,{-1,-1,-1},BOT_HINT_MOMENTUM},
    {21,0x66eaa0c8u,{it_yellowcard,it_redcard,it_bluecard},BOT_HINT_MOMENTUM},
    {22,0x76b51ea6u,{it_bluecard,it_redcard,-1},BOT_HINT_MOMENTUM},
    {23,0xb66dcfbau,{it_yellowcard,-1,-1},0},
    {24,0x4fbc158fu,{it_bluecard,it_redcard,-1},BOT_HINT_MOMENTUM},
    {25,0x74ed0cc4u,{it_bluecard,-1,-1},BOT_HINT_MOMENTUM},
    {26,0xd1b6cd8du,{it_redcard,it_bluecard,it_yellowcard},0},
    {27,0x6235c0eeu,{it_yellowcard,it_bluecard,it_redcard},0},
    {28,0xfe4e62abu,{it_yellowcard,it_redcard,-1},BOT_HINT_MOMENTUM},
    {29,0x296214c6u,{-1,-1,-1},BOT_HINT_MOMENTUM},
    {30,0x0c6dbb9bu,{-1,-1,-1},BOT_HINT_MOMENTUM|BOT_HINT_ICON},
    {31,0x7a43428au,{-1,-1,-1},0},
    {32,0x8edcaccdu,{-1,-1,-1},BOT_HINT_CLEAR}
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

static const bot_campaign_hint_t *Bot_FindCampaignHint(void)
{
    int i;
    for (i=0; i<(int)(sizeof(bot_campaign_hints)/sizeof(bot_campaign_hints[0])); ++i) {
        bot_route_id_t route;
        route.map=bot_campaign_hints[i].map;
        route.geometry=bot_campaign_hints[i].geometry;
        if (Bot_MatchesRoute(&route)) return &bot_campaign_hints[i];
    }
    return NULL;
}
#endif
