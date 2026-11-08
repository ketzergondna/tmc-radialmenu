/*
 * Minish Cap (EU) - Radial item menu
 *
 * Tap L       -> original behaviour (kinstone fusion)
 * Hold L      -> game freezes, radial menu with every owned item (except the sword)
 * D-pad       -> pick an item
 * Release L   -> item is equipped on B (sword is kept on A)
 * B / START   -> close without changes
 *
 * The code lives in free space at the end of the ROM. The only change to the
 * original game is the GameMain_Update pointer in GameTask_Main's state table,
 * which now points to Radial_GameMainUpdate.
 */
#include "global.h"
#include "player.h"
#include "save.h"
#include "message.h"
#include "fade.h"
#include "common.h"
#include "screen.h"
#include "vram.h"
#include "affine.h"
#include "item.h"
#include "itemMetaData.h"
#include "sound.h"
#include "sprite.h"
#include "pauseMenu.h"
#include "entity.h"
#include "main.h"
#include "radial_gfx.h"

/* ---- game symbols (addresses supplied by the linker, from the decomp map) ---- */
extern void GameMain_Update_Orig(void);
extern void sub_080A5F48(u32 item, u32 tileOffset); /* loads an item icon (+ammo digits) into OBJ VRAM */
extern Frame* gSpriteAnimations_322[];
extern void DrawUIElements(void);

/* ---- tuning ---- */
#define HOLD_FRAMES 12     /* frames L must be held before the menu opens (~0.2 s) */
#define OPEN_FRAMES 6      /* expand animation */
#define CENTER_X 120
#define CENTER_Y 84
#define RADIUS 54
#define MAX_ENTRIES 12
#define ITEM_SPRITE 0x141  /* sprite index of the item icon frames (EU) */
#define STABLE_FRAMES 4    /* a direction must be held this long before it moves the cursor */
#define ICON_DY 5
#define DIM_LEVEL 9        /* 0..16 background darkening */

/* VRAM borrowed while the menu is open: 9 gfx slots = 144 tiles */
#define BORROW_SLOTS 9
#define BORROW_BYTES (BORROW_SLOTS * 0x200)
#define TILE_SOCKET 0
#define TILE_HUB 16
#define TILE_BADGE 32
#define TILE_CORNER 36
#define TILE_ITEMS 40

/* ---- state, in unused EWRAM past gEndOfEwram (cleared on boot) ---- */
typedef struct {
    u8 active;
    u8 lHeld;
    u8 inject;
    u8 count;
    u8 sel;
    u8 anim;
    u8 moved;
    u8 dirKeys;
    u8 dirFrames;
    u8 dirDone;
    u8 pad;
    u16 tileBase;
    u16 saveFx, saveAlpha, saveBright, saveWinIn, saveWinOut;
    u8 items[MAX_ENTRIES];
    u32 backup[BORROW_BYTES / 4];
    u16 snap[0x80 * 4]; /* OAM of the last game frame (attr0..2 + affine word) */
} RadialState;

#define R (*(RadialState*)0x0203E000)

/* 256-step sine, Q8 */
static const s16 sSin[64 + 1] = {
    0,   6,   13,  19,  25,  31,  38,  44,  50,  56,  62,  68,  74,  80,  86,  92,  98,
    104, 109, 115, 121, 126, 132, 137, 142, 147, 152, 157, 162, 167, 172, 177, 181, 185,
    190, 194, 198, 202, 206, 209, 213, 216, 220, 223, 226, 229, 231, 234, 237, 239, 241,
    243, 245, 247, 248, 250, 251, 252, 253, 254, 255, 255, 256, 256, 256,
};

static s32 Sin256(u32 a) {
    a &= 255;
    if (a < 64)
        return sSin[a];
    if (a < 128)
        return sSin[128 - a];
    if (a < 192)
        return -sSin[a - 128];
    return -sSin[256 - a];
}
#define Cos256(a) Sin256((a) + 64)

/* ---------------------------------------------------------------- items */

