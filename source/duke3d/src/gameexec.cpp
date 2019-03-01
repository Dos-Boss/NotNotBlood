//-------------------------------------------------------------------------
/*
Copyright (C) 2016 EDuke32 developers and contributors

This file is part of EDuke32.

EDuke32 is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License version 2
as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/
//-------------------------------------------------------------------------

#include "anim.h"
#include "cmdline.h"
#include "colmatch.h"
#include "compat.h"
#include "debugbreak.h"
#include "duke3d.h"
#include "input.h"
#include "menus.h"
#include "osdcmds.h"
#include "savegame.h"
#include "scriplib.h"

#ifdef LUNATIC
# include "lunatic_game.h"
#endif

#include "vfs.h"

#if KRANDDEBUG
# define GAMEEXEC_INLINE
# define GAMEEXEC_STATIC
#else
# define GAMEEXEC_INLINE inline
# define GAMEEXEC_STATIC static
#endif

vmstate_t vm;

#if !defined LUNATIC
int32_t g_tw;
int32_t g_errorLineNum;
int32_t g_currentEvent = -1;

intptr_t const *insptr;

int32_t g_returnVarID    = -1;  // var ID of "RETURN"
int32_t g_weaponVarID    = -1;  // var ID of "WEAPON"
int32_t g_worksLikeVarID = -1;  // var ID of "WORKSLIKE"
int32_t g_zRangeVarID    = -1;  // var ID of "ZRANGE"
int32_t g_angRangeVarID  = -1;  // var ID of "ANGRANGE"
int32_t g_aimAngleVarID  = -1;  // var ID of "AUTOAIMANGLE"
int32_t g_lotagVarID     = -1;  // var ID of "LOTAG"
int32_t g_hitagVarID     = -1;  // var ID of "HITAG"
int32_t g_textureVarID   = -1;  // var ID of "TEXTURE"
int32_t g_thisActorVarID = -1;  // var ID of "THISACTOR"
int32_t g_structVarIDs   = -1;

// for timing events and actors
uint32_t g_eventCalls[MAXEVENTS], g_actorCalls[MAXTILES];
double g_eventTotalMs[MAXEVENTS], g_actorTotalMs[MAXTILES], g_actorMinMs[MAXTILES], g_actorMaxMs[MAXTILES];

GAMEEXEC_STATIC void VM_Execute(native_t loop);

# include "gamestructures.cpp"
#endif

#if !defined LUNATIC
void VM_ScriptInfo(intptr_t const *ptr, int range)
{
    if (!apScript || !ptr || g_currentEvent == -1)
        return;

    initprintf("\n");

    for (auto pScript = max<intptr_t const *>(ptr - (range >> 1), apScript),
                p_end   = min<intptr_t const *>(ptr + (range >> 1), apScript + g_scriptSize);
            pScript < p_end;
            ++pScript)
    {
        initprintf("%5d: %3d: ", (int32_t)(pScript - apScript), (int32_t)(pScript - ptr));

        auto &v = *pScript;

        if (v >> 12 && (v & VM_INSTMASK) < CON_OPCODE_END)
            initprintf("%5d %s (%d)\n", (int32_t)(v >> 12), VM_GetKeywordForID(v & VM_INSTMASK), (int)v & VM_INSTMASK);
        else
            initprintf("%d\n", (int32_t)*pScript);
    }

    initprintf("\n");

    if (ptr == insptr)
    {
        if (vm.pUSprite)
            initprintf("current actor: %d (%d)\n", vm.spriteNum, vm.pUSprite->picnum);

        initprintf("g_errorLineNum: %d, g_tw: %d\n", g_errorLineNum, g_tw);
    }
}
#endif

static void VM_DeleteSprite(int const spriteNum, int const playerNum)
{
    if (EDUKE32_PREDICT_FALSE((unsigned) spriteNum >= MAXSPRITES))
        return;

    // if player was set to squish, first stop that...
    if (EDUKE32_PREDICT_FALSE(playerNum >= 0 && g_player[playerNum].ps->actorsqu == spriteNum))
        g_player[playerNum].ps->actorsqu = -1;

    A_DeleteSprite(spriteNum);
}

intptr_t apScriptEvents[MAXEVENTS];

// May recurse, e.g. through EVENT_XXX -> ... -> EVENT_KILLIT
#ifdef LUNATIC
static FORCE_INLINE int32_t VM_EventCommon__(int const &eventNum, int const &spriteNum, int const &playerNum, int const &playerDist, int32_t returnValue)
{
    const double t = timerGetHiTicks();
    int32_t ret = El_CallEvent(&g_ElState, eventNum, spriteNum, playerNum, playerDist, &returnValue);

    // NOTE: the run times are those of the called event plus any events
    // called by it, *not* "self" time.
    g_eventTotalMs[eventNum] += timerGetHiTicks()-t;
    g_eventCalls[eventNum]++;

    if (ret == 1)
        VM_DeleteSprite(spriteNum, playerNum);

    return returnValue;
}
#else
// do not inline
static void VM_DummySprite(void)
{
    static uspritetype dummy_sprite;
    static actor_t     dummy_actor;

    vm.pUSprite = &dummy_sprite;
    vm.pActor   = &dummy_actor;
    vm.pData    = &dummy_actor.t_data[0];
}

static FORCE_INLINE int32_t VM_EventCommon__(int const &eventNum, int const &spriteNum, int const &playerNum, int const &playerDist, int32_t returnValue)
{
    vmstate_t const tempvm = { spriteNum, playerNum, playerDist, 0, &sprite[spriteNum], &actor[spriteNum].t_data[0], g_player[playerNum].ps, &actor[spriteNum] };

    typedef struct
    {
        vmstate_t vm;
        intptr_t returnVar;
        int currentEvent;
        intptr_t const *insptr;
    } eventbackup_t;

    auto &returnVar = aGameVars[g_returnVarID].global;
    eventbackup_t const backup = { vm, returnVar, eventNum, insptr };
    g_currentEvent = eventNum;

    double const t = timerGetHiTicks();

    vm = tempvm;
    returnVar = returnValue;
    insptr = apScript + apScriptEvents[eventNum];

    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
        VM_DummySprite();

    if (EDUKE32_PREDICT_FALSE((unsigned)playerNum >= (unsigned)g_mostConcurrentPlayers))
        vm.pPlayer = g_player[0].ps;

    VM_Execute(1);

    if (vm.flags & VM_KILL)
        VM_DeleteSprite(vm.spriteNum, vm.playerNum);

    // this needs to happen after VM_DeleteSprite() because VM_DeleteSprite()
    // can trigger additional events

    returnValue    = returnVar;

    vm             = backup.vm;
    returnVar      = backup.returnVar;
    g_currentEvent = backup.currentEvent;
    insptr         = backup.insptr;

    g_eventTotalMs[eventNum] += timerGetHiTicks()-t;
    g_eventCalls[eventNum]++;

    return returnValue;
}
#endif

// the idea here is that the compiler inlines the call to VM_EventCommon_() and gives us a set of full functions
// which are not only optimized further based on lDist or iReturn (or both) having values known at compile time,
// but are called faster due to having less parameters

int32_t VM_OnEventWithBoth__(int const nEventID, int const spriteNum, int const playerNum, int const nDist, int32_t const nReturn)
{
    return VM_EventCommon__(nEventID, spriteNum, playerNum, nDist, nReturn);
}

int32_t VM_OnEventWithReturn__(int const nEventID, int const spriteNum, int const playerNum, int32_t const nReturn)
{
    return VM_EventCommon__(nEventID, spriteNum, playerNum, -1, nReturn);
}

int32_t VM_OnEventWithDist__(int const nEventID, int const spriteNum, int const playerNum, int const nDist)
{
    return VM_EventCommon__(nEventID, spriteNum, playerNum, nDist, 0);
}

int32_t VM_OnEvent__(int const nEventID, int const spriteNum, int const playerNum)
{
    return VM_EventCommon__(nEventID, spriteNum, playerNum, -1, 0);
}

static bool VM_CheckSquished(void)
{
    auto const pSector = (usectortype *)&sector[vm.pSprite->sectnum];

    if (pSector->lotag == ST_23_SWINGING_DOOR || (vm.pSprite->picnum == APLAYER && ud.noclip) ||
        (pSector->lotag == ST_1_ABOVE_WATER && !A_CheckNoSE7Water(vm.pUSprite, vm.pSprite->sectnum, pSector->lotag, NULL)))
        return 0;

    int32_t floorZ = pSector->floorz;
    int32_t ceilZ  = pSector->ceilingz;
#ifdef YAX_ENABLE
    int16_t cb, fb;

    yax_getbunches(vm.pSprite->sectnum, &cb, &fb);

    if (cb >= 0 && (pSector->ceilingstat&512)==0)  // if ceiling non-blocking...
        ceilZ -= ZOFFSET5;  // unconditionally don't squish... yax_getneighborsect is slowish :/
    if (fb >= 0 && (pSector->floorstat&512)==0)
        floorZ += ZOFFSET5;
#endif

    if (vm.pSprite->pal == 1 ? (floorZ - ceilZ >= ZOFFSET5 || (pSector->lotag & 32768u)) : (floorZ - ceilZ >= ZOFFSET4))
        return 0;

    P_DoQuote(QUOTE_SQUISHED, vm.pPlayer);

    if (A_CheckEnemySprite(vm.pSprite))
        vm.pSprite->xvel = 0;

#ifndef EDUKE32_STANDALONE
    if (EDUKE32_PREDICT_FALSE(vm.pSprite->pal == 1)) // frozen
    {
        vm.pActor->picnum = SHOTSPARK1;
        vm.pActor->extra  = 1;
        return 0;
    }
#endif

    return 1;
}

#if !defined LUNATIC
GAMEEXEC_STATIC GAMEEXEC_INLINE void P_ForceAngle(DukePlayer_t *pPlayer)
{
    int const nAngle = 128-(krand()&255);

    pPlayer->q16horiz           += F16(64);
    pPlayer->return_to_center = 9;
    pPlayer->rotscrnang       = nAngle >> 1;
    pPlayer->look_ang         = pPlayer->rotscrnang;
}
#endif

// wow, this function sucks
#ifdef __cplusplus
extern "C"
#endif
bool A_Dodge(spritetype * const);
bool A_Dodge(spritetype * const pSprite)
{
    if (A_CheckEnemySprite(pSprite) && pSprite->extra <= 0)  // hack
        return 0;

    vec2_t const msin = { sintable[(pSprite->ang + 512) & 2047], sintable[pSprite->ang & 2047] };

    for (native_t nexti, SPRITES_OF_STAT_SAFE(STAT_PROJECTILE, i, nexti)) //weapons list
    {
        if (OW(i) == i)
            continue;

        vec2_t const b = { SX(i) - pSprite->x, SY(i) - pSprite->y };
        vec2_t const v = { sintable[(SA(i) + 512) & 2047], sintable[SA(i) & 2047] };

        if (((msin.x * b.x) + (msin.y * b.y) >= 0) && ((v.x * b.x) + (v.y * b.y) < 0))
        {
            if (klabs((v.x * b.y) - (v.y * b.x)) < 65536 << 6)
            {
                pSprite->ang -= 512+(krand()&1024);
                return 1;
            }
        }
    }

    return 0;
}

int A_GetFurthestAngle(int spriteNum, int angDiv)
{
    auto const pSprite = (uspritetype *)&sprite[spriteNum];

    if (pSprite->picnum != APLAYER && (AC_COUNT(actor[spriteNum].t_data)&63) > 2)
        return pSprite->ang + 1024;

    int       furthestAngle = 0;
    int const angIncs       = tabledivide32_noinline(2048, angDiv);
    int32_t   greatestDist  = INT32_MIN;
    hitdata_t hit;

    for (native_t j = pSprite->ang; j < (2048 + pSprite->ang); j += angIncs)
    {
        pSprite->z -= ZOFFSET3;
        hitscan((const vec3_t *)pSprite, pSprite->sectnum, sintable[(j + 512) & 2047], sintable[j & 2047], 0, &hit, CLIPMASK1);
        pSprite->z += ZOFFSET3;

        int const hitDist = klabs(hit.pos.x-pSprite->x) + klabs(hit.pos.y-pSprite->y);

        if (hitDist > greatestDist)
        {
            greatestDist = hitDist;
            furthestAngle = j;
        }
    }

    return furthestAngle & 2047;
}

int A_FurthestVisiblePoint(int const spriteNum, uspritetype * const ts, vec2_t * const vect)
{
    if (AC_COUNT(actor[spriteNum].t_data)&63)
        return -1;

    const uspritetype *const pnSprite = (uspritetype *)&sprite[spriteNum];

    hitdata_t hit;
    int const angincs = 128;
//    ((!g_netServer && ud.multimode < 2) && ud.player_skill < 3) ? 2048 / 2 : tabledivide32_noinline(2048, 1 + (krand() & 1));

    for (native_t j = ts->ang; j < (2048 + ts->ang); j += (angincs /*-(krand()&511)*/))
    {
        ts->z -= ZOFFSET2;
        hitscan((const vec3_t *)ts, ts->sectnum, sintable[(j + 512) & 2047], sintable[j & 2047], 16384 - (krand() & 32767), &hit, CLIPMASK1);
        ts->z += ZOFFSET2;

        if (hit.sect < 0)
            continue;

        int const d  = FindDistance2D(hit.pos.x - ts->x, hit.pos.y - ts->y);
        int const da = FindDistance2D(hit.pos.x - pnSprite->x, hit.pos.y - pnSprite->y);

        if (d < da)
        {
            if (cansee(hit.pos.x, hit.pos.y, hit.pos.z, hit.sect, pnSprite->x, pnSprite->y, pnSprite->z - ZOFFSET2, pnSprite->sectnum))
            {
                vect->x = hit.pos.x;
                vect->y = hit.pos.y;
                return hit.sect;
            }
        }
    }

    return -1;
}

static void VM_GetZRange(int const spriteNum, int32_t * const ceilhit, int32_t * const florhit, int const wallDist)
{
    uspritetype *const pSprite = (uspritetype *)&sprite[spriteNum];
    int const          ocstat  = pSprite->cstat;

    pSprite->cstat = 0;
    pSprite->z -= ZOFFSET;

    getzrange((vec3_t *)pSprite, pSprite->sectnum, &actor[spriteNum].ceilingz, ceilhit, &actor[spriteNum].floorz, florhit, wallDist, CLIPMASK0);

    pSprite->z += ZOFFSET;
    pSprite->cstat = ocstat;
}

void A_GetZLimits(int const spriteNum)
{
    auto const pSprite = &sprite[spriteNum];
    int32_t    ceilhit, florhit;

    VM_GetZRange(spriteNum, &ceilhit, &florhit, (pSprite->statnum == STAT_PROJECTILE) ? 4 : 127);
    actor[spriteNum].flags &= ~SFLAG_NOFLOORSHADOW;

    if ((florhit&49152) == 49152 && (sprite[florhit&(MAXSPRITES-1)].cstat&48) == 0)
    {
        auto const hitspr = (uspritetype *)&sprite[florhit&(MAXSPRITES-1)];

        florhit &= (MAXSPRITES-1);

        // If a non-projectile would fall onto non-frozen enemy OR an enemy onto a player...
        if ((A_CheckEnemySprite(hitspr) && hitspr->pal != 1 && pSprite->statnum != STAT_PROJECTILE)
                || (hitspr->picnum == APLAYER && A_CheckEnemySprite(pSprite)))
        {
            actor[spriteNum].flags |= SFLAG_NOFLOORSHADOW;  // No shadows on actors
            pSprite->xvel = -256;  // SLIDE_ABOVE_ENEMY
            A_SetSprite(spriteNum, CLIPMASK0);
        }
        else if (pSprite->statnum == STAT_PROJECTILE && hitspr->picnum == APLAYER && pSprite->owner==florhit)
        {
            actor[spriteNum].ceilingz = sector[pSprite->sectnum].ceilingz;
            actor[spriteNum].floorz   = sector[pSprite->sectnum].floorz;
        }
    }
}

void A_Fall(int const spriteNum)
{
    auto const pSprite = &sprite[spriteNum];
    int spriteGravity = g_spriteGravity;

    if (EDUKE32_PREDICT_FALSE(G_CheckForSpaceFloor(pSprite->sectnum)))
        spriteGravity = 0;
    else if (sector[pSprite->sectnum].lotag == ST_2_UNDERWATER || EDUKE32_PREDICT_FALSE(G_CheckForSpaceCeiling(pSprite->sectnum)))
        spriteGravity = g_spriteGravity/6;

    if (pSprite->statnum == STAT_ACTOR || pSprite->statnum == STAT_PLAYER || pSprite->statnum == STAT_ZOMBIEACTOR
        || pSprite->statnum == STAT_STANDABLE)
    {
        int32_t ceilhit, florhit;
        VM_GetZRange(spriteNum, &ceilhit, &florhit, 127);
    }
    else
    {
        actor[spriteNum].ceilingz = sector[pSprite->sectnum].ceilingz;
        actor[spriteNum].floorz   = sector[pSprite->sectnum].floorz;
    }

#ifdef YAX_ENABLE
    int fbunch = (sector[pSprite->sectnum].floorstat&512) ? -1 : yax_getbunch(pSprite->sectnum, YAX_FLOOR);
#endif

    if (pSprite->z < actor[spriteNum].floorz-ZOFFSET
#ifdef YAX_ENABLE
            || fbunch >= 0
#endif
       )
    {
        if (sector[pSprite->sectnum].lotag == ST_2_UNDERWATER && pSprite->zvel > 3122)
            pSprite->zvel = 3144;
        pSprite->z += pSprite->zvel = min(6144, pSprite->zvel+spriteGravity);
    }

#ifdef YAX_ENABLE
    if (fbunch >= 0)
        setspritez(spriteNum, (vec3_t *)pSprite);
    else
#endif
        if (pSprite->z >= actor[spriteNum].floorz-ZOFFSET)
        {
            pSprite->z = actor[spriteNum].floorz-ZOFFSET;
            pSprite->zvel = 0;
        }
}

int __fastcall G_GetAngleDelta(int currAngle, int newAngle)
{
    currAngle &= 2047;
    newAngle &= 2047;

    if (klabs(currAngle-newAngle) < 1024)
    {
//        OSD_Printf("G_GetAngleDelta() returning %d\n",na-a);
        return newAngle-currAngle;
    }

    if (newAngle > 1024)
        newAngle -= 2048;
    if (currAngle > 1024)
        currAngle -= 2048;

//    OSD_Printf("G_GetAngleDelta() returning %d\n",na-a);
    return newAngle-currAngle;
}

GAMEEXEC_STATIC void VM_AlterAng(int32_t const moveFlags)
{
    int const elapsedTics = (AC_COUNT(vm.pData))&31;

#if !defined LUNATIC
    const intptr_t *moveptr;
    if (EDUKE32_PREDICT_FALSE((unsigned)AC_MOVE_ID(vm.pData) >= (unsigned)g_scriptSize-1))

    {
        AC_MOVE_ID(vm.pData) = 0;
        OSD_Printf(OSD_ERROR "bad moveptr for actor %d (%d)!\n", vm.spriteNum, vm.pUSprite->picnum);
        return;
    }

    moveptr = apScript + AC_MOVE_ID(vm.pData);

    vm.pSprite->xvel += (moveptr[0] - vm.pSprite->xvel)/5;
    if (vm.pSprite->zvel < 648)
        vm.pSprite->zvel += ((moveptr[1]<<4) - vm.pSprite->zvel)/5;
#else
    vm.pSprite->xvel += (vm.pActor->mv.hvel - vm.pSprite->xvel)/5;
    if (vm.pSprite->zvel < 648)
        vm.pSprite->zvel += ((vm.pActor->mv.vvel<<4) - vm.pSprite->zvel)/5;
#endif

    if (A_CheckEnemySprite(vm.pSprite) && vm.pSprite->extra <= 0) // hack
        return;

    if (moveFlags&seekplayer)
    {
        int const spriteAngle    = vm.pSprite->ang;
        int const holoDukeSprite = vm.pPlayer->holoduke_on;

        // NOTE: looks like 'owner' is set to target sprite ID...

        vm.pSprite->owner = (holoDukeSprite >= 0
                             && cansee(sprite[holoDukeSprite].x, sprite[holoDukeSprite].y, sprite[holoDukeSprite].z, sprite[holoDukeSprite].sectnum,
                                       vm.pSprite->x, vm.pSprite->y, vm.pSprite->z, vm.pSprite->sectnum))
          ? holoDukeSprite
          : vm.pPlayer->i;

        int const goalAng = (sprite[vm.pSprite->owner].picnum == APLAYER)
                  ? getangle(vm.pActor->lastv.x - vm.pSprite->x, vm.pActor->lastv.y - vm.pSprite->y)
                  : getangle(sprite[vm.pSprite->owner].x - vm.pSprite->x, sprite[vm.pSprite->owner].y - vm.pSprite->y);

        if (vm.pSprite->xvel && vm.pSprite->picnum != DRONE)
        {
            int const angDiff = G_GetAngleDelta(spriteAngle, goalAng);

            if (elapsedTics < 2)
            {
                if (klabs(angDiff) < 256)
                {
                    int const angInc = 128-(krand()&256);
                    vm.pSprite->ang += angInc;
                    if (A_GetHitscanRange(vm.spriteNum) < 844)
                        vm.pSprite->ang -= angInc;
                }
            }
            else if (elapsedTics > 18 && elapsedTics < GAMETICSPERSEC) // choose
            {
                if (klabs(angDiff >> 2) < 128)
                    vm.pSprite->ang = goalAng;
                else
                    vm.pSprite->ang += angDiff >> 2;
            }
        }
        else
            vm.pSprite->ang = goalAng;
    }

    if (elapsedTics < 1)
    {
        if (moveFlags&furthestdir)
        {
            vm.pSprite->ang = A_GetFurthestAngle(vm.spriteNum, 2);
            vm.pSprite->owner = vm.pPlayer->i;
        }

        if (moveFlags&fleeenemy)
            vm.pSprite->ang = A_GetFurthestAngle(vm.spriteNum, 2);
    }
}

static inline void VM_AddAngle(int const shift, int const goalAng)
{
    int angDiff = G_GetAngleDelta(vm.pSprite->ang, goalAng) >> shift;

    if ((angDiff > -8 && angDiff < 0) || (angDiff < 8 && angDiff > 0))
        angDiff <<= 1;

    vm.pSprite->ang += angDiff;
}

static inline void VM_FacePlayer(int const shift)
{
    VM_AddAngle(shift, (vm.pPlayer->newowner >= 0) ? getangle(vm.pPlayer->opos.x - vm.pSprite->x, vm.pPlayer->opos.y - vm.pSprite->y)
                                                 : getangle(vm.pPlayer->pos.x - vm.pSprite->x, vm.pPlayer->pos.y - vm.pSprite->y));
}

////////// TROR get*zofslope //////////
// These rather belong into the engine.

static int32_t VM_GetCeilZOfSlope(void)
{
    vec2_t const vect     = *(vec2_t *)vm.pSprite;
    int const    sectnum  = vm.pSprite->sectnum;

#ifdef YAX_ENABLE
    if ((sector[sectnum].ceilingstat&512)==0)
    {
        int const nsect = yax_getneighborsect(vect.x, vect.y, sectnum, YAX_CEILING);
        if (nsect >= 0)
            return getceilzofslope(nsect, vect.x, vect.y);
    }
#endif
    return getceilzofslope(sectnum, vect.x, vect.y);
}

#ifndef EDUKE32_STANDALONE
static int32_t VM_GetFlorZOfSlope(void)
{
    vec2_t const vect    = *(vec2_t *)vm.pSprite;
    int const    sectnum = vm.pSprite->sectnum;

#ifdef YAX_ENABLE
    if ((sector[sectnum].floorstat&512)==0)
    {
        int const nsect = yax_getneighborsect(vect.x, vect.y, sectnum, YAX_FLOOR);
        if (nsect >= 0)
            return getflorzofslope(nsect, vect.x, vect.y);
    }
#endif
    return getflorzofslope(sectnum, vect.x, vect.y);
}
#endif

////////////////////

static int32_t A_GetWaterZOffset(int spritenum);

