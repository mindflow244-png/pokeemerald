#include "global.h"
#include "battle.h"
#include "battle_main.h"
#include "event_data.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "fieldmap.h"
#include "main.h"
#include "menu.h"
#include "overworld.h"
#include "pokemon.h"
#include "random.h"
#include "rt_battle.h"
#include "script.h"
#include "string_util.h"
#include "text.h"
#include "window.h"
#include "constants/abilities.h"
#include "constants/battle_move_effects.h"
#include "constants/event_objects.h"
#include "constants/maps.h"
#include "constants/moves.h"
#include "constants/pokemon.h"
#include "constants/species.h"

// ---------------------------------------------------------------------------
// Combate en tiempo real estilo Chrono Trigger, version minima (etapa S1).
// Cada valor de abajo sale de una hoja de rt-design/ (se indica entre parentesis).
// Si cambias un valor, cambia primero la hoja y luego este archivo.
// ---------------------------------------------------------------------------

// atb_params.json
#define ATB_MAX         6000    // ATB_MAX, ATB_ATTACK_MIN
#define ATB_BASE_FILL   10      // ATB_BASE_FILL
#define ATB_SPEED_DIV   8       // ATB_SPEED_DIV
#define ATB_START       0       // ATB_START_PLAYER, ATB_START_ENEMY
#define ATB_AFTER_ACTION 0      // ATB_AFTER_ACTION

// enemy_visible_types.json (ENEMY_ROUTE_ZIGZAGOON)
#define ENEMY_SPECIES   SPECIES_ZIGZAGOON
#define ENEMY_GFX       OBJ_EVENT_GFX_ZIGZAGOON_1
#define ENEMY_LEVEL     3
#define ENEMY_AGGRO_TILES 2
#define ENEMY_LEASH_TILES 5     // si te alejas mas que esto la pelea se corta (no esta en las hojas aun)
#define ENEMY_LOCALID   0xE0

#define HUD_REFRESH_FRAMES 6
#define END_SHOW_FRAMES    90
#define HUD_WIDTH_TILES   20
#define HUD_HEIGHT_TILES   6

enum
{
    RT_IDLE,
    RT_FIGHT,
    RT_END,
};

enum
{
    RESULT_CRIT       = 1 << 0,
    RESULT_SUPER      = 1 << 1,
    RESULT_WEAK       = 1 << 2,
    RESULT_NO_EFFECT  = 1 << 3,
    RESULT_MISS       = 1 << 4,
};

struct RtHit
{
    s32 damage;
    u8 flags;
};

static u8 sState;
static u8 sHudOpen;
static u8 sHudWindowId;
static u8 sSpawnDone;
static u8 sMapGroup;
static u8 sMapNum;
static u16 sPlayerBar;
static u16 sEnemyBar;
static u16 sRefreshTimer;
static u16 sEndTimer;
static u8 sLogText[32];
static u8 sLineBuf[48];

// copia de la tabla privada de battle_script_commands.c (Cmd_critcalc)
static const u16 sCriticalHitChance[] = {16, 8, 4, 3, 2};

static const struct WindowTemplate sHudWindowTemplate =
{
    .bg = 0,
    .tilemapLeft = 1,
    .tilemapTop = 1,
    .width = HUD_WIDTH_TILES,
    .height = HUD_HEIGHT_TILES,
    .paletteNum = 15,
    .baseBlock = 0xC0,
};

static const u8 sText_YouHp[] = _("YOU HP ");
static const u8 sText_FoeHp[] = _("FOE HP ");
static const u8 sText_Slash[] = _("/");
static const u8 sText_Atb[] = _(" ATB ");
static const u8 sText_ReadyMark[] = _(" A!");
static const u8 sText_YouHit[] = _("YOU HIT ");
static const u8 sText_FoeHit[] = _("FOE HIT ");
static const u8 sText_YouMiss[] = _("YOU MISS");
static const u8 sText_FoeMiss[] = _("FOE MISS");
static const u8 sText_Crit[] = _(" CRIT");
static const u8 sText_Super[] = _(" SUPER");
static const u8 sText_Weak[] = _(" WEAK");
static const u8 sText_NoEffect[] = _(" NO EFFECT");
static const u8 sText_NoMove[] = _("NO USABLE MOVE");
static const u8 sText_FoeFainted[] = _("FOE FAINTED");
static const u8 sText_YouFainted[] = _("YOU FAINTED");
static const u8 sText_Empty[] = _("");

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

