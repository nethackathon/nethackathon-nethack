/* NetHack 3.7	music.c	$NHDT-Date: 1736530208 2025/01/10 09:30:08 $  $NHDT-Branch: NetHack-3.7 $:$NHDT-Revision: 1.120 $ */
/*      Copyright (c) 1989 by Jean-Christophe Collet */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * This file contains the different functions designed to manipulate the
 * musical instruments and their various effects.
 *
 * The list of instruments / effects is :
 *
 * (wooden) flute       may calm snakes if player has enough dexterity
 * magic flute          may put monsters to sleep:  area of effect depends
 *                      on player level.
 * (tooled) horn        Will awaken monsters:  area of effect depends on
 *                      player level.  May also scare monsters.
 * fire horn            Acts like a wand of fire.
 * frost horn           Acts like a wand of cold.
 * bugle                Will awaken soldiers (if any):  area of effect depends
 *                      on player level.
 * (wooden) harp        May calm nymph if player has enough dexterity.
 * magic harp           Charm monsters:  area of effect depends on player
 *                      level.
 * (leather) drum       Will awaken monsters like the horn.
 * drum of earthquake   Will initiate an earthquake whose intensity depends
 *                      on player level.  That is, it creates random pits
 *                      called here chasms.
 */

#include "hack.h"

staticfn void awaken_scare(struct monst *, boolean);
staticfn void awaken_monsters(int);
staticfn void put_monsters_to_sleep(int);
staticfn void charm_snakes(int);
staticfn void calm_nymphs(int);
staticfn void charm_monsters(int);
staticfn void do_pit(coordxy, coordxy, unsigned);
staticfn void do_earthquake(int);
staticfn const char *generic_lvl_desc(void);
staticfn int do_improvisation(struct obj *);
staticfn char *improvised_notes(boolean *);

/* wake up monster, possibly scare it */
staticfn void
awaken_scare(struct monst *mtmp, boolean scary)
{
    mtmp->msleeping = 0;
    mtmp->mcanmove = 1;
    mtmp->mfrozen = 0;
    /* may scare some monsters -- waiting monsters excluded */
    if (!unique_corpstat(mtmp->data)
        && (mtmp->mstrategy & STRAT_WAITMASK) != 0)
        mtmp->mstrategy &= ~STRAT_WAITMASK;
    else if (scary
             && !mindless(mtmp->data)
             && !resist(mtmp, TOOL_CLASS, 0, NOTELL)
             /* some monsters are immune */
             && onscary(0, 0, mtmp))
        monflee(mtmp, 0, FALSE, TRUE);
}

/*
 * Wake every monster in range...
 */

staticfn void
awaken_monsters(int distance)
{
    struct monst *mtmp;
    int distm;

    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp))
            continue;
        if ((distm = mdistu(mtmp)) < distance)
            awaken_scare(mtmp, (distm < distance / 3));
    }
}

/*
 * Make monsters fall asleep.  Note that they may resist the spell.
 */

staticfn void
put_monsters_to_sleep(int distance)
{
    struct monst *mtmp;

    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp))
            continue;
        if (mdistu(mtmp) < distance
            && sleep_monst(mtmp, d(10, 10), TOOL_CLASS)) {
            mtmp->msleeping = 1; /* 10d10 turns + wake_nearby to rouse */
            slept_monst(mtmp);
        }
    }
}

/*
 * Charm snakes in range.  Note that the snakes are NOT tamed.
 */

staticfn void
charm_snakes(int distance)
{
    struct monst *mtmp;
    int could_see_mon, was_peaceful;

    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp))
            continue;
        if (mtmp->data->mlet == S_SNAKE && mtmp->mcanmove
            && mdistu(mtmp) < distance) {
            was_peaceful = mtmp->mpeaceful;
            mtmp->mpeaceful = 1;
            mtmp->mavenge = 0;
            mtmp->mstrategy &= ~STRAT_WAITMASK;
            could_see_mon = canseemon(mtmp);
            mtmp->mundetected = 0;
            newsym(mtmp->mx, mtmp->my);
            if (canseemon(mtmp)) {
                if (!could_see_mon)
                    You("notice %s, swaying with the music.", a_monnam(mtmp));
                else
                    pline("%s freezes, then sways with the music%s.",
                          Monnam(mtmp),
                          was_peaceful ? "" : ", and now seems quieter");
            }
        }
    }
}