GAMEEXEC_STATIC void VM_Move(void)
{
    auto const movflagsptr = &AC_MOVFLAGS(vm.pSprite, &actor[vm.spriteNum]);
    // NOTE: test against -1 commented out and later revived in source history
    // XXX: Does its presence/absence break anything? Where are movflags with all bits set created?
    int const movflags = (*movflagsptr == (std::remove_pointer<decltype(movflagsptr)>::type)-1) ? 0 : *movflagsptr;
    int const deadflag = (A_CheckEnemySprite(vm.pSprite) && vm.pSprite->extra <= 0);

    AC_COUNT(vm.pData)++;

    if (AC_MOVE_ID(vm.pData) == 0 || movflags == 0)
    {
        if (deadflag || (vm.pActor->bpos.x != vm.pSprite->x) || (vm.pActor->bpos.y != vm.pSprite->y))
        {
            vm.pActor->bpos.x = vm.pSprite->x;
            vm.pActor->bpos.y = vm.pSprite->y;
            setsprite(vm.spriteNum, (vec3_t *)vm.pSprite);
        }
        return;
    }

    if (deadflag)
        goto dead;

    if (movflags&face_player)
        VM_FacePlayer(2);

    if (movflags&spin)
        vm.pSprite->ang += sintable[((AC_COUNT(vm.pData)<<3)&2047)]>>6;

    if (movflags&face_player_slow)
        VM_FacePlayer(4);

    if ((movflags&jumptoplayer_bits) == jumptoplayer_bits)
    {
        if (AC_COUNT(vm.pData) < 16)
            vm.pSprite->zvel -= (sintable[(512+(AC_COUNT(vm.pData)<<4))&2047]>>5);
    }

    if (movflags&face_player_smart)
    {
        vec2_t const vect = { vm.pPlayer->pos.x + (vm.pPlayer->vel.x / 768), vm.pPlayer->pos.y + (vm.pPlayer->vel.y / 768) };
        VM_AddAngle(2, getangle(vect.x - vm.pSprite->x, vect.y - vm.pSprite->y));
    }

dead:
#if !defined LUNATIC
    if (EDUKE32_PREDICT_FALSE((unsigned)AC_MOVE_ID(vm.pData) >= (unsigned)g_scriptSize-1))
    {
        AC_MOVE_ID(vm.pData) = 0;
        OSD_Printf(OSD_ERROR "clearing bad moveptr for actor %d (%d)\n", vm.spriteNum, vm.pUSprite->picnum);
        return;
    }

    auto const moveptr = apScript + AC_MOVE_ID(vm.pData);

    if (movflags & geth)
        vm.pSprite->xvel += ((moveptr[0]) - vm.pSprite->xvel) >> 1;
    if (movflags & getv)
        vm.pSprite->zvel += ((moveptr[1] << 4) - vm.pSprite->zvel) >> 1;
#else
    if (movflags & geth)
        vm.pSprite->xvel += (vm.pActor->mv.hvel - vm.pSprite->xvel) >> 1;
    if (movflags & getv)
        vm.pSprite->zvel += (16 * vm.pActor->mv.vvel - vm.pSprite->zvel) >> 1;
#endif

    if (movflags&dodgebullet && !deadflag)
        A_Dodge(vm.pSprite);

    if (vm.pSprite->picnum != APLAYER)
        VM_AlterAng(movflags);

    if (vm.pSprite->xvel > -6 && vm.pSprite->xvel < 6)
        vm.pSprite->xvel = 0;

    int badguyp = A_CheckEnemySprite(vm.pSprite);

    if (vm.pSprite->xvel || vm.pSprite->zvel)
    {
        int spriteXvel = vm.pSprite->xvel;
        int angDiff    = vm.pSprite->ang;

#ifndef EDUKE32_STANDALONE
        if (badguyp && vm.pSprite->picnum != ROTATEGUN)
        {
            if ((vm.pSprite->picnum == DRONE || vm.pSprite->picnum == COMMANDER) && vm.pSprite->extra > 0)
            {
                if (vm.pSprite->picnum == COMMANDER)
                {
                    int32_t nSectorZ;
                    // NOTE: COMMANDER updates both actor[].floorz and
                    // .ceilingz regardless of its zvel.
                    vm.pActor->floorz = nSectorZ = VM_GetFlorZOfSlope();
                    if (vm.pSprite->z > nSectorZ-ZOFFSET3)
                    {
                        vm.pSprite->z = nSectorZ-ZOFFSET3;
                        vm.pSprite->zvel = 0;
                    }

                    vm.pActor->ceilingz = nSectorZ = VM_GetCeilZOfSlope();
                    if (vm.pSprite->z < nSectorZ+(80<<8))
                    {
                        vm.pSprite->z = nSectorZ+(80<<8);
                        vm.pSprite->zvel = 0;
                    }
                }
                else
                {
                    int32_t nSectorZ;
                    // The DRONE updates either .floorz or .ceilingz, not both.
                    if (vm.pSprite->zvel > 0)
                    {
                        vm.pActor->floorz = nSectorZ = VM_GetFlorZOfSlope();
                        if (vm.pSprite->z > nSectorZ-(30<<8))
                            vm.pSprite->z = nSectorZ-(30<<8);
                    }
                    else
                    {
                        vm.pActor->ceilingz = nSectorZ = VM_GetCeilZOfSlope();
                        if (vm.pSprite->z < nSectorZ+(50<<8))
                        {
                            vm.pSprite->z = nSectorZ+(50<<8);
                            vm.pSprite->zvel = 0;
                        }
                    }
                }
            }
            else if (vm.pSprite->picnum != ORGANTIC)
#else
        if (badguyp)
        {
#endif
            {
                // All other actors besides ORGANTIC don't update .floorz or
                // .ceilingz here.
                if (vm.pSprite->zvel > 0)
                {
                    if (vm.pSprite->z > vm.pActor->floorz)
                        vm.pSprite->z = vm.pActor->floorz;
                    vm.pSprite->z += A_GetWaterZOffset(vm.spriteNum);
                }
                else if (vm.pSprite->zvel < 0)
                {
                    int const l = VM_GetCeilZOfSlope();

                    if (vm.pSprite->z < l+(66<<8))
                    {
                        vm.pSprite->z = l+(66<<8);
                        vm.pSprite->zvel >>= 1;
                    }
                }
            }

            if (vm.playerDist < 960 && vm.pSprite->xrepeat > 16)
            {
                spriteXvel = -(1024 - vm.playerDist);
                angDiff = getangle(vm.pPlayer->pos.x - vm.pSprite->x, vm.pPlayer->pos.y - vm.pSprite->y);

                if (vm.playerDist < 512)
                {
                    vm.pPlayer->vel.x = 0;
                    vm.pPlayer->vel.y = 0;
                }
                else
                {
                    vm.pPlayer->vel.x = mulscale16(vm.pPlayer->vel.x, vm.pPlayer->runspeed - 0x2000);
                    vm.pPlayer->vel.y = mulscale16(vm.pPlayer->vel.y, vm.pPlayer->runspeed - 0x2000);
                }
            }
            else
#ifndef EDUKE32_STANDALONE
                if (vm.pSprite->picnum != DRONE && vm.pSprite->picnum != SHARK && vm.pSprite->picnum != COMMANDER)
#endif
            {
                if (vm.pPlayer->actorsqu == vm.spriteNum)
                    return;

                if (!A_CheckSpriteFlags(vm.spriteNum, SFLAG_SMOOTHMOVE))
                {
                    if (AC_COUNT(vm.pData) & 1)
                        return;
                    spriteXvel <<= 1;
                }
            }
        }
        else if (vm.pSprite->picnum == APLAYER)
            if (vm.pSprite->z < vm.pActor->ceilingz+ZOFFSET5)
                vm.pSprite->z = vm.pActor->ceilingz+ZOFFSET5;

        vec3_t const vect
        = { (spriteXvel * (sintable[(angDiff + 512) & 2047])) >> 14, (spriteXvel * (sintable[angDiff & 2047])) >> 14, vm.pSprite->zvel };

        vm.pActor->movflag = A_MoveSprite(vm.spriteNum, &vect, (A_CheckSpriteFlags(vm.spriteNum, SFLAG_NOCLIP) ? 0 : CLIPMASK0));
    }

    if (!badguyp)
        return;

    vm.pSprite->shade += (sector[vm.pSprite->sectnum].ceilingstat & 1) ? (sector[vm.pSprite->sectnum].ceilingshade - vm.pSprite->shade) >> 1
                                                                 : (sector[vm.pSprite->sectnum].floorshade - vm.pSprite->shade) >> 1;
}

static void P_AddWeaponMaybeSwitch(DukePlayer_t * const ps, int const weaponNum)
{
    if ((ps->weaponswitch & (1|4)) == (1|4))
    {
        int const playerNum    = P_Get(ps->i);
        int       new_wchoice  = -1;
        int       curr_wchoice = -1;

        for (native_t i=0; i<=FREEZE_WEAPON && (new_wchoice < 0 || curr_wchoice < 0); i++)
        {
            int w = g_player[playerNum].wchoice[i];

            if (w == KNEE_WEAPON)
                w = FREEZE_WEAPON;
            else
                w--;

            if (w == ps->curr_weapon)
                curr_wchoice = i;
            if (w == weaponNum)
                new_wchoice = i;
        }

        P_AddWeapon(ps, weaponNum, (new_wchoice < curr_wchoice));
    }
    else
    {
        P_AddWeapon(ps, weaponNum, (ps->weaponswitch & 1));
    }
}

#if defined LUNATIC
void        P_AddWeaponMaybeSwitchI(int32_t snum, int32_t weap) { P_AddWeaponMaybeSwitch(g_player[snum].ps, weap); }
#else
static void P_AddWeaponAmmoCommon(DukePlayer_t * const pPlayer, int const weaponNum, int const nAmount)
{
    P_AddAmmo(pPlayer, weaponNum, nAmount);

    if (PWEAPON(vm.playerNum, pPlayer->curr_weapon, WorksLike) == KNEE_WEAPON && (pPlayer->gotweapon & (1 << weaponNum)))
        P_AddWeaponMaybeSwitch(pPlayer, weaponNum);
}

static void VM_AddWeapon(DukePlayer_t * const pPlayer, int const weaponNum, int const nAmount)
{
    if (EDUKE32_PREDICT_FALSE((unsigned)weaponNum >= MAX_WEAPONS))
    {
        CON_ERRPRINTF("invalid weapon %d\n", weaponNum);
        return;
    }

    if ((pPlayer->gotweapon & (1 << weaponNum)) == 0)
    {
        P_AddWeaponMaybeSwitch(pPlayer, weaponNum);
    }
    else if (pPlayer->ammo_amount[weaponNum] >= pPlayer->max_ammo_amount[weaponNum])
    {
        vm.flags |= VM_NOEXECUTE;
        return;
    }

    P_AddWeaponAmmoCommon(pPlayer, weaponNum, nAmount);
}

static void VM_AddAmmo(DukePlayer_t * const pPlayer, int const weaponNum, int const nAmount)
{
    if (EDUKE32_PREDICT_FALSE((unsigned)weaponNum >= MAX_WEAPONS))
    {
        CON_ERRPRINTF("invalid weapon %d\n", weaponNum);
        return;
    }

    if (pPlayer->ammo_amount[weaponNum] >= pPlayer->max_ammo_amount[weaponNum])
    {
        vm.flags |= VM_NOEXECUTE;
        return;
    }

    P_AddWeaponAmmoCommon(pPlayer, weaponNum, nAmount);
}

static void VM_AddInventory(DukePlayer_t * const pPlayer, int const itemNum, int const nAmount)
{
    switch (itemNum)
    {
    case GET_STEROIDS:
    case GET_SCUBA:
    case GET_HOLODUKE:
    case GET_JETPACK:
    case GET_HEATS:
    case GET_FIRSTAID:
    case GET_BOOTS:
        pPlayer->inven_icon = inv_to_icon[itemNum];
        pPlayer->inv_amount[itemNum] = nAmount;
        break;

    case GET_SHIELD:
    {
        int16_t & shield_amount = pPlayer->inv_amount[GET_SHIELD];
        shield_amount = min(shield_amount + nAmount, pPlayer->max_shield_amount);
        break;
    }

    case GET_ACCESS:
        switch (vm.pSprite->pal)
        {
                case 0: pPlayer->got_access |= 1; break;
                case 21: pPlayer->got_access |= 2; break;
                case 23: pPlayer->got_access |= 4; break;
        }
        break;

        default: CON_ERRPRINTF("invalid inventory item %d\n", itemNum); break;
    }
}
#endif

static int A_GetVerticalVel(actor_t const * const pActor)
{
#ifdef LUNATIC
    return pActor->mv.vvel;
#else
    int32_t moveScriptOfs = AC_MOVE_ID(pActor->t_data);

    return ((unsigned) moveScriptOfs < (unsigned) g_scriptSize - 1) ? apScript[moveScriptOfs + 1] : 0;
#endif
}

static int32_t A_GetWaterZOffset(int const spriteNum)
{
    auto const pSprite = (uspritetype *)&sprite[spriteNum];
    auto const pActor  = &actor[spriteNum];

    if (sector[pSprite->sectnum].lotag == ST_1_ABOVE_WATER)
    {
        if (A_CheckSpriteFlags(spriteNum, SFLAG_NOWATERDIP))
            return 0;

        // fix for flying/jumping monsters getting stuck in water
        if ((AC_MOVFLAGS(pSprite, pActor) & jumptoplayer_only) || (G_HaveActor(pSprite->picnum) && A_GetVerticalVel(pActor) != 0))
            return 0;

        return ACTOR_ONWATER_ADDZ;
    }

    return 0;
}

static void VM_Fall(int const spriteNum, spritetype * const pSprite)
{
    int spriteGravity = g_spriteGravity;

    pSprite->xoffset = pSprite->yoffset = 0;

    if (sector[pSprite->sectnum].lotag == ST_2_UNDERWATER || EDUKE32_PREDICT_FALSE(G_CheckForSpaceCeiling(pSprite->sectnum)))
        spriteGravity = g_spriteGravity/6;
    else if (EDUKE32_PREDICT_FALSE(G_CheckForSpaceFloor(pSprite->sectnum)))
        spriteGravity = 0;

    if (!actor[spriteNum].cgg-- || (sector[pSprite->sectnum].floorstat&2))
    {
        A_GetZLimits(spriteNum);
        actor[spriteNum].cgg = 3;
    }

    if (pSprite->z < actor[spriteNum].floorz-ZOFFSET)
    {
        // Free fall.
        pSprite->zvel = min(pSprite->zvel+spriteGravity, ACTOR_MAXFALLINGZVEL);
        int newZ = pSprite->z + pSprite->zvel;

#ifdef YAX_ENABLE
        if (yax_getbunch(pSprite->sectnum, YAX_FLOOR) >= 0 && (sector[pSprite->sectnum].floorstat & 512) == 0)
            setspritez(spriteNum, (vec3_t *)pSprite);
        else
#endif
            if (newZ > actor[spriteNum].floorz - ZOFFSET)
                newZ = actor[spriteNum].floorz - ZOFFSET;

        pSprite->z = newZ;
        return;
    }

    // Preliminary new z position of the actor.
    int newZ = actor[spriteNum].floorz - ZOFFSET;

    if (A_CheckEnemySprite(pSprite) || (pSprite->picnum == APLAYER && pSprite->owner >= 0))
    {
        if (pSprite->zvel > 3084 && pSprite->extra <= 1)
        {
            // I'm guessing this DRONE check is from a beta version of the game
            // where they crashed into the ground when killed
#ifndef EDUKE32_STANDALONE
            if (!(pSprite->picnum == APLAYER && pSprite->extra > 0) && pSprite->pal != 1 && pSprite->picnum != DRONE)
            {
                A_DoGuts(spriteNum,JIBS6,15);
                A_PlaySound(SQUISHED,spriteNum);
                A_Spawn(spriteNum,BLOODPOOL);
            }
#endif
            actor[spriteNum].picnum = SHOTSPARK1;
            actor[spriteNum].extra = 1;
            pSprite->zvel = 0;
        }
        else if (pSprite->zvel > 2048 && sector[pSprite->sectnum].lotag != ST_1_ABOVE_WATER)
        {
            int16_t newsect = pSprite->sectnum;

            pushmove((vec3_t *)pSprite, &newsect, 128, 4<<8, 4<<8, CLIPMASK0);
            if ((unsigned)newsect < MAXSECTORS)
                changespritesect(spriteNum, newsect);

            A_PlaySound(THUD, spriteNum);
        }
    }

    if (sector[pSprite->sectnum].lotag == ST_1_ABOVE_WATER)
    {
        pSprite->z = newZ + A_GetWaterZOffset(spriteNum);
        return;
    }

    pSprite->z = newZ;
    pSprite->zvel = 0;
}

static int32_t VM_ResetPlayer(int const playerNum, int32_t vmFlags, int32_t const resetFlags)
{
    //AddLog("resetplayer");
    if (!g_netServer && ud.multimode < 2 && !(resetFlags & 2))
    {
        if (g_quickload && g_quickload->isValid() && ud.recstat != 2 && !(resetFlags & 8))
        {
            if (resetFlags & 4)
            {
                KB_FlushKeyboardQueue();
                KB_ClearKeysDown();
                FX_StopAllSounds();
                S_ClearSoundLocks();
                if (G_LoadPlayerMaybeMulti(*g_quickload) != 0)
                {
                    g_quickload->reset();
                    goto QuickLoadFailure;
                }
            }
            else if (!(resetFlags & 1))
            {
                Menu_Open(playerNum);
                KB_ClearKeyDown(sc_Space);
                I_AdvanceTriggerClear();
                Menu_Change(MENU_RESETPLAYER);
            }
        }
        else
        {
            QuickLoadFailure:
            g_player[playerNum].ps->gm = MODE_RESTART;
        }
#if !defined LUNATIC
        vmFlags |= VM_NOEXECUTE;
#endif
    }
    else
    {
        if (playerNum == myconnectindex)
        {
            CAMERADIST = 0;
            CAMERACLOCK = totalclock;
        }

        if (g_fakeMultiMode)
            P_ResetMultiPlayer(playerNum);
#ifndef NETCODE_DISABLE
        if (g_netServer)
        {
            P_ResetMultiPlayer(playerNum);
            Net_SpawnPlayer(playerNum);
        }
#endif
    }

    P_UpdateScreenPal(g_player[playerNum].ps);
    //AddLog("EOF: resetplayer");

    return vmFlags;
}

void G_GetTimeDate(int32_t * const pValues)
{
    time_t timeStruct;
    time(&timeStruct);
    struct tm *pTime = localtime(&timeStruct);

    // initprintf("Time&date: %s\n",asctime (ti));

    pValues[0] = pTime->tm_sec;
    pValues[1] = pTime->tm_min;
    pValues[2] = pTime->tm_hour;
    pValues[3] = pTime->tm_mday;
    pValues[4] = pTime->tm_mon;
    pValues[5] = pTime->tm_year+1900;
    pValues[6] = pTime->tm_wday;
    pValues[7] = pTime->tm_yday;
}

static int G_StartTrackSlot(int const volumeNum, int const levelNum)
{
    if ((unsigned)volumeNum <= MAXVOLUMES && (unsigned)levelNum < MAXLEVELS)
    {
        int trackNum = MAXLEVELS*volumeNum + levelNum;

        return S_TryPlaySpecialMusic(trackNum);
    }

    return 1;
}

#ifndef LUNATIC
static int G_StartTrackSlotWrap(int const volumeNum, int const levelNum)
{
    if (EDUKE32_PREDICT_FALSE(G_StartTrackSlot(volumeNum, levelNum)))
    {
        CON_ERRPRINTF("invalid level %d or null music for volume %d level %d\n", levelNum, volumeNum, levelNum);
        return 1;
    }

    return 0;
}
#else
int G_StartTrack(int const levelNum) { return G_StartTrackSlot(ud.volume_number, levelNum); }
#endif

LUNATIC_EXTERN void G_ShowView(vec3_t vec, fix16_t a, fix16_t horiz, int sect, int x1, int y1, int x2, int y2, bool unbiasedp)
{
    if (g_screenCapture)
        return;

    if (offscreenrendering)
    {
        videoClearViewableArea(0);
        return;
    }

    if (x1 > x2)
        swaplong(&x1, &x2);
    if (y1 > y2)
        swaplong(&y1, &y2);

    if (!unbiasedp)
    {
        // The showview command has a rounding bias towards zero,
        // e.g. floor((319*1680)/320) == 1674
        x1 = scale(x1,xdim,320);
        y1 = scale(y1,ydim,200);
        x2 = scale(x2,xdim,320);
        y2 = scale(y2,ydim,200);
    }
    else
    {
        // This will map the maximum 320-based coordinate to the
        // maximum real screen coordinate:
        // floor((319*1679)/319) == 1679
        x1 = scale(x1,xdim-1,319);
        y1 = scale(y1,ydim-1,199);
        x2 = scale(x2,xdim-1,319);
        y2 = scale(y2,ydim-1,199);
    }

    horiz = fix16_clamp(horiz, F16(HORIZ_MIN), F16(HORIZ_MAX));

    int const viewingRange = viewingrange;
    int const yxAspect = yxaspect;

    videoSetViewableArea(x1,y1,x2,y2);
    renderSetAspect(viewingRange, yxAspect);
    int const smoothratio = calc_smoothratio(totalclock, ototalclock);
    G_DoInterpolations(smoothratio);
    G_HandleMirror(vec.x, vec.y, vec.z, a, horiz, smoothratio);
#ifdef POLYMER
    if (videoGetRenderMode() == REND_POLYMER)
        polymer_setanimatesprites(G_DoSpriteAnimations, vec.x, vec.y, fix16_to_int(a), smoothratio);
#endif
    yax_preparedrawrooms();
    renderDrawRoomsQ16(vec.x, vec.y, vec.z, a, horiz, sect);
    yax_drawrooms(G_DoSpriteAnimations, sect, 0, smoothratio);

    display_mirror = 2;
    G_DoSpriteAnimations(vec.x, vec.y, fix16_to_int(a), smoothratio);
    display_mirror = 0;
    renderDrawMasks();
    G_RestoreInterpolations();
    G_UpdateScreenArea();
    renderSetAspect(viewingRange, yxAspect);
}

void Screen_Play(void)
{
    bool running = true;

    I_ClearAllInput();

    do
    {
        G_HandleAsync();

        ototalclock = totalclock + 1; // pause game like ANMs

        if (!G_FPSLimit())
            continue;

        videoClearScreen(0);

        if (VM_OnEventWithReturn(EVENT_SCREEN, g_player[screenpeek].ps->i, screenpeek, I_CheckAllInput()))
            running = false;

        I_ClearAllInput();
    } while (running);
}

#if !defined LUNATIC
// be careful when changing this--the assignment used as a condition doubles as a null pointer check
#define VM_CONDITIONAL(xxx)                                                                       \
    {                                                                                             \
        if ((xxx) || ((insptr = (intptr_t *)insptr[1]) && ((*insptr & VM_INSTMASK) == CON_ELSE))) \
        {                                                                                         \
            insptr += 2;                                                                          \
            VM_Execute(0);                                                                        \
        }                                                                                         \
    }

#if defined __GNUC__ || defined __clang__
# define CON_DIRECT_THREADING_DISPATCH
#endif
#ifdef CON_DIRECT_THREADING_DISPATCH
# define vInstruction(KEYWORDID) VINST_ ## KEYWORDID
# define eval(INSTRUCTION) {if (INSTRUCTION >= 0 && INSTRUCTION <= CON_OPCODE_END) {goto *jumpTable[INSTRUCTION];} else {goto VINST_CON_OPCODE_END;}}
# define dispatch(INSTRUCTION) {if (loop) {tw = *insptr; g_errorLineNum = tw >> 12; g_tw = tw &= VM_INSTMASK; eval(INSTRUCTION)} else {return;}}
# define vInstructionPointer(KEYWORDID) &&VINST_ ## KEYWORDID
# define COMMA ,
# define JUMP_TABLE_ARRAY_LITERAL { TRANSFORM_SCRIPT_KEYWORDS_LIST(vInstructionPointer, COMMA) }
# define vmErrorCase VINST_CON_OPCODE_END
#else
# define vInstruction(KEYWORDID) case KEYWORDID
# define dispatch(INSTRUCTION) continue;
# define eval(INSTRUCTION) switch(INSTRUCTION)
# define vmErrorCase default
#endif

GAMEEXEC_STATIC void VM_Execute(native_t const poop)
{
    native_t loop = poop;

    do
    {
#ifndef CON_DIRECT_THREADING_DISPATCH
    next_instruction:
#endif
        native_t tw = *insptr;
        // set up "p" in between tw and g_errorLineNum to avoid read after write penalty
        auto &p = *(vm.pPlayer);
        g_errorLineNum = tw >> 12;
        g_tw = tw &= VM_INSTMASK;

#ifdef CON_DIRECT_THREADING_DISPATCH
        static void* jumpTable[] = JUMP_TABLE_ARRAY_LITERAL;
#else
        if (tw == CON_ELSE)
        {
            insptr = (intptr_t *)insptr[1];
            goto next_instruction;
        }
        else if (tw == CON_LEFTBRACE)
        {
            insptr++, loop++;
            goto next_instruction;
        }
        else if (tw == CON_RIGHTBRACE)
        {
            insptr++, loop--;
            continue;
        }
        else
#endif
        eval(tw)
        {
#ifdef CON_DIRECT_THREADING_DISPATCH
            vInstruction(CON_LEFTBRACE):
            {
                insptr++, loop++;
                dispatch(tw);
            }

            vInstruction(CON_RIGHTBRACE):
            {
                insptr++, loop--;
                dispatch(tw);
            }

            vInstruction(CON_ELSE):
            {
                insptr = (intptr_t *)insptr[1];
                dispatch(tw);
            }
#endif

            vInstruction(CON_IFVARE_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw == *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARN_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw != *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARAND_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw & *insptr);
                dispatch(tw);
            vInstruction(CON_IFVAROR_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw | *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARXOR_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw ^ *insptr);
                dispatch(tw);
            vInstruction(CON_IFVAREITHER_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw || *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARBOTH_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw && *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARG_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw > *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARGE_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw >= *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARL_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw < *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARLE_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL(tw <= *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARA_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL((uint32_t)tw > (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARAE_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL((uint32_t)tw >= (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARB_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL((uint32_t)tw < (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARBE_GLOBAL):
                insptr++;
                tw = aGameVars[*insptr++].global;
                VM_CONDITIONAL((uint32_t)tw <= (uint32_t)*insptr);
                dispatch(tw);

            vInstruction(CON_SETVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global = insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ADDVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global += insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SUBVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global -= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_MULVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global *= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ANDVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global &= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_XORVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global ^= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ORVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global |= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SHIFTVARL_GLOBAL):
                insptr++;
                aGameVars[*insptr].global <<= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SHIFTVARR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global >>= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_STATE):
                {
                    auto tempscrptr = &insptr[2];
                    insptr = (intptr_t *)insptr[1];
                    VM_Execute(1);
                    insptr = tempscrptr;
                }
                dispatch(tw);

#ifdef INCOMPLETE_STRUCT_ACCESS
            vInstruction(CON_IFVARE_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw == *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARN_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw != *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARAND_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw & *insptr);
                dispatch(tw);
            vInstruction(CON_IFVAROR_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw | *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARXOR_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw ^ *insptr);
                dispatch(tw);
            vInstruction(CON_IFVAREITHER_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw || *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARBOTH_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw && *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARG_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw > *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARGE_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw >= *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARL_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw < *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARLE_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL(tw <= *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARA_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL((uint32_t)tw > (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARAE_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL((uint32_t)tw >= (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARB_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL((uint32_t)tw < (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARBE_ACTOR):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.spriteNum & (MAXSPRITES-1)];
                VM_CONDITIONAL((uint32_t)tw <= (uint32_t)*insptr);
                dispatch(tw);

            vInstruction(CON_SETVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] = insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ADDVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] += insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SUBVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] -= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_MULVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] *= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ANDVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] &= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_XORVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] ^= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ORVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] |= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SHIFTVARL_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] <<= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SHIFTVARR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] >>= insptr[1];
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_IFVARE_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw == *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARN_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw != *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARAND_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw & *insptr);
                dispatch(tw);
            vInstruction(CON_IFVAROR_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw | *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARXOR_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw ^ *insptr);
                dispatch(tw);
            vInstruction(CON_IFVAREITHER_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw || *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARBOTH_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw && *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARG_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw > *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARGE_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw >= *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARL_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw < *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARLE_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL(tw <= *insptr);
                dispatch(tw);
            vInstruction(CON_IFVARA_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL((uint32_t)tw > (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARAE_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL((uint32_t)tw >= (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARB_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL((uint32_t)tw < (uint32_t)*insptr);
                dispatch(tw);
            vInstruction(CON_IFVARBE_PLAYER):
                insptr++;
                tw = aGameVars[*insptr++].pValues[vm.playerNum & (MAXPLAYERS-1)];
                VM_CONDITIONAL((uint32_t)tw <= (uint32_t)*insptr);
                dispatch(tw);

            vInstruction(CON_SETVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] = insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ADDVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] += insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SUBVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] -= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_MULVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] *= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ANDVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] &= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_XORVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] ^= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_ORVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] |= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SHIFTVARL_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] <<= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_SHIFTVARR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] >>= insptr[1];
                insptr += 2;
                dispatch(tw);
#endif

            vInstruction(CON_WHILEVARN_GLOBAL):
            {
                auto const savedinsptr = &insptr[2];
                do
                {
                    insptr = savedinsptr;
                    tw = (aGameVars[insptr[-1]].global != *insptr);
                    VM_CONDITIONAL(tw);
                } while (tw);
                dispatch(tw);
            }

            vInstruction(CON_WHILEVARL_GLOBAL):
            {
                auto const savedinsptr = &insptr[2];
                do
                {
                    insptr = savedinsptr;
                    tw = (aGameVars[insptr[-1]].global < *insptr);
                    VM_CONDITIONAL(tw);
                } while (tw);
                dispatch(tw);
            }

#ifdef INCOMPLETE_STRUCT_ACCESS
            vInstruction(CON_WHILEVARN_ACTOR):
            {
                auto const savedinsptr = &insptr[2];
                auto &v = aGameVars[savedinsptr[-1]].pValues[vm.spriteNum & (MAXSPRITES-1)];
                do
                {
                    insptr = savedinsptr;
                    tw = (v != *insptr);
                    VM_CONDITIONAL(tw);
                } while (tw);

                dispatch(tw);
            }

            vInstruction(CON_WHILEVARL_ACTOR):
            {
                auto const savedinsptr = &insptr[2];
                auto &v = aGameVars[savedinsptr[-1]].pValues[vm.spriteNum & (MAXSPRITES-1)];
                do
                {
                    insptr = savedinsptr;
                    tw = (v < *insptr);
                    VM_CONDITIONAL(tw);
                } while (tw);

                dispatch(tw);
            }

            vInstruction(CON_WHILEVARN_PLAYER):
            {
                auto const savedinsptr = &insptr[2];
                auto &v = aGameVars[savedinsptr[-1]].pValues[vm.playerNum & (MAXPLAYERS-1)];
                do
                {
                    insptr = savedinsptr;
                    tw = (v != *insptr);
                    VM_CONDITIONAL(tw);
                } while (tw);

                dispatch(tw);
            }

            vInstruction(CON_WHILEVARL_PLAYER):
            {
                auto const savedinsptr = &insptr[2];
                auto &v = aGameVars[savedinsptr[-1]].pValues[vm.playerNum & (MAXPLAYERS-1)];
                do
                {
                    insptr = savedinsptr;
                    tw = (v < *insptr);
                    VM_CONDITIONAL(tw);
                } while (tw);

                dispatch(tw);
            }
#endif

            vInstruction(CON_MODVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global %= insptr[1];
                insptr += 2;
                dispatch(tw);
#ifdef INCOMPLETE_STRUCT_ACCESS
            vInstruction(CON_MODVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] %= insptr[1];
                insptr += 2;
                dispatch(tw);
            vInstruction(CON_MODVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] %= insptr[1];
                insptr += 2;
                dispatch(tw);
#endif

            vInstruction(CON_IFVARAND):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw & *insptr);
                dispatch(tw);

            vInstruction(CON_IFVAROR):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw | *insptr);
                dispatch(tw);

            vInstruction(CON_IFVARXOR):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw ^ *insptr);
                dispatch(tw);

            vInstruction(CON_IFVAREITHER):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw || *insptr);
                dispatch(tw);

            vInstruction(CON_IFVARBOTH):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw && *insptr);
                dispatch(tw);

            vInstruction(CON_IFRND):
                VM_CONDITIONAL(rnd(*(++insptr)));
                dispatch(tw);

            vInstruction(CON_IFVARG):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw > *insptr);
                dispatch(tw);

            vInstruction(CON_IFVARGE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw >= *insptr);
                dispatch(tw);

            vInstruction(CON_IFVARL):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw < *insptr);
                dispatch(tw);

            vInstruction(CON_IFVARLE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw <= *insptr);
                dispatch(tw);

            vInstruction(CON_IFVARA):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL((uint32_t)tw > (uint32_t)*insptr);
                dispatch(tw);

            vInstruction(CON_IFVARAE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL((uint32_t)tw >= (uint32_t)*insptr);
                dispatch(tw);

            vInstruction(CON_IFVARB):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL((uint32_t)tw < (uint32_t)*insptr);
                dispatch(tw);

            vInstruction(CON_IFVARBE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL((uint32_t)tw <= (uint32_t)*insptr);
                dispatch(tw);

            vInstruction(CON_SETVARVAR):
                insptr++;
                {
                    tw = *insptr++;
                    int const nValue = Gv_GetVarX(*insptr++);

                    if ((aGameVars[tw].flags & (GAMEVAR_USER_MASK | GAMEVAR_PTR_MASK)) == 0)
                        aGameVars[tw].global = nValue;
                    else
                        Gv_SetVarX(tw, nValue);
                }
                dispatch(tw);

            vInstruction(CON_ADDVARVAR):
                insptr++;
                tw = *insptr++;
                Gv_AddVar(tw, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_SUBVARVAR):
                insptr++;
                tw = *insptr++;
                Gv_SubVar(tw, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_ANDVARVAR):
                insptr++;
                tw = *insptr++;
                Gv_AndVar(tw, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_XORVARVAR):
                insptr++;
                tw = *insptr++;
                Gv_XorVar(tw, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_ORVARVAR):
                insptr++;
                tw = *insptr++;
                Gv_OrVar(tw, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_SHIFTVARVARL):
                insptr++;
                tw = *insptr++;
                Gv_ShiftVarL(tw, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_SHIFTVARVARR):
                insptr++;
                tw = *insptr++;
                Gv_ShiftVarR(tw, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_MULVARVAR):
                insptr++;
                tw = *insptr++;
                Gv_MulVar(tw, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_DIVVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global = tabledivide32(aGameVars[*insptr].global, insptr[1]);
                insptr += 2;
                dispatch(tw);

#ifdef INCOMPLETE_STRUCT_ACCESS
            vInstruction(CON_DIVVAR_PLAYER):
            {
                insptr++;
                auto &v = aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS - 1)];

                v = tabledivide32(v, insptr[1]);
                insptr += 2;
                dispatch(tw);
            }

            vInstruction(CON_DIVVAR_ACTOR):
            {
                insptr++;
                auto &v = aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES - 1)];

                v = tabledivide32(v, insptr[1]);
                insptr += 2;
                dispatch(tw);
            }