static s32 Abs32(s32 value)
{
    return value < 0 ? -value : value;
}

static u8 GetPlayerLeadIndex(void)
{
    u8 i;

    for (i = 0; i < PARTY_SIZE; i++)
    {
        if (GetMonData(&gPlayerParty[i], MON_DATA_SPECIES) != SPECIES_NONE
         && !GetMonData(&gPlayerParty[i], MON_DATA_IS_EGG)
         && GetMonData(&gPlayerParty[i], MON_DATA_HP) > 0)
            return i;
    }
    return PARTY_SIZE;
}

static u16 GetFillPerFrame(struct Pokemon *mon)
{
    u32 speed = GetMonData(mon, MON_DATA_SPEED);

    return ATB_BASE_FILL + speed / ATB_SPEED_DIV; // atb_params: ATB_BASE_FILL + Velocidad / ATB_SPEED_DIV
}

static u8 GetEnemyObjectId(void)
{
    return GetObjectEventIdByLocalIdAndMap(ENEMY_LOCALID, gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);
}

static void RemoveEnemyObject(void)
{
    RemoveObjectEventByLocalIdAndMap(ENEMY_LOCALID, gSaveBlock1Ptr->location.mapNum, gSaveBlock1Ptr->location.mapGroup);
}

static s32 GetDistanceToEnemy(void)
{
    s16 px, py;
    u8 objId = GetEnemyObjectId();

    if (objId >= OBJECT_EVENTS_COUNT)
        return 999;
    PlayerGetDestCoords(&px, &py);
    return Abs32(px - gObjectEvents[objId].currentCoords.x) + Abs32(py - gObjectEvents[objId].currentCoords.y);
}

// ---------------------------------------------------------------------------
// Ventana con las barras (HUD)
// ---------------------------------------------------------------------------

static void OpenHud(void)
{
    if (sHudOpen)
        return;
    sHudWindowId = AddWindow(&sHudWindowTemplate);
    SetStandardWindowBorderStyle(sHudWindowId, FALSE);
    sHudOpen = TRUE;
    sRefreshTimer = 0;
}

static void CloseHud(void)
{
    if (!sHudOpen)
        return;
    ClearStdWindowAndFrameToTransparent(sHudWindowId, TRUE);
    RemoveWindow(sHudWindowId);
    sHudOpen = FALSE;
}

static void BuildStatusLine(u8 *dest, const u8 *label, u32 hp, u32 maxHp, u32 bar, bool8 ready)
{
    u8 *p;

    p = StringCopy(dest, label);
    p = ConvertIntToDecimalStringN(p, hp, STR_CONV_MODE_LEFT_ALIGN, 3);
    p = StringCopy(p, sText_Slash);
    p = ConvertIntToDecimalStringN(p, maxHp, STR_CONV_MODE_LEFT_ALIGN, 3);
    p = StringCopy(p, sText_Atb);
    p = ConvertIntToDecimalStringN(p, bar * 100 / ATB_MAX, STR_CONV_MODE_LEFT_ALIGN, 3);
    if (ready)
        StringCopy(p, sText_ReadyMark);
}

static void DrawHud(void)
{
    u8 lead;

    if (!sHudOpen)
        return;
    lead = GetPlayerLeadIndex();
    FillWindowPixelBuffer(sHudWindowId, PIXEL_FILL(1));

    if (lead < PARTY_SIZE)
    {
        BuildStatusLine(sLineBuf, sText_YouHp,
                        GetMonData(&gPlayerParty[lead], MON_DATA_HP),
                        GetMonData(&gPlayerParty[lead], MON_DATA_MAX_HP),
                        sPlayerBar, sPlayerBar >= ATB_MAX);
        AddTextPrinterParameterized(sHudWindowId, FONT_NORMAL, sLineBuf, 2, 1, 0, NULL);
    }

    BuildStatusLine(sLineBuf, sText_FoeHp,
                    GetMonData(&gEnemyParty[0], MON_DATA_HP),
                    GetMonData(&gEnemyParty[0], MON_DATA_MAX_HP),
                    sEnemyBar, FALSE);
    AddTextPrinterParameterized(sHudWindowId, FONT_NORMAL, sLineBuf, 2, 17, 0, NULL);

    AddTextPrinterParameterized(sHudWindowId, FONT_NORMAL, sLogText, 2, 33, 0, NULL);
    CopyWindowToVram(sHudWindowId, COPYWIN_GFX);
}