static u32 BottleToEquip(void) {
    u32 i, first = 0;
    /* keep the bottle already on B, if any */
    if (gSave.stats.equipped[SLOT_B] >= ITEM_BOTTLE1 && gSave.stats.equipped[SLOT_B] <= ITEM_BOTTLE4)
        return gSave.stats.equipped[SLOT_B];
    for (i = 0; i < 4; i++) {
        if (GetInventoryValue(ITEM_BOTTLE1 + i) == 1) {
            if (!first)
                first = ITEM_BOTTLE1 + i;
            if (gSave.stats.bottles[i] != ITEM_BOTTLE_EMPTY)
                return ITEM_BOTTLE1 + i;
        }
    }
    return first;
}

/* Same rules as the pause menu: the last owned item of each menu slot wins. */
static void BuildItemList(void) {
    static const u8 sOrder[] = {
        MENU_SLOT_GUST_JAR,   MENU_SLOT_CANE,      MENU_SLOT_BOOMERANG, MENU_SLOT_SHIELD,
        MENU_SLOT_MOLE_MITTS, MENU_SLOT_LANTERN,   MENU_SLOT_BOMBS,     MENU_SLOT_PEGASUS_BOOTS,
        MENU_SLOT_ROCS_CAPE,  MENU_SLOT_OCARINA,   MENU_SLOT_BOW,
    };
    u8 bySlot[MENU_SLOT_BOTTLE0];
    u32 item, i, bottle;

    for (i = 0; i < MENU_SLOT_BOTTLE0; i++)
        bySlot[i] = 0;
    for (item = ITEM_SMITH_SWORD; item < ITEM_BOTTLE1; item++) {
        if (GetInventoryValue(item) == 1) {
            u32 slot = gItemMetaData[item].menuSlot;
            if (slot < MENU_SLOT_BOTTLE0)
                bySlot[slot] = item;
        }
    }
    if (bySlot[MENU_SLOT_LANTERN])
        bySlot[MENU_SLOT_LANTERN] =
            IsItemEquipped(ITEM_LANTERN_ON) != EQUIP_SLOT_NONE ? ITEM_LANTERN_ON : ITEM_LANTERN_OFF;

    R.count = 0;
    for (i = 0; i < sizeof(sOrder); i++)
        if (bySlot[sOrder[i]])
            R.items[R.count++] = bySlot[sOrder[i]];
    bottle = BottleToEquip();
    if (bottle)
        R.items[R.count++] = bottle;
}

static u32 BestSword(void) {
    u32 item, best = 0;
    for (item = ITEM_SMITH_SWORD; item <= ITEM_FOURSWORD; item++)
        if (GetInventoryValue(item) == 1)
            best = item;
    return best;
}

static u32 ItemPalette(u32 item) {
    if (item >= ITEM_BOTTLE1 && item <= ITEM_BOTTLE4)
        item = gSave.stats.bottles[item - ITEM_BOTTLE1];
    switch (item) {
        case ITEM_BOMBS:
        case ITEM_REMOTE_BOMBS:
            return gBombBagSizes[gSave.stats.bombBagType] <= gSave.stats.bombCount ? 4 : 3;
        case ITEM_BOW:
        case ITEM_LIGHT_ARROW:
            return gQuiverSizes[gSave.stats.quiverType] <= gSave.stats.arrowCount ? 4 : 3;
    }
    return 3;
}

static u32 IconFrame(u32 item) {
    if (item >= ITEM_BOTTLE1 && item <= ITEM_BOTTLE4)
        item = gSave.stats.bottles[item - ITEM_BOTTLE1];
    return gSpriteAnimations_322[item]->index;
}

/* ---------------------------------------------------------------- open/close */

static bool32 CanOpen(void) {
    u32 fs;
    if (gFadeControl.active || gPauseMenuOptions.disabled || (gMessage.state & MESSAGE_ACTIVE) ||
        gSave.stats.health == 0 || !gSave.inventory[0] || gPlayerState.controlMode != 0 ||
        gPriorityHandler.priority_timer != 0)
        return FALSE;
    fs = gPlayerState.framestate == PL_STATE_IDLE ? gPlayerState.framestate_last : gPlayerState.framestate;
    switch (fs) {
        case PL_STATE_DIE:
        case PL_STATE_TALKEZLO:
        case PL_STATE_ITEMGET:
        case PL_STATE_DROWN:
        case PL_STATE_STAIRS:
            return FALSE;
    }
    return TRUE;
}