#endif

            vInstruction(CON_DIVVARVAR):
                insptr++;
                {
                    tw = *insptr++;

                    int const nValue = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE(!nValue))
                    {
                        CON_CRITICALERRPRINTF("divide by zero!\n");
                        dispatch(tw);
                    }

                    Gv_DivVar(tw, nValue);
                    dispatch(tw);
                }

            vInstruction(CON_IFVARE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw == *insptr);
                dispatch(tw);

            vInstruction(CON_IFVARN):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                VM_CONDITIONAL(tw != *insptr);
                dispatch(tw);

            vInstruction(CON_IFVARVARE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = (tw == Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARN):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = (tw != Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARG):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = (tw > Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARGE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = (tw >= Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARL):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = (tw < Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARLE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = (tw <= Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARA):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = ((uint32_t)tw > (uint32_t)Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARAE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = ((uint32_t)tw >= (uint32_t)Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARB):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = ((uint32_t)tw < (uint32_t)Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARBE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = ((uint32_t)tw <= (uint32_t)Gv_GetVarX(*insptr++));
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARAND):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw &= Gv_GetVarX(*insptr++);
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVAROR):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw |= Gv_GetVarX(*insptr++);
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARXOR):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw ^= Gv_GetVarX(*insptr++);
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVAREITHER):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = (Gv_GetVarX(*insptr++) || tw);
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_IFVARVARBOTH):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                tw = (Gv_GetVarX(*insptr++) && tw);
                insptr--;
                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_WHILEVARN):
            {
                auto const savedinsptr = &insptr[2];
                do
                {
                    insptr = savedinsptr;
                    tw = (Gv_GetVarX(insptr[-1]) != *insptr);
                    VM_CONDITIONAL(tw);
                } while (tw);
                dispatch(tw);
            }

            vInstruction(CON_WHILEVARVARN):
            {
                auto const savedinsptr = &insptr[2];
                do
                {
                    insptr = savedinsptr;
                    tw = Gv_GetVarX(insptr[-1]);
                    tw = (tw != Gv_GetVarX(*insptr++));
                    insptr--;
                    VM_CONDITIONAL(tw);
                } while (tw);
                dispatch(tw);
            }

            vInstruction(CON_WHILEVARL):
            {
                auto const savedinsptr = &insptr[2];
                do
                {
                    insptr = savedinsptr;
                    tw = (Gv_GetVarX(insptr[-1]) < *insptr);
                    VM_CONDITIONAL(tw);
                } while (tw);
                dispatch(tw);
            }

            vInstruction(CON_WHILEVARVARL):
            {
                auto const savedinsptr = &insptr[2];
                do
                {
                    insptr = savedinsptr;
                    tw = Gv_GetVarX(insptr[-1]);
                    tw = (tw < Gv_GetVarX(*insptr++));
                    insptr--;
                    VM_CONDITIONAL(tw);
                } while (tw);
                dispatch(tw);
            }

            vInstruction(CON_SETVAR):
                insptr++;
                Gv_SetVarX(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_ADDVAR):
                insptr++;
                Gv_AddVar(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_SUBVAR):
                insptr++;
                Gv_SubVar(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_MULVAR):
                insptr++;
                Gv_MulVar(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_DIVVAR):
                insptr++;
                Gv_DivVar(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_ANDVAR):
                insptr++;
                Gv_AndVar(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_XORVAR):
                insptr++;
                Gv_XorVar(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_ORVAR):
                insptr++;
                Gv_OrVar(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_SHIFTVARL):
                insptr++;
                Gv_ShiftVarL(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_SHIFTVARR):
                insptr++;
                Gv_ShiftVarR(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_MODVAR):
                insptr++;
                Gv_ModVar(*insptr, insptr[1]);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_MODVARVAR):
                insptr++;
                {
                    tw = *insptr++;

                    int const nValue = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE(!nValue))
                    {
                        CON_CRITICALERRPRINTF("mod by zero!\n");
                        dispatch(tw);
                    }

                    Gv_ModVar(tw, nValue);
                    dispatch(tw);
                }

            vInstruction(CON_RANDVAR):
                insptr++;
                Gv_SetVarX(*insptr, mulscale16(krand(), insptr[1] + 1));
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_RANDVAR_GLOBAL):
                insptr++;
                aGameVars[*insptr].global = mulscale16(krand(), insptr[1] + 1);
                insptr += 2;
                dispatch(tw);

#ifdef INCOMPLETE_STRUCT_ACCESS
            vInstruction(CON_RANDVAR_PLAYER):
                insptr++;
                aGameVars[*insptr].pValues[vm.playerNum & (MAXPLAYERS-1)] = mulscale16(krand(), insptr[1] + 1);
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_RANDVAR_ACTOR):
                insptr++;
                aGameVars[*insptr].pValues[vm.spriteNum & (MAXSPRITES-1)] = mulscale16(krand(), insptr[1] + 1);
                insptr += 2;
                dispatch(tw);
#endif

            vInstruction(CON_RANDVARVAR):
                insptr++;
                tw = *insptr++;
                Gv_SetVarX(tw, mulscale16(krand(), Gv_GetVarX(*insptr++) + 1));
                dispatch(tw);

            vInstruction(CON_SETPLAYER):
                insptr++;
                {
                    int const playerNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.playerNum;
                    int const labelNum  = *insptr++;
                    int const lParm2    = (PlayerLabels[labelNum].flags & LABEL_HASPARM2) ? Gv_GetVarX(*insptr++) : 0;

                    VM_SetPlayer(playerNum, labelNum, lParm2, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETPLAYER):
                insptr++;
                {
                    int const playerNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.playerNum;
                    int const labelNum  = *insptr++;
                    int const lParm2    = (PlayerLabels[labelNum].flags & LABEL_HASPARM2) ? Gv_GetVarX(*insptr++) : 0;

                    Gv_SetVarX(*insptr++, VM_GetPlayer(playerNum, labelNum, lParm2));
                    dispatch(tw);
                }
            vInstruction(CON_SETWALL):
                insptr++;
                {
                    tw = *insptr++;

                    int const wallNum  = Gv_GetVarX(tw);
                    int const labelNum = *insptr++;
                    int const newValue = Gv_GetVarX(*insptr++);
                    auto const &wallLabel = WallLabels[labelNum];

                    if (wallLabel.offset == -1 || wallLabel.flags & LABEL_WRITEFUNC)
                    {
                        VM_SetWall(wallNum, labelNum, newValue);
                        dispatch(tw);
                    }

                    VM_SetStruct(wallLabel.flags, (intptr_t *)((char *)&wall[wallNum] + wallLabel.offset), newValue);

                    dispatch(tw);
                }

            vInstruction(CON_GETWALL):
                insptr++;
                {
                    tw = *insptr++;

                    int const wallNum  = Gv_GetVarX(tw);
                    int const labelNum = *insptr++;
                    auto const &wallLabel = WallLabels[labelNum];

                    Gv_SetVarX(*insptr++,
                               (wallLabel.offset != -1 && (wallLabel.flags & LABEL_READFUNC) != LABEL_READFUNC)
                               ? VM_GetStruct(wallLabel.flags, (intptr_t *)((char *)&wall[wallNum] + wallLabel.offset))
                               : VM_GetWall(wallNum, labelNum));

                    dispatch(tw);
                }

            vInstruction(CON_SETACTORVAR):
            vInstruction(CON_GETACTORVAR):
                insptr++;
                {
                    int const lSprite = Gv_GetVarX(*insptr++);
                    int const lVar1   = *insptr++;
                    int const lVar2   = *insptr++;

                    if (EDUKE32_PREDICT_FALSE((unsigned)lSprite >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", lSprite);
                        dispatch(tw);
                    }

                    if (tw == CON_SETACTORVAR)
                        Gv_SetVar(lVar1, Gv_GetVarX(lVar2), lSprite, vm.playerNum);
                    else
                        Gv_SetVarX(lVar2, Gv_GetVar(lVar1, lSprite, vm.playerNum));

                    dispatch(tw);
                }

            vInstruction(CON_SETPLAYERVAR):
            vInstruction(CON_GETPLAYERVAR):
                insptr++;
                {
                    int const playerNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.playerNum;
                    int const lVar1     = *insptr++;
                    int const lVar2     = *insptr++;

                    if (EDUKE32_PREDICT_FALSE((unsigned)playerNum >= (unsigned)g_mostConcurrentPlayers))
                    {
                        CON_ERRPRINTF("invalid player %d\n", playerNum);
                        dispatch(tw);
                    }

                    if (tw == CON_SETPLAYERVAR)
                        Gv_SetVar(lVar1, Gv_GetVarX(lVar2), vm.spriteNum, playerNum);
                    else
                        Gv_SetVarX(lVar2, Gv_GetVar(lVar1, vm.spriteNum, playerNum));

                    dispatch(tw);
                }

            vInstruction(CON_SETACTOR):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    int const lParm2    = (ActorLabels[labelNum].flags & LABEL_HASPARM2) ? Gv_GetVarX(*insptr++) : 0;
                    auto const &actorLabel = ActorLabels[labelNum];

                    if (EDUKE32_PREDICT_FALSE(((unsigned)spriteNum >= MAXSPRITES)
                                              || (actorLabel.flags & LABEL_HASPARM2 && (unsigned)lParm2 >= (unsigned)actorLabel.maxParm2)))
                    {
                        CON_ERRPRINTF("%s[%d] invalid for sprite %d\n", actorLabel.name, lParm2, spriteNum);
                        dispatch(tw);
                    }

                    VM_SetSprite(spriteNum, labelNum, lParm2, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETACTOR):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    int const lParm2    = (ActorLabels[labelNum].flags & LABEL_HASPARM2) ? Gv_GetVarX(*insptr++) : 0;
                    auto const &actorLabel = ActorLabels[labelNum];

                    if (EDUKE32_PREDICT_FALSE(((unsigned)spriteNum >= MAXSPRITES)
                                              || (actorLabel.flags & LABEL_HASPARM2 && (unsigned)lParm2 >= (unsigned)actorLabel.maxParm2)))
                    {
                        CON_ERRPRINTF("%s[%d] invalid for sprite %d\n", actorLabel.name, lParm2, spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(*insptr++, VM_GetSprite(spriteNum, labelNum, lParm2));
                    dispatch(tw);
                }

            vInstruction(CON_SETACTORSTRUCT):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    auto const &actorLabel = ActorLabels[labelNum];

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    VM_SetStruct(actorLabel.flags, (intptr_t *)((char *)&actor[spriteNum] + actorLabel.offset), Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETACTORSTRUCT):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    auto const &actorLabel = ActorLabels[labelNum];

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(*insptr++, VM_GetStruct(actorLabel.flags, (intptr_t *)((char *)&actor[spriteNum] + actorLabel.offset)));
                    dispatch(tw);
                }

            vInstruction(CON_SETSPRITESTRUCT):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    auto const &spriteLabel = ActorLabels[labelNum];

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    VM_SetStruct(spriteLabel.flags, (intptr_t *)((char *)&sprite[spriteNum] + spriteLabel.offset), Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETSPRITESTRUCT):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    auto const &spriteLabel = ActorLabels[labelNum];

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(*insptr++, VM_GetStruct(spriteLabel.flags, (intptr_t *)((char *)&sprite[spriteNum] + spriteLabel.offset)));
                    dispatch(tw);
                }
            vInstruction(CON_SETSPRITEEXT):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    auto const &spriteExtLabel = ActorLabels[labelNum];

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    VM_SetStruct(spriteExtLabel.flags, (intptr_t *)((char *)&spriteext[spriteNum] + spriteExtLabel.offset), Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETSPRITEEXT):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    auto const &spriteExtLabel = ActorLabels[labelNum];

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(*insptr++, VM_GetStruct(spriteExtLabel.flags, (intptr_t *)((char *)&spriteext[spriteNum] + spriteExtLabel.offset)));
                    dispatch(tw);
                }

            vInstruction(CON_SETTSPR):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    auto const &tsprLabel = TsprLabels[labelNum];

                    VM_SetStruct(tsprLabel.flags, (intptr_t *)((char *)spriteext[spriteNum].tspr + tsprLabel.offset), Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETTSPR):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const labelNum  = *insptr++;
                    auto const &tsprLabel = TsprLabels[labelNum];

                    Gv_SetVarX(*insptr++, VM_GetStruct(tsprLabel.flags, (intptr_t *)((char *)spriteext[spriteNum].tspr + tsprLabel.offset)));
                    dispatch(tw);
                }

            vInstruction(CON_SETSECTOR):
                insptr++;
                {
                    int const   sectNum   = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : sprite[vm.spriteNum].sectnum;
                    int const   labelNum  = *insptr++;
                    auto const &sectLabel = SectorLabels[labelNum];
                    int const   newValue  = Gv_GetVarX(*insptr++);

                    if (sectLabel.offset == -1 || sectLabel.flags & LABEL_WRITEFUNC)
                    {
                        VM_SetSector(sectNum, labelNum, newValue);
                        dispatch(tw);
                    }

                    VM_SetStruct(sectLabel.flags, (intptr_t *)((char *)&sector[sectNum] + sectLabel.offset), newValue);
                    dispatch(tw);
                }

            vInstruction(CON_GETSECTOR):
                insptr++;
                {
                    int const   sectNum   = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : sprite[vm.spriteNum].sectnum;
                    int const   labelNum  = *insptr++;
                    auto const &sectLabel = SectorLabels[labelNum];

                    Gv_SetVarX(*insptr++,
                               (sectLabel.offset != -1 && (sectLabel.flags & LABEL_READFUNC) != LABEL_READFUNC)
                               ? VM_GetStruct(sectLabel.flags, (intptr_t *)((char *)&sector[sectNum] + sectLabel.offset))
                               : VM_GetSector(sectNum, labelNum));
                    dispatch(tw);
                }

            vInstruction(CON_RETURN):
                vm.flags |= VM_RETURN;
#if !defined CON_DIRECT_THREADING_DISPATCH
                fallthrough__;