static void SetLog(const u8 *text)
{
    StringCopy(sLogText, text);
    sRefreshTimer = 0;
}

// ---------------------------------------------------------------------------
// Danos: se reutiliza CalculateBaseDamage y las tablas del juego
// ---------------------------------------------------------------------------

static void MonToBattleMon(struct Pokemon *mon, struct BattlePokemon *dst)
{
    u8 i;
    u16 species = GetMonData(mon, MON_DATA_SPECIES);

    memset(dst, 0, sizeof(*dst));
    dst->species = species;
    dst->attack = GetMonData(mon, MON_DATA_ATK);
    dst->defense = GetMonData(mon, MON_DATA_DEF);
    dst->speed = GetMonData(mon, MON_DATA_SPEED);
    dst->spAttack = GetMonData(mon, MON_DATA_SPATK);
    dst->spDefense = GetMonData(mon, MON_DATA_SPDEF);
    for (i = 0; i < MAX_MON_MOVES; i++)
    {
        dst->moves[i] = GetMonData(mon, MON_DATA_MOVE1 + i);
        dst->pp[i] = GetMonData(mon, MON_DATA_PP1 + i);
    }
    for (i = 0; i < NUM_BATTLE_STATS; i++)
        dst->statStages[i] = DEFAULT_STAT_STAGE;
    dst->ability = GetMonAbility(mon);
    dst->types[0] = gSpeciesInfo[species].types[0];
    dst->types[1] = gSpeciesInfo[species].types[1];
    dst->hp = GetMonData(mon, MON_DATA_HP);
    dst->maxHP = GetMonData(mon, MON_DATA_MAX_HP);
    dst->level = GetMonData(mon, MON_DATA_LEVEL);
    dst->item = GetMonData(mon, MON_DATA_HELD_ITEM);
    dst->status1 = GetMonData(mon, MON_DATA_STATUS);
    dst->status2 = 0;
}

static s32 ApplyTypeEffectiveness(s32 damage, u8 moveType, u8 defType1, u8 defType2, u8 *flags)
{
    s32 i = 0;
    u8 mul;

    // Misma recorrida que Cmd_typecalc (sin Foresight)
    while (TYPE_EFFECT_ATK_TYPE(i) != TYPE_ENDTABLE)
    {
        if (TYPE_EFFECT_ATK_TYPE(i) == TYPE_FORESIGHT)
        {
            i += 3;
            continue;
        }
        if (TYPE_EFFECT_ATK_TYPE(i) == moveType)
        {
            mul = TYPE_EFFECT_MULTIPLIER(i);
            if (TYPE_EFFECT_DEF_TYPE(i) == defType1
             || (TYPE_EFFECT_DEF_TYPE(i) == defType2 && defType1 != defType2))
            {
                damage = damage * mul / 10;
                if (mul == TYPE_MUL_NO_EFFECT)
                    *flags |= RESULT_NO_EFFECT;
                else if (mul > TYPE_MUL_NORMAL)
                    *flags |= RESULT_SUPER;
                else if (mul < TYPE_MUL_NORMAL)
                    *flags |= RESULT_WEAK;
                if (mul != TYPE_MUL_NO_EFFECT && damage == 0)
                    damage = 1;
            }
        }
        i += 3;
    }
    return damage;
}