static void Copy32(const void* src, void* dst, u32 bytes) {
    const u32* s = src;
    u32* d = dst;
    for (bytes >>= 2; bytes; bytes--)
        *d++ = *s++;
}

static u32 PickSlots(void) {
    /* Prefer gfx slots no entity is using; otherwise borrow the top slots and hide entities. */
    u32 i, run = 0;
    for (i = 4; i < MAX_GFX_SLOTS; i++) {
        u32 st = gGFXSlots.slots[i].status;
        if (st == GFX_SLOT_FREE || st == GFX_SLOT_UNLOADED) {
            if (++run == BORROW_SLOTS)
                return i + 1 - BORROW_SLOTS;
        } else {
            run = 0;
        }
    }
    /* no free run: borrow the top slots; sprites using them are skipped while the menu is open */
    return MAX_GFX_SLOTS - BORROW_SLOTS;
}

static void Open(void) {
    u32 i, cur, slot;
    u8* vram;

    /* freeze-frame of everything the game drew last frame (entities, Link, HUD) */
    Copy32(gOAMControls.oam, R.snap, sizeof(R.snap));

    slot = PickSlots();
    R.tileBase = 0x140 + slot * 16;
    vram = (u8*)(OBJ_VRAM0 + R.tileBase * 0x20);
    Copy32(vram, R.backup, BORROW_BYTES); /* restored on close, so cached gfx stay intact */

    Copy32(sRadialGfx, vram, RADIAL_GFX_TILES * 0x20);
    for (i = 0; i < R.count; i++)
        sub_080A5F48(R.items[i], R.tileBase + TILE_ITEMS + i * 8);

    /* start on the item that is currently on B */
    cur = gSave.stats.equipped[SLOT_B];
    R.sel = 0;
    for (i = 0; i < R.count; i++)
        if (R.items[i] == cur)
            R.sel = i;

    /* darken everything except our (semi-transparent mode) sprites */
    R.saveFx = gScreen.controls.layerFXControl;
    R.saveAlpha = gScreen.controls.alphaBlend;
    R.saveBright = gScreen.controls.layerBrightness;
    R.saveWinIn = gScreen.controls.windowInsideControl;
    R.saveWinOut = gScreen.controls.windowOutsideControl;
    gScreen.controls.layerFXControl = 0x3F | 0xC0 | (0x2F << 8); /* all -> brightness down, 2nd target BGs+BD */
    gScreen.controls.alphaBlend = 16;                              /* our OBJs: 16/16 own colour, unblended */
    gScreen.controls.layerBrightness = DIM_LEVEL;
    gScreen.controls.windowInsideControl |= 0x2020;
    gScreen.controls.windowOutsideControl |= 0x2020;

    R.anim = 0;
    R.moved = 0;
    R.dirKeys = 0;
    R.dirFrames = 0;
    R.dirDone = 0;
    R.active = 1;
    SoundReq(SFX_TEXTBOX_OPEN);
}

static void Close(bool32 apply) {
    u32 sword;
    Copy32(R.backup, (u8*)(OBJ_VRAM0 + R.tileBase * 0x20), BORROW_BYTES);
    gScreen.controls.layerFXControl = R.saveFx;
    gScreen.controls.alphaBlend = R.saveAlpha;
    gScreen.controls.layerBrightness = R.saveBright;
    gScreen.controls.windowInsideControl = R.saveWinIn;
    gScreen.controls.windowOutsideControl = R.saveWinOut;
    R.active = 0;

    if (!apply) {
        SoundReq(SFX_MENU_CANCEL);
        return;
    }

    ForceEquipItem(R.items[R.sel], EQUIP_SLOT_B);
    sword = BestSword();
    if (sword && gSave.stats.equipped[SLOT_A] != sword)
        ForceEquipItem(sword, EQUIP_SLOT_A);
    SoundReq(SFX_TEXTBOX_SELECT);
}

/* ---------------------------------------------------------------- drawing */