/*
 * Calm nymphs in range.
 */

staticfn void
calm_nymphs(int distance)
{
    struct monst *mtmp;

    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp))
            continue;
        if (mtmp->data->mlet == S_NYMPH && mtmp->mcanmove
            && mdistu(mtmp) < distance) {
            mtmp->msleeping = 0;
            mtmp->mpeaceful = 1;
            mtmp->mavenge = 0;
            mtmp->mstrategy &= ~STRAT_WAITMASK;
            if (canseemon(mtmp))
                pline(
                    "%s listens cheerfully to the music, then seems quieter.",
                      Monnam(mtmp));
        }
    }
}

/* Awake soldiers anywhere the level (and any nearby monster). */
void
awaken_soldiers(struct monst *bugler  /* monster that played instrument */)
{
    struct monst *mtmp;
    int distance, distm;

    /* distance of affected non-soldier monsters to bugler */
    distance = ((bugler == &gy.youmonst) ? u.ulevel
                                         : bugler->data->mlevel) * 30;

    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp))
            continue;
        if (is_mercenary(mtmp->data) && mtmp->data != &mons[PM_GUARD]) {
            if (!mtmp->mtame)
                mtmp->mpeaceful = 0;
            mtmp->msleeping = mtmp->mfrozen = 0;
            mtmp->mcanmove = 1;
            mtmp->mstrategy &= ~STRAT_WAITMASK;
            if (canseemon(mtmp))
                pline("%s is now ready for battle!", Monnam(mtmp));
            else if (!Deaf)
                Norep("%s the rattle of battle gear being readied.",
                      "You hear");  /* Deaf-aware */
        } else if ((distm = ((bugler == &gy.youmonst)
                                 ? mdistu(mtmp)
                                 : dist2(bugler->mx, bugler->my, mtmp->mx,
                                         mtmp->my))) < distance) {
            awaken_scare(mtmp, (distm < distance / 3));
        }
    }
}

/* Charm monsters in range.  Note that they may resist the spell. */
staticfn void
charm_monsters(int distance)
{
    struct monst *mtmp, *mtmp2;

    if (u.uswallow)
        distance = 0; /* only u.ustuck will be affected (u.usteed is Null
                       * since hero gets forcibly dismounted when engulfed) */

    for (mtmp = fmon; mtmp; mtmp = mtmp2) {
        mtmp2 = mtmp->nmon;
        if (DEADMONSTER(mtmp))
            continue;

        if (mdistu(mtmp) <= distance) {
            /* a shopkeeper can't be tamed but tamedog() pacifies an angry
               one; do that even if mtmp resists in order to behave the same
               as a non-cursed scroll of taming or spell of charm monster */
            if (!resist(mtmp, TOOL_CLASS, 0, NOTELL) || mtmp->isshk)
                (void) tamedog(mtmp, (struct obj *) 0, TRUE);
        }
    }
}