#endif
            vInstruction(CON_ENDSWITCH):
            vInstruction(CON_ENDA):
            vInstruction(CON_BREAK):
            vInstruction(CON_ENDS):
            vInstruction(CON_ENDEVENT): return;

            vInstruction(CON_JUMP):  // this is used for event chaining
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                insptr = (intptr_t *)(tw + apScript);
                dispatch(tw);

            vInstruction(CON_SWITCH):
                insptr++;
                {
                    // command format:
                    // variable ID to check
                    // script offset to 'end'
                    // count of case statements
                    // script offset to default case (null if none)
                    // For each case: value, ptr to code
                    int const lValue    = Gv_GetVarX(*insptr++);
                    int const endOffset = *insptr++;
                    int const numCases  = *insptr++;

                    auto lpDefault = insptr++;
                    auto lpCases   = insptr;

                    int left  = 0;
                    int right = numCases - 1;

                    insptr += numCases << 1;

                    do
                    {
                        int const lCheckCase = (left + right) >> 1;

                        if (lpCases[lCheckCase << 1] > lValue)
                            right = lCheckCase - 1;
                        else if (lpCases[lCheckCase << 1] < lValue)
                            left = lCheckCase + 1;
                        else if (lpCases[lCheckCase << 1] == lValue)
                        {
                            // fake a 2-d Array
                            insptr = (intptr_t *)(lpCases[(lCheckCase << 1) + 1] + &apScript[0]);
                            VM_Execute(1);
                            goto matched;
                        }

                        if (right - left < 0)
                            break;
                    } while (1);

                    if (*lpDefault)
                    {
                        insptr = (intptr_t *)(*lpDefault + &apScript[0]);
                        VM_Execute(1);
                    }

                matched:
                    insptr = (intptr_t *)(endOffset + (intptr_t)&apScript[0]);

                    dispatch(tw);
                }

            vInstruction(CON_FOR):  // special-purpose iteration
                insptr++;
                {
                    int const returnVar = *insptr++;
                    int const iterType  = *insptr++;
                    int const nIndex    = iterType <= ITER_DRAWNSPRITES ? 0 : Gv_GetVarX(*insptr++);

                    auto const pEnd  = insptr + *insptr;
                    auto const pNext = ++insptr;

                    switch (iterType)
                    {
                        case ITER_ALLSPRITES:
                            for (native_t jj = 0; jj < MAXSPRITES; ++jj)
                            {
                                if (sprite[jj].statnum == MAXSTATUS)
                                    continue;

                                Gv_SetVarX(returnVar, jj);
                                insptr = pNext;
                                VM_Execute(0);
                            }
                            break;
                        case ITER_ALLSPRITESBYSTAT:
                            for (native_t statNum = 0; statNum < MAXSTATUS; ++statNum)
                            {
                                for (native_t jj = headspritestat[statNum]; jj >= 0;)
                                {
                                    int const kk = nextspritestat[jj];
                                    Gv_SetVarX(returnVar, jj);
                                    insptr = pNext;
                                    VM_Execute(0);
                                    jj = kk;
                                }
                            }
                            break;
                        case ITER_ALLSPRITESBYSECT:
                            for (native_t sectNum = 0; sectNum < numsectors; ++sectNum)
                            {
                                for (native_t jj = headspritesect[sectNum]; jj >= 0;)
                                {
                                    int const kk = nextspritesect[jj];
                                    Gv_SetVarX(returnVar, jj);
                                    insptr = pNext;
                                    VM_Execute(0);
                                    jj = kk;
                                }
                            }
                            break;
                        case ITER_ALLSECTORS:
                            for (native_t jj = 0; jj < numsectors; ++jj)
                            {
                                Gv_SetVarX(returnVar, jj);
                                insptr = pNext;
                                VM_Execute(0);
                            }
                            break;
                        case ITER_ALLWALLS:
                            for (native_t jj = 0; jj < numwalls; ++jj)
                            {
                                Gv_SetVarX(returnVar, jj);
                                insptr = pNext;
                                VM_Execute(0);
                            }
                            break;
                        case ITER_ACTIVELIGHTS:
#ifdef POLYMER
                            for (native_t jj = 0; jj < PR_MAXLIGHTS; ++jj)
                            {
                                if (!prlights[jj].flags.active)
                                    continue;

                                Gv_SetVarX(returnVar, jj);
                                insptr = pNext;
                                VM_Execute(0);
                            }
#endif
                            break;

                        case ITER_DRAWNSPRITES:
                        {
                            for (native_t ii = 0; ii < spritesortcnt; ii++)
                            {
                                Gv_SetVarX(returnVar, ii);
                                insptr = pNext;
                                VM_Execute(0);
                            }
                            break;
                        }

                        case ITER_SPRITESOFSECTOR:
                            if ((unsigned)nIndex >= MAXSECTORS)
                                goto badindex;
                            for (native_t jj = headspritesect[nIndex]; jj >= 0;)
                            {
                                int const kk = nextspritesect[jj];
                                Gv_SetVarX(returnVar, jj);
                                insptr = pNext;
                                VM_Execute(0);
                                jj = kk;
                            }
                            break;
                        case ITER_SPRITESOFSTATUS:
                            if ((unsigned)nIndex >= MAXSTATUS)
                                goto badindex;
                            for (native_t jj = headspritestat[nIndex]; jj >= 0;)
                            {
                                int const kk = nextspritestat[jj];
                                Gv_SetVarX(returnVar, jj);
                                insptr = pNext;
                                VM_Execute(0);
                                jj = kk;
                            }
                            break;
                        case ITER_WALLSOFSECTOR:
                            if ((unsigned)nIndex >= MAXSECTORS)
                                goto badindex;
                            for (native_t jj = sector[nIndex].wallptr, endwall = jj + sector[nIndex].wallnum - 1; jj <= endwall; jj++)
                            {
                                Gv_SetVarX(returnVar, jj);
                                insptr = pNext;
                                VM_Execute(0);
                            }
                            break;
                        case ITER_LOOPOFWALL:
                            if ((unsigned)nIndex >= (unsigned)numwalls)
                                goto badindex;
                            {
                                int jj = nIndex;
                                do
                                {
                                    Gv_SetVarX(returnVar, jj);
                                    insptr = pNext;
                                    VM_Execute(0);
                                    jj = wall[jj].point2;
                                } while (jj != nIndex);
                            }
                            break;
                        case ITER_RANGE:
                            for (native_t jj = 0; jj < nIndex; jj++)
                            {
                                Gv_SetVarX(returnVar, jj);
                                insptr = pNext;
                                VM_Execute(0);
                            }
                            break;
badindex:
                            OSD_Printf(OSD_ERROR "Line %d, for %s: index %d out of range!\n", g_errorLineNum, iter_tokens[iterType].token, nIndex);
                            vm.flags |= VM_RETURN;
                            continue;
                    }
                    insptr = pEnd;
                }
                dispatch(tw);

            vInstruction(CON_REDEFINEQUOTE):
                insptr++;
                {
                    int const strIndex  = *insptr++;
                    int const XstrIndex = *insptr++;

                    Bstrcpy(apStrings[strIndex], apXStrings[XstrIndex]);
                    dispatch(tw);
                }

            vInstruction(CON_GETTHISPROJECTILE):
                insptr++;
                {
                    tw                  = *insptr++;
                    int const spriteNum = (tw != g_thisActorVarID) ? Gv_GetVarX(tw) : vm.spriteNum;
                    int const labelNum  = *insptr++;

                    Gv_SetVarX(*insptr++, VM_GetActiveProjectile(spriteNum, labelNum));
                    dispatch(tw);
                }

            vInstruction(CON_SETTHISPROJECTILE):
                insptr++;
                {
                    tw                  = *insptr++;
                    int const spriteNum = (tw != g_thisActorVarID) ? Gv_GetVarX(tw) : vm.spriteNum;
                    int const labelNum  = *insptr++;

                    VM_SetActiveProjectile(spriteNum, labelNum, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_IFCANSHOOTTARGET):
            {
                if (vm.playerDist > 1024)
                {
                    int16_t temphit;

                    if ((tw = A_CheckHitSprite(vm.spriteNum, &temphit)) == (1 << 30))
                    {
                        VM_CONDITIONAL(1);
                        dispatch(tw);
                    }

                    int dist    = 768;
                    int angDiff = 16;

                    if (A_CheckEnemySprite(vm.pSprite) && vm.pSprite->xrepeat > 56)
                    {
                        dist    = 3084;
                        angDiff = 48;
                    }

#define CHECK(x)                                          \
    if (x >= 0 && sprite[x].picnum == vm.pSprite->picnum) \
    {                                                     \
        VM_CONDITIONAL(0);                                \
        dispatch(tw);                                         \
    }
#define CHECK2(x)                                      \
    do                                                 \
    {                                                  \
        vm.pSprite->ang += x;                          \
        tw = A_CheckHitSprite(vm.spriteNum, &temphit); \
        vm.pSprite->ang -= x;                          \
    } while (0)

                    if (tw > dist)
                    {
                        CHECK(temphit);
                        CHECK2(angDiff);

                        if (tw > dist)
                        {
                            CHECK(temphit);
                            CHECK2(-angDiff);

                            if (tw > 768)
                            {
                                CHECK(temphit);
                                VM_CONDITIONAL(1);
                                dispatch(tw);
                            }
                        }
                    }
                }
                VM_CONDITIONAL(1);
            }
                dispatch(tw);

#undef CHECK
#undef CHECK2

            vInstruction(CON_IFCANSEETARGET):
                tw = cansee(vm.pSprite->x, vm.pSprite->y, vm.pSprite->z - ((krand() & 41) << 8), vm.pSprite->sectnum, p.pos.x, p.pos.y,
                            p.pos.z /*-((krand()&41)<<8)*/, sprite[p.i].sectnum);
                VM_CONDITIONAL(tw);
                if (tw)
                    vm.pActor->timetosleep = SLEEPTIME;
                dispatch(tw);

            vInstruction(CON_IFACTION):
                VM_CONDITIONAL(AC_ACTION_ID(vm.pData) == *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFACTIONCOUNT):
                VM_CONDITIONAL(AC_ACTION_COUNT(vm.pData) >= *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFACTOR):
                VM_CONDITIONAL(vm.pSprite->picnum == *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFACTORNOTSTAYPUT):
                VM_CONDITIONAL(vm.pActor->stayput == -1);
                dispatch(tw);

            vInstruction(CON_IFAI):
                VM_CONDITIONAL(AC_AI_ID(vm.pData) == *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFBULLETNEAR):
                VM_CONDITIONAL(A_Dodge(vm.pSprite) == 1);
                dispatch(tw);

            vInstruction(CON_IFCEILINGDISTL):
                VM_CONDITIONAL((vm.pSprite->z - vm.pActor->ceilingz) <= (*(++insptr) << 8));
                dispatch(tw);

            vInstruction(CON_IFCLIENT):
                VM_CONDITIONAL(g_netClient != NULL);
                dispatch(tw);

            vInstruction(CON_IFCOUNT):
                VM_CONDITIONAL(AC_COUNT(vm.pData) >= *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFDEAD):
                VM_CONDITIONAL(vm.pSprite->extra <= 0);
                dispatch(tw);

            vInstruction(CON_IFFLOORDISTL):
                VM_CONDITIONAL((vm.pActor->floorz - vm.pSprite->z) <= (*(++insptr) << 8));
                dispatch(tw);

            vInstruction(CON_IFGAPZL):
                VM_CONDITIONAL(((vm.pActor->floorz - vm.pActor->ceilingz) >> 8) < *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFHITSPACE):
                VM_CONDITIONAL(TEST_SYNC_KEY(g_player[vm.playerNum].inputBits->bits, SK_OPEN));
                dispatch(tw);

            vInstruction(CON_IFHITWEAPON):
                VM_CONDITIONAL(A_IncurDamage(vm.spriteNum) >= 0);
                dispatch(tw);

            vInstruction(CON_IFINSPACE):
                VM_CONDITIONAL(G_CheckForSpaceCeiling(vm.pSprite->sectnum));
                dispatch(tw);

            vInstruction(CON_IFINWATER):
                VM_CONDITIONAL(sector[vm.pSprite->sectnum].lotag == ST_2_UNDERWATER);
                dispatch(tw);

            vInstruction(CON_IFONWATER):
                VM_CONDITIONAL(sector[vm.pSprite->sectnum].lotag == ST_1_ABOVE_WATER
                               && klabs(vm.pSprite->z - sector[vm.pSprite->sectnum].floorz) < ZOFFSET5);
                dispatch(tw);

            vInstruction(CON_IFMOVE):
                VM_CONDITIONAL(AC_MOVE_ID(vm.pData) == *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFMULTIPLAYER):
                VM_CONDITIONAL((g_netServer || g_netClient || ud.multimode > 1));
                dispatch(tw);

            vInstruction(CON_IFOUTSIDE):
                VM_CONDITIONAL(sector[vm.pSprite->sectnum].ceilingstat & 1);
                dispatch(tw);

            vInstruction(CON_IFPLAYBACKON):
                VM_CONDITIONAL(0);
                dispatch(tw);

            vInstruction(CON_IFPLAYERSL):
                VM_CONDITIONAL(numplayers < *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFSERVER):
                VM_CONDITIONAL(g_netServer != NULL);
                dispatch(tw);

            vInstruction(CON_IFSQUISHED):
                VM_CONDITIONAL(VM_CheckSquished());
                dispatch(tw);

            vInstruction(CON_IFSTRENGTH):
                VM_CONDITIONAL(vm.pSprite->extra <= *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFSPAWNEDBY):
            vInstruction(CON_IFWASWEAPON):
                VM_CONDITIONAL(vm.pActor->picnum == *(++insptr));
                dispatch(tw);

            vInstruction(CON_IFPDISTL):
                VM_CONDITIONAL(vm.playerDist < *(++insptr));
                if (vm.playerDist > MAXSLEEPDIST && vm.pActor->timetosleep == 0)
                    vm.pActor->timetosleep = SLEEPTIME;
                dispatch(tw);

            vInstruction(CON_IFPDISTG):
                VM_CONDITIONAL(vm.playerDist > *(++insptr));
                if (vm.playerDist > MAXSLEEPDIST && vm.pActor->timetosleep == 0)
                    vm.pActor->timetosleep = SLEEPTIME;
                dispatch(tw);

            vInstruction(CON_IFRESPAWN):
                if (A_CheckEnemySprite(vm.pSprite))
                    VM_CONDITIONAL(ud.respawn_monsters)
                else if (A_CheckInventorySprite(vm.pSprite))
                    VM_CONDITIONAL(ud.respawn_inventory)
                else
                    VM_CONDITIONAL(ud.respawn_items)
                dispatch(tw);

            vInstruction(CON_IFINOUTERSPACE):
                VM_CONDITIONAL(G_CheckForSpaceFloor(vm.pSprite->sectnum));
                dispatch(tw);

            vInstruction(CON_IFNOTMOVING):
                VM_CONDITIONAL((vm.pActor->movflag & 49152) > 16384);
                dispatch(tw);

            vInstruction(CON_IFCANSEE):
            {
                auto pSprite = (uspritetype *)&sprite[p.i];

// select sprite for monster to target
// if holoduke is on, let them target holoduke first.
//
#ifndef EDUKE32_STANDALONE
                if (p.holoduke_on >= 0)
                {
                    pSprite = (uspritetype *)&sprite[p.holoduke_on];
                    tw = cansee(vm.pSprite->x, vm.pSprite->y, vm.pSprite->z - (krand() & (ZOFFSET5 - 1)), vm.pSprite->sectnum, pSprite->x, pSprite->y,
                                pSprite->z, pSprite->sectnum);

                    if (tw == 0)
                    {
                        // they can't see player's holoduke
                        // check for player...
                        pSprite = (uspritetype *)&sprite[p.i];
                    }
                }
#endif
                // can they see player, (or player's holoduke)
                tw = cansee(vm.pSprite->x, vm.pSprite->y, vm.pSprite->z - (krand() & ((47 << 8))), vm.pSprite->sectnum, pSprite->x, pSprite->y,
                            pSprite->z - (24 << 8), pSprite->sectnum);

                if (tw == 0)
                {
                    // search around for target player
                    // also modifies 'target' x&y if found..

                    tw = (A_FurthestVisiblePoint(vm.spriteNum, pSprite, &vm.pActor->lastv) != -1);
                }
                else
                {
                    // else, they did see it.
                    // save where we were looking...
                    vm.pActor->lastv.x = pSprite->x;
                    vm.pActor->lastv.y = pSprite->y;
                }

                if (tw && (vm.pSprite->statnum == STAT_ACTOR || vm.pSprite->statnum == STAT_STANDABLE))
                    vm.pActor->timetosleep = SLEEPTIME;

                VM_CONDITIONAL(tw);
                dispatch(tw);
            }

            vInstruction(CON_AI):
                insptr++;
                // Following changed to use pointersizes
                AC_AI_ID(vm.pData)     = *insptr++;                         // Ai
                AC_ACTION_ID(vm.pData) = *(apScript + AC_AI_ID(vm.pData));  // Action

                // NOTE: "if" check added in r1155. It used to be a pointer though.
                if (AC_AI_ID(vm.pData))
                    AC_MOVE_ID(vm.pData) = *(apScript + AC_AI_ID(vm.pData) + 1);  // move

                vm.pSprite->hitag = *(apScript + AC_AI_ID(vm.pData) + 2);  // move flags

                AC_COUNT(vm.pData)        = 0;
                AC_ACTION_COUNT(vm.pData) = 0;
                AC_CURFRAME(vm.pData)     = 0;

                if (!A_CheckEnemySprite(vm.pSprite) || vm.pSprite->extra > 0)  // hack
                    if (vm.pSprite->hitag & random_angle)
                        vm.pSprite->ang = krand() & 2047;
                dispatch(tw);

            vInstruction(CON_ACTION):
                insptr++;
                AC_ACTION_COUNT(vm.pData) = 0;
                AC_CURFRAME(vm.pData)     = 0;
                AC_ACTION_ID(vm.pData)    = *insptr++;
                dispatch(tw);

            vInstruction(CON_ADDSTRENGTH):
                insptr++;
                vm.pSprite->extra += *insptr++;
                dispatch(tw);

            vInstruction(CON_STRENGTH):
                insptr++;
                vm.pSprite->extra = *insptr++;
                dispatch(tw);

            vInstruction(CON_IFGOTWEAPONCE):
                insptr++;

                if ((g_gametypeFlags[ud.coop] & GAMETYPE_WEAPSTAY) && (g_netServer || ud.multimode > 1))
                {
                    if (*insptr == 0)
                    {
                        int j = 0;
                        for (; j < p.weapreccnt; ++j)
                            if (p.weaprecs[j] == vm.pSprite->picnum)
                                break;

                        VM_CONDITIONAL(j < p.weapreccnt && vm.pSprite->owner == vm.spriteNum);
                        dispatch(tw);
                    }
                    else if (p.weapreccnt < MAX_WEAPONS)
                    {
                        p.weaprecs[p.weapreccnt++] = vm.pSprite->picnum;
                        VM_CONDITIONAL(vm.pSprite->owner == vm.spriteNum);
                        dispatch(tw);
                    }
                }
                VM_CONDITIONAL(0);
                dispatch(tw);

            vInstruction(CON_GETLASTPAL):
                insptr++;
                if (vm.pSprite->picnum == APLAYER)
                    vm.pSprite->pal = g_player[P_GetP(vm.pSprite)].ps->palookup;
                else
                {
                    if (vm.pSprite->pal == 1 && vm.pSprite->extra == 0)  // hack for frozen
                        vm.pSprite->extra++;
                    vm.pSprite->pal = vm.pActor->tempang;
                }
                vm.pActor->tempang = 0;
                dispatch(tw);

            vInstruction(CON_TOSSWEAPON):
                insptr++;
                // NOTE: assumes that current actor is APLAYER
                P_DropWeapon(P_GetP(vm.pSprite));
                dispatch(tw);

            vInstruction(CON_MIKESND):
                insptr++;
                if (EDUKE32_PREDICT_FALSE(((unsigned)vm.pSprite->yvel >= MAXSOUNDS)))
                {
                    CON_ERRPRINTF("invalid sound %d\n", vm.pUSprite->yvel);
                    dispatch(tw);
                }
                if (!S_CheckSoundPlaying(vm.pSprite->yvel))
                    A_PlaySound(vm.pSprite->yvel, vm.spriteNum);
                dispatch(tw);

            vInstruction(CON_PKICK):
                insptr++;

                if ((g_netServer || ud.multimode > 1) && vm.pSprite->picnum == APLAYER)
                {
                    if (g_player[otherp].ps->quick_kick == 0)
                        g_player[otherp].ps->quick_kick = 14;
                }
                else if (vm.pSprite->picnum != APLAYER && p.quick_kick == 0)
                    p.quick_kick = 14;
                dispatch(tw);

            vInstruction(CON_SIZETO):
                insptr++;

                tw = (*insptr++ - vm.pSprite->xrepeat) << 1;
                vm.pSprite->xrepeat += ksgn(tw);

                if ((vm.pSprite->picnum == APLAYER && vm.pSprite->yrepeat < 36) || *insptr < vm.pSprite->yrepeat
                    || ((vm.pSprite->yrepeat * (tilesiz[vm.pSprite->picnum].y + 8)) << 2) < (vm.pActor->floorz - vm.pActor->ceilingz))
                {
                    tw = ((*insptr) - vm.pSprite->yrepeat) << 1;
                    if (klabs(tw))
                        vm.pSprite->yrepeat += ksgn(tw);
                }

                insptr++;

                dispatch(tw);

            vInstruction(CON_SIZEAT):
                insptr++;
                vm.pSprite->xrepeat = (uint8_t)*insptr++;
                vm.pSprite->yrepeat = (uint8_t)*insptr++;
                dispatch(tw);

            vInstruction(CON_IFACTORSOUND):
                insptr++;
                {
                    int const spriteNum = Gv_GetVarX(*insptr++);
                    int const soundNum  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)soundNum >= MAXSOUNDS))
                    {
                        CON_ERRPRINTF("invalid sound %d\n", soundNum);
                        dispatch(tw);
                    }

                    insptr--;
                    VM_CONDITIONAL(A_CheckSoundPlaying(spriteNum, soundNum));
                }
                dispatch(tw);

            vInstruction(CON_IFSOUND):
                if (EDUKE32_PREDICT_FALSE((unsigned)*(++insptr) >= MAXSOUNDS))
                {
                    CON_ERRPRINTF("invalid sound %d\n", (int32_t)*insptr);
                    dispatch(tw);
                }
                VM_CONDITIONAL(S_CheckSoundPlaying(*insptr));
                //    VM_DoConditional(SoundOwner[*insptr][0].ow == vm.spriteNum);
                dispatch(tw);

            vInstruction(CON_STOPACTORSOUND):
                insptr++;
                {
                    int const spriteNum = Gv_GetVarX(*insptr++);
                    int const soundNum  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)soundNum >= MAXSOUNDS))
                    {
                        CON_ERRPRINTF("invalid sound %d\n", soundNum);
                        dispatch(tw);
                    }

                    if (A_CheckSoundPlaying(spriteNum, soundNum))
                        S_StopEnvSound(soundNum, spriteNum);

                    dispatch(tw);
                }

            vInstruction(CON_ACTORSOUND):
                insptr++;
                {
                    int const spriteNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.spriteNum;
                    int const soundNum  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)soundNum >= MAXSOUNDS))
                    {
                        CON_ERRPRINTF("invalid sound %d\n", soundNum);
                        dispatch(tw);
                    }

                    A_PlaySound(soundNum, spriteNum);

                    dispatch(tw);
                }

            vInstruction(CON_SETACTORSOUNDPITCH):
                insptr++;
                {
                    int const spriteNum = Gv_GetVarX(*insptr++);
                    int const soundNum  = Gv_GetVarX(*insptr++);
                    int const newPitch  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)soundNum >= MAXSOUNDS))
                    {
                        CON_ERRPRINTF("invalid sound %d\n", soundNum);
                        dispatch(tw);
                    }

                    S_ChangeSoundPitch(soundNum, spriteNum, newPitch);

                    dispatch(tw);
                }

            vInstruction(CON_TIP):
                insptr++;
                p.tipincs = GAMETICSPERSEC;
                dispatch(tw);

            vInstruction(CON_FALL):
                insptr++;
                VM_Fall(vm.spriteNum, vm.pSprite);
                dispatch(tw);

            vInstruction(CON_NULLOP): insptr++; dispatch(tw);

            vInstruction(CON_ADDAMMO):
                insptr++;
                {
                    int const weaponNum = *insptr++;
                    int const addAmount = *insptr++;

                    VM_AddAmmo(&p, weaponNum, addAmount);

                    dispatch(tw);
                }

            vInstruction(CON_MONEY):
                insptr++;
                A_SpawnMultiple(vm.spriteNum, MONEY, *insptr++);
                dispatch(tw);

            vInstruction(CON_MAIL):
                insptr++;
                A_SpawnMultiple(vm.spriteNum, MAIL, *insptr++);
                dispatch(tw);

            vInstruction(CON_SLEEPTIME):
                insptr++;
                vm.pActor->timetosleep = (int16_t)*insptr++;
                dispatch(tw);

            vInstruction(CON_PAPER):
                insptr++;
                A_SpawnMultiple(vm.spriteNum, PAPER, *insptr++);
                dispatch(tw);

            vInstruction(CON_ADDKILLS):
                insptr++;
                P_AddKills(&p, *insptr++);
                vm.pActor->stayput = -1;
                dispatch(tw);

            vInstruction(CON_LOTSOFGLASS):
                insptr++;
#ifndef EDUKE32_STANDALONE
                A_SpawnGlass(vm.spriteNum, *insptr++);
#else
                insptr++;