static void PutObj(s32 x, s32 y, u32 shape, u32 size, u32 tile, u32 pal, u32 flip) {
    u16* o;
    if (gOAMControls.updated >= 0x80)
        return;
    o = (u16*)&gOAMControls.oam[gOAMControls.updated++];
    o[0] = (y & 0xFF) | 0x400 | (shape << 14);                     /* semi-transparent mode */
    o[1] = (x & 0x1FF) | (flip << 12) | (size << 14);
    o[2] = (R.tileBase + tile) | (pal << 12);                      /* priority 0 */
}

static void PutIcon(s32 x, s32 y, u32 i) {
    gOamCmd.x = x;
    gOamCmd.y = y + ICON_DY; /* item frames are anchored 13px above the 16x16 icon's top: re-centre */
    gOamCmd._4 = 0x400;
    gOamCmd._6 = 0;
    gOamCmd._8 = (ItemPalette(R.items[i]) << 12) | (R.tileBase + TILE_ITEMS + i * 8);
    DrawDirect(ITEM_SPRITE, IconFrame(R.items[i]));
}

static void SocketPos(u32 i, s32* x, s32* y) {
    s32 r = RADIUS * (R.anim + 1) / (OPEN_FRAMES + 1);
    u32 a = 192 + (256 * i) / R.count; /* start at the top, clockwise */
    *x = CENTER_X + ((Cos256(a) * r) >> 8);
    *y = CENTER_Y + ((Sin256(a) * r) >> 8);
}

static void Draw(void) {
    u32 i;
    s32 x, y, o;

    FlushSprites();

    /* cursor: four orange corners, pulsing like the pause menu */
    SocketPos(R.sel, &x, &y);
    o = (gMain.ticks & 0x10) ? 12 : 11;
    PutObj(x - o - 1, y - o - 1, 0, 0, TILE_CORNER, 0, 0);
    PutObj(x + o - 6, y - o - 1, 0, 0, TILE_CORNER, 0, 1);
    PutObj(x - o - 1, y + o - 6, 0, 0, TILE_CORNER, 0, 2);
    PutObj(x + o - 6, y + o - 6, 0, 0, TILE_CORNER, 0, 3);

    /* B badge + selected item in the hub */
    PutObj(CENTER_X + 6, CENTER_Y + 6, 0, 1, TILE_BADGE, 3, 0);
    PutIcon(CENTER_X, CENTER_Y, R.sel);
    PutObj(CENTER_X - 16, CENTER_Y - 16, 0, 2, TILE_HUB, 3, 0);

    for (i = 0; i < R.count; i++) {
        SocketPos(i, &x, &y);
        PutIcon(x, y, i);
        PutObj(x - 16, y - 16, 0, 2, TILE_SOCKET, 4, 0);
    }

    /* frozen game sprites underneath, minus any that used the borrowed tiles */
    for (i = 0; i < 0x80 && gOAMControls.updated < 0x80; i++) {
        u16* s = &R.snap[i * 4];
        u32 tile = s[2] & 0x3FF;
        if ((s[0] & 0x300) == 0x200) /* hidden */
            continue;
        if (tile >= R.tileBase && tile < R.tileBase + BORROW_SLOTS * 16)
            continue;
        o = gOAMControls.updated++;
        ((u16*)gOAMControls.oam)[o * 4 + 0] = s[0];
        ((u16*)gOAMControls.oam)[o * 4 + 1] = s[1];
        ((u16*)gOAMControls.oam)[o * 4 + 2] = s[2];
    }
    /* affine matrices live in the 4th halfword of each entry: keep them where they were */
    for (i = 0; i < 0x80; i++)
        ((u16*)gOAMControls.oam)[i * 4 + 3] = R.snap[i * 4 + 3];
    CopyOAM();
}

/* ---------------------------------------------------------------- input */

static void SetSel(u32 i) {
    R.moved = 1;
    if (i != R.sel) {
        R.sel = i;
        SoundReq(SFX_TEXTBOX_CHOICE);
    }
}