/* Try to make a pit. */
staticfn void
do_pit(coordxy x, coordxy y, unsigned tu_pit)
{
    struct monst *mtmp;
    struct obj *otmp;
    struct trap *chasm;
    schar filltype;

    chasm = maketrap(x, y, PIT);
    if (!chasm)
        return; /* no pit if portal at that location */
    chasm->tseen = 1;

    mtmp = m_at(x, y); /* (redundant?) */
    if ((otmp = sobj_at(BOULDER, x, y)) != 0) {
        if (cansee(x, y))
            pline("KADOOM!  The boulder falls into a chasm%s!",
                  u_at(x, y) ? " below you" : "");
        if (mtmp)
            mtmp->mtrapped = 0;
        obj_extract_self(otmp);
        (void) flooreffects(otmp, x, y, "");
        return;
    }

    /* Let liquid flow into the newly created chasm.
       Adjust corresponding code in apply.c for exploding
       wand of digging if you alter this sequence. */
    filltype = fillholetyp(x, y, FALSE);
    if (filltype != ROOM) {
        set_levltyp(x, y, filltype); /* levl[x][y] = filltype; */
        liquid_flow(x, y, filltype, chasm, (char *) 0);
        /* liquid_flow() deletes trap, might kill mtmp */
        if ((chasm = t_at(x, y)) == NULL)
            return;
    }

    /* We have to check whether monsters or hero falls into a
       new pit....  Note: if we get here, chasm is non-Null. */
    if (mtmp) {
        if (!is_flyer(mtmp->data) && !is_clinger(mtmp->data)) {
            boolean m_already_trapped = mtmp->mtrapped;

            mtmp->mtrapped = 1;
            if (!m_already_trapped) { /* suppress messages */
                if (cansee(x, y)) {
                    pline("%s falls into a chasm!", Monnam(mtmp));
                } else if (humanoid(mtmp->data)) {
                    Soundeffect(se_scream, 50);
                    You_hear("a scream!");
                }
            }
            /* Falling is okay for falling down
               within a pit from jostling too */
            mselftouch(mtmp, "Falling, ", TRUE);
            if (!DEADMONSTER(mtmp)) {
                mtmp->mhp -= rnd(m_already_trapped ? 4 : 6);
                if (DEADMONSTER(mtmp)) {
                    if (!cansee(x, y)) {
                        pline("It is destroyed!");
                    } else {
                        You("destroy %s!",
                            mtmp->mtame
                             ? x_monnam(mtmp, ARTICLE_THE, "poor",
                                        has_mgivenname(mtmp)
                                         ? SUPPRESS_SADDLE : 0,
                                        FALSE)
                             : mon_nam(mtmp));
                    }
                    xkilled(mtmp, XKILL_NOMSG);
                }
            }
        }
    } else if (u_at(x, y)) {
        if (u.utrap && u.utraptype == TT_BURIEDBALL) {
            /* Note:  the chain should break if a pit gets
               created at the buried ball's location, which
               is not necessarily here.  But if we don't do
               things this way, entering the new pit below
               will override current trap anyway, but too
               late to get Lev and Fly handling. */
            Your("chain breaks!");
            reset_utrap(TRUE);
        }
        if (Levitation || Flying || is_clinger(gy.youmonst.data)) {
            if (!tu_pit) { /* no pit here previously */
                pline("A chasm opens up under you!");
                You("don't fall in!");
            }
        } else if (!tu_pit || !u.utrap || u.utraptype != TT_PIT) {
            /* no pit here previously, or you were
               not in it even if there was */
            You("fall into a chasm!");
            set_utrap(rn1(6, 2), TT_PIT);
            losehp(Maybe_Half_Phys(rnd(6)),
                   "fell into a chasm", NO_KILLER_PREFIX);
            selftouch("Falling, you");
        } else if (u.utrap && u.utraptype == TT_PIT) {
            boolean keepfooting =
                    (!(Fumbling && rn2(5))
                     && (!(rnl(Role_if(PM_ARCHEOLOGIST) ? 3 : 9))
                         || ((ACURR(A_DEX) > 7) && rn2(5))));

            You("are jostled around violently!");
            set_utrap(rn1(6, 2), TT_PIT);
            losehp(Maybe_Half_Phys(rnd(keepfooting ? 2 : 4)),
                   "hurt in a chasm", NO_KILLER_PREFIX);
            if (keepfooting)
                exercise(A_DEX, TRUE);
            else
                selftouch((Upolyd && (slithy(gy.youmonst.data)
                                    || nolimbs(gy.youmonst.data)))
                          ? "Shaken, you"
                          : "Falling down, you");
        }
    } else {
        newsym(x, y);
    }
}

/* Generate earthquake :-) of desired force.
 * That is:  create random chasms (pits).
 */