// DMG_01..DMG_05 de damage_pipeline.json
static void ResolveHit(struct BattlePokemon *atk, struct BattlePokemon *def, u16 move, struct RtHit *hit)
{
    u32 savedTypeFlags = gBattleTypeFlags;
    u16 savedWeather = gBattleWeather;
    u8 savedCount = gBattlersCount;
    u8 critChance;
    u8 moveType = gBattleMoves[move].type;
    u8 accuracy = gBattleMoves[move].accuracy;
    u8 effect = gBattleMoves[move].effect;
    s32 damage;

    hit->damage = 0;
    hit->flags = 0;

    // DMG_02 precision (sin etapas de precision/evasion en S1)
    if (accuracy != 0 && (Random() % 100) + 1 > accuracy)
    {
        hit->flags = RESULT_MISS;
        return;
    }

    // DMG_03 critico (misma formula que Cmd_critcalc, sin objetos ni Focus Energy)
    critChance = (effect == EFFECT_HIGH_CRITICAL)
               + (effect == EFFECT_SKY_ATTACK)
               + (effect == EFFECT_BLAZE_KICK)
               + (effect == EFFECT_POISON_TAIL);
    if (critChance >= ARRAY_COUNT(sCriticalHitChance))
        critChance = ARRAY_COUNT(sCriticalHitChance) - 1;
    gCritMultiplier = 1;
    if (def->ability != ABILITY_BATTLE_ARMOR && def->ability != ABILITY_SHELL_ARMOR
     && !(Random() % sCriticalHitChance[critChance]))
    {
        gCritMultiplier = 2;
        hit->flags |= RESULT_CRIT;
    }

    // DMG_04 dano base: CalculateBaseDamage necesita estos globals de batalla
    gBattleTypeFlags = 0;
    gBattleWeather = 0;
    gBattlersCount = 2;
    gBattleMons[0] = *atk;
    gBattleMons[1] = *def;
    gCurrentMove = move;
    damage = CalculateBaseDamage(&gBattleMons[0], &gBattleMons[1], move, 0, 0, 0, 0, 1);
    gBattleTypeFlags = savedTypeFlags;
    gBattleWeather = savedWeather;
    gBattlersCount = savedCount;

    damage *= gCritMultiplier;

    // STAB (igual que Cmd_typecalc)
    if (atk->types[0] == moveType || atk->types[1] == moveType)
        damage = damage * 15 / 10;

    // DMG_05 efectividad de tipos
    if (def->ability == ABILITY_LEVITATE && moveType == TYPE_GROUND)
    {
        hit->flags |= RESULT_NO_EFFECT;
        damage = 0;
    }
    else
    {
        damage = ApplyTypeEffectiveness(damage, moveType, def->types[0], def->types[1], &hit->flags);
    }

    if (damage < 1 && !(hit->flags & RESULT_NO_EFFECT))
        damage = 1;
    hit->damage = damage;
}

static void LogHit(bool8 fromPlayer, struct RtHit *hit)
{
    u8 *p;

    if (hit->flags & RESULT_MISS)
    {
        SetLog(fromPlayer ? sText_YouMiss : sText_FoeMiss);
        return;
    }
    p = StringCopy(sLogText, fromPlayer ? sText_YouHit : sText_FoeHit);
    p = ConvertIntToDecimalStringN(p, hit->damage, STR_CONV_MODE_LEFT_ALIGN, 3);
    if (hit->flags & RESULT_CRIT)
        p = StringCopy(p, sText_Crit);
    if (hit->flags & RESULT_SUPER)
        p = StringCopy(p, sText_Super);
    if (hit->flags & RESULT_WEAK)
        p = StringCopy(p, sText_Weak);
    if (hit->flags & RESULT_NO_EFFECT)
        p = StringCopy(p, sText_NoEffect);
    sRefreshTimer = 0;
}

static void SubtractHp(struct Pokemon *mon, s32 damage)
{
    s32 hp = GetMonData(mon, MON_DATA_HP);

    hp -= damage;
    if (hp < 0)
        hp = 0;
    SetMonData(mon, MON_DATA_HP, &hp);
}