#endif
                dispatch(tw);

            vInstruction(CON_SPAWNWALLGLASS):
                insptr++;
                {
#ifndef EDUKE32_STANDALONE
                    int const wallNum   = Gv_GetVarX(*insptr++);
                    int const numShards = Gv_GetVarX(*insptr++);
                    A_SpawnWallGlass(vm.spriteNum, wallNum, numShards);
#else
                    Gv_GetVarX(*insptr++);
                    Gv_GetVarX(*insptr++);
#endif
                }
                dispatch(tw);

            vInstruction(CON_SPAWNWALLSTAINEDGLASS):
                insptr++;
                {
#ifndef EDUKE32_STANDALONE
                    int const wallNum   = Gv_GetVarX(*insptr++);
                    int const numShards = Gv_GetVarX(*insptr++);
                    A_SpawnRandomGlass(vm.spriteNum, wallNum, numShards);
#else
                    Gv_GetVarX(*insptr++);
                    Gv_GetVarX(*insptr++);
#endif
                }
                dispatch(tw);

            vInstruction(CON_SPAWNCEILINGGLASS):
                insptr++;
                {
#ifndef EDUKE32_STANDALONE
                    int const sectNum   = Gv_GetVarX(*insptr++);
                    int const numShards = Gv_GetVarX(*insptr++);
                    A_SpawnCeilingGlass(vm.spriteNum, sectNum, numShards);
#else
                    Gv_GetVarX(*insptr++);
                    Gv_GetVarX(*insptr++);
#endif
                }
                dispatch(tw);

            vInstruction(CON_KILLIT):
                insptr++;
                vm.flags |= VM_KILL;
                return;

            vInstruction(CON_DEBUG):
                insptr++;
                buildprint(*insptr++, "\n");
                dispatch(tw);

            vInstruction(CON_ENDOFGAME):
            vInstruction(CON_ENDOFLEVEL):
                insptr++;
                p.timebeforeexit  = *insptr++;
                p.customexitsound = -1;
                ud.eog = 1;
                dispatch(tw);

            vInstruction(CON_ADDPHEALTH):
                insptr++;

                {
                    if (p.newowner >= 0)
                        G_ClearCameraView(&p);

                    int newHealth = sprite[p.i].extra;

#ifndef EDUKE32_STANDALONE
                    if (vm.pSprite->picnum == ATOMICHEALTH)
                    {
                        if (newHealth > 0)
                            newHealth += *insptr;
                        if (newHealth > (p.max_player_health << 1))
                            newHealth = (p.max_player_health << 1);
                    }
                    else
#endif
                    {
                        if (newHealth > p.max_player_health && *insptr > 0)
                        {
                            insptr++;
                            dispatch(tw);
                        }
                        else
                        {
                            if (newHealth > 0)
                                newHealth += *insptr;
                            if (newHealth > p.max_player_health && *insptr > 0)
                                newHealth = p.max_player_health;
                        }
                    }

                    if (newHealth < 0)
                        newHealth = 0;

                    if (ud.god == 0)
                    {
                        if (*insptr > 0)
                        {
#ifndef EDUKE32_STANDALONE
                            if ((newHealth - *insptr) < (p.max_player_health >> 2) && newHealth >= (p.max_player_health >> 2))
                                A_PlaySound(DUKE_GOTHEALTHATLOW, p.i);
#endif
                            p.last_extra = newHealth;
                        }

                        sprite[p.i].extra = newHealth;
                    }
                }

                insptr++;
                dispatch(tw);

            vInstruction(CON_MOVE):
                insptr++;
                AC_COUNT(vm.pData)   = 0;
                AC_MOVE_ID(vm.pData) = *insptr++;
                vm.pSprite->hitag    = *insptr++;
                if (A_CheckEnemySprite(vm.pSprite) && vm.pSprite->extra <= 0)  // hack
                    dispatch(tw);
                if (vm.pSprite->hitag & random_angle)
                    vm.pSprite->ang = krand() & 2047;
                dispatch(tw);

            vInstruction(CON_ADDWEAPON):
                insptr++;
                {
                    int const weaponNum = Gv_GetVarX(*insptr++);
                    VM_AddWeapon(&p, weaponNum, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_SETASPECT):
                insptr++;
                {
                    int const xRange = Gv_GetVarX(*insptr++);
                    renderSetAspect(xRange, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_SSP):
                insptr++;
                {
                    int const spriteNum = Gv_GetVarX(*insptr++);
                    int const clipType  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }
                    A_SetSprite(spriteNum, clipType);
                    dispatch(tw);
                }

            vInstruction(CON_ACTIVATEBYSECTOR):
                insptr++;
                {
                    int const sectNum   = Gv_GetVarX(*insptr++);
                    int const spriteNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)sectNum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", sectNum);
                        dispatch(tw);
                    }
                    G_ActivateBySector(sectNum, spriteNum);
                    dispatch(tw);
                }

            vInstruction(CON_OPERATESECTORS):
                insptr++;
                {
                    int const sectNum   = Gv_GetVarX(*insptr++);
                    int const spriteNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)sectNum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", sectNum);
                        dispatch(tw);
                    }
                    G_OperateSectors(sectNum, spriteNum);
                    dispatch(tw);
                }

            vInstruction(CON_OPERATEACTIVATORS):
                insptr++;
                {
                    int const nTag      = Gv_GetVarX(*insptr++);
                    int const playerNum = (*insptr++ == g_thisActorVarID) ? vm.playerNum : Gv_GetVarX(insptr[-1]);

                    if (EDUKE32_PREDICT_FALSE((unsigned)playerNum >= (unsigned)g_mostConcurrentPlayers))
                    {
                        CON_ERRPRINTF("invalid player %d\n", playerNum);
                        dispatch(tw);
                    }
                    G_OperateActivators(nTag, playerNum);
                    dispatch(tw);
                }


            vInstruction(CON_CANSEESPR):
                insptr++;
                {
                    int const nSprite1 = Gv_GetVarX(*insptr++);
                    int const nSprite2 = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)nSprite1 >= MAXSPRITES || (unsigned)nSprite2 >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", (unsigned)nSprite1 >= MAXSPRITES ? nSprite1 : nSprite2);
                        dispatch(tw);
                    }

                    int const nResult = cansee(sprite[nSprite1].x, sprite[nSprite1].y, sprite[nSprite1].z, sprite[nSprite1].sectnum,
                                               sprite[nSprite2].x, sprite[nSprite2].y, sprite[nSprite2].z, sprite[nSprite2].sectnum);

                    Gv_SetVarX(*insptr++, nResult);
                    dispatch(tw);
                }

            vInstruction(CON_OPERATERESPAWNS):
                insptr++;
                G_OperateRespawns(Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_OPERATEMASTERSWITCHES):
                insptr++;
                G_OperateMasterSwitches(Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_CHECKACTIVATORMOTION):
                insptr++;
                aGameVars[g_returnVarID].global = G_CheckActivatorMotion(Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_INSERTSPRITEQ):
                insptr++;
                A_AddToDeleteQueue(vm.spriteNum);
                dispatch(tw);

            vInstruction(CON_QSTRLEN):
                insptr++;
                {
                    int const gameVar  = *insptr++;
                    int const quoteNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE(apStrings[quoteNum] == NULL))
                    {
                        CON_ERRPRINTF("null quote %d\n", quoteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(gameVar, Bstrlen(apStrings[quoteNum]));
                    dispatch(tw);
                }

            vInstruction(CON_QSTRDIM):
                insptr++;
                {
                    int const widthVar  = *insptr++;
                    int const heightVar = *insptr++;

                    struct
                    {
                        int32_t tileNum;
                        vec3_t  vect;
                        int32_t blockAngle, quoteNum, orientation;
                        vec2_t  offset, between;
                        int32_t f;
                        vec2_t  bound[2];
                    } v;
                    Gv_FillWithVars(v);

                    if (EDUKE32_PREDICT_FALSE(v.tileNum < 0 || v.tileNum + 127 >= MAXTILES))
                        CON_ERRPRINTF("invalid base tilenum %d\n", v.tileNum);
                    else if (EDUKE32_PREDICT_FALSE((unsigned)v.quoteNum >= MAXQUOTES || apStrings[v.quoteNum] == NULL))
                        CON_ERRPRINTF("invalid quote %d\n", v.quoteNum);
                    else
                    {
                        vec2_t dim = G_ScreenTextSize(v.tileNum, v.vect.x, v.vect.y, v.vect.z, v.blockAngle, apStrings[v.quoteNum], 2 | v.orientation,
                                                      v.offset.x, v.offset.y, v.between.x, v.between.y, v.f, v.bound[0].x, v.bound[0].y, v.bound[1].x,
                                                      v.bound[1].y);

                        Gv_SetVarX(widthVar, dim.x);
                        Gv_SetVarX(heightVar, dim.y);
                    }
                    dispatch(tw);
                }

            vInstruction(CON_HEADSPRITESTAT):
                insptr++;
                {
                    int const gameVar = *insptr++;
                    int const statNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)statNum > MAXSTATUS))
                    {
                        CON_ERRPRINTF("invalid status list %d\n", statNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(gameVar, headspritestat[statNum]);
                    dispatch(tw);
                }

            vInstruction(CON_PREVSPRITESTAT):
                insptr++;
                {
                    int const gameVar   = *insptr++;
                    int const spriteNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(gameVar, prevspritestat[spriteNum]);
                    dispatch(tw);
                }

            vInstruction(CON_NEXTSPRITESTAT):
                insptr++;
                {
                    int const gameVar   = *insptr++;
                    int const spriteNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(gameVar, nextspritestat[spriteNum]);
                    dispatch(tw);
                }

            vInstruction(CON_HEADSPRITESECT):
                insptr++;
                {
                    int const gameVar = *insptr++;
                    int const sectNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)sectNum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", sectNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(gameVar, headspritesect[sectNum]);
                    dispatch(tw);
                }

            vInstruction(CON_PREVSPRITESECT):
                insptr++;
                {
                    int const gameVar   = *insptr++;
                    int const spriteNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(gameVar, prevspritesect[spriteNum]);
                    dispatch(tw);
                }

            vInstruction(CON_NEXTSPRITESECT):
                insptr++;
                {
                    int const gameVar   = *insptr++;
                    int const spriteNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(gameVar, nextspritesect[spriteNum]);
                    dispatch(tw);
                }

            vInstruction(CON_GETKEYNAME):
                insptr++;
                {
                    int const quoteIndex = Gv_GetVarX(*insptr++);
                    int const gameFunc   = Gv_GetVarX(*insptr++);
                    int const funcPos    = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)quoteIndex >= MAXQUOTES || apStrings[quoteIndex] == NULL))
                    {
                        CON_ERRPRINTF("invalid quote %d\n", quoteIndex);
                        dispatch(tw);
                    }
                    else if (EDUKE32_PREDICT_FALSE((unsigned)gameFunc >= NUMGAMEFUNCTIONS))
                    {
                        CON_ERRPRINTF("invalid function %d\n", gameFunc);
                        dispatch(tw);
                    }
                    else
                    {
                        if (funcPos < 2)
                            Bstrcpy(tempbuf, KB_ScanCodeToString(ud.config.KeyboardKeys[gameFunc][funcPos]));
                        else
                        {
                            Bstrcpy(tempbuf, KB_ScanCodeToString(ud.config.KeyboardKeys[gameFunc][0]));

                            if (!*tempbuf)
                                Bstrcpy(tempbuf, KB_ScanCodeToString(ud.config.KeyboardKeys[gameFunc][1]));
                        }
                    }

                    if (*tempbuf)
                        Bstrcpy(apStrings[quoteIndex], tempbuf);

                    dispatch(tw);
                }

            vInstruction(CON_QSUBSTR):
                insptr++;
                {
                    struct
                    {
                        int32_t outputQuote, inputQuote, quotePos, quoteLength;
                    } v;
                    Gv_FillWithVars(v);

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.outputQuote >= MAXQUOTES || apStrings[v.outputQuote] == NULL
                                              || (unsigned)v.inputQuote >= MAXQUOTES
                                              || apStrings[v.inputQuote] == NULL))
                    {
                        CON_ERRPRINTF("invalid quote %d\n", apStrings[v.outputQuote] ? v.inputQuote : v.outputQuote);
                        dispatch(tw);
                    }

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.quotePos >= MAXQUOTELEN))
                    {
                        CON_ERRPRINTF("invalid position %d\n", v.quotePos);
                        dispatch(tw);
                    }

                    if (EDUKE32_PREDICT_FALSE(v.quoteLength < 0))
                    {
                        CON_ERRPRINTF("invalid length %d\n", v.quoteLength);
                        dispatch(tw);
                    }

                    char *      pOutput = apStrings[v.outputQuote];
                    char const *pInput  = apStrings[v.inputQuote];

                    while (*pInput && v.quotePos--)
                        pInput++;
                    while ((*pOutput = *pInput) && v.quoteLength--)
                    {
                        pOutput++;
                        pInput++;
                    }
                    *pOutput = '\0';

                    dispatch(tw);
                }

            vInstruction(CON_QSTRCMP):
                insptr++;
                {
                    int const quote1  = Gv_GetVarX(*insptr++);
                    int const quote2  = Gv_GetVarX(*insptr++);
                    int const gameVar = *insptr++;

                    if (EDUKE32_PREDICT_FALSE(apStrings[quote1] == NULL || apStrings[quote2] == NULL))
                    {
                        CON_ERRPRINTF("null quote %d\n", apStrings[quote1] ? quote2 : quote1);
                        dispatch(tw);
                    }

                    Gv_SetVarX(gameVar, strcmp(apStrings[quote1], apStrings[quote2]));
                    dispatch(tw);
                }

            vInstruction(CON_GETPNAME):
            vInstruction(CON_QSTRNCAT):
            vInstruction(CON_QSTRCAT):
            vInstruction(CON_QSTRCPY):
            vInstruction(CON_QGETSYSSTR):
                insptr++;
                {
                    int const q = Gv_GetVarX(*insptr++);
                    int j;
                    if (tw == CON_GETPNAME && *insptr == g_thisActorVarID)
                    {
                        j = vm.playerNum;
                        insptr++;
                    }
                    else
                        j = Gv_GetVarX(*insptr++);

                    switch (tw)
                    {
                        case CON_GETPNAME:
                            if (EDUKE32_PREDICT_FALSE((unsigned)q >= MAXQUOTES || apStrings[q] == NULL))
                            {
                                CON_ERRPRINTF("invalid quote %d\n", q);
                                break;
                            }
                            if (g_player[j].user_name[0])
                                Bstrcpy(apStrings[q], g_player[j].user_name);
                            else
                                Bsprintf(apStrings[q], "%d", j);
                            break;
                        case CON_QGETSYSSTR:
                            if (EDUKE32_PREDICT_FALSE((unsigned)q >= MAXQUOTES || apStrings[q] == NULL))
                            {
                                CON_ERRPRINTF("invalid quote %d\n", q);
                                dispatch(tw);
                            }
                            switch (j)
                            {
                                case STR_MAPNAME:
                                case STR_MAPFILENAME:
                                {
                                    int const levelNum = ud.volume_number * MAXLEVELS + ud.level_number;
                                    const char *pName;

                                    if (EDUKE32_PREDICT_FALSE((unsigned)levelNum >= ARRAY_SIZE(g_mapInfo)))
                                    {
                                        CON_ERRPRINTF("out of bounds map number (vol=%d, lev=%d)\n", ud.volume_number, ud.level_number);
                                        dispatch(tw);
                                    }

                                    pName = j == STR_MAPNAME ? g_mapInfo[levelNum].name : g_mapInfo[levelNum].filename;

                                    if (EDUKE32_PREDICT_FALSE(pName == NULL))
                                    {
                                        CON_ERRPRINTF("attempted access to %s of non-existent map (vol=%d, lev=%d)",
                                                      j == STR_MAPNAME ? "name" : "file name", ud.volume_number, ud.level_number);
                                        dispatch(tw);
                                    }

                                    Bstrcpy(apStrings[q], j == STR_MAPNAME ? g_mapInfo[levelNum].name : g_mapInfo[levelNum].filename);
                                    break;
                                }
                                case STR_PLAYERNAME:
                                    if (EDUKE32_PREDICT_FALSE((unsigned)vm.playerNum >= (unsigned)g_mostConcurrentPlayers))
                                    {
                                        CON_ERRPRINTF("invalid player %d\n", vm.playerNum);
                                        dispatch(tw);
                                    }
                                    Bstrcpy(apStrings[q], g_player[vm.playerNum].user_name);
                                    break;
                                case STR_VERSION:
                                    Bsprintf(tempbuf, HEAD2 " %s", s_buildRev);
                                    Bstrcpy(apStrings[q], tempbuf);
                                    break;
                                case STR_GAMETYPE: Bstrcpy(apStrings[q], g_gametypeNames[ud.coop]); break;
                                case STR_VOLUMENAME:
                                    if (EDUKE32_PREDICT_FALSE((unsigned)ud.volume_number >= MAXVOLUMES))
                                    {
                                        CON_ERRPRINTF("invalid volume %d\n", ud.volume_number);
                                        dispatch(tw);
                                    }
                                    Bstrcpy(apStrings[q], g_volumeNames[ud.volume_number]);
                                    break;
                                case STR_YOURTIME:        Bstrcpy(apStrings[q], G_PrintYourTime());     break;
                                case STR_PARTIME:         Bstrcpy(apStrings[q], G_PrintParTime());      break;
                                case STR_DESIGNERTIME:    Bstrcpy(apStrings[q], G_PrintDesignerTime()); break;
                                case STR_BESTTIME:        Bstrcpy(apStrings[q], G_PrintBestTime());     break;
                                case STR_USERMAPFILENAME: Bstrcpy(apStrings[q], boardfilename);         break;
                                default: CON_ERRPRINTF("invalid string index %d or %d\n", q, j); dispatch(tw);
                            }
                            break;
                        case CON_QSTRCAT:
                            if (EDUKE32_PREDICT_FALSE(apStrings[q] == NULL || apStrings[j] == NULL))
                                goto nullquote;
                            Bstrncat(apStrings[q], apStrings[j], (MAXQUOTELEN - 1) - Bstrlen(apStrings[q]));
                            break;
                        case CON_QSTRNCAT:
                            if (EDUKE32_PREDICT_FALSE(apStrings[q] == NULL || apStrings[j] == NULL))
                                goto nullquote;
                            Bstrncat(apStrings[q], apStrings[j], Gv_GetVarX(*insptr++));
                            break;
                        case CON_QSTRCPY:
                            if (EDUKE32_PREDICT_FALSE(apStrings[q] == NULL || apStrings[j] == NULL))
                                goto nullquote;
                            if (q != j)
                                Bstrcpy(apStrings[q], apStrings[j]);
                            break;
                        default:
                        nullquote:
                            CON_ERRPRINTF("invalid quote %d\n", apStrings[q] ? j : q);
                            dispatch(tw);
                    }
                    dispatch(tw);
                }

            vInstruction(CON_CHANGESPRITESECT):
                insptr++;
                {
                    int const spriteNum = Gv_GetVarX(*insptr++);
                    int const sectNum   = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES || (unsigned)sectNum >= MAXSECTORS))
                    {
                        CON_ERRPRINTF("invalid parameters: %d, %d\n", spriteNum, sectNum);
                        dispatch(tw);
                    }

                    if (sprite[spriteNum].sectnum == sectNum)
                        dispatch(tw);

                    changespritesect(spriteNum, sectNum);
                    dispatch(tw);
                }

            vInstruction(CON_CHANGESPRITESTAT):
                insptr++;
                {
                    int const spriteNum = Gv_GetVarX(*insptr++);
                    int const statNum   = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)spriteNum >= MAXSPRITES || (unsigned)statNum >= MAXSECTORS))
                    {
                        CON_ERRPRINTF("invalid parameters: %d, %d\n", spriteNum, statNum);
                        dispatch(tw);
                    }

                    if (sprite[spriteNum].statnum == statNum)
                        dispatch(tw);

                    /* initialize actor data when changing to an actor statnum because there's usually
                    garbage left over from being handled as a hard coded object */

                    if (sprite[spriteNum].statnum > STAT_ZOMBIEACTOR && (statNum == STAT_ACTOR || statNum == STAT_ZOMBIEACTOR))
                    {
                        auto pActor = &actor[spriteNum];
                        auto pSprite = &sprite[spriteNum];


                        Bmemset(&pActor->t_data, 0, sizeof pActor->t_data);

                        pActor->lastv       = { 0, 0 };
                        pActor->timetosleep = 0;
                        pActor->cgg         = 0;
                        pActor->movflag     = 0;
                        pActor->tempang     = 0;
                        pActor->dispicnum   = 0;
                        pActor->flags       = 0;
                        pSprite->hitag      = 0;

                        if (G_HaveActor(pSprite->picnum))
                        {
                            auto actorptr = g_tile[pSprite->picnum].execPtr;
                            // offsets
                            AC_ACTION_ID(pActor->t_data) = actorptr[1];
                            AC_MOVE_ID(pActor->t_data)   = actorptr[2];
                            AC_MOVFLAGS(pSprite, pActor) = actorptr[3];  // ai bits (movflags)
                        }
                    }

                    changespritestat(spriteNum, statNum);
                    dispatch(tw);
                }

            vInstruction(CON_STARTLEVEL):
                insptr++;  // skip command
                {
                    // from 'level' cheat in game.c (about line 6250)
                    int const volumeNum = Gv_GetVarX(*insptr++);
                    int const levelNum  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)volumeNum >= MAXVOLUMES || (unsigned)levelNum >= MAXLEVELS))
                    {
                        CON_ERRPRINTF("invalid parameters: %d, %d\n", volumeNum, levelNum);
                        dispatch(tw);
                    }

                    ud.m_volume_number = ud.volume_number = volumeNum;
                    ud.m_level_number = ud.level_number = levelNum;
                    // if (numplayers > 1 && g_netServer)
                    //    Net_NewGame(volnume,levnume);
                     //else
                    {
                        g_player[myconnectindex].ps->gm |= MODE_EOL;
                        ud.display_bonus_screen = 0;
                    }  // MODE_RESTART;

                    dispatch(tw);
                }

            vInstruction(CON_MYOSX):
            vInstruction(CON_MYOSPALX):
            vInstruction(CON_MYOS):
            vInstruction(CON_MYOSPAL):
                insptr++;
                {
                    struct
                    {
                        vec2_t  pos;
                        int32_t tilenum, shade, orientation;
                    } v;
                    Gv_FillWithVars(v);

                    switch (tw)
                    {
                        case CON_MYOS: VM_DrawTile(v.pos.x, v.pos.y, v.tilenum, v.shade, v.orientation); break;
                        case CON_MYOSPAL: VM_DrawTilePal(v.pos.x, v.pos.y, v.tilenum, v.shade, v.orientation, Gv_GetVarX(*insptr++)); break;
                        case CON_MYOSX: VM_DrawTileSmall(v.pos.x, v.pos.y, v.tilenum, v.shade, v.orientation); break;
                        case CON_MYOSPALX: VM_DrawTilePalSmall(v.pos.x, v.pos.y, v.tilenum, v.shade, v.orientation, Gv_GetVarX(*insptr++)); break;
                    }
                    dispatch(tw);
                }

            vInstruction(CON_DISPLAYRAND):
                insptr++;
                Gv_SetVarX(*insptr++, system_15bit_rand());
                dispatch(tw);

            vInstruction(CON_DRAGPOINT):
                insptr++;
                {
                    int const wallNum = Gv_GetVarX(*insptr++);
                    vec2_t    n;
                    Gv_FillWithVars(n);

                    if (EDUKE32_PREDICT_FALSE((unsigned)wallNum >= (unsigned)numwalls))
                    {
                        CON_ERRPRINTF("invalid wall %d\n", wallNum);
                        dispatch(tw);
                    }

                    dragpoint(wallNum, n.x, n.y, 0);
                    dispatch(tw);
                }

            vInstruction(CON_LDIST):
            vInstruction(CON_DIST):
                insptr++;
                {
                    int const out = *insptr++;
                    vec2_t    in;
                    Gv_FillWithVars(in);

                    if (EDUKE32_PREDICT_FALSE((unsigned)in.x >= MAXSPRITES || (unsigned)in.y >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d, %d\n", in.x, in.y);
                        dispatch(tw);
                    }

                    Gv_SetVarX(out, (tw == CON_LDIST ? ldist : dist)(&sprite[in.x], &sprite[in.y]));
                    dispatch(tw);
                }

            vInstruction(CON_GETANGLE):
            vInstruction(CON_GETINCANGLE):
                insptr++;
                {
                    int const out = *insptr++;
                    vec2_t    in;
                    Gv_FillWithVars(in);
                    Gv_SetVarX(out, (tw == CON_GETANGLE ? getangle : G_GetAngleDelta)(in.x, in.y));
                    dispatch(tw);
                }

            vInstruction(CON_MULSCALE):
            vInstruction(CON_DIVSCALE):
                insptr++;
                {
                    int const out = *insptr++;
                    vec3_t    in;
                    Gv_FillWithVars(in);

                    if (tw == CON_MULSCALE)
                        Gv_SetVarX(out, mulscale(in.x, in.y, in.z));
                    else
                        Gv_SetVarX(out, divscale(in.x, in.y, in.z));

                    dispatch(tw);
                }

            vInstruction(CON_SCALEVAR):
                insptr++;
                {
                    int const out = *insptr++;
                    vec3_t    in;
                    Gv_FillWithVars(in);
                    Gv_SetVarX(out, scale(in.x, in.y, in.z));
                    dispatch(tw);
                }

            vInstruction(CON_INITTIMER):
                insptr++;
                G_InitTimer(Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_NEXTSECTORNEIGHBORZ):
                insptr++;
                {
                    int32_t params[4];
                    Gv_FillWithVars(params);
                    aGameVars[g_returnVarID].global = nextsectorneighborz(params[0], params[1], params[2], params[3]);
                }
                dispatch(tw);

            vInstruction(CON_MOVESECTOR):
                insptr++;
                A_MoveSector(Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_TIME): insptr += 2; dispatch(tw);

            vInstruction(CON_ESPAWN):
            vInstruction(CON_EQSPAWN):
            vInstruction(CON_QSPAWN):
                insptr++;
                {
                    int const tileNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)vm.pSprite->sectnum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", vm.pUSprite->sectnum);
                        dispatch(tw);
                    }

                    int const spriteNum = A_Spawn(vm.spriteNum, tileNum);

                    switch (tw)
                    {
                        case CON_EQSPAWN:
                            if (spriteNum != -1)
                                A_AddToDeleteQueue(spriteNum);
                            fallthrough__;
                        case CON_ESPAWN: aGameVars[g_returnVarID].global = spriteNum; break;
                        case CON_QSPAWN:
                            if (spriteNum != -1)
                                A_AddToDeleteQueue(spriteNum);
                            break;
                    }
                    dispatch(tw);
                }

            vInstruction(CON_SHOOT):
            vInstruction(CON_ESHOOT):
                insptr++;
                {
                    int j = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)vm.pSprite->sectnum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", vm.pUSprite->sectnum);
                        dispatch(tw);
                    }

                    j = A_Shoot(vm.spriteNum, j);

                    if (tw == CON_ESHOOT)
                        aGameVars[g_returnVarID].global = j;

                    dispatch(tw);
                }

            vInstruction(CON_EZSHOOT):
            vInstruction(CON_ZSHOOT):
                insptr++;
                {
                    int const zvel = (int16_t)Gv_GetVarX(*insptr++);
                    int       j    = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)vm.pSprite->sectnum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", vm.pUSprite->sectnum);
                        dispatch(tw);
                    }

                    j = A_ShootWithZvel(vm.spriteNum, j, zvel);

                    if (tw == CON_EZSHOOT)
                        aGameVars[g_returnVarID].global = j;

                    dispatch(tw);
                }

            vInstruction(CON_CMENU):
                insptr++;
                Menu_Change(Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_SOUND):
            vInstruction(CON_STOPSOUND):
            vInstruction(CON_SOUNDONCE):
            vInstruction(CON_GLOBALSOUND):
            vInstruction(CON_SCREENSOUND):
                insptr++;
                {
                    int const soundNum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)soundNum >= MAXSOUNDS))
                    {
                        CON_ERRPRINTF("invalid sound %d\n", soundNum);
                        dispatch(tw);
                    }

                    switch (tw)
                    {
                        case CON_SOUNDONCE:
                            if (!S_CheckSoundPlaying(soundNum))
                            {
                                fallthrough__;
                                case CON_SOUND: A_PlaySound((int16_t)soundNum, vm.spriteNum);
                            }
                            dispatch(tw);
                        case CON_GLOBALSOUND: A_PlaySound((int16_t)soundNum, g_player[screenpeek].ps->i); dispatch(tw);
                        case CON_STOPSOUND:
                            if (S_CheckSoundPlaying(soundNum))
                                S_StopSound((int16_t)soundNum);
                            dispatch(tw);
                        case CON_SCREENSOUND: S_PlaySound(soundNum); dispatch(tw);
                    }
                }
                dispatch(tw);

            vInstruction(CON_STARTCUTSCENE):
            vInstruction(CON_IFCUTSCENE):
                insptr++;
                {
                    int const nQuote = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)nQuote >= MAXQUOTES || apStrings[nQuote] == NULL))
                    {
                        CON_ERRPRINTF("invalid quote %d for anim!\n", nQuote);
                        dispatch(tw);
                    }

                    if (tw == CON_IFCUTSCENE)
                    {
                        insptr--;
                        VM_CONDITIONAL(g_animPtr == Anim_Find(apStrings[nQuote]));
                        dispatch(tw);
                    }

                    tw = p.palette;
                    I_ClearAllInput();
                    Anim_Play(apStrings[nQuote]);
                    P_SetGamePalette(&p, tw, 2 + 16);
                    dispatch(tw);
                }

            vInstruction(CON_STARTSCREEN):
                insptr++;
                I_ClearAllInput();
                Screen_Play();
                dispatch(tw);

            vInstruction(CON_GUNIQHUDID):
                insptr++;
                {
                    tw = Gv_GetVarX(*insptr++);
                    if (EDUKE32_PREDICT_FALSE((unsigned)tw >= MAXUNIQHUDID - 1))
                        CON_ERRPRINTF("invalid value %d\n", (int)tw);
                    else
                        guniqhudid = tw;

                    dispatch(tw);
                }

            vInstruction(CON_SAVEGAMEVAR):
            vInstruction(CON_READGAMEVAR):
            {
                int32_t nValue = 0;
                insptr++;
                if (ud.config.scripthandle < 0)
                {
                    insptr++;
                    dispatch(tw);
                }
                switch (tw)
                {
                    case CON_SAVEGAMEVAR:
                        nValue = Gv_GetVarX(*insptr);
                        SCRIPT_PutNumber(ud.config.scripthandle, "Gamevars", aGameVars[*insptr++].szLabel, nValue, FALSE, FALSE);
                        break;
                    case CON_READGAMEVAR:
                        SCRIPT_GetNumber(ud.config.scripthandle, "Gamevars", aGameVars[*insptr].szLabel, &nValue);
                        Gv_SetVarX(*insptr++, nValue);
                        break;
                }
                dispatch(tw);
            }

            vInstruction(CON_SHOWVIEW):
            vInstruction(CON_SHOWVIEWUNBIASED):
            vInstruction(CON_SHOWVIEWQ16):
            vInstruction(CON_SHOWVIEWQ16UNBIASED):
                insptr++;
                {
                    struct
                    {
                        vec3_t  vec;
                        int32_t params[3];
                        vec2_t  scrn[2];
                    } v;
                    Gv_FillWithVars(v);

                    if (EDUKE32_PREDICT_FALSE(v.scrn[0].x < 0 || v.scrn[0].y < 0 || v.scrn[1].x >= 320 || v.scrn[1].y >= 200))
                    {
                        CON_ERRPRINTF("incorrect coordinates\n");
                        dispatch(tw);
                    }

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.params[2] >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", v.params[2]);
                        dispatch(tw);
                    }

                    if (tw != CON_SHOWVIEWQ16 && tw != CON_SHOWVIEWQ16UNBIASED)
                    {
                        v.params[0] <<= 16;
                        v.params[1] <<= 16;
                    }

                    G_ShowView(v.vec, v.params[0], v.params[1], v.params[2], v.scrn[0].x, v.scrn[0].y, v.scrn[1].x, v.scrn[1].y,
                               (tw != CON_SHOWVIEW && tw != CON_SHOWVIEWQ16));

                    dispatch(tw);
                }

            vInstruction(CON_ROTATESPRITEA):
            vInstruction(CON_ROTATESPRITE16):
            vInstruction(CON_ROTATESPRITE):
                insptr++;
                {
                    struct
                    {
                        vec3_t  pos;
                        int32_t ang, tilenum, shade, pal, orientation;
                    } v;
                    Gv_FillWithVars(v);

                    int32_t alpha = (tw == CON_ROTATESPRITEA) ? Gv_GetVarX(*insptr++) : 0;

                    vec2_t bound[2];
                    Gv_FillWithVars(bound);

                    if (tw != CON_ROTATESPRITE16 && !(v.orientation & ROTATESPRITE_FULL16))
                    {
                        v.pos.x <<= 16;
                        v.pos.y <<= 16;
                    }

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.tilenum >= MAXTILES))
                    {
                        CON_ERRPRINTF("invalid tilenum %d\n", v.tilenum);
                        dispatch(tw);
                    }

                    int32_t blendidx = 0;

                    NEG_ALPHA_TO_BLEND(alpha, blendidx, v.orientation);

                    rotatesprite_(v.pos.x, v.pos.y, v.pos.z, v.ang, v.tilenum, v.shade, v.pal, 2 | (v.orientation & (ROTATESPRITE_MAX - 1)), alpha,
                                  blendidx, bound[0].x, bound[0].y, bound[1].x, bound[1].y);
                    dispatch(tw);
                }

            vInstruction(CON_GAMETEXT):
            vInstruction(CON_GAMETEXTZ):
                insptr++;
                {
                    struct
                    {
                        int32_t tilenum;
                        vec2_t  pos;
                        int32_t nQuote, shade, pal, orientation;
                        vec2_t  bound[2];
                    } v;
                    Gv_FillWithVars(v);

                    int32_t const z = (tw == CON_GAMETEXTZ) ? Gv_GetVarX(*insptr++) : 65536;

                    if (EDUKE32_PREDICT_FALSE(v.tilenum < 0 || v.tilenum + 127 >= MAXTILES))
                    {
                        CON_ERRPRINTF("invalid base tilenum %d\n", v.tilenum);
                        dispatch(tw);
                    }

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.nQuote >= MAXQUOTES || apStrings[v.nQuote] == NULL))
                    {
                        CON_ERRPRINTF("invalid quote %d\n", v.nQuote);
                        dispatch(tw);
                    }

                    G_PrintGameText(v.tilenum, v.pos.x >> 1, v.pos.y, apStrings[v.nQuote], v.shade, v.pal, v.orientation & (ROTATESPRITE_MAX - 1),
                                    v.bound[0].x, v.bound[0].y, v.bound[1].x, v.bound[1].y, z, 0);
                    dispatch(tw);
                }

            vInstruction(CON_DIGITALNUMBER):
            vInstruction(CON_DIGITALNUMBERZ):
                insptr++;
                {
                    struct
                    {
                        int32_t tilenum;
                        vec2_t  pos;
                        int32_t nQuote, shade, pal, orientation;
                        vec2_t  bound[2];
                    } v;
                    Gv_FillWithVars(v);

                    int32_t const nZoom = (tw == CON_DIGITALNUMBERZ) ? Gv_GetVarX(*insptr++) : 65536;

                    // NOTE: '-' not taken into account, but we have rotatesprite() bound check now anyway
                    if (EDUKE32_PREDICT_FALSE(v.tilenum < 0 || v.tilenum + 9 >= MAXTILES))
                    {
                        CON_ERRPRINTF("invalid base tilenum %d\n", v.tilenum);
                        dispatch(tw);
                    }

                    G_DrawTXDigiNumZ(v.tilenum, v.pos.x, v.pos.y, v.nQuote, v.shade, v.pal, v.orientation & (ROTATESPRITE_MAX - 1), v.bound[0].x,
                                     v.bound[0].y, v.bound[1].x, v.bound[1].y, nZoom);
                    dispatch(tw);
                }

            vInstruction(CON_MINITEXT):
                insptr++;
                {
                    struct
                    {
                        vec2_t  pos;
                        int32_t nQuote, shade, pal;
                    } v;
                    Gv_FillWithVars(v);

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.nQuote >= MAXQUOTES || apStrings[v.nQuote] == NULL))
                    {
                        CON_ERRPRINTF("invalid quote %d\n", v.nQuote);
                        dispatch(tw);
                    }

                    minitextshade(v.pos.x, v.pos.y, apStrings[v.nQuote], v.shade, v.pal, 2 + 8 + 16);
                    dispatch(tw);
                }

            vInstruction(CON_SCREENTEXT):
                insptr++;
                {
                    struct
                    {
                        int32_t tilenum;
                        vec3_t  v;
                        int32_t blockangle, charangle, nQuote, shade, pal, orientation, alpha;
                        vec2_t  spacing, between;
                        int32_t nFlags;
                        vec2_t  bound[2];
                    } v;
                    Gv_FillWithVars(v);

                    if (EDUKE32_PREDICT_FALSE(v.tilenum < 0 || v.tilenum + 127 >= MAXTILES))
                    {
                        CON_ERRPRINTF("invalid base tilenum %d\n", v.tilenum);
                        dispatch(tw);
                    }

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.nQuote >= MAXQUOTES || apStrings[v.nQuote] == NULL))
                    {
                        CON_ERRPRINTF("invalid quote %d\n", v.nQuote);
                        dispatch(tw);
                    }

                    G_ScreenText(v.tilenum, v.v.x, v.v.y, v.v.z, v.blockangle, v.charangle, apStrings[v.nQuote], v.shade, v.pal,
                                 2 | (v.orientation & (ROTATESPRITE_MAX - 1)), v.alpha, v.spacing.x, v.spacing.y, v.between.x, v.between.y, v.nFlags,
                                 v.bound[0].x, v.bound[0].y, v.bound[1].x, v.bound[1].y);
                    dispatch(tw);
                }

            vInstruction(CON_GETZRANGE):
                insptr++;
                {
                    struct
                    {
                        vec3_t  vect;
                        int32_t sectNum;
                    } v;
                    Gv_FillWithVars(v);

                    int const ceilzvar   = *insptr++;
                    int const ceilhitvar = *insptr++;
                    int const florzvar   = *insptr++;
                    int const florhitvar = *insptr++;

                    struct
                    {
                        int32_t walldist, clipmask;
                    } v2;
                    Gv_FillWithVars(v2);

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.sectNum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", v.sectNum);
                        dispatch(tw);
                    }

                    int32_t ceilz, ceilhit, florz, florhit;

                    getzrange(&v.vect, v.sectNum, &ceilz, &ceilhit, &florz, &florhit, v2.walldist, v2.clipmask);
                    Gv_SetVarX(ceilzvar, ceilz);
                    Gv_SetVarX(ceilhitvar, ceilhit);
                    Gv_SetVarX(florzvar, florz);
                    Gv_SetVarX(florhitvar, florhit);

                    dispatch(tw);
                }

            vInstruction(CON_SECTSETINTERPOLATION):
            vInstruction(CON_SECTCLEARINTERPOLATION):
                insptr++;
                {
                    int const sectnum = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)sectnum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", sectnum);
                        dispatch(tw);
                    }

                    if (tw == CON_SECTSETINTERPOLATION)
                        Sect_SetInterpolation(sectnum);
                    else
                        Sect_ClearInterpolation(sectnum);

                    dispatch(tw);
                }

            vInstruction(CON_CALCHYPOTENUSE):
                insptr++;
                {
                    int32_t returnVar = *insptr++;
                    vec2_t  da;
                    Gv_FillWithVars(da);
                    int64_t const hypsq = (int64_t)da.x * da.x + (int64_t)da.y * da.y;

                    Gv_SetVarX(returnVar, (hypsq > (int64_t)INT32_MAX) ? (int32_t)sqrt((double)hypsq) : ksqrt((uint32_t)hypsq));
                    dispatch(tw);
                }

            vInstruction(CON_LINEINTERSECT):
            vInstruction(CON_RAYINTERSECT):
                insptr++;
                {
                    struct
                    {
                        vec3_t vec[2];
                        vec2_t vec2[2];
                    } v;
                    Gv_FillWithVars(v);

                    int const intxvar = *insptr++;
                    int const intyvar = *insptr++;
                    int const intzvar = *insptr++;
                    int const retvar  = *insptr++;
                    vec3_t    in;

                    int ret = ((tw == CON_LINEINTERSECT) ? lintersect : rayintersect)(v.vec[0].x, v.vec[0].y, v.vec[0].z, v.vec[1].x, v.vec[1].y,
                                                                                      v.vec[1].z, v.vec2[0].x, v.vec2[0].y, v.vec2[1].x, v.vec2[1].y,
                                                                                      &in.x, &in.y, &in.z);

                    Gv_SetVarX(retvar, ret);

                    if (ret)
                    {
                        Gv_SetVarX(intxvar, in.x);
                        Gv_SetVarX(intyvar, in.y);
                        Gv_SetVarX(intzvar, in.z);
                    }

                    dispatch(tw);
                }

            vInstruction(CON_CLIPMOVE):
            vInstruction(CON_CLIPMOVENOSLIDE):
                insptr++;
                {
                    typedef struct
                    {
                        int32_t w, f, c;
                    } vec3dist_t;

                    int const returnVar = *insptr++;
                    int const xReturn   = *insptr++;
                    int const yReturn   = *insptr++;

                    insptr -= 2;

                    typedef struct
                    {
                        vec3_t     vec3;
                        int32_t    sectNum32;
                        vec2_t     vec2;
                        vec3dist_t dist;
                        int32_t    clipMask;
                    } clipmoveparams_t;

                    int32_t const sectReturn = insptr[offsetof(clipmoveparams_t, sectNum32) / sizeof(int32_t)];

                    clipmoveparams_t v;
                    Gv_FillWithVars(v);
                    int16_t sectNum = v.sectNum32;

                    if (EDUKE32_PREDICT_FALSE((unsigned)sectNum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", sectNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(
                    returnVar,
                    clipmovex(&v.vec3, &sectNum, v.vec2.x, v.vec2.y, v.dist.w, v.dist.f, v.dist.c, v.clipMask, (tw == CON_CLIPMOVENOSLIDE)));
                    Gv_SetVarX(sectReturn, v.sectNum32);
                    Gv_SetVarX(xReturn, v.vec3.x);
                    Gv_SetVarX(yReturn, v.vec3.y);

                    dispatch(tw);
                }

            vInstruction(CON_HITSCAN):
                insptr++;
                {
                    struct
                    {
                        vec3_t  origin;
                        int32_t sectnum;
                        vec3_t  vect;
                    } v;
                    Gv_FillWithVars(v);

                    int const sectReturn   = *insptr++;
                    int const wallReturn   = *insptr++;
                    int const spriteReturn = *insptr++;
                    int const xReturn      = *insptr++;
                    int const yReturn      = *insptr++;
                    int const zReturn      = *insptr++;
                    int const clipType     = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.sectnum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", v.sectnum);
                        dispatch(tw);
                    }

                    hitdata_t hit;
                    hitscan(&v.origin, v.sectnum, v.vect.x, v.vect.y, v.vect.z, &hit, clipType);

                    Gv_SetVarX(sectReturn, hit.sect);
                    Gv_SetVarX(wallReturn, hit.wall);
                    Gv_SetVarX(spriteReturn, hit.sprite);
                    Gv_SetVarX(xReturn, hit.pos.x);
                    Gv_SetVarX(yReturn, hit.pos.y);
                    Gv_SetVarX(zReturn, hit.pos.z);
                    dispatch(tw);
                }

            vInstruction(CON_CANSEE):
                insptr++;
                {
                    struct
                    {
                        vec3_t  vec1;
                        int32_t firstSector;
                        vec3_t  vec2;
                        int32_t secondSector;
                    } v;
                    Gv_FillWithVars(v);

                    int const returnVar = *insptr++;

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.firstSector >= (unsigned)numsectors || (unsigned)v.secondSector >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", (unsigned)v.firstSector >= (unsigned)numsectors ? v.firstSector : v.secondSector);
                        dispatch(tw);
                    }

                    Gv_SetVarX(returnVar, cansee(v.vec1.x, v.vec1.y, v.vec1.z, v.firstSector, v.vec2.x, v.vec2.y, v.vec2.z, v.secondSector));
                    dispatch(tw);
                }

            vInstruction(CON_ROTATEPOINT):
                insptr++;
                {
                    struct
                    {
                        vec2_t  point[2];
                        int32_t angle;
                    } v;
                    Gv_FillWithVars(v);

                    int const xReturn = *insptr++;
                    int const yReturn = *insptr++;
                    vec2_t    result;

                    rotatepoint(v.point[0], v.point[1], v.angle, &result);

                    Gv_SetVarX(xReturn, result.x);
                    Gv_SetVarX(yReturn, result.y);
                    dispatch(tw);
                }

            vInstruction(CON_NEARTAG):
                insptr++;
                {
                    //             neartag(int32_t x, int32_t y, int32_t z, short sectnum, short ang,  //Starting position & angle
                    //                     short *neartagsector,   //Returns near sector if sector[].tag != 0
                    //                     short *neartagwall,     //Returns near wall if wall[].tag != 0
                    //                     short *neartagsprite,   //Returns near sprite if sprite[].tag != 0
                    //                     int32_t *neartaghitdist,   //Returns actual distance to object (scale: 1024=largest grid size)
                    //                     int32_t neartagrange,      //Choose maximum distance to scan (scale: 1024=largest grid size)
                    //                     char tagsearch)         //1-lotag only, 2-hitag only, 3-lotag&hitag

                    struct
                    {
                        vec3_t  point;
                        int32_t sectNum, nAngle;
                    } v;
                    Gv_FillWithVars(v);

                    int const sectReturn   = *insptr++;
                    int const wallReturn   = *insptr++;
                    int const spriteReturn = *insptr++;
                    int const distReturn   = *insptr++;

                    struct
                    {
                        int32_t tagRange, tagSearch;
                    } v2;
                    Gv_FillWithVars(v2);

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.sectNum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("Invalid sector %d\n", v.sectNum);
                        dispatch(tw);
                    }

                    int16_t neartagsector, neartagwall, neartagsprite;
                    int32_t neartaghitdist;

                    neartag(v.point.x, v.point.y, v.point.z, v.sectNum, v.nAngle, &neartagsector, &neartagwall, &neartagsprite, &neartaghitdist,
                            v2.tagRange, v2.tagSearch, NULL);

                    Gv_SetVarX(sectReturn, neartagsector);
                    Gv_SetVarX(wallReturn, neartagwall);
                    Gv_SetVarX(spriteReturn, neartagsprite);
                    Gv_SetVarX(distReturn, neartaghitdist);
                    dispatch(tw);
                }

            vInstruction(CON_GETTIMEDATE):
                insptr++;
                {
                    int32_t values[8];
                    G_GetTimeDate(values);

                    for (int value : values)
                        Gv_SetVarX(*insptr++, value);

                    dispatch(tw);
                }

            vInstruction(CON_MOVESPRITE):
                insptr++;
                {
                    struct
                    {
                        int32_t spriteNum;
                        vec3_t  vect;
                        int32_t clipType;
                    } v;
                    Gv_FillWithVars(v);

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", v.spriteNum);
                        dispatch(tw);
                    }

                    Gv_SetVarX(*insptr++, A_MoveSprite(v.spriteNum, &v.vect, v.clipType));
                    dispatch(tw);
                }

            vInstruction(CON_SETSPRITE):
                insptr++;
                {
                    struct
                    {
                        int32_t spriteNum;
                        vec3_t  vect;
                    } v;
                    Gv_FillWithVars(v);

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.spriteNum >= MAXSPRITES))
                    {
                        CON_ERRPRINTF("invalid sprite %d\n", v.spriteNum);
                        dispatch(tw);
                    }
                    setsprite(v.spriteNum, &v.vect);
                    dispatch(tw);
                }

            vInstruction(CON_GETFLORZOFSLOPE):
            vInstruction(CON_GETCEILZOFSLOPE):
                insptr++;
                {
                    struct
                    {
                        int32_t sectNum;
                        vec2_t  vect;
                    } v;
                    Gv_FillWithVars(v);

                    if (EDUKE32_PREDICT_FALSE((unsigned)v.sectNum >= (unsigned)numsectors))
                    {
                        CON_ERRPRINTF("invalid sector %d\n", v.sectNum);
                        dispatch(tw);
                    }
                    Gv_SetVarX(*insptr++, (tw == CON_GETFLORZOFSLOPE ? getflorzofslope : getceilzofslope)(v.sectNum, v.vect.x, v.vect.y));
                    dispatch(tw);
                }

            vInstruction(CON_UPDATESECTOR):
                insptr++;
                {
                    vec2_t vect = { 0, 0 };
                    Gv_FillWithVars(vect);

                    int const returnVar = *insptr++;
                    int16_t   sectNum   = sprite[vm.spriteNum].sectnum;

                    updatesector(vect.x, vect.y, &sectNum);
                    Gv_SetVarX(returnVar, sectNum);
                    dispatch(tw);
                }

            vInstruction(CON_UPDATESECTORZ):
                insptr++;
                {
                    vec3_t vect = { 0, 0, 0 };
                    Gv_FillWithVars(vect);

                    int const returnVar = *insptr++;
                    int16_t   sectNum   = sprite[vm.spriteNum].sectnum;

                    updatesectorz(vect.x, vect.y, vect.z, &sectNum);
                    Gv_SetVarX(returnVar, sectNum);
                    dispatch(tw);
                }

            vInstruction(CON_SPAWN):
                insptr++;
                if ((unsigned)vm.pSprite->sectnum >= MAXSECTORS)
                {
                    CON_ERRPRINTF("invalid sector %d\n", vm.pUSprite->sectnum);
                    dispatch(tw);
                }
                A_Spawn(vm.spriteNum, *insptr++);
                dispatch(tw);

            vInstruction(CON_RESETACTIONCOUNT):
                insptr++;
                AC_ACTION_COUNT(vm.pData) = 0;
                dispatch(tw);

            vInstruction(CON_DEBRIS):
                insptr++;
                {
#ifndef EDUKE32_STANDALONE
                    int debrisTile = *insptr++;

                    if ((unsigned)vm.pSprite->sectnum < MAXSECTORS)
                        for (native_t cnt = (*insptr) - 1; cnt >= 0; cnt--)
                        {
                            int const tileOffset = (vm.pSprite->picnum == BLIMP && debrisTile == SCRAP1) ? 0 : (krand() % 3);

                            int const spriteNum = A_InsertSprite(vm.pSprite->sectnum, vm.pSprite->x + (krand() & 255) - 128,
                                                                 vm.pSprite->y + (krand() & 255) - 128, vm.pSprite->z - (8 << 8) - (krand() & 8191),
                                                                 debrisTile + tileOffset, vm.pSprite->shade, 32 + (krand() & 15), 32 + (krand() & 15),
                                                                 krand() & 2047, (krand() & 127) + 32, -(krand() & 2047), vm.spriteNum, 5);

                            sprite[spriteNum].yvel = (vm.pSprite->picnum == BLIMP && debrisTile == SCRAP1) ? g_blimpSpawnItems[cnt % 14] : -1;
                            sprite[spriteNum].pal  = vm.pSprite->pal;
                        }
#else
                    insptr++;
#endif
                    insptr++;
                }
                dispatch(tw);

            vInstruction(CON_COUNT):
                insptr++;
                AC_COUNT(vm.pData) = (int16_t)*insptr++;
                dispatch(tw);

            vInstruction(CON_CSTATOR):
                insptr++;
                vm.pSprite->cstat |= (int16_t)*insptr++;
                dispatch(tw);

            vInstruction(CON_CLIPDIST):
                insptr++;
                vm.pSprite->clipdist = (int16_t)*insptr++;
                dispatch(tw);

            vInstruction(CON_CSTAT):
                insptr++;
                vm.pSprite->cstat = (int16_t)*insptr++;
                dispatch(tw);

            vInstruction(CON_SAVENN):
            vInstruction(CON_SAVE):
                insptr++;
                {
                    int32_t const requestedSlot = *insptr++;

                    if ((unsigned)requestedSlot >= 10)
                        dispatch(tw);

                    // check if we need to make a new file
                    if (strcmp(g_lastautosave.path, g_lastusersave.path) == 0 || requestedSlot != g_lastAutoSaveArbitraryID)
                    {
                        g_lastautosave.reset();
                    }

                    g_lastAutoSaveArbitraryID = requestedSlot;

                    if (tw == CON_SAVE || g_lastautosave.name[0] == 0)
                    {
                        time_t     timeStruct = time(NULL);
                        struct tm *pTime      = localtime(&timeStruct);

                        strftime(g_lastautosave.name, sizeof(g_lastautosave.name), "%d %b %Y %I:%M%p", pTime);
                    }

                    g_saveRequested = true;

                    dispatch(tw);
                }

            vInstruction(CON_QUAKE):
                insptr++;
                g_earthquakeTime = Gv_GetVarX(*insptr++);
                A_PlaySound(EARTHQUAKE, g_player[screenpeek].ps->i);
                dispatch(tw);

            vInstruction(CON_RESETPLAYER):
                insptr++;
                vm.flags = VM_ResetPlayer(vm.playerNum, vm.flags, 0);
                dispatch(tw);

            vInstruction(CON_RESETPLAYERFLAGS):
                insptr++;
                vm.flags = VM_ResetPlayer(vm.playerNum, vm.flags, Gv_GetVarX(*insptr++));
                dispatch(tw);

            vInstruction(CON_RESETCOUNT):
                insptr++;
                AC_COUNT(vm.pData) = 0;
                dispatch(tw);

            vInstruction(CON_ADDINVENTORY):
                insptr += 2;

                VM_AddInventory(&p, insptr[-1], *insptr);

                insptr++;
                dispatch(tw);

            vInstruction(CON_HITRADIUS):
                insptr++;
                {
                    int32_t params[5];
                    Gv_FillWithVars(params);
                    A_RadiusDamage(vm.spriteNum, params[0], params[1], params[2], params[3], params[4]);
                }
                dispatch(tw);

            vInstruction(CON_IFP):
            {
                int const moveFlags  = *(++insptr);
                int       nResult    = 0;
                int const playerXVel = sprite[p.i].xvel;
                int const syncBits   = g_player[vm.playerNum].inputBits->bits;

                if (((moveFlags & pducking) && p.on_ground && TEST_SYNC_KEY(syncBits, SK_CROUCH))
                    || ((moveFlags & pfalling) && p.jumping_counter == 0 && !p.on_ground && p.vel.z > 2048)
                    || ((moveFlags & pjumping) && p.jumping_counter > 348)
                    || ((moveFlags & pstanding) && playerXVel >= 0 && playerXVel < 8)
                    || ((moveFlags & pwalking) && playerXVel >= 8 && !TEST_SYNC_KEY(syncBits, SK_RUN))
                    || ((moveFlags & prunning) && playerXVel >= 8 && TEST_SYNC_KEY(syncBits, SK_RUN))
                    || ((moveFlags & phigher) && p.pos.z < (vm.pSprite->z - (48 << 8)))
                    || ((moveFlags & pwalkingback) && playerXVel <= -8 && !TEST_SYNC_KEY(syncBits, SK_RUN))
                    || ((moveFlags & prunningback) && playerXVel <= -8 && TEST_SYNC_KEY(syncBits, SK_RUN))
                    || ((moveFlags & pkicking)
                        && (p.quick_kick > 0
                            || (PWEAPON(vm.playerNum, p.curr_weapon, WorksLike) == KNEE_WEAPON && p.kickback_pic > 0)))
                    || ((moveFlags & pshrunk) && sprite[p.i].xrepeat < 32)
                    || ((moveFlags & pjetpack) && p.jetpack_on)
                    || ((moveFlags & ponsteroids) && p.inv_amount[GET_STEROIDS] > 0 && p.inv_amount[GET_STEROIDS] < 400)
                    || ((moveFlags & ponground) && p.on_ground)
                    || ((moveFlags & palive) && sprite[p.i].xrepeat > 32 && sprite[p.i].extra > 0 && p.timebeforeexit == 0)
                    || ((moveFlags & pdead) && sprite[p.i].extra <= 0))
                    nResult = 1;
                else if ((moveFlags & pfacing))
                {
                    nResult
                    = (vm.pSprite->picnum == APLAYER && (g_netServer || ud.multimode > 1))
                      ? G_GetAngleDelta(fix16_to_int(g_player[otherp].ps->q16ang),
                                        getangle(p.pos.x - g_player[otherp].ps->pos.x, p.pos.y - g_player[otherp].ps->pos.y))
                      : G_GetAngleDelta(fix16_to_int(p.q16ang), getangle(vm.pSprite->x - p.pos.x, vm.pSprite->y - p.pos.y));

                    nResult = (nResult > -128 && nResult < 128);
                }
                VM_CONDITIONAL(nResult);
            }
                dispatch(tw);

            vInstruction(CON_GUTS):