staticfn void
do_earthquake(int force)
{
    static const char into_a_chasm[] = " into a chasm";
    coordxy x, y;
    struct monst *mtmp;
    struct trap *trap_at_u = t_at(u.ux, u.uy);
    int start_x, start_y, end_x, end_y, amsk;
    aligntyp algn;
    unsigned tu_pit = 0;

    if (trap_at_u)
        tu_pit = is_pit(trap_at_u->ttyp);
    if (force > 13) /* sanity precaution; maximum used is actually 10 */
        force = 13;
    start_x = u.ux - (force * 2);
    start_y = u.uy - (force * 2);
    end_x = u.ux + (force * 2);
    end_y = u.uy + (force * 2);
    start_x = max(start_x, 1);
    start_y = max(start_y, 0);
    end_x = min(end_x, COLNO - 1);
    end_y = min(end_y, ROWNO - 1);
    for (x = start_x; x <= end_x; x++)
        for (y = start_y; y <= end_y; y++) {
            if ((mtmp = m_at(x, y)) != 0) {
                wakeup(mtmp, TRUE); /* peaceful monster will become hostile */
                if (mtmp->mundetected) {
                    mtmp->mundetected = 0;
                    newsym(x, y);
                    if (ceiling_hider(mtmp->data)) {
                        if (cansee(x, y)) {
                            pline("%s is shaken loose from the ceiling!",
                                  Amonnam(mtmp));
                        } else if (!is_flyer(mtmp->data)) {
                            Soundeffect(se_thump, 50);
                            You_hear("a thump.");
                        }
                    }
                }
                if (M_AP_TYPE(mtmp) != M_AP_NOTHING
                    && M_AP_TYPE(mtmp) != M_AP_MONSTER)
                    seemimic(mtmp);
            }
            if (rn2(14 - force))
                continue;

       /*
        * Possible extensions:
        *  When a door is trapped, explode it instead of silently
        *   turning it into an empty doorway.
        *  Trigger divine wrath when an altar is dumped into a chasm.
        *  Sometimes replace sink with fountain or fountain with pool
        *   instead of always producing a pit.
        *  Sometimes release monster and/or treasure from a grave or
        *   a throne instead of just dumping them into the chasm.
        *  Chance to destroy wall segments?  Trees too?
        *  Honor non-diggable for locked doors, walls, and trees.
        *   Treat non-passwall as if it was non-diggable?
        *  Conjoin some of the umpteen pits when they're adjacent?
        *
        *  Replace 'goto do_pit;' with 'do_pit = TRUE; break;' and
        *   move the pit code to after the switch.
        */

            switch (levl[x][y].typ) {
            case FOUNTAIN: /* make the fountain disappear */
                if (cansee(x, y))
                    pline_The("fountain falls%s.", into_a_chasm);
                do_pit(x, y, tu_pit);
                break;
            case SINK:
                if (cansee(x, y))
                    pline_The("kitchen sink falls%s.", into_a_chasm);
                do_pit(x, y, tu_pit);
                break;
            case ALTAR:
                amsk = altarmask_at(x, y);
                /* always preserve the high altars */
                if ((amsk & AM_SANCTUM) != 0)
                    break;
                algn = Amask2align(amsk & AM_MASK);
                if (cansee(x, y))
                    pline_The("%s altar falls%s.",
                              align_str(algn), into_a_chasm);
                desecrate_altar(FALSE, algn);
                do_pit(x, y, tu_pit);
                break;
            case GRAVE:
                if (cansee(x, y))
                    pline_The("headstone topples%s.", into_a_chasm);
                do_pit(x, y, tu_pit);
                break;
            case THRONE:
                if (cansee(x, y))
                    pline_The("throne falls%s.", into_a_chasm);
                do_pit(x, y, tu_pit);
                break;
            case SCORR:
                levl[x][y].typ = CORR;
                unblock_point(x, y);
                if (cansee(x, y))
                    pline("A secret corridor is revealed.");
                FALLTHROUGH;
                /*FALLTHRU*/
            case CORR:
            case ROOM:
                do_pit(x, y, tu_pit);
                break;
            case SDOOR:
                cvt_sdoor_to_door(&levl[x][y]); /* .typ = DOOR */
                if (cansee(x, y))
                    pline("A secret door is revealed.");
                FALLTHROUGH;
                /*FALLTHRU*/
            case DOOR: /* make the door collapse */
                /* if already doorless, treat like room or corridor */
                if (levl[x][y].doormask == D_NODOOR) {
                    do_pit(x, y, tu_pit);
                    break;
                }
                /* wasn't doorless, now it will be */
                levl[x][y].doormask = D_NODOOR;
                recalc_block_point(x, y);
                newsym(x, y); /* before pline */
                if (cansee(x, y))
                    pline_The("door collapses.");
                if (*in_rooms(x, y, SHOPBASE))
                    add_damage(x, y, 0L);
                break;
            }
        }
}

