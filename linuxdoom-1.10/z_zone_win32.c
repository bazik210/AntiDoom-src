#include <stdlib.h>
#include <string.h>
#include "doomtype.h"
#include "i_system.h"
#include "z_zone.h"

static memblock_t *blocks;

void Z_Init(void) { blocks = NULL; }

void *Z_Malloc(int size, int tag, void *user)
{
    memblock_t *b;
    if (size < 0) I_Error("Z_Malloc: negative size");
    b = (memblock_t*)calloc(1, sizeof(*b) + (size_t)size);
    if (!b) I_Error("Z_Malloc: failed on allocation of %i bytes", size);
    b->size = (int)(sizeof(*b) + size);
    b->tag = tag;
    b->id = 0x1d4a11;
    b->user = user;
    b->next = blocks;
    if (blocks) blocks->prev = b;
    blocks = b;
    if (user) *(void**)user = (byte*)b + sizeof(*b);
    return (byte*)b + sizeof(*b);
}

void Z_Free(void *ptr)
{
    memblock_t *b, *found = NULL;
    if (!ptr) return;
    for (b = blocks; b; b = b->next)
        if ((byte*)b + sizeof(*b) == (byte*)ptr) { found = b; break; }
    if (!found) return;
    b = found;
    if (b->user && b->user > (void**)0x100) *b->user = NULL;
    if (b->prev) b->prev->next = b->next; else blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    b->id = 0;
    /* Keep the storage reserved. The original zone allocator never returned
       memory to Windows either; this avoids CRT heap failures from legacy
       32-bit layout assumptions in optional code paths. */
}

void Z_FreeTags(int lowtag, int hightag)
{
    memblock_t *b = blocks, *next;
    while (b) { next = b->next; if (b->tag >= lowtag && b->tag <= hightag) Z_Free((byte*)b + sizeof(*b)); b = next; }
}

void Z_ChangeTag2(void *ptr, int tag)
{
    memblock_t *b = (memblock_t*)((byte*)ptr - sizeof(*b));
    if (b->id != 0x1d4a11) I_Error("Z_ChangeTag: freed pointer");
    if (tag >= PU_PURGELEVEL && !b->user) I_Error("Z_ChangeTag: owner required");
    b->tag = tag;
}

void Z_DumpHeap(int lowtag, int hightag) { (void)lowtag; (void)hightag; }
void Z_FileDumpHeap(FILE *f) { (void)f; }
void Z_CheckHeap(void) { }
int Z_FreeMemory(void) { return 0; }