#ifndef EDUKE32_STANDALONE
                A_DoGuts(vm.spriteNum, insptr[1], insptr[2]);
#endif
                insptr += 3;
                dispatch(tw);

            vInstruction(CON_WACKPLAYER):
                insptr++;
                P_ForceAngle(&p);
                dispatch(tw);

            vInstruction(CON_FLASH):
                insptr++;
                sprite[vm.spriteNum].shade = -127;
                p.visibility        = -127;
                dispatch(tw);

            vInstruction(CON_SAVEMAPSTATE):
                G_SaveMapState();
                insptr++;
                dispatch(tw);

            vInstruction(CON_LOADMAPSTATE):
                G_RestoreMapState();
                insptr++;
                dispatch(tw);

            vInstruction(CON_CLEARMAPSTATE):
                insptr++;
                {
                    int const levelNum = Gv_GetVarX(*insptr++);
                    if (EDUKE32_PREDICT_FALSE((unsigned)levelNum >= MAXVOLUMES * MAXLEVELS))
                    {
                        CON_ERRPRINTF("invalid map number %d\n", levelNum);
                        dispatch(tw);
                    }

                    G_FreeMapState(levelNum);
                }
                dispatch(tw);

            vInstruction(CON_STOPALLSOUNDS):
                insptr++;
                if (screenpeek == vm.playerNum)
                    FX_StopAllSounds();
                dispatch(tw);

            vInstruction(CON_STOPALLMUSIC):
                insptr++;
                S_StopMusic();
                dispatch(tw);

            vInstruction(CON_OPERATE):
                insptr++;
                if (sector[vm.pSprite->sectnum].lotag == 0)
                {
                    int16_t foundSect, foundWall, foundSprite;
                    int32_t foundDist;

                    neartag(vm.pSprite->x, vm.pSprite->y, vm.pSprite->z - ZOFFSET5, vm.pSprite->sectnum, vm.pSprite->ang, &foundSect, &foundWall,
                            &foundSprite, &foundDist, 768, 4 + 1, NULL);

                    if (foundSect >= 0 && isanearoperator(sector[foundSect].lotag))
                        if ((sector[foundSect].lotag & 0xff) == ST_23_SWINGING_DOOR || sector[foundSect].floorz == sector[foundSect].ceilingz)
                            if ((sector[foundSect].lotag & (16384u | 32768u)) == 0)
                            {
                                int32_t j;

                                for (SPRITES_OF_SECT(foundSect, j))
                                    if (sprite[j].picnum == ACTIVATOR)
                                        break;

                                if (j == -1)
                                    G_OperateSectors(foundSect, vm.spriteNum);
                            }
                }
                dispatch(tw);


            vInstruction(CON_SPRITEPAL):
                insptr++;
                if (vm.pSprite->picnum != APLAYER)
                    vm.pActor->tempang = vm.pSprite->pal;
                vm.pSprite->pal = *insptr++;
                dispatch(tw);

            vInstruction(CON_CACTOR):
                insptr++;
                vm.pSprite->picnum = *insptr++;
                dispatch(tw);

            vInstruction(CON_PALFROM):
                insptr++;
                if (EDUKE32_PREDICT_FALSE((unsigned)vm.playerNum >= (unsigned)g_mostConcurrentPlayers))
                {
                    CON_ERRPRINTF("invalid player %d\n", vm.playerNum);
                    dispatch(tw);
                }
                else
                {
                    palette_t const pal = { uint8_t(insptr[1]), uint8_t(insptr[2]), uint8_t(insptr[3]), uint8_t(insptr[0]) };
                    insptr += 4;
                    P_PalFrom(&p, pal.f, pal.r, pal.g, pal.b);
                }
                dispatch(tw);

            vInstruction(CON_SCREENPAL):
                insptr++;
                {
                    int32_t params[4];
                    Gv_FillWithVars(params);
                    videoFadePalette(params[0], params[1], params[2], params[3]);
                }
                dispatch(tw);

            vInstruction(CON_SECTOROFWALL):
                insptr++;
                tw = *insptr++;
                Gv_SetVarX(tw, sectorofwall(Gv_GetVarX(*insptr++)));
                dispatch(tw);

            vInstruction(CON_QSPRINTF):
                insptr++;
                {
                    int const outputQuote = Gv_GetVarX(*insptr++);
                    int const inputQuote  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE(apStrings[inputQuote] == NULL || apStrings[outputQuote] == NULL))
                    {
                        CON_ERRPRINTF("null quote %d\n", apStrings[inputQuote] ? outputQuote : inputQuote);
                        dispatch(tw);
                    }

                    auto &inBuf = apStrings[inputQuote];

                    int32_t arg[32];
                    char    outBuf[MAXQUOTELEN];

                    int const quoteLen = Bstrlen(inBuf);

                    int inputPos  = 0;
                    int outputPos = 0;
                    int argIdx    = 0;

                    while ((*insptr & VM_INSTMASK) != CON_NULLOP && argIdx < 32)
                        arg[argIdx++] = Gv_GetVarX(*insptr++);

                    int numArgs = argIdx;

                    insptr++;  // skip the NOP

                    argIdx = 0;

                    do
                    {
                        while (inputPos < quoteLen && outputPos < MAXQUOTELEN && inBuf[inputPos] != '%')
                            outBuf[outputPos++] = inBuf[inputPos++];

                        if (inBuf[inputPos] == '%')
                        {
                            inputPos++;
                            switch (inBuf[inputPos])
                            {
                                case 'l':
                                    if (inBuf[inputPos + 1] != 'd')
                                    {
                                        // write the % and l
                                        outBuf[outputPos++] = inBuf[inputPos - 1];
                                        outBuf[outputPos++] = inBuf[inputPos++];
                                        break;
                                    }
                                    inputPos++;
                                    fallthrough__;
                                case 'd':
                                {
                                    if (argIdx >= numArgs)
                                        goto finish_qsprintf;

                                    char buf[16];
                                    Bsprintf(buf, "%d", arg[argIdx++]);

                                    int const bufLen = Bstrlen(buf);
                                    Bmemcpy(&outBuf[outputPos], buf, bufLen);
                                    outputPos += bufLen;
                                    inputPos++;
                                }
                                break;

                                case 's':
                                {
                                    if (argIdx >= numArgs)
                                        goto finish_qsprintf;

                                    int const argLen = Bstrlen(apStrings[arg[argIdx]]);

                                    Bmemcpy(&outBuf[outputPos], apStrings[arg[argIdx]], argLen);
                                    outputPos += argLen;
                                    argIdx++;
                                    inputPos++;
                                }
                                break;

                                default: outBuf[outputPos++] = inBuf[inputPos - 1]; break;
                            }
                        }
                    } while (inputPos < quoteLen && outputPos < MAXQUOTELEN);
                finish_qsprintf:
                    outBuf[outputPos] = '\0';
                    Bstrncpyz(apStrings[outputQuote], outBuf, MAXQUOTELEN);
                    dispatch(tw);
                }

            vInstruction(CON_ADDLOGVAR):
                insptr++;
                {
                    int32_t m = 1;
                    char    szBuf[256];
                    int32_t lVarID = *insptr;

                    if ((lVarID >= g_gameVarCount) || lVarID < 0)
                    {
                        if (*insptr == MAXGAMEVARS)  // addlogvar for a constant?  Har.
                            insptr++;
                        //                else if (*insptr > g_gameVarCount && (*insptr < (MAXGAMEVARS<<1)+MAXGAMEVARS+1+MAXGAMEARRAYS))
                        else if (*insptr & (MAXGAMEVARS << 2))
                        {
                            int32_t index;

                            lVarID ^= (MAXGAMEVARS << 2);

                            if (lVarID & GV_FLAG_NEGATIVE)
                            {
                                m = -m;
                                lVarID ^= GV_FLAG_NEGATIVE;
                            }

                            insptr++;

                            index = Gv_GetVarX(*insptr++);
                            if (EDUKE32_PREDICT_TRUE((unsigned)index < (unsigned)aGameArrays[lVarID].size))
                            {
                                initprintf(OSDTEXT_GREEN "CONLOGVAR: L=%d %s[%d] =%d\n", g_errorLineNum, aGameArrays[lVarID].szLabel, index,
                                           (int32_t)(m * Gv_GetArrayValue(lVarID, index)));
                                dispatch(tw);
                            }
                            else
                            {
                                CON_ERRPRINTF("invalid array index\n");
                                dispatch(tw);
                            }
                        }
                        else if (*insptr & (MAXGAMEVARS << 3))
                        {
                            //                    FIXME FIXME FIXME
                            if ((lVarID & (MAXGAMEVARS - 1)) == g_structVarIDs + STRUCT_ACTORVAR)
                            {
                                auto const oinsptr = insptr++;
                                int32_t    index   = Gv_GetVarX(*insptr++);
                                insptr             = oinsptr;

                                if (EDUKE32_PREDICT_FALSE((unsigned)index >= MAXSPRITES - 1))
                                {
                                    CON_ERRPRINTF("invalid array index\n");
                                    dispatch(tw);
                                }
                                initprintf(OSDTEXT_GREEN "CONLOGVAR: L=%d %d %d\n", g_errorLineNum, index, Gv_GetVar(*insptr++, index, vm.playerNum));
                                dispatch(tw);
                            }
                        }
                        else if (EDUKE32_PREDICT_TRUE(*insptr & GV_FLAG_NEGATIVE))
                        {
                            m = -m;
                            lVarID ^= GV_FLAG_NEGATIVE;
                        }
                        else
                        {
                            // invalid varID
                            CON_ERRPRINTF("invalid variable\n");
                            dispatch(tw);  // out of switch
                        }
                    }
                    Bsprintf(tempbuf, "CONLOGVAR: L=%d %s ", g_errorLineNum, aGameVars[lVarID].szLabel);

                    if (aGameVars[lVarID].flags & GAMEVAR_READONLY)
                    {
                        Bsprintf(szBuf, " (read-only)");
                        Bstrcat(tempbuf, szBuf);
                    }
                    if (aGameVars[lVarID].flags & GAMEVAR_PERPLAYER)
                    {
                        Bsprintf(szBuf, " (Per Player. Player=%d)", vm.playerNum);
                    }
                    else if (aGameVars[lVarID].flags & GAMEVAR_PERACTOR)
                    {
                        Bsprintf(szBuf, " (Per Actor. Actor=%d)", vm.spriteNum);
                    }
                    else
                    {
                        Bsprintf(szBuf, " (Global)");
                    }
                    Bstrcat(tempbuf, szBuf);
                    Bsprintf(szBuf, " =%d\n", Gv_GetVarX(lVarID) * m);
                    Bstrcat(tempbuf, szBuf);
                    initprintf(OSDTEXT_GREEN "%s", tempbuf);
                    insptr++;
                    dispatch(tw);
                }

            vInstruction(CON_SQRT):
                insptr++;
                {
                    // syntax sqrt <invar> <outvar>
                    int const sqrtval = ksqrt((uint32_t)Gv_GetVarX(*insptr++));
                    Gv_SetVarX(*insptr++, sqrtval);
                    dispatch(tw);
                }

            vInstruction(CON_FINDNEARACTOR):
            vInstruction(CON_FINDNEARSPRITE):
            vInstruction(CON_FINDNEARACTOR3D):
            vInstruction(CON_FINDNEARSPRITE3D):
                insptr++;
                {
                    // syntax findnearactorvar <type> <maxdistvar> <getvar>
                    // gets the sprite ID of the nearest actor within max dist
                    // that is of <type> into <getvar>
                    // -1 for none found
                    // <type> <maxdistvarid> <varid>
                    int const findPicnum = *insptr++;
                    int const maxDist    = Gv_GetVarX(*insptr++);
                    int const returnVar  = *insptr++;

                    int foundSprite = -1;
                    int findStatnum = STAT_ACTOR;
                    int spriteNum;

                    if (tw == CON_FINDNEARSPRITE || tw == CON_FINDNEARSPRITE3D)
                        findStatnum = MAXSTATUS - 1;

                    if (tw == CON_FINDNEARACTOR3D || tw == CON_FINDNEARSPRITE3D)
                    {
                        do
                        {
                            spriteNum = headspritestat[findStatnum];  // all sprites

                            while (spriteNum >= 0)
                            {
                                if (sprite[spriteNum].picnum == findPicnum && spriteNum != vm.spriteNum
                                    && dist(&sprite[vm.spriteNum], &sprite[spriteNum]) < maxDist)
                                {
                                    foundSprite = spriteNum;
                                    spriteNum   = MAXSPRITES;
                                    break;
                                }
                                spriteNum = nextspritestat[spriteNum];
                            }
                            if (spriteNum == MAXSPRITES || tw == CON_FINDNEARACTOR3D)
                                break;
                        } while (findStatnum--);
                        Gv_SetVarX(returnVar, foundSprite);
                        dispatch(tw);
                    }

                    do
                    {
                        spriteNum = headspritestat[findStatnum];  // all sprites

                        while (spriteNum >= 0)
                        {
                            if (sprite[spriteNum].picnum == findPicnum && spriteNum != vm.spriteNum
                                && ldist(&sprite[vm.spriteNum], &sprite[spriteNum]) < maxDist)
                            {
                                foundSprite = spriteNum;
                                spriteNum   = MAXSPRITES;
                                break;
                            }
                            spriteNum = nextspritestat[spriteNum];
                        }

                        if (spriteNum == MAXSPRITES || tw == CON_FINDNEARACTOR)
                            break;
                    } while (findStatnum--);
                    Gv_SetVarX(returnVar, foundSprite);
                    dispatch(tw);
                }

            vInstruction(CON_FINDNEARACTORZ):
            vInstruction(CON_FINDNEARSPRITEZ):
                insptr++;
                {
                    // syntax findnearactorvar <type> <maxdistvar> <getvar>
                    // gets the sprite ID of the nearest actor within max dist
                    // that is of <type> into <getvar>
                    // -1 for none found
                    // <type> <maxdistvarid> <varid>
                    int const findPicnum = *insptr++;
                    int const maxDist    = Gv_GetVarX(*insptr++);
                    int const maxZDist   = Gv_GetVarX(*insptr++);
                    int const returnVar  = *insptr++;

                    int foundSprite = -1;
                    int findStatnum = MAXSTATUS - 1;

                    do
                    {
                        int spriteNum = headspritestat[tw == CON_FINDNEARACTORZ ? STAT_ACTOR : findStatnum];  // all sprites

                        if (spriteNum == -1)
                            dispatch(tw);
                        do
                        {
                            if (sprite[spriteNum].picnum == findPicnum && spriteNum != vm.spriteNum)
                            {
                                if (ldist(&sprite[vm.spriteNum], &sprite[spriteNum]) < maxDist)
                                {
                                    if (klabs(sprite[vm.spriteNum].z - sprite[spriteNum].z) < maxZDist)
                                    {
                                        foundSprite = spriteNum;
                                        spriteNum   = MAXSPRITES;
                                        break;
                                    }
                                }
                            }
                            spriteNum = nextspritestat[spriteNum];
                        } while (spriteNum >= 0);

                        if (tw == CON_FINDNEARACTORZ || spriteNum == MAXSPRITES)
                            break;
                    } while (findStatnum--);
                    Gv_SetVarX(returnVar, foundSprite);

                    dispatch(tw);
                }

            vInstruction(CON_FINDPLAYER):
            {
                int32_t tw;
                insptr++;
                aGameVars[g_returnVarID].global = A_FindPlayer(&sprite[vm.spriteNum], &tw);
                Gv_SetVarX(*insptr++, tw);
                dispatch(tw);
            }

            vInstruction(CON_FINDOTHERPLAYER):
            {
                int32_t tw;
                insptr++;
                aGameVars[g_returnVarID].global = P_FindOtherPlayer(vm.playerNum, &tw);
                Gv_SetVarX(*insptr++, tw);
                dispatch(tw);
            }


            vInstruction(CON_GETINPUT):
                insptr++;
                {
                    int const playerNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.playerNum;
                    int const labelNum  = *insptr++;

                    Gv_SetVarX(*insptr++, VM_GetPlayerInput(playerNum, labelNum));
                    dispatch(tw);
                }

            vInstruction(CON_SETINPUT):
                insptr++;
                {
                    int const playerNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.playerNum;
                    int const labelNum  = *insptr++;

                    VM_SetPlayerInput(playerNum, labelNum, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETUSERDEF):
                insptr++;
                {
                    int const labelNum = *insptr++;
                    int const lParm2   = (UserdefsLabels[labelNum].flags & LABEL_HASPARM2) ? Gv_GetVarX(*insptr++) : 0;

                    Gv_SetVarX(*insptr++, VM_GetUserdef(labelNum, lParm2));
                    dispatch(tw);
                }

            vInstruction(CON_GETTILEDATA):
                insptr++;
                {
                    int const tileNum  = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.pSprite->picnum;
                    int const labelNum = *insptr++;

                    Gv_SetVarX(*insptr++, VM_GetTileData(tileNum, labelNum));
                    dispatch(tw);
                }

            vInstruction(CON_SETTILEDATA):
                insptr++;
                {
                    int const tileNum  = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(insptr[-1]) : vm.pSprite->picnum;
                    int const labelNum = *insptr++;

                    VM_SetTileData(tileNum, labelNum, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_SETUSERDEF):
                insptr++;
                {
                    int const labelNum = *insptr++;
                    int const lParm2   = (UserdefsLabels[labelNum].flags & LABEL_HASPARM2) ? Gv_GetVarX(*insptr++) : 0;

                    VM_SetUserdef(labelNum, lParm2, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETPROJECTILE):
                insptr++;
                {
                    tw = Gv_GetVarX(*insptr++);
                    int const labelNum = *insptr++;
                    Gv_SetVarX(*insptr++, VM_GetProjectile(tw, labelNum));
                    dispatch(tw);
                }

            vInstruction(CON_SETPROJECTILE):
                insptr++;
                {
                    tw = Gv_GetVarX(*insptr++);
                    int const labelNum = *insptr++;
                    VM_SetProjectile(tw, labelNum, Gv_GetVarX(*insptr++));
                    dispatch(tw);
                }

            vInstruction(CON_GETANGLETOTARGET):
                insptr++;
                // vm.pActor->lastvx and lastvy are last known location of target.
                Gv_SetVarX(*insptr++, getangle(vm.pActor->lastv.x - vm.pSprite->x, vm.pActor->lastv.y - vm.pSprite->y));
                dispatch(tw);

            vInstruction(CON_ANGOFF):
                insptr++;
                spriteext[vm.spriteNum].angoff = Gv_GetVarX(*insptr++);
                dispatch(tw);

            vInstruction(CON_LOCKPLAYER):
                insptr++;
                p.transporter_hold = Gv_GetVarX(*insptr++);
                dispatch(tw);

            vInstruction(CON_CHECKAVAILWEAPON):
            vInstruction(CON_CHECKAVAILINVEN):
            {
                insptr++;
                int const playerNum = (*insptr++ != g_thisActorVarID) ? Gv_GetVarX(*insptr) : vm.playerNum;

                if (EDUKE32_PREDICT_FALSE((unsigned)playerNum >= (unsigned)g_mostConcurrentPlayers))
                {
                    CON_ERRPRINTF("invalid player %d\n", (int)playerNum);
                    dispatch(tw);
                }

                if (tw == CON_CHECKAVAILWEAPON)
                    P_CheckWeapon(g_player[playerNum].ps);
                else
                    P_SelectNextInvItem(g_player[playerNum].ps);

                dispatch(tw);
            }

            vInstruction(CON_GETPLAYERANGLE):
                insptr++;
                Gv_SetVarX(*insptr++, fix16_to_int(p.q16ang));
                dispatch(tw);

            vInstruction(CON_GETACTORANGLE):
                insptr++;
                Gv_SetVarX(*insptr++, vm.pSprite->ang);
                dispatch(tw);

            vInstruction(CON_SETPLAYERANGLE):
                insptr++;
                p.q16ang = fix16_from_int(Gv_GetVarX(*insptr++) & 2047);
                dispatch(tw);

            vInstruction(CON_SETACTORANGLE):
                insptr++;
                vm.pSprite->ang = Gv_GetVarX(*insptr++) & 2047;
                dispatch(tw);

            vInstruction(CON_KLABS):
                if ((aGameVars[insptr[1]].flags & (GAMEVAR_USER_MASK | GAMEVAR_PTR_MASK)) == 0)
                    aGameVars[insptr[1]].global = klabs(aGameVars[insptr[1]].global);
                else
                    Gv_SetVarX(insptr[1], klabs(Gv_GetVarX(insptr[1])));
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_SETARRAY):
                insptr++;
                {
                    tw = *insptr++;

                    int const arrayIndex = Gv_GetVarX(*insptr++);
                    int const newValue   = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE((unsigned)tw >= (unsigned)g_gameArrayCount || (unsigned)arrayIndex >= (unsigned)aGameArrays[tw].size))
                    {
                        OSD_Printf(OSD_ERROR "Gv_SetVar(): tried to set invalid array %d or index out of bounds from "
                                             "sprite %d (%d), player %d\n",
                                   (int)tw, vm.spriteNum, TrackerCast(sprite[vm.spriteNum].picnum), vm.playerNum);
                        vm.flags |= VM_RETURN;
                        dispatch(tw);
                    }

                    auto &arr = aGameArrays[tw];

                    if (EDUKE32_PREDICT_FALSE(arr.flags & GAMEARRAY_READONLY))
                    {
                        OSD_Printf(OSD_ERROR "Tried to set value in read-only array `%s'", arr.szLabel);
                        vm.flags |= VM_RETURN;
                        dispatch(tw);
                    }

                    switch (arr.flags & GAMEARRAY_TYPE_MASK)
                    {
                        case 0: arr.pValues[arrayIndex]                              = newValue; break;
                        case GAMEARRAY_INT16: ((int16_t *)arr.pValues)[arrayIndex]   = newValue; break;
                        case GAMEARRAY_INT8: ((int8_t *)arr.pValues)[arrayIndex]     = newValue; break;
                        case GAMEARRAY_UINT16: ((uint16_t *)arr.pValues)[arrayIndex] = newValue; break;
                        case GAMEARRAY_UINT8: ((int8_t *)arr.pValues)[arrayIndex]    = newValue; break;
                        case GAMEARRAY_BITMAP:
                        {
                            uint32_t const mask  = (1 << (arrayIndex & 7));
                            uint8_t &value = ((uint8_t *)arr.pValues)[arrayIndex >> 3];
                            value = (value & ~mask) | (-!!newValue & mask);
                            break;
                        }
                    }

                    dispatch(tw);
                }

            vInstruction(CON_READARRAYFROMFILE):
                insptr++;
                {
                    int const arrayNum      = *insptr++;
                    int const quoteFilename = *insptr++;

                    if (EDUKE32_PREDICT_FALSE(apStrings[quoteFilename] == NULL))
                    {
                        CON_ERRPRINTF("null quote %d\n", quoteFilename);
                        dispatch(tw);
                    }

                    buildvfs_kfd kFile = kopen4loadfrommod(apStrings[quoteFilename], 0);

                    if (kFile == buildvfs_kfd_invalid)
                        dispatch(tw);

                    size_t const filelength  = kfilelength(kFile);
                    size_t const numElements = Gv_GetArrayCountFromFile(arrayNum, filelength);

                    if (numElements > 0)
                    {
                        size_t const newBytes  = Gv_GetArrayAllocSizeForCount(arrayNum, numElements);
                        size_t const readBytes = min(newBytes, filelength);
                        size_t const oldBytes  = Gv_GetArrayAllocSize(arrayNum);

                        intptr_t *&pValues = aGameArrays[arrayNum].pValues;

                        if (newBytes != oldBytes)
                        {
                            Baligned_free(pValues);
                            pValues = (intptr_t *)Xaligned_alloc(ARRAY_ALIGNMENT, newBytes);
                        }

                        aGameArrays[arrayNum].size = numElements;

                        uintptr_t const flags = aGameArrays[arrayNum].flags;

                        switch (flags & GAMEARRAY_SIZE_MASK)
                        {
                            case 0:
#ifdef BITNESS64
                            {
                                void *const pArray = Xcalloc(1, newBytes);

                                kread(kFile, pArray, readBytes);

                                if (flags & GAMEARRAY_UNSIGNED)
                                {
                                    for (unative_t i = 0; i < numElements; ++i)
                                        pValues[i]   = ((uint32_t *)pArray)[i];
                                }
                                else
                                {
                                    for (unative_t i = 0; i < numElements; ++i)
                                        pValues[i]   = ((int32_t *)pArray)[i];
                                }

                                Bfree(pArray);
                                break;
                            }
#endif
                            default:
                                memset((char *)pValues + readBytes, 0, newBytes - readBytes);
                                kread(kFile, pValues, readBytes);
                                break;
                        }
                    }

                    kclose(kFile);
                    dispatch(tw);
                }

            vInstruction(CON_WRITEARRAYTOFILE):
                insptr++;
                {
                    int const arrayNum      = *insptr++;
                    int const quoteFilename = *insptr++;

                    if (EDUKE32_PREDICT_FALSE(apStrings[quoteFilename] == NULL))
                    {
                        CON_ERRPRINTF("null quote %d\n", quoteFilename);
                        dispatch(tw);
                    }

                    char temp[BMAX_PATH];

                    if (EDUKE32_PREDICT_FALSE(G_ModDirSnprintf(temp, sizeof(temp), "%s", apStrings[quoteFilename])))
                    {
                        CON_ERRPRINTF("file name too long\n");
                        dispatch(tw);
                    }

                    buildvfs_FILE const fil = buildvfs_fopen_write(temp);

                    if (EDUKE32_PREDICT_FALSE(fil == NULL))
                    {
                        CON_ERRPRINTF("couldn't open file \"%s\"\n", temp);
                        dispatch(tw);
                    }

                    switch (aGameArrays[arrayNum].flags & GAMEARRAY_SIZE_MASK)
                    {
                        case 0:
#ifdef BITNESS64
                        {
                            size_t const   numElements  = aGameArrays[arrayNum].size;
                            size_t const   numDiskBytes = numElements * sizeof(int32_t);
                            int32_t *const pArray       = (int32_t *)Xmalloc(numDiskBytes);

                            for (unative_t k = 0; k < numElements; ++k)
                                pArray[k]    = Gv_GetArrayValue(arrayNum, k);

                            buildvfs_fwrite(pArray, 1, numDiskBytes, fil);
                            Bfree(pArray);
                            break;
                        }
#endif
                        default: buildvfs_fwrite(aGameArrays[arrayNum].pValues, 1, Gv_GetArrayAllocSize(arrayNum), fil); break;
                    }

                    buildvfs_fclose(fil);
                    dispatch(tw);
                }

            vInstruction(CON_GETARRAYSIZE):
                insptr++;
                tw = *insptr++;
                Gv_SetVarX(*insptr++, (aGameArrays[tw].flags & GAMEARRAY_VARSIZE) ? Gv_GetVarX(aGameArrays[tw].size) : aGameArrays[tw].size);
                dispatch(tw);

            vInstruction(CON_RESIZEARRAY):
                insptr++;
                {
                    tw = *insptr++;

                    auto &arr = aGameArrays[tw];

                    int const newSize = Gv_GetVarX(*insptr++);
                    int const oldSize = arr.size;

                    if (newSize == oldSize || newSize < 0)
                        dispatch(tw);
#if 0
                    OSD_Printf(OSDTEXT_GREEN "CON_RESIZEARRAY: resizing array %s from %d to %d\n",
                               array.szLabel, array.size, newSize);
#endif
                    if (newSize == 0)
                    {
                        Baligned_free(arr.pValues);
                        arr.pValues = nullptr;
                        arr.size = 0;
                        dispatch(tw);
                    }

                    size_t const oldBytes = Gv_GetArrayAllocSizeForCount(tw, oldSize);
                    size_t const newBytes = Gv_GetArrayAllocSizeForCount(tw, newSize);

                    auto const oldArray = arr.pValues;
                    auto const newArray = (intptr_t *)Xaligned_alloc(ARRAY_ALIGNMENT, newBytes);

                    if (oldSize != 0)
                        Bmemcpy(newArray, oldArray, min(oldBytes, newBytes));

                    if (newSize > oldSize)
                        Bmemset((char *)newArray + oldBytes, 0, newBytes - oldBytes);

                    arr.pValues = newArray;
                    arr.size = newSize;

                    Baligned_free(oldArray);

                    dispatch(tw);
                }

            vInstruction(CON_COPY):
                insptr++;
                {
                    int const srcArray       = *insptr++;
                    int       srcArrayIndex  = Gv_GetVarX(*insptr++);  //, vm.spriteNum, vm.playerNum);
                    int const destArray      = *insptr++;
                    int       destArrayIndex = Gv_GetVarX(*insptr++);
                    int       numElements    = Gv_GetVarX(*insptr++);

                    auto &src = aGameArrays[srcArray];
                    auto &dest = aGameArrays[destArray];

                    int const srcArraySize = (src.flags & GAMEARRAY_VARSIZE) ? Gv_GetVarX(src.size) : src.size;
                    int const destArraySize = (dest.flags & GAMEARRAY_VARSIZE) ? Gv_GetVarX(dest.size) : dest.size;

                    if (EDUKE32_PREDICT_FALSE(srcArrayIndex > srcArraySize || destArrayIndex > destArraySize))
                        dispatch(tw);

                    if ((srcArrayIndex + numElements) > srcArraySize)
                        numElements = srcArraySize - srcArrayIndex;

                    if ((destArrayIndex + numElements) > destArraySize)
                        numElements = destArraySize - destArrayIndex;

                    // Switch depending on the source array type.

                    int const srcInc  = 1 << (int)!!(EDUKE32_PREDICT_FALSE(src.flags & GAMEARRAY_STRIDE2));
                    int const destInc = 1 << (int)!!(EDUKE32_PREDICT_FALSE(dest.flags & GAMEARRAY_STRIDE2));

                    // matching array types, no BITMAPs, no STRIDE2 flag
                    if ((src.flags & GAMEARRAY_SIZE_MASK) == (dest.flags & GAMEARRAY_SIZE_MASK)
                        && !((src.flags | dest.flags) & GAMEARRAY_BITMAP) && (srcInc & destInc) == 1)
                    {
                        Bmemcpy(dest.pValues + destArrayIndex, src.pValues + srcArrayIndex,
                                numElements * Gv_GetArrayElementSize(srcArray));
                        dispatch(tw);
                    }

                    switch (dest.flags & GAMEARRAY_TYPE_MASK)
                    {
                        case 0:
                            for (; numElements > 0; --numElements)
                            {
                                dest.pValues[destArrayIndex] = Gv_GetArrayValue(srcArray, srcArrayIndex++);
                                destArrayIndex += destInc;
                            }
                            break;
                        case GAMEARRAY_INT16:
                            for (; numElements > 0; --numElements)
                            {
                                ((int16_t *)dest.pValues)[destArrayIndex] = Gv_GetArrayValue(srcArray, srcArrayIndex++);
                                destArrayIndex += destInc;
                            }
                            break;
                        case GAMEARRAY_INT8:
                            for (; numElements > 0; --numElements)
                            {
                                ((int8_t *)dest.pValues)[destArrayIndex] = Gv_GetArrayValue(srcArray, srcArrayIndex++);
                                destArrayIndex += destInc;
                            }
                            break;
                        case GAMEARRAY_UINT16:
                            for (; numElements > 0; --numElements)
                            {
                                ((uint16_t *)dest.pValues)[destArrayIndex] = Gv_GetArrayValue(srcArray, srcArrayIndex++);
                                destArrayIndex += destInc;
                            }
                            break;
                        case GAMEARRAY_UINT8:
                            for (; numElements > 0; --numElements)
                            {
                                ((uint8_t *)dest.pValues)[destArrayIndex] = Gv_GetArrayValue(srcArray, srcArrayIndex++);
                                destArrayIndex += destInc;
                            }
                            break;
                        case GAMEARRAY_BITMAP:
                            for (; numElements > 0; --numElements)
                            {
                                uint32_t const newValue = Gv_GetArrayValue(srcArray, srcArrayIndex++);
                                uint32_t const mask = 1 << (destArrayIndex & 7);
                                uint8_t & value = ((uint8_t *)dest.pValues)[destArrayIndex >> 3];
                                value = (value & ~mask) | (-!!newValue & mask);
                                destArrayIndex += destInc;
                            }
                            break;
                    }

                    dispatch(tw);
                }

            vInstruction(CON_SWAPARRAYS):
                insptr++;
                {
                    auto &array1 = aGameArrays[*insptr++];
                    auto &array2 = aGameArrays[*insptr++];

                    swap(&array1.size, &array2.size);
                    swap(&array1.pValues, &array2.pValues);

                    dispatch(tw);
                }

            vInstruction(CON_DISPLAYRANDVAR):
                insptr++;
                Gv_SetVarX(*insptr, mulscale15(system_15bit_rand(), insptr[1] + 1));
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_CLAMP):
                insptr++;
                {
                    tw = *insptr++;
                    int const min = Gv_GetVarX(*insptr++);
                    Gv_SetVarX(tw, clamp2(Gv_GetVarX(tw), min, Gv_GetVarX(*insptr++)));
                }
                dispatch(tw);

            vInstruction(CON_GETCLOSESTCOL):
                insptr++;
                {
                    tw = *insptr++;
                    int32_t const rgb = Gv_GetVarX(*insptr++);
                    Gv_SetVarX(tw, getclosestcol_lim(rgb & 0xFF, (rgb >> 8) & 0xFF, (rgb >> 16) & 0xFF, Gv_GetVarX(*insptr++)));
                }
                dispatch(tw);

            vInstruction(CON_DRAWLINE256):
                insptr++;
                {
                    struct
                    {
                        vec2_t  pos[2];
                        int32_t index;
                    } v;

                    Gv_FillWithVars(v);

                    renderDrawLine(v.pos[0].x, v.pos[0].y, v.pos[1].x, v.pos[1].y, v.index);
                }
                dispatch(tw);

            vInstruction(CON_DRAWLINERGB):
                insptr++;
                {
                    struct
                    {
                        vec2_t  pos[2];
                        int32_t index, rgb;
                    } v;

                    Gv_FillWithVars(v);

                    palette_t const p
                    = { (uint8_t)(v.rgb & 0xFF), (uint8_t)((v.rgb >> 8) & 0xFF), (uint8_t)((v.rgb >> 16) & 0xFF), (uint8_t)v.index };

                    drawlinergb(v.pos[0].x, v.pos[0].y, v.pos[1].x, v.pos[1].y, p);
                }
                dispatch(tw);

            vInstruction(CON_INV):
                if ((aGameVars[insptr[1]].flags & (GAMEVAR_USER_MASK | GAMEVAR_PTR_MASK)) == 0)
                    aGameVars[insptr[1]].global = -aGameVars[insptr[1]].global;
                else
                    Gv_SetVarX(insptr[1], -Gv_GetVarX(insptr[1]));
                insptr += 2;
                dispatch(tw);

            vInstruction(CON_DISPLAYRANDVARVAR):
                insptr++;
                tw = *insptr++;
                Gv_SetVarX(tw, mulscale15(system_15bit_rand(), Gv_GetVarX(*insptr++) + 1));
                dispatch(tw);

            vInstruction(CON_GMAXAMMO):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                if (EDUKE32_PREDICT_FALSE((unsigned)tw >= MAX_WEAPONS))
                {
                    CON_ERRPRINTF("invalid weapon %d\n", (int)tw);
                    dispatch(tw);
                }
                Gv_SetVarX(*insptr++, p.max_ammo_amount[tw]);
                dispatch(tw);

            vInstruction(CON_SMAXAMMO):
                insptr++;
                tw = Gv_GetVarX(*insptr++);
                if (EDUKE32_PREDICT_FALSE((unsigned)tw >= MAX_WEAPONS))
                {
                    CON_ERRPRINTF("invalid weapon %d\n", (int)tw);
                    dispatch(tw);
                }
                p.max_ammo_amount[tw] = Gv_GetVarX(*insptr++);
                dispatch(tw);


            vInstruction(CON_DIVR):  // div round to nearest
                insptr++;
                {
                    tw = *insptr++;

                    int const dividend = Gv_GetVarX(tw);
                    int const divisor  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE(!divisor))
                    {
                        CON_CRITICALERRPRINTF("divide by zero!\n");
                        dispatch(tw);
                    }

                    Gv_SetVarX(tw, tabledivide32((dividend + ksgn(dividend) * klabs(divisor / 2)), divisor));
                    dispatch(tw);
                }

            vInstruction(CON_DIVRU):  // div round away from zero
                insptr++;
                {
                    tw = *insptr++;

                    int const dividend = Gv_GetVarX(tw);
                    int const divisor  = Gv_GetVarX(*insptr++);

                    if (EDUKE32_PREDICT_FALSE(!divisor))
                    {
                        CON_CRITICALERRPRINTF("divide by zero!\n");
                        dispatch(tw);
                    }

                    Gv_SetVarX(tw, tabledivide32((dividend + ksgn(dividend) * klabs(divisor) + 1), divisor));
                    dispatch(tw);
                }

            vInstruction(CON_SIN):
                insptr++;
                tw = *insptr++;
                Gv_SetVarX(tw, sintable[Gv_GetVarX(*insptr++) & 2047]);
                dispatch(tw);

            vInstruction(CON_COS):
                insptr++;
                tw = *insptr++;
                Gv_SetVarX(tw, sintable[(Gv_GetVarX(*insptr++) + 512) & 2047]);
                dispatch(tw);


            vInstruction(CON_SPGETLOTAG):
                insptr++;
                aGameVars[g_lotagVarID].global = vm.pSprite->lotag;
                dispatch(tw);

            vInstruction(CON_SPGETHITAG):
                insptr++;
                aGameVars[g_hitagVarID].global = vm.pSprite->hitag;
                dispatch(tw);

            vInstruction(CON_SECTGETLOTAG):
                insptr++;
                aGameVars[g_lotagVarID].global = sector[vm.pSprite->sectnum].lotag;
                dispatch(tw);

            vInstruction(CON_SECTGETHITAG):
                insptr++;
                aGameVars[g_hitagVarID].global = sector[vm.pSprite->sectnum].hitag;
                dispatch(tw);

            vInstruction(CON_GETTEXTUREFLOOR):
                insptr++;
                aGameVars[g_textureVarID].global = sector[vm.pSprite->sectnum].floorpicnum;
                dispatch(tw);

            vInstruction(CON_STARTTRACK):
                insptr++;
                G_StartTrackSlotWrap(ud.volume_number, Gv_GetVarX(*(insptr++)));
                dispatch(tw);

            vInstruction(CON_STARTTRACKSLOT):
                insptr++;
                {
                    int const volumeNum = Gv_GetVarX(*(insptr++));
                    int const levelNum  = Gv_GetVarX(*(insptr++));
                    G_StartTrackSlotWrap(volumeNum == -1 ? MAXVOLUMES : volumeNum, levelNum);
                }
                dispatch(tw);

            vInstruction(CON_SWAPTRACKSLOT):
                insptr++;
                {
                    int const volumeNum = Gv_GetVarX(*(insptr++));
                    int const levelNum  = Gv_GetVarX(*(insptr++));

                    if (volumeNum == ud.music_episode && levelNum == ud.music_level)
                        dispatch(tw);

                    // This is the best ASS can do right now. Better implementation pending.
                    int32_t position = S_GetMusicPosition();
                    if (!G_StartTrackSlotWrap(volumeNum == -1 ? MAXVOLUMES : volumeNum, levelNum))
                        S_SetMusicPosition(position);
                }
                dispatch(tw);

            vInstruction(CON_PRELOADTRACKSLOTFORSWAP):
                // ASS can't even handle this command right now.
                insptr++;
                Gv_GetVarX(*(insptr++));
                Gv_GetVarX(*(insptr++));
                dispatch(tw);

            vInstruction(CON_SETMUSICPOSITION):
                insptr++;
                Gv_GetVarX(*(insptr++));
                dispatch(tw);
            vInstruction(CON_GETMUSICPOSITION): insptr += 2; dispatch(tw);

            vInstruction(CON_ACTIVATECHEAT):
                insptr++;
                tw = Gv_GetVarX(*(insptr++));
                if (EDUKE32_PREDICT_FALSE(numplayers != 1 || !(g_player[myconnectindex].ps->gm & MODE_GAME)))
                {
                    CON_ERRPRINTF("not in a single-player game.\n");
                    dispatch(tw);
                }
                osdcmd_cheatsinfo_stat.cheatnum = tw;
                dispatch(tw);

            vInstruction(CON_SETGAMEPALETTE):
                insptr++;
                P_SetGamePalette(&p, Gv_GetVarX(*(insptr++)), 2 + 16);
                dispatch(tw);

            vInstruction(CON_GETTEXTURECEILING):
                insptr++;
                aGameVars[g_textureVarID].global = sector[vm.pSprite->sectnum].ceilingpicnum;
                dispatch(tw);

            vInstruction(CON_IFPHEALTHL):
                insptr++;
                VM_CONDITIONAL(sprite[p.i].extra < *insptr);
                dispatch(tw);

            vInstruction(CON_IFPINVENTORY):
                insptr++;

                switch (*insptr++)
                {
                    case GET_STEROIDS:
                    case GET_SHIELD:
                    case GET_SCUBA:
                    case GET_HOLODUKE:
                    case GET_HEATS:
                    case GET_FIRSTAID:
                    case GET_BOOTS:
                    case GET_JETPACK: tw = (p.inv_amount[insptr[-1]] != *insptr); break;

                    case GET_ACCESS:
                        switch (vm.pSprite->pal)
                        {
                            case 0: tw  = (p.got_access & 1); break;
                            case 21: tw = (p.got_access & 2); break;
                            case 23: tw = (p.got_access & 4); break;
                        }
                        break;
                    default: tw = 0; CON_ERRPRINTF("invalid inventory item %d\n", (int32_t) * (insptr - 1));
                        dispatch(tw);
                }

                VM_CONDITIONAL(tw);
                dispatch(tw);

            vInstruction(CON_PSTOMP):
                insptr++;
                if (p.knee_incs == 0 && sprite[p.i].xrepeat >= 40)
                    if (cansee(vm.pSprite->x, vm.pSprite->y, vm.pSprite->z - ZOFFSET6, vm.pSprite->sectnum, p.pos.x, p.pos.y,
                               p.pos.z + ZOFFSET2, sprite[p.i].sectnum))
                    {
                        int numPlayers = g_mostConcurrentPlayers - 1;

                        for (; numPlayers >= 0; --numPlayers)
                        {
                            if (g_player[numPlayers].ps->actorsqu == vm.spriteNum)
                                break;
                        }

                        if (numPlayers == -1)
                        {
                            if (p.weapon_pos == 0)
                                p.weapon_pos = -1;

                            p.actorsqu  = vm.spriteNum;
                            p.knee_incs = 1;
                        }
                    }
                dispatch(tw);

            vInstruction(CON_IFAWAYFROMWALL):
            {
                int16_t otherSectnum = vm.pSprite->sectnum;
                tw                   = 0;

#define IFAWAYDIST 108

                updatesector(vm.pSprite->x + IFAWAYDIST, vm.pSprite->y + IFAWAYDIST, &otherSectnum);
                if (otherSectnum == vm.pSprite->sectnum)
                {
                    updatesector(vm.pSprite->x - IFAWAYDIST, vm.pSprite->y - IFAWAYDIST, &otherSectnum);
                    if (otherSectnum == vm.pSprite->sectnum)
                    {
                        updatesector(vm.pSprite->x + IFAWAYDIST, vm.pSprite->y - IFAWAYDIST, &otherSectnum);
                        if (otherSectnum == vm.pSprite->sectnum)
                        {
                            updatesector(vm.pSprite->x - IFAWAYDIST, vm.pSprite->y + IFAWAYDIST, &otherSectnum);
                            if (otherSectnum == vm.pSprite->sectnum)
                                tw = 1;
                        }
                    }
                }

                VM_CONDITIONAL(tw);

#undef IFAWAYDIST
            }
                dispatch(tw);

            vInstruction(CON_QUOTE):
                insptr++;

                if (EDUKE32_PREDICT_FALSE((unsigned)vm.playerNum >= MAXPLAYERS))
                {
                    CON_ERRPRINTF("invalid player %d\n", vm.playerNum);
                    dispatch(tw);
                }

                P_DoQuote(*(insptr++) | MAXQUOTES, &p);
                dispatch(tw);

            vInstruction(CON_USERQUOTE):
                insptr++;
                tw = Gv_GetVarX(*insptr++);

                if (EDUKE32_PREDICT_FALSE((unsigned)tw >= MAXQUOTES || apStrings[tw] == NULL))
                {
                    CON_ERRPRINTF("invalid quote %d\n", (int)tw);
                    dispatch(tw);
                }

                G_AddUserQuote(apStrings[tw]);
                dispatch(tw);

            vInstruction(CON_ECHO):
                insptr++;
                tw = Gv_GetVarX(*insptr++);

                if (EDUKE32_PREDICT_FALSE((unsigned)tw >= MAXQUOTES || apStrings[tw] == NULL))
                {
                    CON_ERRPRINTF("invalid quote %d\n", (int)tw);
                    dispatch(tw);
                }

                OSD_Printf("%s\n", apStrings[tw]);
                dispatch(tw);

            vInstruction(CON_RESPAWNHITAG):
                insptr++;
                switch (DYNAMICTILEMAP(vm.pSprite->picnum))
                {
#ifndef EDUKE32_STANDALONE
                    case FEM1__STATIC:
                    case FEM2__STATIC:
                    case FEM3__STATIC:
                    case FEM4__STATIC:
                    case FEM5__STATIC:
                    case FEM6__STATIC:
                    case FEM7__STATIC:
                    case FEM8__STATIC:
                    case FEM9__STATIC:
                    case FEM10__STATIC:
                    case PODFEM1__STATIC:
                    case NAKED1__STATIC:
                    case STATUE__STATIC:
                        if (vm.pSprite->yvel)
                            G_OperateRespawns(vm.pSprite->yvel);
                        break;
#endif
                    default:
                        if (vm.pSprite->hitag >= 0)
                            G_OperateRespawns(vm.pSprite->hitag);
                        break;
                }
                dispatch(tw);

            vInstruction(CON_IFSPRITEPAL):
                insptr++;
                VM_CONDITIONAL(vm.pSprite->pal == *insptr);
                dispatch(tw);

            vInstruction(CON_IFANGDIFFL):
                insptr++;
                tw = klabs(G_GetAngleDelta(fix16_to_int(p.q16ang), vm.pSprite->ang));
                VM_CONDITIONAL(tw <= *insptr);
                dispatch(tw);

            vInstruction(CON_IFNOSOUNDS): VM_CONDITIONAL(!A_CheckAnySoundPlaying(vm.spriteNum)); dispatch(tw);

            vInstruction(CON_SPRITEFLAGS):
                insptr++;
                vm.pActor->flags = Gv_GetVarX(*insptr++);
                dispatch(tw);

            vInstruction(CON_GETTICKS):
                insptr++;
                Gv_SetVarX(*insptr++, timerGetTicks());
                dispatch(tw);

            vInstruction(CON_GETCURRADDRESS):
                insptr++;
                tw = *insptr++;
                Gv_SetVarX(tw, (intptr_t)(insptr - apScript));
                dispatch(tw);

            vmErrorCase: // you're not supposed to be here
                VM_ScriptInfo(insptr, 64);
                debug_break();
                G_GameExit("An error has occurred in the " APPNAME " virtual machine.\n\n"
                           "If you are an end user, please e-mail the file " APPBASENAME ".log\n"
                           "along with links to any mods you're using to development@voidpoint.com.\n\n"
                           "If you are a developer, please attach all of your script files\n"
                           "along with instructions on how to reproduce this error.\n\n"
                           "Thank you!");