staticfn const char *
generic_lvl_desc(void)
{
    if (Is_astralevel(&u.uz))
        return "astral plane";
    else if (In_endgame(&u.uz))
        return "plane";
    else if (Is_sanctum(&u.uz))
        return "sanctum";
    else if (In_sokoban(&u.uz))
        return "puzzle";
    else if (In_V_tower(&u.uz))
        return "tower";
    else
        return "dungeon";
}

static const char *beats[] = {
    "stepper", "one drop", "slow two", "triple stroke roll",
    "double shuffle", "half-time shuffle", "second line", "train"
};

/*
 * The player is trying to extract something from his/her instrument.
 */
staticfn int
do_improvisation(struct obj *instr)
{
    int damage, mode, do_spec = !(Stunned || Confusion);
    struct obj itmp;
    boolean mundane = FALSE, same_old_song = FALSE;
    static char my_goto_song[] = {'C', '\0'},
                *improvisation = my_goto_song;

    itmp = *instr;
    itmp.oextra = (struct oextra *) 0; /* ok on this copy as instr maintains
                                        * the ptr to free at some point if
                                        * there is one */

    /* if won't yield special effect, make sound of mundane counterpart */
    if (!do_spec || instr->spe <= 0)
        while (objects[itmp.otyp].oc_magic) {
            itmp.otyp -= 1;
            mundane = TRUE;
        }

#define PLAY_NORMAL   0x00
#define PLAY_STUNNED  0x01
#define PLAY_CONFUSED 0x02
#define PLAY_HALLU    0x04
    mode = PLAY_NORMAL;
    if (Stunned)
        mode |= PLAY_STUNNED;
    if (Confusion)
        mode |= PLAY_CONFUSED;
    if (Hallucination)
        mode |= PLAY_HALLU;

    if (!rn2(2)) {
        /*
         * TEMPORARY?  for multiple impairments, don't always
         * give the generic "it's far from music" message.
         */
        /* remove if STUNNED+CONFUSED ever gets its own message below */
        if (mode == (PLAY_STUNNED | PLAY_CONFUSED))
            mode = !rn2(2) ? PLAY_STUNNED : PLAY_CONFUSED;
        /* likewise for stunned and/or confused combined with hallucination */
        if (mode & PLAY_HALLU)
            mode = PLAY_HALLU;
    }

    /* 3.6.3: most of these gave "You produce <blah>" and then many of
       the instrument-specific messages below which immediately follow
       also gave "You produce <something>."  That looked strange so we
       now use a different verb here */
    switch (mode) {
    case PLAY_NORMAL:
        You("start playing %s.", yname(instr));
        break;
    case PLAY_STUNNED:
        if (!Deaf)
            You("radiate an obnoxious droning sound.");
        else
            You_feel("a monotonous vibration.");
        break;
    case PLAY_CONFUSED:
        if (!Deaf)
            You("generate a raucous noise.");
        else
            You_feel("a jarring vibration.");
        break;
    case PLAY_HALLU:
        You("disseminate a kaleidoscopic display of floating butterflies.");
        break;
    /* TODO? give some or all of these combinations their own feedback;
       hallucination ones should reference senses other than hearing... */
    case PLAY_STUNNED | PLAY_CONFUSED:
    case PLAY_STUNNED | PLAY_HALLU:
    case PLAY_CONFUSED | PLAY_HALLU:
    case PLAY_STUNNED | PLAY_CONFUSED | PLAY_HALLU:
    default:
        pline("What you perform is quite far from music...");
        break;
    }
#undef PLAY_NORMAL
#undef PLAY_STUNNED
#undef PLAY_CONFUSED
#undef PLAY_HALLU

    improvisation = improvised_notes(&same_old_song);

    switch (itmp.otyp) { /* note: itmp.otyp might differ from instr->otyp */
    case MAGIC_FLUTE: /* Make monster fall asleep */
        consume_obj_charge(instr, TRUE);

        You("%sproduce %s%s music.", !Deaf ? "" : "seem to ",
            Hallucination ? "piped" : "soft",
            same_old_song ? ", familiar" : "");
        Hero_playnotes(obj_to_instr(&itmp), improvisation, 50);
        put_monsters_to_sleep(u.ulevel * 5);
        exercise(A_DEX, TRUE);
        break;
    case WOODEN_FLUTE: /* May charm snakes */
        do_spec &= (rn2(ACURR(A_DEX)) + u.ulevel > 25);
        if (!Deaf)
            pline("%s%s.", Tobjnam(instr, do_spec ? "trill" : "toot"),
                  same_old_song ? " a familiar tune" : "");
        else
            You_feel("%s %s.", yname(instr), do_spec ? "trill" : "toot");
        Hero_playnotes(obj_to_instr(&itmp), improvisation, 50);
        if (do_spec)
            charm_snakes(u.ulevel * 3);
        exercise(A_DEX, TRUE);
        break;
    case FIRE_HORN:  /* Idem wand of fire */
    case FROST_HORN: /* Idem wand of cold */
        consume_obj_charge(instr, TRUE);

        if (!getdir((char *) 0)) {
            pline("%s.", Tobjnam(instr, "vibrate"));
            break;
        } else if (!u.dx && !u.dy && !u.dz) {
            if ((damage = zapyourself(instr, TRUE)) != 0) {
                char buf[BUFSZ];

                Sprintf(buf, "using a magical horn on %sself", uhim());
                Hero_playnotes(obj_to_instr(&itmp), improvisation, 50);
                losehp(damage, buf, KILLED_BY); /* fire or frost damage */
            }
        } else {
            int type = BZ_OFS_AD((instr->otyp == FROST_HORN) ? AD_COLD
                                                             : AD_FIRE);

            if (!Blind)
                pline("A %s blasts out of the horn!", flash_str(type, FALSE));
            Hero_playnotes(obj_to_instr(&itmp), improvisation, 50);
            gc.current_wand = instr;
            ubuzz(BZ_U_WAND(type), rn1(6, 6));
            gc.current_wand = 0;
        }
        makeknown(instr->otyp);
        break;
    case TOOLED_HORN: /* Awaken or scare monsters */
        if (!Deaf)
            You("produce a frightful, grave%s sound.",
                same_old_song ? ", yet familiar," : "");
        else
            You("blow into the horn.");
        Hero_playnotes(obj_to_instr(&itmp), improvisation, 80);
        awaken_monsters(u.ulevel * 30);
        exercise(A_WIS, FALSE);
        break;
    case BUGLE: /* Awaken & attract soldiers */
        if (!Deaf)
            You("extract a loud%s noise from %s.",
                same_old_song ? ", familiar" : "", yname(instr));
        else
            You("blow into the bugle.");
        Hero_playnotes(obj_to_instr(&itmp), improvisation, 80);
        awaken_soldiers(&gy.youmonst);
        exercise(A_WIS, FALSE);
        break;
    case MAGIC_HARP: /* Charm monsters */
        consume_obj_charge(instr, TRUE);

        if (!Deaf)
            pline("%s very attractive%s music.",
                  Tobjnam(instr, "produce"),
                  same_old_song ? " and familiar" : "");
        else
            You_feel("very soothing vibrations.");
        Hero_playnotes(obj_to_instr(&itmp), improvisation, 50);
        charm_monsters((u.ulevel - 1) / 3 + 1);
        exercise(A_DEX, TRUE);
        break;
    case WOODEN_HARP: /* May calm Nymph */
        do_spec &= (rn2(ACURR(A_DEX)) + u.ulevel > 25);
        if (!Deaf)
            pline("%s %s.", Yname2(instr),
                  (do_spec && same_old_song)
                  ? "produces a familiar, lilting melody"
                  : (do_spec) ? "produces a lilting melody"
                    : (same_old_song) ? "twangs a familiar tune"
                      : "twangs");
        else
            You_feel("soothing vibrations.");
        Hero_playnotes(obj_to_instr(&itmp), improvisation, 50);
        if (do_spec)
            calm_nymphs(u.ulevel * 3);
        exercise(A_DEX, TRUE);
        break;
    case DRUM_OF_EARTHQUAKE: /* create several pits */
        /* a drum of earthquake does not cause deafness
           while still magically functional, nor afterwards
           when it invokes the LEATHER_DRUM case instead and
           mundane is flagged */
        consume_obj_charge(instr, TRUE);

        You("produce a heavy, thunderous rolling!");
        Hero_playnotes(obj_to_instr(&itmp), "C", 100);
        pline_The("entire %s is shaking around you!", generic_lvl_desc());
        do_earthquake((u.ulevel - 1) / 3 + 1);
        /* shake up monsters in a much larger radius... */
        awaken_monsters(ROWNO * COLNO);
        makeknown(DRUM_OF_EARTHQUAKE);
        break;
    case LEATHER_DRUM: /* Awaken monsters */
        if (!mundane) {
            if (!Deaf) {
                You("beat a %sdeafening row!",
                    same_old_song ? "familiar " : "");
                Hero_playnotes(obj_to_instr(&itmp), "CCC", 100);
                incr_itimeout(&HDeaf, rn1(20, 30));
            } else {
                You("pound on the drum.");
            }
            exercise(A_WIS, FALSE);
        } else {
            /* TODO maybe: sound effects for these riffs */
            You("%s %s.",
                rn2(2) ? "butcher" : rn2(2) ? "manage" : "pull off",
                an(ROLL_FROM(beats)));
            Hero_playnotes(obj_to_instr(&itmp), improvisation, 50);
        }
        awaken_monsters(u.ulevel * (mundane ? 5 : 40));
        disp.botl = TRUE;
        break;
    default:
        impossible("What a weird instrument (%d)!", instr->otyp);
        return 0;
    }
    nhUse(improvisation);
    return 2; /* That takes time */
}