// Devuelve TRUE si el jugador pudo atacar (y por lo tanto gasta la barra)
static bool8 DoPlayerAttack(void)
{
    u8 lead = GetPlayerLeadIndex();
    u8 slot;
    u16 move;
    u8 pp;
    struct BattlePokemon atk, def;
    struct RtHit hit;

    if (lead >= PARTY_SIZE)
        return FALSE;

    // S1: se usa el primer movimiento con dano y PP. La eleccion de movimiento llega despues.
    for (slot = 0; slot < MAX_MON_MOVES; slot++)
    {
        move = GetMonData(&gPlayerParty[lead], MON_DATA_MOVE1 + slot);
        pp = GetMonData(&gPlayerParty[lead], MON_DATA_PP1 + slot);
        if (move != MOVE_NONE && pp > 0 && gBattleMoves[move].power > 0)
            break;
    }
    if (slot >= MAX_MON_MOVES)
    {
        SetLog(sText_NoMove);
        return FALSE;
    }

    pp--;
    SetMonData(&gPlayerParty[lead], MON_DATA_PP1 + slot, &pp);

    MonToBattleMon(&gPlayerParty[lead], &atk);
    MonToBattleMon(&gEnemyParty[0], &def);
    ResolveHit(&atk, &def, move, &hit);
    SubtractHp(&gEnemyParty[0], hit.damage);
    LogHit(TRUE, &hit);
    return TRUE;
}

static void DoEnemyAttack(void)
{
    u8 lead = GetPlayerLeadIndex();
    u8 slot;
    u8 candidates[MAX_MON_MOVES];
    u8 count = 0;
    u16 move;
    struct BattlePokemon atk, def;
    struct RtHit hit;

    if (lead >= PARTY_SIZE)
        return;

    for (slot = 0; slot < MAX_MON_MOVES; slot++)
    {
        move = GetMonData(&gEnemyParty[0], MON_DATA_MOVE1 + slot);
        if (move != MOVE_NONE && gBattleMoves[move].power > 0)
            candidates[count++] = slot;
    }
    if (count == 0)
        return;

    move = GetMonData(&gEnemyParty[0], MON_DATA_MOVE1 + candidates[Random() % count]);
    MonToBattleMon(&gEnemyParty[0], &atk);
    MonToBattleMon(&gPlayerParty[lead], &def);
    ResolveHit(&atk, &def, move, &hit);
    SubtractHp(&gPlayerParty[lead], hit.damage);
    LogHit(FALSE, &hit);
}

// ---------------------------------------------------------------------------
// Maquina de estados
// ---------------------------------------------------------------------------

static void StartFight(void)
{
    sState = RT_FIGHT;
    sPlayerBar = ATB_START;
    sEnemyBar = ATB_START;
    SetLog(sText_Empty);
    OpenHud();
    DrawHud();
}

static void EndFight(bool8 removeEnemy, const u8 *message)
{
    if (removeEnemy)
        RemoveEnemyObject();
    SetLog(message);
    DrawHud();
    sState = RT_END;
    sEndTimer = END_SHOW_FRAMES;
}

static bool8 FindSpawnSpot(s16 *outX, s16 *outY)
{
    static const s8 dirX[] = {0, 0, -1, 1};
    static const s8 dirY[] = {-1, 1, 0, 0};
    s16 px, py, x, y;
    s32 dist;
    u8 dir;

    PlayerGetDestCoords(&px, &py);
    for (dist = 4; dist >= 3; dist--)
    {
        for (dir = 0; dir < 4; dir++)
        {
            x = px + dirX[dir] * dist;
            y = py + dirY[dir] * dist;
            if (MapGridGetCollisionAt(x, y) == 0
             && GetObjectEventIdByXY(x, y) == OBJECT_EVENTS_COUNT)
            {
                *outX = x;
                *outY = y;
                return TRUE;
            }
        }
    }
    return FALSE;
}

static void TrySpawnEnemy(void)
{
    s16 x, y;

    if (GetPlayerLeadIndex() >= PARTY_SIZE)
        return;
    if (gSaveBlock1Ptr->location.mapGroup != MAP_GROUP(MAP_ROUTE101)
     || gSaveBlock1Ptr->location.mapNum != MAP_NUM(MAP_ROUTE101))
        return;
    if (!FindSpawnSpot(&x, &y))
        return;

    CreateMon(&gEnemyParty[0], ENEMY_SPECIES, ENEMY_LEVEL, USE_RANDOM_IVS, FALSE, 0, OT_ID_RANDOM_NO_SHINY, 0);
    SpawnSpecialObjectEventParameterized(ENEMY_GFX, MOVEMENT_TYPE_FACE_DOWN, ENEMY_LOCALID, x, y,
                                         gObjectEvents[gPlayerAvatar.objectEventId].currentElevation);
    sSpawnDone = TRUE;
}