#if !defined CON_DIRECT_THREADING_DISPATCH
                break;
#endif
        }
    } while (loop && (vm.flags & (VM_RETURN|VM_KILL|VM_NOEXECUTE)) == 0);
}

// NORECURSE
void A_LoadActor(int32_t spriteNum)
{
    vm.spriteNum = spriteNum;           // Sprite ID
    vm.pSprite   = &sprite[spriteNum];  // Pointer to sprite structure
    vm.pActor    = &actor[spriteNum];

    if (g_tile[vm.pSprite->picnum].loadPtr == NULL)
        return;

    vm.pData      = &actor[spriteNum].t_data[0];  // Sprite's 'extra' data
    vm.playerNum  = -1;                           // Player ID
    vm.playerDist = -1;                           // Distance
    vm.pPlayer    = g_player[0].ps;

    vm.flags &= ~(VM_RETURN|VM_KILL|VM_NOEXECUTE);

    if ((unsigned)vm.pSprite->sectnum >= MAXSECTORS)
    {
        A_DeleteSprite(vm.spriteNum);
        return;
    }

    insptr = g_tile[vm.pSprite->picnum].loadPtr;
    VM_Execute(1);
    insptr = NULL;

    if (vm.flags & VM_KILL)
        A_DeleteSprite(vm.spriteNum);
}
#endif