staticfn char *
improvised_notes(boolean *same_as_last_time)
{
    static const char notes[7] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G' };
    /* target buffer has to be in svc.context, otherwise saving game
     * between improvised recitals would not be able to maintain
     * the same_as_last_time context. */

    /* You can change your tune, usually */
    if (!(Unchanging && svc.context.jingle[0] != '\0')) {
        int i, notecount = rnd(SIZE(svc.context.jingle) - 1); /* 1 - 5 */

        for (i = 0; i < notecount; ++i) {
            svc.context.jingle[i] = ROLL_FROM(notes);
        }
        svc.context.jingle[notecount] = '\0';
        *same_as_last_time = FALSE;
    } else {
        *same_as_last_time = TRUE;
    }
    return svc.context.jingle;
}

/*
 * So you want music...
 */
int
do_play_instrument(struct obj *instr)
{
    char buf[BUFSZ] = DUMMY, c = 'y';
    char *s;
    coordxy x, y;
    boolean ok;

    if (Underwater) {
        You_cant("play music underwater!");
        return ECMD_OK;
    } else if ((instr->otyp == WOODEN_FLUTE || instr->otyp == MAGIC_FLUTE
                || instr->otyp == TOOLED_HORN || instr->otyp == FROST_HORN
                || instr->otyp == FIRE_HORN || instr->otyp == BUGLE)
               && !can_blow(&gy.youmonst)) {
        You("are incapable of playing %s.", thesimpleoname(instr));
        return ECMD_OK;
    }
    if (instr->otyp != LEATHER_DRUM && instr->otyp != DRUM_OF_EARTHQUAKE
        && !(Stunned || Confusion || Hallucination)) {
        c = ynq("Improvise?");
        if (c == 'q')
            goto nevermind;
    }

    if (c != 'n')
        return do_improvisation(instr) ? ECMD_TIME : ECMD_OK;

    if (u.uevent.uheard_tune == 2)
        c = ynq("Play the passtune?");
    if (c == 'q') {
        goto nevermind;
    } else if (c == 'y') {
        Strcpy(buf, svt.tune);
    } else {
        getlin("What tune are you playing? [5 notes, A-G]", buf);
        (void) mungspaces(buf);
        if (*buf == '\033')
            goto nevermind;

        /* convert to uppercase and change any "H" to the expected "B" */
        for (s = buf; *s; s++) {
            *s = highc(*s);
            if (*s == 'H')
                *s = 'B';
        }
    }

    You(!Deaf ? "extract a strange sound from %s!"
              : "can feel %s emitting vibrations.", the(xname(instr)));
    Hero_playnotes(obj_to_instr(instr), buf, 50);


    /* Check if there was the Stronghold drawbridge near
     * and if the tune conforms to what we're waiting for.
     */
    if (Is_stronghold(&u.uz)) {
        exercise(A_WIS, TRUE); /* just for trying */
        if (!strcmp(buf, svt.tune)) {
            /* Search for the drawbridge */
            for (y = u.uy - 1; y <= u.uy + 1; y++)
                for (x = u.ux - 1; x <= u.ux + 1; x++) {
                    if (!isok(x, y))
                        continue;
                    if (find_drawbridge(&x, &y)) {
                        /* tune now fully known */
                        u.uevent.uheard_tune = 2;
                        record_achievement(ACH_TUNE);
                        if (levl[x][y].typ == DRAWBRIDGE_DOWN)
                            close_drawbridge(x, y);
                        else
                            open_drawbridge(x, y);
                        return ECMD_TIME;
                    }
                }
        } else if (!Deaf) {
            if (u.uevent.uheard_tune < 1)
                u.uevent.uheard_tune = 1;
            /* Okay, it wasn't the right tune, but perhaps
             * we can give the player some hints like in the
             * Mastermind game */
            ok = FALSE;
            for (y = u.uy - 1; y <= u.uy + 1 && !ok; y++)
                for (x = u.ux - 1; x <= u.ux + 1 && !ok; x++)
                    if (isok(x, y))
                        if (IS_DRAWBRIDGE(levl[x][y].typ)
                            || is_drawbridge_wall(x, y) >= 0)
                            ok = TRUE;
            if (ok) { /* There is a drawbridge near */
                int tumblers, gears;
                boolean matched[5];

                tumblers = gears = 0;
                for (x = 0; x < 5; x++)
                    matched[x] = FALSE;

                for (x = 0; x < (int) strlen(buf); x++)
                    if (x < 5) {
                        if (buf[x] == svt.tune[x]) {
                            gears++;
                            matched[x] = TRUE;
                        } else {
                            for (y = 0; y < 5; y++)
                                if (!matched[y] && buf[x] == svt.tune[y]
                                    && buf[y] != svt.tune[y]) {
                                    tumblers++;
                                    matched[y] = TRUE;
                                    break;
                                }
                        }
                    }
                if (tumblers) {
                    if (gears) {
                        Soundeffect(se_tumbler_click, 50);
                        Soundeffect(se_gear_turn, 50);
                        You_hear("%d tumbler%s click and %d gear%s turn.",
                                 tumblers, plur(tumblers), gears,
                                 plur(gears));
                    } else {
                        Soundeffect(se_tumbler_click, 50);
                        You_hear("%d tumbler%s click.", tumblers,
                                 plur(tumblers));
                    }
                } else if (gears) {
                    You_hear("%d gear%s turn.", gears, plur(gears));
                    /* could only get `gears == 5' by playing five
                       correct notes followed by excess; otherwise,
                       tune would have matched above */
                    if (gears == 5) {
                        u.uevent.uheard_tune = 2;
                        record_achievement(ACH_TUNE);
                    }
                }
            }
        }
    }
    return ECMD_TIME;

 nevermind:
    pline1(Never_mind);
    return ECMD_OK;
}

enum instruments
obj_to_instr(struct obj *obj SOUNDLIBONLY) {
    enum instruments ret_instr = ins_no_instrument;

#if defined(SND_LIB_INTEGRATED)
    switch(obj->otyp) {
        case WOODEN_FLUTE:
            ret_instr = ins_flute;
            break;
        case MAGIC_FLUTE:
            ret_instr = ins_pan_flute;
            break;
        case TOOLED_HORN:
            ret_instr = ins_english_horn;
            break;
        case FROST_HORN:
            ret_instr = ins_french_horn;
            break;
        case FIRE_HORN:
            ret_instr = ins_baritone_sax;
            break;
        case BUGLE:
            ret_instr = ins_trumpet;
            break;
        case WOODEN_HARP:
            ret_instr = ins_orchestral_harp;
            break;
        case MAGIC_HARP:
            ret_instr = ins_cello;
            break;
        case BELL:
        case BELL_OF_OPENING:
            ret_instr = ins_tinkle_bell;
            break;
        case DRUM_OF_EARTHQUAKE:
            ret_instr = ins_taiko_drum;
            break;
        case LEATHER_DRUM:
            ret_instr = ins_melodic_tom;
            break;
    }
#endif
    return ret_instr;
}
/*music.c*/