static void TickIdle(void)
{
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
        return;

    if (!sSpawnDone)
    {
        TrySpawnEnemy();
        return;
    }

    if (GetPlayerLeadIndex() < PARTY_SIZE
     && GetDistanceToEnemy() <= ENEMY_AGGRO_TILES
     && GetMonData(&gEnemyParty[0], MON_DATA_HP) > 0)
        StartFight();
}

static void TickFight(u16 newKeys)
{
    u8 lead;
    u16 fill;

    // Mientras hay un dialogo o menu abierto el tiempo se detiene (como el modo Espera)
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
        return;

    lead = GetPlayerLeadIndex();
    if (lead >= PARTY_SIZE)
    {
        EndFight(FALSE, sText_YouFainted);
        return;
    }
    if (GetEnemyObjectId() >= OBJECT_EVENTS_COUNT)
    {
        EndFight(FALSE, sText_Empty);
        return;
    }
    if (GetDistanceToEnemy() > ENEMY_LEASH_TILES)
    {
        // Te alejaste: la pelea termina pero el enemigo sigue ahi
        EndFight(FALSE, sText_Empty);
        return;
    }

    // Las dos barras suben solas segun la velocidad de cada uno
    fill = GetFillPerFrame(&gPlayerParty[lead]);
    if (sPlayerBar + fill > ATB_MAX)
        sPlayerBar = ATB_MAX;
    else
        sPlayerBar += fill;

    fill = GetFillPerFrame(&gEnemyParty[0]);
    if (sEnemyBar + fill > ATB_MAX)
        sEnemyBar = ATB_MAX;
    else
        sEnemyBar += fill;

    // Solo se puede atacar con la barra llena (atb_params: ATB_ATTACK_MIN)
    if (sPlayerBar >= ATB_MAX && (newKeys & A_BUTTON))
    {
        if (DoPlayerAttack())
            sPlayerBar = ATB_AFTER_ACTION;
        if (GetMonData(&gEnemyParty[0], MON_DATA_HP) == 0)
        {
            EndFight(TRUE, sText_FoeFainted);
            return;
        }
    }

    if (sEnemyBar >= ATB_MAX)
    {
        DoEnemyAttack();
        sEnemyBar = ATB_AFTER_ACTION;
        if (GetPlayerLeadIndex() >= PARTY_SIZE)
        {
            EndFight(TRUE, sText_YouFainted);
            return;
        }
    }

    if (sRefreshTimer == 0)
    {
        DrawHud();
        sRefreshTimer = HUD_REFRESH_FRAMES;
    }
    else
    {
        sRefreshTimer--;
    }
}

static void TickEnd(void)
{
    if (sEndTimer > 0)
    {
        sEndTimer--;
        return;
    }
    CloseHud();
    sState = RT_IDLE;
}

static void CheckMapChange(void)
{
    if (gSaveBlock1Ptr->location.mapGroup == sMapGroup && gSaveBlock1Ptr->location.mapNum == sMapNum)
        return;

    sMapGroup = gSaveBlock1Ptr->location.mapGroup;
    sMapNum = gSaveBlock1Ptr->location.mapNum;
    // Al cambiar de mapa el juego reinicia las ventanas y quita al enemigo
    sHudOpen = FALSE;
    sState = RT_IDLE;
    sSpawnDone = FALSE;
}

void RT_Tick(u16 newKeys)
{
    CheckMapChange();
    switch (sState)
    {
    case RT_IDLE:
        TickIdle();
        break;
    case RT_FIGHT:
        TickFight(newKeys);
        break;
    case RT_END:
        TickEnd();
        break;
    }
}

bool8 RT_IsActive(void)
{
    return sState != RT_IDLE;
}

// El enemigo se crea al vuelo y no tiene datos de mapa (plantilla con script). Si el jugador pulsa A
// frente a el, el juego intentaria leer ese script inexistente. Por eso se bloquea la interaccion
// con A mientras dura la pelea o mientras el enemigo esta pegado al jugador.
bool8 RT_ShouldBlockInteraction(void)
{
    if (sState != RT_IDLE)
        return TRUE;
    return sSpawnDone && GetDistanceToEnemy() <= 1;
}