void VM_UpdateAnim(int spriteNum, int32_t *pData)
{
#if !defined LUNATIC
    size_t const actionofs = AC_ACTION_ID(pData);
    auto const actionptr = (actionofs != 0 && actionofs + (ACTION_PARAM_COUNT-1) < (unsigned) g_scriptSize) ? &apScript[actionofs] : NULL;

    if (actionptr != NULL)
#endif
    {
#if !defined LUNATIC
        int const action_frames = actionptr[ACTION_NUMFRAMES];
        int const action_incval = actionptr[ACTION_INCVAL];
        int const action_delay  = actionptr[ACTION_DELAY];
#else
        int const action_frames = actor[spriteNum].ac.numframes;
        int const action_incval = actor[spriteNum].ac.incval;
        int const action_delay  = actor[spriteNum].ac.delay;
#endif
        auto actionticsptr = &AC_ACTIONTICS(&sprite[spriteNum], &actor[spriteNum]);
        *actionticsptr += TICSPERFRAME;

        if (*actionticsptr > action_delay)
        {
            *actionticsptr = 0;
            AC_ACTION_COUNT(pData)++;
            AC_CURFRAME(pData) += action_incval;
        }

        if (klabs(AC_CURFRAME(pData)) >= klabs(action_frames * action_incval))
            AC_CURFRAME(pData) = 0;
    }
}

// NORECURSE
void A_Execute(int spriteNum, int playerNum, int playerDist)
{
    // for some reason this is faster than using the C++ syntax; e.g vm = vmstate_t{ ... }
    vmstate_t const tempvm
    = { spriteNum, playerNum, playerDist, 0, &sprite[spriteNum], &actor[spriteNum].t_data[0], g_player[playerNum].ps, &actor[spriteNum] };
    vm = tempvm;

#ifdef LUNATIC
    int32_t killit=0;
#endif

/*
    if (g_netClient && A_CheckSpriteFlags(spriteNum, SFLAG_NULL))
    {
        A_DeleteSprite(spriteNum);
        return;
    }
*/

    if (g_netClient) // [75] The server should not overwrite its own randomseed
        randomseed = ticrandomseed;

    if (EDUKE32_PREDICT_FALSE((unsigned)vm.pSprite->sectnum >= MAXSECTORS))
    {
        if (A_CheckEnemySprite(vm.pSprite))
            P_AddKills(vm.pPlayer, 1);

        A_DeleteSprite(vm.spriteNum);
        return;
    }

    VM_UpdateAnim(vm.spriteNum, vm.pData);

    double t = timerGetHiTicks();
#ifdef LUNATIC
    int const picnum = vm.pSprite->picnum;

    if (L_IsInitialized(&g_ElState) && El_HaveActor(picnum))
        killit = (El_CallActor(&g_ElState, picnum, spriteNum, playerNum, playerDist)==1);
#else
    int const picnum = vm.pSprite->picnum;
    insptr = 4 + (g_tile[vm.pSprite->picnum].execPtr);
    VM_Execute(1);
    insptr = NULL;
#endif

    t = timerGetHiTicks()-t;
    g_actorTotalMs[picnum] += t;
    g_actorMinMs[picnum] = min(g_actorMinMs[picnum], t);
    g_actorMaxMs[picnum] = max(g_actorMaxMs[picnum], t);
    g_actorCalls[picnum]++;

#ifdef LUNATIC
    if (killit)
#else
    if (vm.flags & VM_KILL)
#endif
    {
        VM_DeleteSprite(spriteNum, playerNum);
        return;
    }

    VM_Move();

    if (vm.pSprite->statnum != STAT_ACTOR)
    {
#ifndef EDUKE32_STANDALONE
        if (vm.pSprite->statnum == STAT_STANDABLE)
        {
            switch (DYNAMICTILEMAP(vm.pSprite->picnum))
            {
                case RUBBERCAN__STATIC:
                case EXPLODINGBARREL__STATIC:
                case WOODENHORSE__STATIC:
                case HORSEONSIDE__STATIC:
                case CANWITHSOMETHING__STATIC:
                case FIREBARREL__STATIC:
                case NUKEBARREL__STATIC:
                case NUKEBARRELDENTED__STATIC:
                case NUKEBARRELLEAKED__STATIC:
                case TRIPBOMB__STATIC:
                case EGG__STATIC:
                    if (vm.pActor->timetosleep > 1)
                        vm.pActor->timetosleep--;
                    else if (vm.pActor->timetosleep == 1)
                        changespritestat(vm.spriteNum, STAT_ZOMBIEACTOR);
                default: break;
            }
        }
#endif
        return;
    }

    if (A_CheckEnemySprite(vm.pSprite))
    {
        if (vm.pSprite->xrepeat > 60 || (ud.respawn_monsters == 1 && vm.pSprite->extra <= 0))
            return;
    }
    else if (EDUKE32_PREDICT_FALSE(ud.respawn_items == 1 && (vm.pSprite->cstat & 32768)))
        return;

    if (A_CheckSpriteFlags(vm.spriteNum, SFLAG_USEACTIVATOR) && sector[vm.pSprite->sectnum].lotag & 16384)
        changespritestat(vm.spriteNum, STAT_ZOMBIEACTOR);
    else if (vm.pActor->timetosleep > 1)
        vm.pActor->timetosleep--;
    else if (vm.pActor->timetosleep == 1)
    {
        // hack for 1.3D fire sprites
#ifndef EDUKE32_STANDALONE
        if (EDUKE32_PREDICT_FALSE(g_scriptVersion == 13 && (vm.pSprite->picnum == FIRE || vm.pSprite->picnum == FIRE2)))
            return;
#endif
        changespritestat(vm.spriteNum, STAT_ZOMBIEACTOR);
    }
}

void G_SaveMapState(void)
{
    int const    levelNum = ud.volume_number * MAXLEVELS + ud.level_number;
    map_t *const pMapInfo = &g_mapInfo[levelNum];

    if (pMapInfo->savedstate == NULL)
    {
        pMapInfo->savedstate = (mapstate_t *) Xaligned_alloc(ACTOR_VAR_ALIGNMENT, sizeof(mapstate_t));
        Bmemset(pMapInfo->savedstate, 0, sizeof(mapstate_t));
    }

    mapstate_t *save = pMapInfo->savedstate;

    if (save == NULL)
        return;

    save->numwalls = numwalls;
    Bmemcpy(&save->wall[0],&wall[0],sizeof(walltype)*MAXWALLS);
    save->numsectors = numsectors;
    Bmemcpy(&save->sector[0],&sector[0],sizeof(sectortype)*MAXSECTORS);
    Bmemcpy(&save->sprite[0],&sprite[0],sizeof(spritetype)*MAXSPRITES);

    // If we're in EVENT_ANIMATESPRITES, we'll be saving pointer values to disk :-/
#if !defined LUNATIC
    if (g_currentEvent == EVENT_ANIMATESPRITES)
        initprintf("Line %d: savemapstate called from EVENT_ANIMATESPRITES. WHY?\n", g_errorLineNum);
#endif
    Bmemcpy(&save->spriteext[0],&spriteext[0],sizeof(spriteext_t)*MAXSPRITES);
#ifndef NEW_MAP_FORMAT
    Bmemcpy(&save->wallext[0],&wallext[0],sizeof(wallext_t)*MAXWALLS);
#endif

    save->numsprites = Numsprites;
    save->tailspritefree = tailspritefree;
    Bmemcpy(&save->headspritesect[0],&headspritesect[0],sizeof(headspritesect));
    Bmemcpy(&save->prevspritesect[0],&prevspritesect[0],sizeof(prevspritesect));
    Bmemcpy(&save->nextspritesect[0],&nextspritesect[0],sizeof(nextspritesect));
    Bmemcpy(&save->headspritestat[0],&headspritestat[0],sizeof(headspritestat));
    Bmemcpy(&save->prevspritestat[0],&prevspritestat[0],sizeof(prevspritestat));
    Bmemcpy(&save->nextspritestat[0],&nextspritestat[0],sizeof(nextspritestat));
#ifdef YAX_ENABLE
    save->numyaxbunches = numyaxbunches;
# if !defined NEW_MAP_FORMAT
    Bmemcpy(save->yax_bunchnum, yax_bunchnum, sizeof(yax_bunchnum));
    Bmemcpy(save->yax_nextwall, yax_nextwall, sizeof(yax_nextwall));
# endif
#endif
    Bmemcpy(&save->actor[0],&actor[0],sizeof(actor_t)*MAXSPRITES);

    save->g_cyclerCnt = g_cyclerCnt;
    Bmemcpy(save->g_cyclers, g_cyclers, sizeof(g_cyclers));
    Bmemcpy(&save->g_playerSpawnPoints[0],&g_playerSpawnPoints[0],sizeof(g_playerSpawnPoints));
    save->g_animWallCnt = g_animWallCnt;
    Bmemcpy(&save->SpriteDeletionQueue[0],&SpriteDeletionQueue[0],sizeof(SpriteDeletionQueue));
    save->g_spriteDeleteQueuePos = g_spriteDeleteQueuePos;
    Bmemcpy(&save->animwall[0],&animwall[0],sizeof(animwall));
    Bmemcpy(&save->origins[0],&g_origins[0],sizeof(g_origins));
    Bmemcpy(&save->g_mirrorWall[0],&g_mirrorWall[0],sizeof(g_mirrorWall));
    Bmemcpy(&save->g_mirrorSector[0],&g_mirrorSector[0],sizeof(g_mirrorSector));
    save->g_mirrorCount = g_mirrorCount;
    Bmemcpy(&save->show2dsector[0],&show2dsector[0],sizeof(show2dsector));
    save->g_cloudCnt = g_cloudCnt;
    Bmemcpy(&save->g_cloudSect[0],&g_cloudSect[0],sizeof(g_cloudSect));
    save->g_cloudX = g_cloudX;
    save->g_cloudY = g_cloudY;
    save->pskyidx = g_pskyidx;
    Bmemcpy(&save->g_animateGoal[0],&g_animateGoal[0],sizeof(g_animateGoal));
    Bmemcpy(&save->g_animateVel[0],&g_animateVel[0],sizeof(g_animateVel));
    save->g_animateCnt = g_animateCnt;
    Bmemcpy(&save->g_animateSect[0],&g_animateSect[0],sizeof(g_animateSect));

    G_Util_PtrToIdx(g_animatePtr, g_animateCnt, sector, P2I_FWD);
    Bmemcpy(&save->g_animatePtr[0],&g_animatePtr[0],sizeof(g_animatePtr));
    G_Util_PtrToIdx(g_animatePtr, g_animateCnt, sector, P2I_BACK);

    {
        EDUKE32_STATIC_ASSERT(sizeof(save->g_animatePtr) == sizeof(g_animatePtr));
    }

    save->g_playerSpawnCnt = g_playerSpawnCnt;
    save->g_earthquakeTime = g_earthquakeTime;
    save->lockclock        = lockclock;
    save->randomseed       = randomseed;
    save->g_globalRandom   = g_globalRandom;

#if !defined LUNATIC
    for (native_t i=g_gameVarCount-1; i>=0; i--)
    {
        if (aGameVars[i].flags & GAMEVAR_NORESET)
            continue;
        if (aGameVars[i].flags & GAMEVAR_PERPLAYER)
        {
            if (!save->vars[i])
                save->vars[i] = (intptr_t *)Xaligned_alloc(PLAYER_VAR_ALIGNMENT, MAXPLAYERS * sizeof(intptr_t));
            Bmemcpy(&save->vars[i][0], aGameVars[i].pValues, sizeof(intptr_t) * MAXPLAYERS);
        }
        else if (aGameVars[i].flags & GAMEVAR_PERACTOR)
        {
            if (!save->vars[i])
                save->vars[i] = (intptr_t *)Xaligned_alloc(ACTOR_VAR_ALIGNMENT, MAXSPRITES * sizeof(intptr_t));
            Bmemcpy(&save->vars[i][0], aGameVars[i].pValues, sizeof(intptr_t) * MAXSPRITES);
        }
        else
            save->vars[i] = (intptr_t *)aGameVars[i].global;
    }

    for (native_t i=g_gameArrayCount-1; i>=0; i--)
    {
        if ((aGameArrays[i].flags & GAMEARRAY_RESTORE) == 0)
            continue;

        save->arraysiz[i] = aGameArrays[i].size;
        Baligned_free(save->arrays[i]);
        save->arrays[i] = (intptr_t *)Xaligned_alloc(ARRAY_ALIGNMENT, Gv_GetArrayAllocSize(i));
        Bmemcpy(&save->arrays[i][0], aGameArrays[i].pValues, Gv_GetArrayAllocSize(i));
    }
#else
    int32_t slen;
    const char *svcode = El_SerializeGamevars(&slen, levelNum);

    if (slen < 0)
    {
        El_OnError("ERROR: savemapstate: serialization failed!");
    }
    else
    {
        char *savecode = Xstrdup(svcode);
        Bfree(save->savecode);
        save->savecode = savecode;
    }
#endif
    ototalclock = totalclock;
}

void G_RestoreMapState(void)
{
    int const   levelNum    = ud.volume_number * MAXLEVELS + ud.level_number;
    mapstate_t *pSavedState = g_mapInfo[levelNum].savedstate;

    if (pSavedState != NULL)
    {
        int playerHealth[MAXPLAYERS];

        for (native_t i=0; i<g_mostConcurrentPlayers; i++)
            playerHealth[i] = sprite[g_player[i].ps->i].extra;

        pub = NUMPAGES;
        pus = NUMPAGES;
        G_UpdateScreenArea();

        numwalls = pSavedState->numwalls;
        Bmemcpy(&wall[0],&pSavedState->wall[0],sizeof(walltype)*MAXWALLS);
#ifndef NEW_MAP_FORMAT
        Bmemcpy(&wallext[0],&pSavedState->wallext[0],sizeof(wallext_t)*MAXWALLS);
#endif
        numsectors = pSavedState->numsectors;
        Bmemcpy(&sector[0],&pSavedState->sector[0],sizeof(sectortype)*MAXSECTORS);
        Bmemcpy(&sprite[0],&pSavedState->sprite[0],sizeof(spritetype)*MAXSPRITES);
        Bmemcpy(&spriteext[0],&pSavedState->spriteext[0],sizeof(spriteext_t)*MAXSPRITES);

        // If we're restoring from EVENT_ANIMATESPRITES, all spriteext[].tspr
        // will be overwritten, so NULL them.
#if !defined LUNATIC
        if (g_currentEvent == EVENT_ANIMATESPRITES)
        {
            initprintf("Line %d: loadmapstate called from EVENT_ANIMATESPRITES. WHY?\n",g_errorLineNum);
            for (native_t i=0; i<MAXSPRITES; i++)
                spriteext[i].tspr = NULL;
        }
#endif
        Numsprites = pSavedState->numsprites;
        tailspritefree = pSavedState->tailspritefree;
        Bmemcpy(&headspritesect[0],&pSavedState->headspritesect[0],sizeof(headspritesect));
        Bmemcpy(&prevspritesect[0],&pSavedState->prevspritesect[0],sizeof(prevspritesect));
        Bmemcpy(&nextspritesect[0],&pSavedState->nextspritesect[0],sizeof(nextspritesect));
        Bmemcpy(&headspritestat[0],&pSavedState->headspritestat[0],sizeof(headspritestat));
        Bmemcpy(&prevspritestat[0],&pSavedState->prevspritestat[0],sizeof(prevspritestat));
        Bmemcpy(&nextspritestat[0],&pSavedState->nextspritestat[0],sizeof(nextspritestat));
#ifdef YAX_ENABLE
        numyaxbunches = pSavedState->numyaxbunches;
# if !defined NEW_MAP_FORMAT
        Bmemcpy(yax_bunchnum, pSavedState->yax_bunchnum, sizeof(yax_bunchnum));
        Bmemcpy(yax_nextwall, pSavedState->yax_nextwall, sizeof(yax_nextwall));
# endif
#endif
        Bmemcpy(&actor[0],&pSavedState->actor[0],sizeof(actor_t)*MAXSPRITES);

        g_cyclerCnt = pSavedState->g_cyclerCnt;
        Bmemcpy(g_cyclers, pSavedState->g_cyclers, sizeof(g_cyclers));
        Bmemcpy(&g_playerSpawnPoints[0],&pSavedState->g_playerSpawnPoints[0],sizeof(g_playerSpawnPoints));
        g_animWallCnt = pSavedState->g_animWallCnt;
        Bmemcpy(&SpriteDeletionQueue[0],&pSavedState->SpriteDeletionQueue[0],sizeof(SpriteDeletionQueue));
        g_spriteDeleteQueuePos = pSavedState->g_spriteDeleteQueuePos;
        Bmemcpy(&animwall[0],&pSavedState->animwall[0],sizeof(animwall));
        Bmemcpy(&g_origins[0],&pSavedState->origins[0],sizeof(g_origins));
        Bmemcpy(&g_mirrorWall[0],&pSavedState->g_mirrorWall[0],sizeof(g_mirrorWall));
        Bmemcpy(&g_mirrorSector[0],&pSavedState->g_mirrorSector[0],sizeof(g_mirrorSector));
        g_mirrorCount = pSavedState->g_mirrorCount;
        Bmemcpy(&show2dsector[0],&pSavedState->show2dsector[0],sizeof(show2dsector));
        g_cloudCnt = pSavedState->g_cloudCnt;
        Bmemcpy(&g_cloudSect[0],&pSavedState->g_cloudSect[0],sizeof(g_cloudSect));
        g_cloudX = pSavedState->g_cloudX;
        g_cloudY = pSavedState->g_cloudY;
        g_pskyidx = pSavedState->pskyidx;
        Bmemcpy(&g_animateGoal[0],&pSavedState->g_animateGoal[0],sizeof(g_animateGoal));
        Bmemcpy(&g_animateVel[0],&pSavedState->g_animateVel[0],sizeof(g_animateVel));
        g_animateCnt = pSavedState->g_animateCnt;
        Bmemcpy(&g_animateSect[0],&pSavedState->g_animateSect[0],sizeof(g_animateSect));

        Bmemcpy(&g_animatePtr[0],&pSavedState->g_animatePtr[0],sizeof(g_animatePtr));
        G_Util_PtrToIdx(g_animatePtr, g_animateCnt, sector, P2I_BACK);

        g_playerSpawnCnt = pSavedState->g_playerSpawnCnt;
        g_earthquakeTime = pSavedState->g_earthquakeTime;
        lockclock = pSavedState->lockclock;
        randomseed = pSavedState->randomseed;
        g_globalRandom = pSavedState->g_globalRandom;

#if !defined LUNATIC
        for (native_t i=g_gameVarCount-1; i>=0; i--)
        {
            if (aGameVars[i].flags & GAMEVAR_NORESET)
                continue;
            if (aGameVars[i].flags & GAMEVAR_PERPLAYER)
            {
                if (!pSavedState->vars[i])
                    continue;
                Bmemcpy(aGameVars[i].pValues, pSavedState->vars[i], sizeof(intptr_t) * MAXPLAYERS);
            }
            else if (aGameVars[i].flags & GAMEVAR_PERACTOR)
            {
                if (!pSavedState->vars[i])
                    continue;
                Bmemcpy(aGameVars[i].pValues, pSavedState->vars[i], sizeof(intptr_t) * MAXSPRITES);
            }
            else
                aGameVars[i].global = (intptr_t)pSavedState->vars[i];
        }

        for (native_t i=g_gameArrayCount-1; i>=0; i--)
        {
            if ((aGameArrays[i].flags & GAMEARRAY_RESTORE) == 0)
                continue;

            aGameArrays[i].size = pSavedState->arraysiz[i];
            Baligned_free(aGameArrays[i].pValues);
            aGameArrays[i].pValues = (intptr_t *) Xaligned_alloc(ARRAY_ALIGNMENT, Gv_GetArrayAllocSize(i));

            Bmemcpy(aGameArrays[i].pValues, pSavedState->arrays[i], Gv_GetArrayAllocSize(i));
        }

        Gv_RefreshPointers();
#else
        if (pSavedState->savecode)
        {
            El_RestoreGamevars(pSavedState->savecode);
        }
#endif
        // Update g_player[].ps->i (sprite indices of players) to be consistent
        // with just loaded sprites.
        // Otherwise, crashes may ensue: e.g. WGR2 SVN r391, map spiderden:
        // - walk forward (to door leading to other level "Shadowpine Forest")
        // - in new level, walk backward to get back to the Spider Den
        // - walk backward to the door leading to Shadowpine Forest --> crash.
        for (native_t SPRITES_OF(STAT_PLAYER, i))
        {
            int32_t snum = P_Get(i);
            Bassert((unsigned)snum < MAXPLAYERS);
            g_player[snum].ps->i = i;
        }

        for (native_t i=0; i<g_mostConcurrentPlayers; i++)
            sprite[g_player[i].ps->i].extra = playerHealth[i];

        if (g_player[myconnectindex].ps->over_shoulder_on != 0)
        {
            CAMERADIST = 0;
            CAMERACLOCK = 0;
            g_player[myconnectindex].ps->over_shoulder_on = 1;
        }

        screenpeek = myconnectindex;

#ifndef EDUKE32_STANDALONE
        if (ud.lockout)
        {
            for (native_t x=g_animWallCnt-1; x>=0; x--)
                switch (DYNAMICTILEMAP(wall[animwall[x].wallnum].picnum))
                {
                    case FEMPIC1__STATIC: wall[animwall[x].wallnum].picnum = BLANKSCREEN; break;
                case FEMPIC2__STATIC:
                    case FEMPIC3__STATIC: wall[animwall[x].wallnum].picnum = SCREENBREAK6; break;
                }
        }
#if 0
        else
        {
            for (native_t x=g_numAnimWalls-1; x>=0; x--)
                if (wall[animwall[x].wallnum].extra >= 0)
                    wall[animwall[x].wallnum].picnum = wall[animwall[x].wallnum].extra;
        }
#endif
#endif
#ifdef YAX_ENABLE
        sv_postyaxload();
#endif
        G_ResetInterpolations();

        Net_ResetPrediction();

        G_ClearFIFO();
        G_ResetTimers(0);
    }
}

#ifdef LUNATIC
void VM_FallSprite(int32_t i) { VM_Fall(i, &sprite[i]); }

int32_t VM_ResetPlayer2(int32_t snum, int32_t flags) { return VM_ResetPlayer(snum, 0, flags); }

int32_t VM_CheckSquished2(int32_t i, int32_t snum)
{
    vm.spriteNum = i;
    vm.pSprite   = &sprite[i];
    vm.playerNum = snum;
    vm.pPlayer   = g_player[snum].ps;

    return VM_CheckSquished();
}
#endif

// MYOS* CON commands.
LUNATIC_EXTERN void VM_DrawTileGeneric(int32_t x, int32_t y, int32_t zoom, int32_t tilenum, int32_t shade, int32_t orientation, int32_t p)
{
    orientation &= (ROTATESPRITE_MAX-1);

    int const rotAngle = (orientation&4) ? 1024 : 0;

    if (!(orientation&ROTATESPRITE_FULL16))
    {
        x<<=16;
        y<<=16;
    }

    rotatesprite_win(x, y, zoom, rotAngle, tilenum, shade, p, 2|orientation);
}

#if !defined LUNATIC
void VM_DrawTile(int32_t x, int32_t y, int32_t tilenum, int32_t shade, int32_t orientation)
{
    DukePlayer_t *pPlayer = g_player[screenpeek].ps;
    int32_t       tilePal = pPlayer->cursectnum >= 0 ? sector[pPlayer->cursectnum].floorpal : 0;

    VM_DrawTileGeneric(x, y, 65536, tilenum, shade, orientation, tilePal);
}

void VM_DrawTilePal(int32_t x, int32_t y, int32_t tilenum, int32_t shade, int32_t orientation, int32_t p)
{
    VM_DrawTileGeneric(x, y, 65536, tilenum, shade, orientation, p);
}

void VM_DrawTileSmall(int32_t x, int32_t y, int32_t tilenum, int32_t shade, int32_t orientation)
{
    DukePlayer_t *const pPlayer = g_player[screenpeek].ps;
    int32_t             tilePal = pPlayer->cursectnum >= 0 ? sector[pPlayer->cursectnum].floorpal : 0;

    VM_DrawTileGeneric(x, y, 32768, tilenum, shade, orientation, tilePal);
}

void VM_DrawTilePalSmall(int32_t x, int32_t y, int32_t tilenum, int32_t shade, int32_t orientation, int32_t p)
{
    VM_DrawTileGeneric(x, y, 32768, tilenum, shade, orientation, p);
}
#endif