static void UpdateSelection(u32 held) {
    s32 dx = 0, dy = 0;
    u32 a, i, best = R.sel, bestDiff = 999;
    u32 dir = (held >> 4) & 0xF; /* d-pad bits */

    /* R: step clockwise (reaches every item even with an 8-way d-pad) */
    if (gInput.newKeys & R_BUTTON) {
        SetSel(R.sel + 1 >= R.count ? 0 : R.sel + 1);
        return;
    }

    /* debounce: when a diagonal is released one key lifts a few frames early,
       so only act on a direction that stayed the same for STABLE_FRAMES */
    if (dir != R.dirKeys) {
        R.dirKeys = dir;
        R.dirFrames = 0;
        R.dirDone = 0;
        return;
    }
    R.dirFrames++;
    if (R.dirFrames < STABLE_FRAMES)
        return;
    if (R.dirDone) {
        if (R.dirFrames < STABLE_FRAMES + 14)
            return; /* hold to keep moving around the ring */
        R.dirFrames = STABLE_FRAMES + 6;
    }
    R.dirDone = 1;

    if (held & DPAD_LEFT)
        dx = -1;
    if (held & DPAD_RIGHT)
        dx = 1;
    if (held & DPAD_UP)
        dy = -1;
    if (held & DPAD_DOWN)
        dy = 1;
    if (!dx && !dy)
        return;

    /* spatial navigation: from the selected socket, move to the socket that lies
       best in the pressed direction. With 8 directions and up to 12 sockets an
       absolute "point at it" scheme leaves items unreachable; stepping between
       neighbours reaches all of them. */
    {
        s32 sx, sy, bestScore = 0x7FFFFFFF;
        u32 saveAnim = R.anim;
        R.anim = OPEN_FRAMES;
        SocketPos(R.sel, &sx, &sy);
        for (i = 0; i < R.count; i++) {
            s32 x, y, vx, vy, dot, cross, score;
            if (i == R.sel)
                continue;
            SocketPos(i, &x, &y);
            vx = x - sx;
            vy = y - sy;
            dot = vx * dx + vy * dy;
            cross = vx * dy - vy * dx;
            if (cross < 0)
                cross = -cross;
            if (dot <= 0 || cross > dot * 2) /* outside ~63 degrees */
                continue;
            score = dot + cross * 2;
            if (score < bestScore) {
                bestScore = score;
                best = i;
            }
        }
        R.anim = saveAnim;
    }
    (void)a;
    (void)bestDiff;
    SetSel(best);
}

/* Returns TRUE when the radial menu consumed this frame. */
static bool32 Radial_Frame(void) {
    u32 held = gInput.heldKeys;

    if (R.active) {
        if (gInput.newKeys & (B_BUTTON | START_BUTTON)) {
            Close(FALSE);
        } else if (!(held & L_BUTTON)) {
            Close(R.moved); /* released without touching the d-pad: no change */
        } else {
            UpdateSelection(held);
            if (R.anim < OPEN_FRAMES)
                R.anim++;
            Draw();
            return TRUE;
        }
        /* swallow the closing press for this frame (only newKeys: heldKeys drives edge detection) */
        gInput.newKeys &= ~(L_BUTTON | B_BUTTON | START_BUTTON);
        return FALSE;
    }

    if (held & L_BUTTON) {
        if (R.lHeld || ((gInput.newKeys & L_BUTTON) && CanOpen())) {
            BuildItemList();
            if (R.count == 0) {
                R.lHeld = 0;
                return FALSE; /* nothing to choose: plain L */
            }
            gInput.heldKeys &= ~L_BUTTON;
            gInput.newKeys &= ~L_BUTTON;
            if (++R.lHeld >= HOLD_FRAMES && CanOpen()) {
                R.lHeld = 0;
                Open();
                Draw();
                return TRUE;
            }
        }
    } else {
        if (R.lHeld) {
            R.inject = 2; /* it was a tap: give the game its L press */
            R.lHeld = 0;
        }
    }

    if (R.inject) {
        gInput.heldKeys |= L_BUTTON;
        if (R.inject == 2)
            gInput.newKeys |= L_BUTTON;
        R.inject--;
    }
    return FALSE;
}

void Radial_GameMainUpdate(void) {
    if (Radial_Frame())
        return;
    GameMain_Update_Orig();
}
