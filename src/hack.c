/* NetHack 3.7	hack.c	$NHDT-Date: 1736530208 2025/01/10 09:30:08 $  $NHDT-Branch: NetHack-3.7 $:$NHDT-Revision: 1.477 $ */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/*-Copyright (c) Derek S. Ray, 2015. */
/* NetHack may be freely redistributed.  See license for details. */

#include "hack.h"
#include "extern.h"

/* #define DEBUG */ /* uncomment for debugging */

staticfn boolean could_move_onto_boulder(coordxy, coordxy);
staticfn void dopush(coordxy, coordxy, coordxy, coordxy, struct obj *,
                     boolean);
staticfn void cannot_push_msg(struct obj *, coordxy, coordxy);
staticfn int cannot_push(struct obj *, coordxy, coordxy);
staticfn void rock_disappear_msg(struct obj *);
staticfn void moverock_done(coordxy, coordxy);
staticfn int moverock(void);
staticfn int moverock_core(coordxy, coordxy);
staticfn void dosinkfall(void);
staticfn boolean findtravelpath(int);
staticfn boolean trapmove(coordxy, coordxy, struct trap *);
staticfn int QSORTCALLBACK notice_mons_cmp(const genericptr,
                                           const genericptr) NONNULLPTRS;
staticfn schar u_simple_floortyp(coordxy, coordxy);
staticfn boolean swim_move_danger(coordxy, coordxy);
staticfn boolean domove_bump_mon(struct monst *, int) NONNULLARG1;
staticfn boolean domove_attackmon_at(struct monst *, coordxy, coordxy,
                                   boolean *) NONNULLPTRS;
staticfn boolean domove_fight_ironbars(coordxy, coordxy);
staticfn boolean domove_fight_web(coordxy, coordxy);
staticfn boolean domove_swap_with_pet(struct monst *,
                                    coordxy, coordxy) NONNULLARG1;
staticfn boolean domove_fight_empty(coordxy, coordxy);
staticfn boolean air_turbulence(void);
staticfn void slippery_ice_fumbling(void);
staticfn boolean impaired_movement(coordxy *, coordxy *) NONNULLPTRS;
staticfn boolean avoid_moving_on_trap(coordxy, coordxy, boolean);
staticfn boolean avoid_moving_on_liquid(coordxy, coordxy, boolean);
staticfn boolean avoid_running_into_trap_or_liquid(coordxy, coordxy);
staticfn boolean avoid_trap_andor_region(coordxy, coordxy);
staticfn boolean move_out_of_bounds(coordxy, coordxy);
staticfn boolean carrying_too_much(void);
staticfn boolean escape_from_sticky_mon(coordxy, coordxy);
staticfn void domove_core(void);
staticfn void maybe_smudge_engr(coordxy, coordxy, coordxy, coordxy);
staticfn struct monst *monstinroom(struct permonst *, int) NONNULLARG1;
staticfn boolean furniture_present(int, int);
staticfn void move_update(boolean);
staticfn int pickup_checks(void);
staticfn void maybe_wail(void);
staticfn boolean water_turbulence(coordxy *, coordxy *);
staticfn int QSORTCALLBACK cmp_weights(const void *, const void *);

#define IS_SHOP(x) (svr.rooms[x].rtype >= SHOPBASE)

/* XXX: if more sources of water walking than just boots are added,
   cause_known(insight.c) should be externified and used for this */
/* I looked at following this advice for Fountainbane but cause_known
    assumes the item giving water walking is worn */
#define WW_BOOTS_KNOWN \
    (uarmf && uarmf->otyp == WATER_WALKING_BOOTS \
     && objects[WATER_WALKING_BOOTS].oc_name_known)

#define FOUNTAINBANE_HELD \
    (uwep && is_art(uwep, ART_FOUNTAINBANE))

#define Known_wwalking \
    ((WW_BOOTS_KNOWN || \
       FOUNTAINBANE_HELD) \
       && !u.usteed)

#define Known_lwalking \
    (((WW_BOOTS_KNOWN && uarmf->oerodeproof && uarmf->rknown) \
     || FOUNTAINBANE_HELD) \
     && Fire_resistance)

/* mode values for findtravelpath() */
#define TRAVP_TRAVEL 0
#define TRAVP_GUESS  1
#define TRAVP_VALID  2

anything *
uint_to_any(unsigned ui)
{
    gt.tmp_anything = cg.zeroany;
    gt.tmp_anything.a_uint = ui;
    return &gt.tmp_anything;
}

anything *
long_to_any(long lng)
{
    gt.tmp_anything = cg.zeroany;
    gt.tmp_anything.a_long = lng;
    return &gt.tmp_anything;
}

anything *
monst_to_any(struct monst *mtmp)
{
    gt.tmp_anything = cg.zeroany;
    gt.tmp_anything.a_monst = mtmp;
    return &gt.tmp_anything;
}

anything *
obj_to_any(struct obj *obj)
{
    gt.tmp_anything = cg.zeroany;
    gt.tmp_anything.a_obj = obj;
    return &gt.tmp_anything;
}

boolean
revive_nasty(coordxy x, coordxy y, const char *msg)
{
    struct obj *otmp, *otmp2;
    struct monst *mtmp;
    coord cc;
    boolean revived = FALSE;

    for (otmp = svl.level.objects[x][y]; otmp; otmp = otmp2) {
        otmp2 = otmp->nexthere;
        if (otmp->otyp == CORPSE
            && (is_rider(&mons[otmp->corpsenm])
                || otmp->corpsenm == PM_WIZARD_OF_YENDOR)) {
            /* move any living monster already at that location */
            if ((mtmp = m_at(x, y)) && enexto(&cc, x, y, mtmp->data))
                rloc_to(mtmp, cc.x, cc.y);
            if (msg)
                Norep("%s", msg);
            revived = revive_corpse(otmp);
        }
    }

    /* this location might not be safe, if not, move revived monster */
    if (revived) {
        mtmp = m_at(x, y);
        if (mtmp && !goodpos(x, y, mtmp, 0)
            && enexto(&cc, x, y, mtmp->data)) {
            rloc_to(mtmp, cc.x, cc.y);
        }
        /* else impossible? */
    }

    return revived;
}

#define squeezeablylightinvent() (!gi.invent \
    || inv_weight() <= (WT_SQUEEZABLE_INV * -1))

/* can hero move onto a spot containing one or more boulders?
   used for m<dir> and travel and during boulder push failure */
staticfn boolean
could_move_onto_boulder(coordxy sx, coordxy sy)
{
    /* can if able to phaze through rock (must be poly'd, so not riding) */
    if (Passes_walls)
        return TRUE;
    /* can't when riding */
    if (u.usteed)
        return FALSE;
    /* can if a giant, unless doing so allows hero to pass into a
       diagonal squeeze at the same time */
    if (throws_rocks(gy.youmonst.data))
        return (!u.dx || !u.dy || !(IS_OBSTRUCTED(levl[u.ux][sy].typ)
                                    && IS_OBSTRUCTED(levl[sx][u.uy].typ)));
    /* can if tiny (implies carrying very little else couldn't move at all) */
    if (verysmall(gy.youmonst.data))
        return TRUE;
    /* can squeeze to spot if carrying extremely little, otherwise can't */
    return squeezeablylightinvent();
}

staticfn void
dopush(
    coordxy sx,
    coordxy sy,
    coordxy rx,
    coordxy ry,
    struct obj *otmp,
    boolean costly)
{
    struct monst *shkp;

    {
        const char *what;
        boolean givemesg, easypush;
        /* give boulder pushing feedback if this is a different
           boulder than the last one pushed or if it's been at
           least 2 turns since we last pushed this boulder;
           unlike with Norep(), intervening messages don't cause
           it to repeat, only doing something else in the meantime */
        if (otmp->o_id != gb.bldrpush_oid) {
            gb.bldrpushtime = svm.moves + 1L;
            gb.bldrpush_oid = otmp->o_id;
        }
        givemesg = (svm.moves > gb.bldrpushtime + 2L
                    || svm.moves < gb.bldrpushtime);
        what = givemesg ? the(xname(otmp)) : 0;
        if (!u.usteed) {
            easypush = throws_rocks(gy.youmonst.data);
            if (givemesg)
                pline("With %s effort you move %s.",
                      easypush ? "little" : "great", what);
            if (!easypush)
                exercise(A_STR, TRUE);
        } else {
            if (givemesg)
                pline("%s moves %s.", YMonnam(u.usteed), what);
        }
        gb.bldrpushtime = svm.moves;
    }

    /* Move the boulder *after* the message. */
    if (glyph_is_invisible(levl[rx][ry].glyph))
        unmap_object(rx, ry);
    otmp->next_boulder = 0;
    movobj(otmp, rx, ry); /* does newsym(rx,ry) */
    if (Blind) {
        feel_location(rx, ry);
        feel_location(sx, sy);
    } else {
        newsym(sx, sy);
    }
    /* maybe adjust bill if boulder was pushed across shop boundary;
       normally otmp->unpaid would not apply because otmp isn't in
       hero's inventory, but addtobill() sets it and subfrombill()
       clears it */
    if (costly && !costly_spot(rx, ry)) {
        /* pushing from inside shop to its boundary (or free spot) */
        addtobill(otmp, FALSE, FALSE, FALSE);
    } else if (!costly && costly_spot(rx, ry) && otmp->unpaid
               && ((shkp = shop_keeper(*in_rooms(rx, ry, SHOPBASE)))
                   != 0)
               && onshopbill(otmp, shkp, TRUE)) {
        /* this can happen if hero pushes boulder from farther inside
           shop into shop's free spot (which will add it to the bill),
           then teleports or Passes_walls to doorway (without exiting
           the shop), and then pushes the boulder from the free spot
           back into the shop; it's contingent upon the shopkeeper not
           "muttering an incantation" to fracture the boulder while it
           is unpaid at the free spot */
        subfrombill(otmp, shkp);
    } else if (otmp->unpaid
               && (shkp = find_objowner(otmp, sx, sy)) != 0
               && !strchr(in_rooms(rx, ry, SHOPBASE),
                          ESHK(shkp)->shoproom)) {
        /* once the boulder is fully out of the shop, so that it's
         * impossible to change your mind and push it back in without
         * leaving and triggering Kops, switch it to stolen_value */
        stolen_value(otmp, sx, sy, TRUE, FALSE);
    }
}

staticfn void
cannot_push_msg(struct obj *otmp, coordxy sx, coordxy sy)
{
    const char *what;

    what = the(xname(otmp));
    if (u.usteed)
        pline("%s tries to move %s, but cannot.",
              YMonnam(u.usteed), what);
    else
        You("try to move %s, but in vain.", what);
    if (Blind)
        feel_location(sx, sy);
}

staticfn int
cannot_push(struct obj *otmp, coordxy sx, coordxy sy)
{
    if (throws_rocks(gy.youmonst.data)) {
        boolean
            canpickup = (!Sokoban
                         /* similar exception as in can_lift():
                            when poly'd into a giant, you can
                            pick up a boulder if you have a free
                            slot or into the overflow ('#') slot
                            unless already carrying at least one */
                        && (inv_cnt(FALSE) < invlet_basic
                               || !carrying(BOULDER))),
            willpickup = (canpickup
                          && (flags.pickup && !svc.context.nopick)
                          && autopick_testobj(otmp, TRUE));

        if (u.usteed && P_SKILL(P_RIDING) < P_BASIC) {
            You("aren't skilled enough to %s %s from %s.",
                willpickup ? "pick up" : "push aside",
                the(xname(otmp)), y_monnam(u.usteed));
        } else {
            /*
             * will pick up:  you easily pick it up
             * can but won't: you maneuver over it and could pick it up
             * can't pick up: you maneuver over it (possibly followed
             *     by feedback from failed auto-pickup attempt)
             */
            pline("However, you %s%s.",
                  willpickup ? "easily pick it up"
                             : "maneuver over it",
                  (canpickup && !willpickup)
                             ? " and could pick it up"
                             : "");
            /* similar to dropping everything and squeezing onto
               a Sokoban boulder's spot, moving to same breaks the
               Sokoban rules because on next step you could go
               past it without pushing it to plug a pit or hole */
            sokoban_guilt();
        }
        return 0;
    }

    if (could_move_onto_boulder(sx, sy)) {
        pline(
           "However, you can squeeze yourself into a small opening.");
        sokoban_guilt();
        return 0;
    } else {
        return -1;
    }
}

staticfn void
rock_disappear_msg(struct obj *otmp)
{
    if (u.usteed)
        pline("%s pushes %s and suddenly it disappears!",
                YMonnam(u.usteed), the(xname(otmp)));
    else
        You("push %s and suddenly it disappears!",
            the(xname(otmp)));

}

staticfn void
moverock_done(coordxy sx, coordxy sy)
{
    struct obj *otmp;
    for (otmp = svl.level.objects[sx][sy]; otmp; otmp = otmp->nexthere)
        if (otmp->otyp == BOULDER)
            otmp->next_boulder = 0; /* resume normal xname() for this obj */
}

staticfn int
moverock(void)
{
    coordxy sx, sy;
    int ret;

    sx = u.ux + u.dx, sy = u.uy + u.dy; /* boulder starting position */
    ret = moverock_core(sx, sy);
    moverock_done(sx, sy);
    return ret;
}

staticfn int
moverock_core(coordxy sx, coordxy sy)
{
    coordxy rx, ry;
    struct obj *otmp;
    struct trap *ttmp;
    struct monst *mtmp;
    boolean costly, firstboulder = TRUE;

    while ((otmp = sobj_at(BOULDER, sx, sy)) != 0) {

        if (Blind && glyph_to_obj(glyph_at(sx, sy)) != BOULDER) {
            pline("That feels like a boulder.");
            map_object(otmp, TRUE);
            nomul(0);
            return -1;
        }

        /* when otmp->next_boulder is 1, xname() will format it as
           "next boulder" instead of just "boulder"; affects
           boulder_hits_pool()'s messages as well as messages below */
        otmp->next_boulder = firstboulder ? 0 : 1;
        /* FIXME?  'firstboulder' should be reset to True if this boulder
           isn't the first and the previous one is named differently from
           this one.  Probably not worth bothering with... */
        firstboulder = FALSE;

        /* make sure that this boulder is visible as the top object */
        if (otmp != svl.level.objects[sx][sy])
            movobj(otmp, sx, sy);

        rx = u.ux + 2 * u.dx; /* boulder destination position */
        ry = u.uy + 2 * u.dy;
        nomul(0);

        /* using m<dir> towards an adjacent boulder steps over/onto it if
           poly'd into a giant or squeezes under/beside it if small/light
           enough but is a no-op in other circumstances unless move attempt
           reveals an unseen boulder or lack of remembered, unseen monster */
        if (svc.context.nopick) {
            int oldglyph = glyph_at(sx, sy); /* before feel_location() */
            int res;

            feel_location(sx, sy); /* same for all 3 if/else-if/else cases */
            if (throws_rocks(gy.youmonst.data)) {
                /* player has used 'm<dir>' to move, so step to boulder's
                   spot without pushing it; hero is poly'd into a giant,
                   so exotic forms of locomotion are out, but might be
                   levitating (ring, potion, spell) or flying (amulet) */
                You("%s over a boulder here.", u_locomotion("step"));
                /* ["over" seems weird on air level but what else to say?] */
                sokoban_guilt();
                res = 0; /* move to <sx,sy> */
            } else if (could_move_onto_boulder(sx, sy)) {
                You("squeeze yourself %s the boulder.",
                    Flying ? "over" : "against");
                sokoban_guilt();
                res = 0; /* move to <sx,sy> */
            } else {
                There("is a boulder in your way.");
                /* use a move if hero learns something; see test_move() for
                   how/why 'context.door_opened' is being dragged into this */
                if (glyph_at(sx, sy) != oldglyph)
                    svc.context.door_opened = svc.context.move = TRUE;
                res = -1; /* don't move to <sx,sy>, so no soko guilt */
            }
            return res;
        }
        if (Levitation || Is_airlevel(&u.uz)) {
            /* FIXME?  behavior in an air bubble on the water level should
               be similar to being on the air level; both cases probably
               ought to let push attempt proceed when flying (which implies
               not levitating) */
            if (Blind)
                feel_location(sx, sy);
            You("don't have enough leverage to push %s.", the(xname(otmp)));
            /* Give them a chance to climb over it? */
            return -1;
        }
        if (verysmall(gy.youmonst.data) && !u.usteed) {
            if (Blind)
                feel_location(sx, sy);
            pline("You're too small to push that %s.", xname(otmp));
            return cannot_push(otmp, sx, sy);
        }
        if (isok(rx, ry) && !IS_OBSTRUCTED(levl[rx][ry].typ)
            && levl[rx][ry].typ != IRONBARS
            && (!IS_DOOR(levl[rx][ry].typ) || !(u.dx && u.dy)
                || doorless_door(rx, ry)) && !sobj_at(BOULDER, rx, ry)) {
            ttmp = t_at(rx, ry);
            mtmp = m_at(rx, ry);
            costly = (costly_spot(sx, sy)
                      && shop_keeper(*in_rooms(sx, sy, SHOPBASE)));

            /* KMH -- Sokoban doesn't let you push boulders diagonally */
            if (Sokoban && u.dx && u.dy) {
                if (Blind)
                    feel_location(sx, sy);
                pline("%s won't roll diagonally on this %s.",
                      The(xname(otmp)), surface(sx, sy));
                return cannot_push(otmp, sx, sy);
            }

            if (revive_nasty(rx, ry,
                             "You sense movement on the other side.")) {
                return -1;
            }

            if (mtmp && !noncorporeal(mtmp->data)
                && (!mtmp->mtrapped || !(ttmp && is_pit(ttmp->ttyp)))) {
                boolean deliver_part1 = FALSE;

                if (Blind)
                    feel_location(sx, sy);
                if (canspotmon(mtmp)) {
                    pline("There's %s on the other side.", a_monnam(mtmp));
                    deliver_part1 = TRUE;
                } else {
                    Soundeffect(se_monster_behind_boulder, 50);
                    You_hear("a monster behind %s.", the(xname(otmp)));
                    if (!Deaf)
                        deliver_part1 = TRUE;
                    map_invisible(rx, ry);
                }
                if (flags.verbose) {
                    char you_or_steed[BUFSZ];

                    Strcpy(you_or_steed,
                           u.usteed ? y_monnam(u.usteed) : "you");
                    pline("%s%s cannot move %s.",
                          deliver_part1 ? "Perhaps that's why " : "",
                          deliver_part1 ? you_or_steed
                                        : upstart(you_or_steed),
                          deliver_part1 ? "it" : the(xname(otmp)));
                }
                return cannot_push(otmp, sx, sy);
            }

            if (closed_door(rx, ry)) {
                cannot_push_msg(otmp, sx, sy);
                return cannot_push(otmp, sx, sy);
            }

            /* at this point the boulder should be able to move (though
               potentially into something like a trap, pool, or lava) */

            /* rumbling disturbs buried zombies */
            disturb_buried_zombies(sx, sy);

            if (ttmp) {
                int newlev = 0; /* lint suppression */
                d_level dest;

                /* if a trap operates on the boulder, don't attempt
                   to move any others at this location; return -1
                   if another boulder is in hero's way, or 0 if he
                   should advance to the vacated boulder position */
                switch (ttmp->ttyp) {
                case LANDMINE:
                    if (rn2(10)) {
                        obj_extract_self(otmp);
                        place_object(otmp, rx, ry);
                        newsym(sx, sy);
                        pline("%s!  %s %s land mine.",
                              /* "kablam" is a variation of "ka-boom" or
                                 "kablooey", rather cartoonish descriptions
                                 of the sound of an explosion, but give it
                                 even when deaf if hero sees the explosion */
                              (!Deaf || !Blind) ? "KAABLAMM!!"
                              /* use an alternate exclamation when feeling
                                 the floor/ground/whatever shake (or maybe
                                 a weak shockwave if levitating or flying) */
                                                : "Gadzooks",
                              Tobjnam(otmp, "trigger"),
                              ttmp->madeby_u ? "your" : "a");
                        blow_up_landmine(ttmp);
                        /* if the boulder remains, it should fill the pit */
                        fill_pit(u.ux, u.uy);
                        if (cansee(rx, ry))
                            newsym(rx, ry);
                        return sobj_at(BOULDER, sx, sy) ? -1 : 0;
                    }
                    break;
                case SPIKED_PIT:
                case PIT:
                    obj_extract_self(otmp);
                    /* vision kludge to get messages right;
                       the pit will temporarily be seen even
                       if this is one among multiple boulders */
                    if (!Blind)
                        gv.viz_array[ry][rx] |= IN_SIGHT;
                    if (!flooreffects(otmp, rx, ry, "fall")) {
                        place_object(otmp, rx, ry);
                    }
                    if (mtmp && !Blind)
                        newsym(rx, ry);
                    return sobj_at(BOULDER, sx, sy) ? -1 : 0;
                case HOLE:
                case TRAPDOOR:
                    Soundeffect(se_kerplunk_boulder_gone, 40);
                    if (Blind)
                        pline("Kerplunk!  You no longer feel %s.",
                              the(xname(otmp)));
                    else
                        pline("%s%s and %s a %s in the %s!",
                              Tobjnam(otmp, (ttmp->ttyp == TRAPDOOR)
                                                ? "trigger"
                                                : "fall"),
                              (ttmp->ttyp == TRAPDOOR) ? "" : " into",
                              otense(otmp, "plug"),
                              (ttmp->ttyp == TRAPDOOR) ? "trap door" : "hole",
                              surface(rx, ry));
                    deltrap(ttmp);
                    useupf(otmp, 1L);
                    bury_objs(rx, ry);
                    levl[rx][ry].wall_info &= ~W_NONDIGGABLE;
                    levl[rx][ry].candig = 1;
                    if (cansee(rx, ry))
                        newsym(rx, ry);
                    return sobj_at(BOULDER, sx, sy) ? -1 : 0;
                case LEVEL_TELEP:
                    /* 20% chance of picking current level; 100% chance for
                       that if in single-level branch (Knox) or in endgame */
                    newlev = random_teleport_level();
                    /* if trap doesn't work, skip "disappears" message */
                    if (newlev == depth(&u.uz)) {
                        dopush(sx, sy, rx, ry, otmp, costly);
                        continue;
                    }
                    FALLTHROUGH;
                    /*FALLTHRU*/
                case TELEP_TRAP:
                    rock_disappear_msg(otmp);
                    otmp->next_boulder = 0; /* reset before moving it */
                    if (ttmp->ttyp == TELEP_TRAP) {
                        (void) rloco(otmp);
                    } else {
                        if (costly)
                            stolen_value(otmp, rx, ry, !ttmp->tseen, FALSE);
                        obj_extract_self(otmp);
                        add_to_migration(otmp);
                        get_level(&dest, newlev);
                        otmp->ox = dest.dnum;
                        otmp->oy = dest.dlevel;
                        otmp->owornmask = (long) MIGR_RANDOM;
                    }
                    seetrap(ttmp);
                    return sobj_at(BOULDER, sx, sy) ? -1 : 0;
                default:
                    break; /* boulder not affected by this trap */
                }
            }

            if (boulder_hits_pool(otmp, rx, ry, TRUE))
                continue;

            /*
             * Re-link at top of fobj chain so that pile order is preserved
             * when level is restored.
             */
            if (otmp != fobj) {
                remove_object(otmp);
                place_object(otmp, otmp->ox, otmp->oy);
            }
            dopush(sx, sy, rx, ry, otmp, costly);
        } else {
            cannot_push_msg(otmp, sx, sy);
            return cannot_push(otmp, sx, sy);
        }
    }
    return 0;
}

/*
 *  still_chewing()
 *
 *  Chew on a wall, door, or boulder.  [What about statues?]
 *  Returns TRUE if still eating, FALSE when done.
 */
int
still_chewing(coordxy x, coordxy y)
{
    struct rm *lev = &levl[x][y];
    struct obj *boulder = sobj_at(BOULDER, x, y);
    const char *digtxt = (char *) 0, *dmgtxt = (char *) 0;

    if (svc.context.digging.down) /* not continuing prev dig (w/ pick-axe) */
        (void) memset((genericptr_t) &svc.context.digging, 0,
                      sizeof (struct dig_info));

    if (!boulder
        && ((IS_OBSTRUCTED(lev->typ) && !may_dig(x, y))
            /* may_dig() checks W_NONDIGGABLE but doesn't handle iron bars */
            || (lev->typ == IRONBARS && (lev->wall_info & W_NONDIGGABLE)))) {
        You("hurt your teeth on the %s.",
            (lev->typ == IRONBARS)
                ? "bars"
                : IS_TREE(lev->typ)
                    ? "tree"
                    : "hard stone");
        nomul(0);
        return 1;
    } else if (lev->typ == IRONBARS
               && metallivorous(gy.youmonst.data) && u.uhunger > 1500) {
        /* finishing eating via 'morehungry()' doesn't handle choking */
        You("are too full to eat the bars.");
        nomul(0);
        return 1;
    } else if (!svc.context.digging.chew
               || svc.context.digging.pos.x != x
               || svc.context.digging.pos.y != y
               || !on_level(&svc.context.digging.level, &u.uz)) {
        svc.context.digging.down = FALSE;
        svc.context.digging.chew = TRUE;
        svc.context.digging.warned = FALSE;
        svc.context.digging.pos.x = x;
        svc.context.digging.pos.y = y;
        assign_level(&svc.context.digging.level, &u.uz);
        /* solid rock takes more work & time to dig through */
        svc.context.digging.effort =
            (IS_OBSTRUCTED(lev->typ) && !IS_TREE(lev->typ) ? 30 : 60) + u.udaminc;
        You("start chewing %s %s.",
            (boulder || IS_TREE(lev->typ) || lev->typ == IRONBARS)
                ? "on a"
                : "a hole in the",
            boulder
                ? "boulder"
                : IS_TREE(lev->typ)
                    ? "tree"
                    : IS_OBSTRUCTED(lev->typ)
                        ? "rock"
                        : (lev->typ == IRONBARS)
                            ? "bar"
                            : "door");
        watch_dig((struct monst *) 0, x, y, FALSE);
        return 1;
    } else if ((svc.context.digging.effort += (30 + u.udaminc)) <= 100) {
        if (flags.verbose)
            You("%s chewing on the %s.",
                svc.context.digging.chew ? "continue" : "begin",
                boulder
                    ? "boulder"
                    : IS_TREE(lev->typ)
                        ? "tree"
                        : IS_OBSTRUCTED(lev->typ)
                            ? "rock"
                            : (lev->typ == IRONBARS)
                                ? "bars"
                                : "door");
        svc.context.digging.chew = TRUE;
        watch_dig((struct monst *) 0, x, y, FALSE);
        return 1;
    }

    /* Okay, you've chewed through something */
    if (!u.uconduct.food++)
        livelog_printf(LL_CONDUCT,
                       "ate for the first time, by chewing through %s",
                       boulder ? "a boulder"
                       : IS_TREE(lev->typ) ? "a tree"
                         : IS_OBSTRUCTED(lev->typ) ? "rock"
                           : (lev->typ == IRONBARS) ? "iron bars"
                             : "a door");
    u.uhunger += rnd(20);

    if (boulder) {
        delobj(boulder);         /* boulder goes bye-bye */
        You("eat the boulder."); /* yum */

        /*
         *  The location could still block because of
         *      1. More than one boulder
         *      2. Boulder stuck in a wall/stone/door.
         *
         *  [perhaps use does_block() below (from vision.c)]
         */
        if (IS_OBSTRUCTED(lev->typ) || closed_door(x, y)
            || sobj_at(BOULDER, x, y)) {
            block_point(x, y); /* delobj will unblock the point */
            /* reset dig state */
            (void) memset((genericptr_t) &svc.context.digging, 0,
                          sizeof (struct dig_info));
            return 1;
        }

    } else if (IS_WALL(lev->typ)) {
        if (*in_rooms(x, y, SHOPBASE)) {
            add_damage(x, y, SHOP_WALL_DMG);
            dmgtxt = "damage";
        }
        digtxt = "chew a hole in the wall.";
        if (svl.level.flags.is_maze_lev) {
            lev->typ = ROOM;
        } else if (svl.level.flags.is_cavernous_lev && !in_town(x, y)) {
            lev->typ = CORR;
        } else {
            lev->typ = DOOR;
            lev->doormask = D_NODOOR;
        }
    } else if (IS_TREE(lev->typ)) {
        digtxt = "chew through the tree.";
        lev->typ = ROOM;
    } else if (lev->typ == IRONBARS) {
        if (metallivorous(gy.youmonst.data)) { /* should always be True here */
            /* arbitrary amount; unlike proper eating, nutrition is
               bestowed in a lump sum at the end */
            int nut = (int) objects[HEAVY_IRON_BALL].oc_weight;

            /* lesshungry() requires that victual be set up, so skip it;
               morehungry() of a negative amount will increase nutrition
               without any possibility of choking to death on the meal;
               updates hunger state and requests status update if changed */
            morehungry(-nut);
        }
        digtxt = u_at(x, y)
                 ? "devour the iron bars."
                 : "eat through the bars.";
        dissolve_bars(x, y);
    } else if (lev->typ == SDOOR) {
        if (lev->doormask & D_TRAPPED) {
            lev->doormask = D_NODOOR;
            b_trapped("secret door", NO_PART);
        } else {
            digtxt = "chew through the secret door.";
            lev->doormask = D_BROKEN;
        }
        lev->typ = DOOR;

    } else if (IS_DOOR(lev->typ)) {
        if (*in_rooms(x, y, SHOPBASE)) {
            add_damage(x, y, SHOP_DOOR_COST);
            dmgtxt = "break";
        }
        if (lev->doormask & D_TRAPPED) {
            lev->doormask = D_NODOOR;
            b_trapped("door", NO_PART);
        } else {
            digtxt = "chew through the door.";
            lev->doormask = D_BROKEN;
        }

    } else { /* STONE or SCORR */
        digtxt = "chew a passage through the rock.";
        lev->typ = CORR;
    }

    recalc_block_point(x, y); /* vision */
    newsym(x, y);
    if (digtxt)
        You1(digtxt); /* after newsym */
    if (dmgtxt)
        pay_for_damage(dmgtxt, FALSE);
    (void) memset((genericptr_t) &svc.context.digging, 0,
                  sizeof (struct dig_info));
    return 0;
}

void
movobj(struct obj *obj, coordxy ox, coordxy oy)
{
    /* optimize by leaving on the fobj chain? */
    remove_object(obj);
    maybe_unhide_at(obj->ox, obj->oy);
    newsym(obj->ox, obj->oy);
    place_object(obj, ox, oy);
    newsym(ox, oy);
}

staticfn void
dosinkfall(void)
{
    static const char fell_on_sink[] = "fell onto a sink";
    struct obj *obj;
    int dmg;
    boolean lev_boots = (uarmf && uarmf->otyp == LEVITATION_BOOTS),
            innate_lev = ((HLevitation & (FROMOUTSIDE | FROMFORM)) != 0L),
            /* to handle being chained to buried iron ball, trying to
               levitate but being blocked, then moving onto adjacent sink;
               no need to worry about being blocked by terrain because we
               couldn't be over a sink at the same time */
            blockd_lev = (BLevitation == I_SPECIAL),
            ufall = (!innate_lev && !blockd_lev
                     && !(HFlying || EFlying)); /* BFlying */

    if (!ufall) {
        You((innate_lev || blockd_lev) ? "wobble unsteadily for a moment."
                                       : "gain control of your flight.");
    } else {
        long save_ELev = ELevitation, save_HLev = HLevitation;

        /* fake removal of levitation in advance so that final
           disclosure will be right in case this turns out to
           be fatal; fortunately the fact that rings and boots
           are really still worn has no effect on bones data */
        ELevitation = HLevitation = 0L;
        You("crash to the floor!");
        dmg = rn1(8, 25 - (int) ACURR(A_CON));
        losehp(Maybe_Half_Phys(dmg), fell_on_sink, NO_KILLER_PREFIX);
        exercise(A_DEX, FALSE);
        selftouch("Falling, you");
        for (obj = svl.level.objects[u.ux][u.uy]; obj; obj = obj->nexthere)
            if (obj->oclass == WEAPON_CLASS || is_weptool(obj)) {
                You("fell on %s.", doname(obj));
                losehp(Maybe_Half_Phys(rnd(3)), fell_on_sink,
                       NO_KILLER_PREFIX);
                exercise(A_CON, FALSE);
            }
        ELevitation = save_ELev;
        HLevitation = save_HLev;
    }

    /*
     * Interrupt multi-turn putting on/taking off of armor (in which
     * case we reached the sink due to being teleported while busy;
     * in 3.4.3, Boots_on()/Boots_off() [called via (*afternmv)() when
     * 'multi' reaches 0] triggered a crash if we were donning/doffing
     * levitation boots [because the Boots_off() below causes 'uarmf'
     * to be null by the time 'afternmv' gets called]).
     *
     * Interrupt donning/doffing if we fall onto the sink, or if the
     * code below is going to remove levitation boots even when we
     * haven't fallen (innate floating or flying becoming unblocked).
     */
    if (ufall || lev_boots) {
        (void) stop_donning(lev_boots ? uarmf : (struct obj *) 0);
        /* recalculate in case uarmf just got set to null */
        lev_boots = (uarmf && uarmf->otyp == LEVITATION_BOOTS);
    }

    /* remove worn levitation items */
    ELevitation &= ~W_ARTI;
    HLevitation &= ~(I_SPECIAL | TIMEOUT);
    HLevitation++;
    if (uleft && uleft->otyp == RIN_LEVITATION) {
        obj = uleft;
        Ring_off(obj);
        off_msg(obj);
    }
    if (uright && uright->otyp == RIN_LEVITATION) {
        obj = uright;
        Ring_off(obj);
        off_msg(obj);
    }
    if (lev_boots) {
        obj = uarmf;
        (void) Boots_off();
        off_msg(obj);
    }
    HLevitation--;
    /* probably moot; we're either still levitating or went
       through float_down(), but make sure BFlying is up to date */
    float_vs_flight();
}

/* intended to be called only on ROCKs or TREEs */
boolean
may_dig(coordxy x, coordxy y)
{
    struct rm *lev = &levl[x][y];

    return (boolean) !((IS_STWALL(lev->typ) || IS_TREE(lev->typ))
                       && (lev->wall_info & W_NONDIGGABLE));
}

boolean
may_passwall(coordxy x, coordxy y)
{
    return (boolean) !(IS_STWALL(levl[x][y].typ)
                       && (levl[x][y].wall_info & W_NONPASSWALL));
}

boolean
bad_rock(struct permonst *mdat, coordxy x, coordxy y)
{
    return (boolean) ((Sokoban && sobj_at(BOULDER, x, y))
                      || (IS_OBSTRUCTED(levl[x][y].typ)
                          && (!tunnels(mdat) || needspick(mdat)
                              || !may_dig(x, y))
                          && !(passes_walls(mdat) && may_passwall(x, y))));
}

/* caller has already decided that it's a tight diagonal; check whether a
   monster--who might be the hero--can fit through, and if not then return
   the reason why:  1: can't fit, 2: possessions won't fit, 3: sokoban
   returns 0 if we can squeeze through */
int
cant_squeeze_thru(struct monst *mon)
{
    int amt;
    struct permonst *ptr = mon->data;

    if ((mon == &gy.youmonst) ? Passes_walls : passes_walls(ptr))
        return 0;

    /* too big? */
    if (bigmonst(ptr)
        && !(amorphous(ptr) || is_whirly(ptr) || noncorporeal(ptr)
             || slithy(ptr) || can_fog(mon)))
        return 1;

    /* lugging too much junk? */
    amt = (mon == &gy.youmonst) ? inv_weight() + weight_cap()
                               : curr_mon_load(mon);
    if (amt > 600)
        return 2;

    /* Sokoban restriction applies to hero only */
    if (mon == &gy.youmonst && Sokoban)
        return 3;

    /* can squeeze through */
    return 0;
}

boolean
invocation_pos(coordxy x, coordxy y)
{
    return (boolean) (Invocation_lev(&u.uz)
                      && x == svi.inv_pos.x && y == svi.inv_pos.y);
}

/* return TRUE if (ux+dx,uy+dy) is an OK place to move;
   mode is one of DO_MOVE, TEST_MOVE, TEST_TRAV, or TEST_TRAP */
boolean
test_move(
    coordxy ux, coordxy uy,
    coordxy dx, coordxy dy, /* these are -1|0|+1, not coordinates */
    int mode)
{
    coordxy x = ux + dx;
    coordxy y = uy + dy;
    struct rm *tmpr;
    struct rm *ust;

    svc.context.door_opened = FALSE;

    if (!isok(x, y))
        return FALSE;

    tmpr = &levl[x][y];

    /*
     *  Check for physical obstacles.  First, the place we are going.
     */
    if (IS_OBSTRUCTED(tmpr->typ) || tmpr->typ == IRONBARS) {
        if (Blind && mode == DO_MOVE)
            feel_location(x, y);
        if (Passes_walls && may_passwall(x, y)) {
            ; /* do nothing */
        } else if (Underwater) {
            /* note: if water_friction() changes direction due to
               turbulence, new target destination will always be water,
               so we won't get here, hence don't need to worry about
               "there" being somewhere the player isn't sure of */
            if (mode == DO_MOVE)
                There("is an obstacle there.");
            return FALSE;
        } else if (tmpr->typ == IRONBARS) {
            if (mode == DO_MOVE
                && (dmgtype(gy.youmonst.data, AD_RUST)
                    || dmgtype(gy.youmonst.data, AD_CORR)
                    || metallivorous(gy.youmonst.data))
                && still_chewing(x, y)) {
                return FALSE;
            }
            if (!(Passes_walls || passes_bars(gy.youmonst.data))) {
                if (mode == DO_MOVE && flags.mention_walls)
                    You("cannot pass through the bars.");
                return FALSE;
            }
        } else if (tunnels(gy.youmonst.data)
                   && !needspick(gy.youmonst.data)) {
            /* Eat the rock. */
            if (mode == DO_MOVE && still_chewing(x, y))
                return FALSE;
        } else if (flags.autodig && !svc.context.run && !svc.context.nopick
                   && uwep && is_pick(uwep)) {
            /* MRKR: Automatic digging when wielding the appropriate tool */
            if (mode == DO_MOVE)
                (void) use_pick_axe2(uwep);
            return FALSE;
        } else {
            if (mode == DO_MOVE) {
                if (is_db_wall(x, y)) {
                    pline("That drawbridge is up!");
                } else if (Passes_walls && !may_passwall(x, y)
                           && In_sokoban(&u.uz)) {
                    /* soko restriction stays even after puzzle is solved */
                    pline_The("Sokoban walls resist your ability.");
                } else if (flags.mention_walls) {
                    char buf[BUFSZ];
                    int glyph = back_to_glyph(x, y),
                        sym = glyph_is_cmap(glyph) ? glyph_to_cmap(glyph)
                                                   : -1;

                    if (sym == S_stone)
                        Strcpy(buf, "solid stone");
                    else if (sym >= 0)
                        Strcpy(buf, an(defsyms[sym].explanation));
                    else
                        Sprintf(buf, "impossible [background glyph=%d]",
                                glyph);
                    pline_dir(xytod(dx, dy), "It's %s.", buf);
                }
            }
            return FALSE;
        }
    } else if (IS_DOOR(tmpr->typ)) {
        if (closed_door(x, y)) {
            if (Blind && mode == DO_MOVE)
                feel_location(x, y);
            if (Passes_walls) {
                ; /* do nothing */
            } else if (can_ooze(&gy.youmonst)) {
                if (mode == DO_MOVE)
                    You("ooze under the door.");
            } else if (Underwater) {
                if (mode == DO_MOVE)
                    pline("There is an obstacle there.");
                return FALSE;
            } else if (tunnels(gy.youmonst.data)
                       && !needspick(gy.youmonst.data)) {
                /* Eat the door. */
                if (mode == DO_MOVE && still_chewing(x, y))
                    return FALSE;
            } else {
                if (mode == DO_MOVE) {
                    if (amorphous(gy.youmonst.data))
                        You("try to ooze under the door,"
                            " but can't squeeze your possessions through.");
                    if (flags.autoopen && !svc.context.run
                        && !Confusion && !Stunned && !Fumbling) {
                        int tmp = doopen_indir(x, y);
                        /* if 'autounlock' includes Kick, we might have a
                           kick at the door queued up after doopen_indir() */
                        struct _cmd_queue *cq = cmdq_peek(CQ_CANNED);

                        if (tmp == ECMD_OK && cq && cq->typ == CMDQ_EXTCMD
                            && cq->ec_entry == ext_func_tab_from_func(dokick))
                            /* door hasn't been opened, but fake it so that
                               canned kick will be executed as next command */
                            svc.context.door_opened = TRUE;
                        else
                            svc.context.door_opened = !closed_door(x, y);
                        svc.context.move = (ux != u.ux || uy != u.uy);
                    } else if (x == ux || y == uy) {
                        if (Blind || Stunned || ACURR(A_DEX) < 10
                            || Fumbling) {
                            if (u.usteed) {
                                You_cant("lead %s through that closed door.",
                                         y_monnam(u.usteed));
                            } else {
                                pline("Ouch!  You bump into a door.");
                                exercise(A_DEX, FALSE);
                            }
                            /* use current move; needed for the "ouch" case
                               but done for steed case too for consistency;
                               we haven't opened a door but we're going to
                               return False and without having 'door_opened'
                               set, 'move' would get reset by caller */
                            svc.context.door_opened = svc.context.move = TRUE;
                            /* since we've just lied about successfully
                               moving, we need to manually stop running */
                            nomul(0);
                        } else
                            pline("That door is closed.");
                    }
                } else if (mode == TEST_TRAV || mode == TEST_TRAP)
                    goto testdiag;
                return FALSE;
            }
        } else {
 testdiag:
            if (dx && dy && !Passes_walls
                && (!doorless_door(x, y) || block_door(x, y))) {
                /* Diagonal moves into a door are not allowed. */
                if (mode == DO_MOVE) {
                    if (Blind)
                        feel_location(x, y);
                    if (Underwater || flags.mention_walls)
                        You_cant("move diagonally into an intact doorway.");
                }
                return FALSE;
            }
        }
    }
    if (dx && dy && bad_rock(gy.youmonst.data, ux, y)
        && bad_rock(gy.youmonst.data, x, uy)) {
        /* Move at a diagonal. */
        switch (cant_squeeze_thru(&gy.youmonst)) {
        case 3:
            if (mode == DO_MOVE)
                You("cannot pass that way.");
            return FALSE;
        case 2:
            if (mode == DO_MOVE)
                You("are carrying too much to get through.");
            return FALSE;
        case 1:
            if (mode == DO_MOVE)
                Your("body is too large to fit through.");
            return FALSE;
        default:
            break; /* can squeeze through */
        }
    } else if (dx && dy && worm_cross(ux, uy, x, y)) {
        /* consecutive long worm segments are at <ux,y> and <x,uy> */
        if (mode == DO_MOVE)
            pline("%s is in your way.", YMonnam(m_at(ux, y)));
        return FALSE;
    }
    /* Pick travel path that does not require crossing a trap.
     * Avoid water and lava using the usual running rules.
     * (but not u.ux/u.uy because findtravelpath walks toward u.ux/u.uy) */
    if (svc.context.run == 8 && (mode != DO_MOVE) && !u_at(x, y)) {
        struct trap *t = t_at(x, y);

        if (t && t->tseen && t->ttyp != VIBRATING_SQUARE)
            return (mode == TEST_TRAP);

        /* FIXME: should be using lastseentyp[x][y] rather than seen vector
         */
        if ((levl[x][y].seenv && is_pool_or_lava(x, y)) /* known pool/lava */
            && ((IS_WATERWALL(levl[x][y].typ) /* never enter wall of liquid */
                 || levl[x][y].typ == LAVAWALL)
                /* don't enter pool or lava (must be one of the two to
                   get here) unless flying or levitating or have known
                   water-walking for pool or known lava-walking and
                   already be on/over lava for lava */
                || !(Levitation || Flying
                     || (is_pool(x, y) ? Known_wwalking
                         : (Known_lwalking && is_lava(u.ux, u.uy))))))
            return (mode == TEST_TRAP);
    }

    if (mode == TEST_TRAP)
        return FALSE; /* do not move through traps */

    ust = &levl[ux][uy];

    /* Now see if other things block our way . . */
    if (dx && dy && !Passes_walls && IS_DOOR(ust->typ)
        && (!doorless_door(ux, uy) || block_entry(x, y))) {
        /* Can't move at a diagonal out of a doorway with door. */
        if (mode == DO_MOVE && flags.mention_walls)
            You_cant("move diagonally out of an intact doorway.");
        return FALSE;
    }

    if (sobj_at(BOULDER, x, y) && (Sokoban || !Passes_walls)) {
        if (mode != TEST_TRAV && svc.context.run >= 2
            && !(Blind || Hallucination) && !could_move_onto_boulder(x, y)) {
            if (mode == DO_MOVE && flags.mention_walls)
                pline_dir(xytod(dx,dy), "A boulder blocks your path.");
            return FALSE;
        }
        if (mode == DO_MOVE) {
            /* tunneling monsters will chew before pushing */
            if (tunnels(gy.youmonst.data) && !needspick(gy.youmonst.data)
                && !Sokoban) {
                if (still_chewing(x, y))
                    return FALSE;
            } else if (moverock() < 0)
                return FALSE;
        } else if (mode == TEST_TRAV) {
            struct obj *obj;

            /* never travel through boulders in Sokoban */
            if (Sokoban)
                return FALSE;

            /* don't pick two boulders in a row, unless there's a way thru */
            if (sobj_at(BOULDER, ux, uy) && !Sokoban) {
                if (!Passes_walls
                    && !could_move_onto_boulder(ux, uy)
                    && !(tunnels(gy.youmonst.data)
                         && !needspick(gy.youmonst.data))
                    && !carrying(PICK_AXE) && !carrying(DWARVISH_MATTOCK)
                    && !((obj = carrying(WAN_DIGGING))
                         && !objects[obj->otyp].oc_name_known))
                    return FALSE;
            }
        }
        /* assume you'll be able to push it when you get there... */
    }

    /* OK, it is a legal place to move. */
    return TRUE;
}

/*
 * Find a path from the destination (u.tx,u.ty) back to (u.ux,u.uy).
 * A shortest path is returned.  If guess is TRUE, consider various
 * inaccessible locations as valid intermediate path points.
 * Returns TRUE if a path was found.
 * gt.travelmap keeps track of map locations we've moved through
 * this travel session. It will be cleared once the travel stops.
 */
staticfn boolean
findtravelpath(int mode)
{
    if (!gt.travelmap)
        gt.travelmap = selection_new();
    /* if travel to adjacent, reachable location, use normal movement rules */
    if ((mode == TRAVP_TRAVEL || mode == TRAVP_VALID) && svc.context.travel1
        /* was '&& distmin(u.ux, u.uy, u.tx, u.ty) == 1' */
        && next2u(u.tx, u.ty) /* one step away */
        /* handle restricted diagonals */
        && crawl_destination(u.tx, u.ty)) {
        end_running(FALSE);
        if (test_move(u.ux, u.uy, u.tx - u.ux, u.ty - u.uy, TEST_MOVE)) {
            if (mode == TRAVP_TRAVEL) {
                u.dx = u.tx - u.ux;
                u.dy = u.ty - u.uy;
                nomul(0);
                iflags.travelcc.x = iflags.travelcc.y = 0;
            }
            return TRUE;
        }
        if (mode == TRAVP_TRAVEL)
            svc.context.run = 8;
    }
    if (u.tx != u.ux || u.ty != u.uy) {
        coordxy travel[COLNO][ROWNO];
        coordxy travelstepx[2][COLNO * ROWNO];
        coordxy travelstepy[2][COLNO * ROWNO];
        coordxy tx, ty, ux, uy;
        int n = 1;      /* max offset in travelsteps */
        int set = 0;    /* two sets current and previous */
        int radius = 1; /* search radius */
        int i;

        /* If guessing, first find an "obvious" goal location.  The obvious
         * goal is the position the player knows of, or might figure out
         * (couldsee) that is closest to the target on a straight path.
         */
        if (mode == TRAVP_GUESS || mode == TRAVP_VALID) {
            tx = u.ux;
            ty = u.uy;
            ux = u.tx;
            uy = u.ty;
        } else {
            tx = u.tx;
            ty = u.ty;
            ux = u.ux;
            uy = u.uy;
        }

 noguess:
        (void) memset((genericptr_t) travel, 0, sizeof travel);
        travelstepx[0][0] = tx;
        travelstepy[0][0] = ty;

        while (n != 0) {
            int nn = 0;

            for (i = 0; i < n; i++) {
                int dir;
                coordxy x = travelstepx[set][i];
                coordxy y = travelstepy[set][i];
                /* no diagonal movement for grid bugs */
                int dirmax = NODIAG(u.umonnum) ? 4 : N_DIRS;
                boolean alreadyrepeated = FALSE;

                for (dir = 0; dir < dirmax; ++dir) {
                    coordxy nx = x + xdir[dirs_ord[dir]];
                    coordxy ny = y + ydir[dirs_ord[dir]];

                    /*
                     * When guessing and trying to travel as close as possible
                     * to an unreachable target space, don't include spaces
                     * that would never be picked as a guessed target in the
                     * travel matrix describing hero-reachable spaces.
                     * This stops travel from getting confused and moving
                     * the hero back and forth in certain degenerate
                     * configurations of sight-blocking obstacles, e.g.
                     *
                     *  T         1. Dig this out and carry enough to not be
                     *   ####       able to squeeze through diagonal gaps.
                     *   #--.---    Stand at @ and target travel at space T.
                     *    @.....
                     *    |.....
                     *
                     *  T         2. couldsee() marks spaces marked a and x
                     *   ####       as eligible guess spaces to move the hero
                     *   a--.---    towards.  Space a is closest to T, so it
                     *    @xxxxx    gets chosen.  Travel system moves @ right
                     *    |xxxxx    to travel to space a.
                     *
                     *  T         3. couldsee() marks spaces marked b, c and x
                     *   ####       as eligible guess spaces to move the hero
                     *   a--c---    towards.  Since findtravelpath() is called
                     *    b@xxxx    repeatedly during travel, it doesn't
                     *    |xxxxx    remember that it wanted to go to space a,
                     *              so in comparing spaces b and c, b is
                     *              chosen, since it seems like the closest
                     *              eligible space to T. Travel system moves @
                     *              left to go to space b.
                     *
                     *            4. Go to 2.
                     *
                     * By limiting the travel matrix here, space a in the
                     * example above is never included in it, preventing
                     * the cycle.
                     */
                    if (!isok(nx, ny)
                        || ((mode == TRAVP_GUESS) && !couldsee(nx, ny)))
                        continue;
                    if ((!Passes_walls && !can_ooze(&gy.youmonst)
                         && closed_door(x, y))
                        || (sobj_at(BOULDER, x, y)
                            && !could_move_onto_boulder(x, y))
                        || test_move(x, y, nx - x, ny - y, TEST_TRAP)) {
                        /* closed doors and boulders usually cause a delay,
                           so prefer another path; however, giants and tiny
                           creatures can use m<dir> to move onto a boulder's
                           spot without pushing, so allow boulders for them */
                        if (travel[x][y] > radius - 3) {
                            if (!alreadyrepeated) {
                                travelstepx[1 - set][nn] = x;
                                travelstepy[1 - set][nn] = y;
                                /* don't change travel matrix! */
                                nn++;
                                alreadyrepeated = TRUE;
                            }
                            continue;
                        }
                    }
                    if (test_move(x, y, nx - x, ny - y, TEST_TRAV)
                        && (levl[nx][ny].seenv
                            || (!Blind && couldsee(nx, ny)))) {
                        if (nx == ux && ny == uy) {
                            if (mode == TRAVP_TRAVEL || mode == TRAVP_VALID) {
                                boolean visited
                                    = selection_getpoint(x, y, gt.travelmap);
                                u.dx = x - ux;
                                u.dy = y - uy;
                                if (mode == TRAVP_TRAVEL
                                    && ((x == u.tx && y == u.ty)
                                        || visited)) {
                                    nomul(0);
                                    /* reset run so domove run checks work */
                                    svc.context.run = 8;
                                    if (visited)
                                        You("stop, unsure which way to go.");
                                    else
                                        iflags.travelcc.x
                                        = iflags.travelcc.y = 0;
                                }
                                selection_setpoint(u.ux, u.uy,
                                                   gt.travelmap, 1);
                                return TRUE;
                            }
                        } else if (!travel[nx][ny]) {
                            travelstepx[1 - set][nn] = nx;
                            travelstepy[1 - set][nn] = ny;
                            travel[nx][ny] = radius;
                            nn++;
                        }
                    }
                }
            }

#ifdef DEBUG
            if (iflags.trav_debug) {
                /* Use of warning glyph is arbitrary. It stands out. */
                tmp_at(DISP_ALL, warning_to_glyph(1));
                for (i = 0; i < nn; ++i) {
                    tmp_at(travelstepx[1 - set][i], travelstepy[1 - set][i]);
                }
                nh_delay_output();
                if (flags.runmode == RUN_CRAWL) {
                    nh_delay_output();
                    nh_delay_output();
                }
                tmp_at(DISP_END, 0);
            }
#endif /* DEBUG */

            n = nn;
            set = 1 - set;
            radius++;
        }

        /* if guessing, find best location in travel matrix and go there */
        if (mode == TRAVP_GUESS) {
            int px = tx, py = ty; /* pick location */
            int dist, nxtdist, d2, nd2;
            int ctrav, ptrav = COLNO*ROWNO;

            dist = distmin(ux, uy, tx, ty);
            d2 = dist2(ux, uy, tx, ty);
            for (tx = 1; tx < COLNO; ++tx)
                for (ty = 0; ty < ROWNO; ++ty)
                    if (couldsee(tx, ty) && (ctrav = travel[tx][ty]) > 0) {
                        nxtdist = distmin(ux, uy, tx, ty);
                        if (nxtdist == dist && ctrav < ptrav) {
                            nd2 = dist2(ux, uy, tx, ty);
                            if (nd2 < d2) {
                                /* prefer non-zigzag path */
                                px = tx;
                                py = ty;
                                d2 = nd2;
                                ptrav = ctrav;
                            }
                        } else if (nxtdist < dist) {
                            px = tx;
                            py = ty;
                            dist = nxtdist;
                            d2 = dist2(ux, uy, tx, ty);
                            ptrav = ctrav;
                        }
                    }

            if (u_at(px, py)) {
                /* no guesses, just go in the general direction */
                u.dx = sgn(u.tx - u.ux);
                u.dy = sgn(u.ty - u.uy);
                if (test_move(u.ux, u.uy, u.dx, u.dy, TEST_MOVE)) {
                    selection_setpoint(u.ux, u.uy, gt.travelmap, 1);
                    return TRUE;
                }
                goto found;
            }
#ifdef DEBUG
            if (iflags.trav_debug) {
                /* Use of warning glyph is arbitrary. It stands out. */
                tmp_at(DISP_ALL, warning_to_glyph(2));
                tmp_at(px, py);
                nh_delay_output();
                if (flags.runmode == RUN_CRAWL) {
                    nh_delay_output();
                    nh_delay_output();
                    nh_delay_output();
                    nh_delay_output();
                }
                tmp_at(DISP_END, 0);
            }
#endif /* DEBUG */
            tx = px;
            ty = py;
            ux = u.ux;
            uy = u.uy;
            set = 0;
            n = radius = 1;
            mode = TRAVP_TRAVEL;
            goto noguess;
        }
        return FALSE;
    }

 found:
    u.dx = 0;
    u.dy = 0;
    nomul(0);
    return FALSE;
}

boolean
is_valid_travelpt(coordxy x, coordxy y)
{
    int tx = u.tx;
    int ty = u.ty;
    boolean ret;
    int glyph = glyph_at(x,y);

    if (u_at(x, y))
        return TRUE;
    if (isok(x,y) && glyph_is_cmap(glyph) && S_stone == glyph_to_cmap(glyph)
        && !levl[x][y].seenv)
        return FALSE;
    u.tx = x;
    u.ty = y;
    ret = findtravelpath(TRAVP_VALID);
    u.tx = tx;
    u.ty = ty;
    return ret;
}

/* try to escape being stuck in a trapped state by walking out of it;
   return true iff moving should continue to intended destination
   (all failures and most successful escapes leave hero at original spot) */
staticfn boolean
trapmove(
    coordxy x, coordxy y, /* targeted destination, <u.ux+u.dx,u.uy+u.dy> */
    struct trap *desttrap) /* nonnull if another trap at <x,y> */
{
    boolean anchored = FALSE;
    const char *predicament, *culprit;
    char *steedname = !u.usteed ? (char *) 0 : y_monnam(u.usteed);

    if (!u.utrap)
        return TRUE; /* sanity check */

    /*
     * Note: caller should call reset_utrap() when we set u.utrap to 0.
     */

    switch (u.utraptype) {
    case TT_BEARTRAP:
        if (flags.verbose) {
            predicament = "caught in a bear trap";
            if (u.usteed)
                Norep("%s is %s.", upstart(steedname), predicament);
            else
                Norep("You are %s.", predicament);
        }
        /* [why does diagonal movement give quickest escape?] */
        if ((u.dx && u.dy) || !rn2(5))
            u.utrap--;
        if (!u.utrap)
            goto wriggle_free;
        break;
    case TT_PIT:
        if (desttrap && desttrap->tseen
            && is_pit(desttrap->ttyp))
            return TRUE; /* move into adjacent pit */
        /* try to escape; position stays same regardless of success */
        climb_pit();
        break;
    case TT_WEB:
        if (u_wield_art(ART_STING)) {
            /* escape trap but don't move and don't destroy it */
            u.utrap = 0; /* caller will call reset_utrap() */
            pline("Sting cuts through the web!");
            break;
        }
        if (--u.utrap) {
            if (flags.verbose) {
                predicament = "stuck to the web";
                if (u.usteed)
                    Norep("%s is %s.", upstart(steedname), predicament);
                else
                    Norep("You are %s.", predicament);
            }
        } else {
            if (u.usteed)
                pline("%s breaks out of the web.", upstart(steedname));
            else
                You("disentangle yourself.");
        }
        break;
    case TT_LAVA:
        if (flags.verbose) {
            predicament = "stuck in the lava";
            if (u.usteed)
                Norep("%s is %s.", upstart(steedname), predicament);
            else
                Norep("You are %s.", predicament);
        }
        if (!is_lava(x, y)) {
            u.utrap--;
            if ((u.utrap & 0xff) == 0) {
                u.utrap = 0;
                if (u.usteed)
                    You("lead %s to the edge of the %s.", steedname,
                        hliquid("lava"));
                else
                    You("pull yourself to the edge of the %s.",
                        hliquid("lava"));
            }
        }
        u.umoved = TRUE;
        break;
    case TT_INFLOOR:
    case TT_BURIEDBALL:
        anchored = (u.utraptype == TT_BURIEDBALL);
        if (anchored) {
            coord cc;

            cc.x = u.ux, cc.y = u.uy;
            /* can move normally within radius 1 of buried ball */
            if (buried_ball(&cc) && dist2(x, y, cc.x, cc.y) <= 2) {
                /* ugly hack: we need to issue some message here
                   in case "you are chained to the buried ball"
                   was the most recent message given, otherwise
                   our next attempt to move out of tether range
                   after this successful move would have its
                   can't-do-that message suppressed by Norep */
                if (flags.verbose)
                    Norep("You move within the chain's reach.");
                return TRUE;
            }
        }
        if (--u.utrap) {
            if (flags.verbose) {
                if (anchored) {
                    predicament = "chained to the";
                    culprit = "buried ball";
                } else {
                    predicament = "stuck in the";
                    culprit = surface(u.ux, u.uy);
                }
                if (u.usteed) {
                    if (anchored)
                        Norep("You and %s are %s %s.", steedname, predicament,
                              culprit);
                    else
                        Norep("%s is %s %s.", upstart(steedname), predicament,
                              culprit);
                } else
                    Norep("You are %s %s.", predicament, culprit);
            }
        } else {
 wriggle_free:
            if (u.usteed)
                pline("%s finally %s free.", upstart(steedname),
                      !anchored ? "lurches" : "wrenches the ball");
            else
                You("finally %s free.",
                    !anchored ? "wriggle" : "wrench the ball");
            if (anchored)
                buried_ball_to_punishment();
        }
        break;
    case TT_NONE:
        impossible("trapmove: trapped in nothing?");
        break;
    default:
        impossible("trapmove: stuck in unknown trap? (%d)",
                   (int) u.utraptype);
        break;
    }
    return FALSE;
}

boolean
u_rooted(void)
{
    if (!gy.youmonst.data->mmove) {
        You("are rooted %s.",
            Levitation || Is_airlevel(&u.uz) || Is_waterlevel(&u.uz)
                ? "in place"
                : "to the ground");
        nomul(0);
        return TRUE;
    }
    return FALSE;
}

void
notice_mon(struct monst *mtmp)
{
    if (a11y.mon_notices && !a11y.mon_notices_blocked) {
        boolean spot = canspotmon(mtmp)
            && !(is_hider(mtmp->data)
                 && (mtmp->mundetected
                     || M_AP_TYPE(mtmp) == M_AP_FURNITURE
                     || M_AP_TYPE(mtmp) == M_AP_OBJECT));

        if (spot && !mtmp->mspotted && !DEADMONSTER(mtmp)) {
            mtmp->mspotted = TRUE;
            set_msg_xy(mtmp->mx, mtmp->my);
            You("%s %s.", canseemon(mtmp) ? "see" : "notice",
                x_monnam(mtmp,
                     mtmp->mtame ? ARTICLE_YOUR
                     : (!has_mgivenname(mtmp)
                        && !type_is_pname(mtmp->data)) ? ARTICLE_A
                     : ARTICLE_NONE,
                     (mtmp->mpeaceful && !mtmp->mtame) ? "peaceful" : 0,
                     has_mgivenname(mtmp) ? SUPPRESS_SADDLE : 0, FALSE));
        } else if (!spot) {
            mtmp->mspotted = FALSE;
        }
    }
}

staticfn int QSORTCALLBACK
notice_mons_cmp(const genericptr ptr1, const genericptr ptr2)
{
    const struct monst *m1 = *(const struct monst **) ptr1,
        *m2 = *(const struct monst **) ptr2;

    return (distu(m1->mx, m1->my) - distu(m2->mx, m2->my));
}

void
notice_all_mons(boolean reset)
{
    if (a11y.mon_notices && !a11y.mon_notices_blocked) {
        struct monst *mtmp;
        struct monst **arr = NULL;
        int j, i = 0, cnt = 0;

        for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
            if (DEADMONSTER(mtmp))
                continue;
            if (canspotmon(mtmp))
                cnt++;
            else if (reset)
                mtmp->mspotted = FALSE;
        }
        if (!cnt)
            return;

        arr = (struct monst **) alloc(cnt * sizeof (struct monst *));

        for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
            if (DEADMONSTER(mtmp))
                continue;
            if (!canspotmon(mtmp))
                mtmp->mspotted = FALSE;
            else if (i < cnt)
                arr[i++] = mtmp;
        }

        if (i) {
            qsort((genericptr_t) arr, (size_t) i, sizeof *arr,
                  notice_mons_cmp);

            for (j = 0; j < i; j++)
                notice_mon(arr[j]);
        }

        free(arr);
    }
}

/* maybe disturb buried zombies when an object is dropped or thrown nearby */
void
impact_disturbs_zombies(struct obj *obj, boolean violent)
{
    /* if object won't make a noticeable impact, let buried zombies rest */
    if (obj->owt < (violent ? 10U : 100U) || is_flimsy(obj))
        return;

    disturb_buried_zombies(obj->ox, obj->oy);
}

/* reduce zombification timeout of buried zombies around px, py */
void
disturb_buried_zombies(coordxy x, coordxy y)
{
    struct obj *otmp;
    long t;

    for (otmp = svl.level.buriedobjlist; otmp; otmp = otmp->nobj) {
        if (otmp->otyp == CORPSE && otmp->timed
            && otmp->ox >= x - 1 && otmp->ox <= x + 1
            && otmp->oy >= y - 1 && otmp->oy <= y + 1
            && (t = peek_timer(ZOMBIFY_MON, obj_to_any(otmp))) > 0) {
            t = stop_timer(ZOMBIFY_MON, obj_to_any(otmp));
            (void) start_timer(max(1, (t*2/3)), TIMER_OBJECT,
                               ZOMBIFY_MON, obj_to_any(otmp));
        }
    }
}

/* return an appropriate locomotion word for hero */
const char *
u_locomotion(const char *def)
{
    boolean capitalize = (*def == highc(*def));

    /* regular locomotion() takes a monster type rather than a specific
       monster, so can't tell whether it is operating on hero;
       its is_flyer() and is_floater() tests wouldn't work on hero except
       when hero is polymorphed and not wearing an amulet of flying
       or boots/ring/spell of levitation */
    return Levitation ? (capitalize ? "Float" : "float")
           : Flying ? (capitalize ? "Fly" : "fly")
             : locomotion(gy.youmonst.data, def);
}

/* Return a simplified floor solid/liquid state based on hero's state */
staticfn schar
u_simple_floortyp(coordxy x, coordxy y)
{
    boolean u_in_air = (Levitation || Flying || !grounded(gy.youmonst.data));

    if (is_waterwall(x, y))
        return WATER; /* wall of water, fly/lev does not matter */
    if (levl[x][y].typ == LAVAWALL)
        return LAVAWALL; /* wall of lava, fly/lev does not matter */
    if (!u_in_air) {
        if (is_pool(x, y))
            return POOL;
        if (is_lava(x, y))
            return LAVAPOOL;
    }
    return ROOM;
}

/* maybe show a helpful gameplay tip? returns True if tip gets shown */
boolean
handle_tip(int tip)
{
    if (!flags.tips)
        return FALSE;

    if (tip >= 0 && tip < NUM_TIPS && !svc.context.tips[tip]) {
        svc.context.tips[tip] = TRUE;
        /* the "Tip:" prefix is a hint to use of OPTIONS=!tips to suppress */
        switch (tip) {
        case TIP_ENHANCE:
            pline("(Tip: use the #enhance command to advance them.)");
            break;
        case TIP_SWIM:
            pline("(Tip: use '%s' prefix to step in if you really want to.)",
                  visctrl(cmd_from_func(do_reqmenu)));
            break;
        case TIP_UNTRAP_MON:
            pline("(Tip: perhaps #untrap would help?)");
            break;
        case TIP_GETPOS:
            l_nhcore_call(NHCORE_GETPOS_TIP);
            break;
        default:
            impossible("Unknown tip in handle_tip(%i)", tip);
            break;
        }
        return TRUE;
    }
    return FALSE;
}

/* Is it dangerous for hero to move to x,y due to water or lava? */
staticfn boolean
swim_move_danger(coordxy x, coordxy y)
{
    schar newtyp = u_simple_floortyp(x, y);
    boolean liquid_wall = IS_WATERWALL(newtyp) || newtyp == LAVAWALL;

    if (Underwater && (is_pool(x,y) || IS_WATERWALL(newtyp)))
        return FALSE;

    if ((newtyp != u_simple_floortyp(u.ux, u.uy))
        && !Stunned && !Confusion && levl[x][y].seenv
        && (is_pool(x, y) || is_lava(x, y) || liquid_wall)) {

        /* FIXME: This can be exploited to identify ring of fire resistance
         * if the player is wearing it unidentified and has identified
         * fireproof boots of water walking and is walking over lava. However,
         * this is such a marginal case that it may not be worth fixing. */
        if ((is_pool(x, y) && !Known_wwalking)
            /* is_lava(ux,uy): don't move onto/over lava with known
               lava-walking because it isn't completely safe, but do
               continue to move over lava if already doing so */
            || (is_lava(x, y) && !Known_lwalking && !is_lava(u.ux, u.uy))
            || liquid_wall) {
            if (svc.context.nopick) {
                /* moving with m-prefix */
                svc.context.tips[TIP_SWIM] = TRUE;
                return FALSE;
            } else if (ParanoidSwim || liquid_wall) {
                You("avoid %s into the %s.",
                    ing_suffix(u_locomotion("step")),
                    waterbody_name(x, y));
                (void) handle_tip(TIP_SWIM);
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* moving with 'm' prefix, bump into a monster? */
staticfn boolean
domove_bump_mon(struct monst *mtmp, int glyph)
{
    /* If they used a 'm' command, trying to move onto a monster
     * prints the below message and wastes a turn.  The exception is
     * if the monster is unseen and the player doesn't remember an
     * invisible monster--then, we fall through to do_attack() and
     * attack_check(), which still wastes a turn, but prints a
     * different message and makes the player remember the monster.
     */
    if (svc.context.nopick && !svc.context.travel
        && (canspotmon(mtmp) || glyph_is_invisible(glyph)
            || glyph_is_warning(glyph))) {
        if (M_AP_TYPE(mtmp) && !Protection_from_shape_changers
            && !sensemon(mtmp))
            stumble_onto_mimic(mtmp);
        else if (mtmp->mpeaceful && !Hallucination)
            /* m_monnam(): "dog" or "Fido", no "invisible dog" or "it" */
            pline("Pardon me, %s.", m_monnam(mtmp));
        else
            You("move right into %s.", mon_nam(mtmp));
        return TRUE;
    }
    return FALSE;
}

/* hero is moving, do we maybe attack a monster at (x,y)?
   returns TRUE if hero movement is used up.
   sets displaceu, if hero and monster could swap places instead.
*/
staticfn boolean
domove_attackmon_at(
    struct monst *mtmp,
    coordxy x, coordxy y,
    boolean *displaceu)
{
    /* assert(mtmp != NULL) */
    /* only attack if we know it's there
     * or if we used the 'F' command to fight blindly
     * or if it hides_under, in which case we call do_attack() to print
     * the Wait! message.
     * This is different from ceiling hiders, who aren't handled in
     * do_attack().
     */
    if (svc.context.forcefight || !mtmp->mundetected || sensemon(mtmp)
        || ((hides_under(mtmp->data) || mtmp->data->mlet == S_EEL)
            && !is_safemon(mtmp))) {
        /* target monster might decide to switch places with you... */
        *displaceu = (is_displaced(mtmp->data) && !rn2(2)
                      && mtmp->mux == u.ux0 && mtmp->muy == u.uy0
                      && !helpless(mtmp)
                      && !mtmp->meating && !mtmp->mtrapped
                      && !u.utrap && !u.ustuck && !u.usteed
                      && !(u.dx && u.dy
                           && (NODIAG(u.umonnum)
                               || (bad_rock(mtmp->data, x, u.uy0)
                                   && bad_rock(mtmp->data, u.ux0, y))
                               || (bad_rock(gy.youmonst.data, u.ux0, y)
                                   && bad_rock(gy.youmonst.data, x, u.uy0))))
                      && goodpos(u.ux0, u.uy0, mtmp, GP_ALLOW_U));
        /* if not displacing, try to attack; note that it might evade;
           also, we don't attack tame or peaceful when safemon() */
        if (!*displaceu) {
            if (do_attack(mtmp))
                return TRUE;
        }
    }
    return FALSE;
}

/* force-fight iron bars with your weapon? */
staticfn boolean
domove_fight_ironbars(coordxy x, coordxy y)
{
    if (svc.context.forcefight && levl[x][y].typ == IRONBARS && uwep) {
        struct obj *obj = uwep;
        unsigned breakflags = (BRK_BY_HERO | BRK_FROM_INV | BRK_MELEE);

        if (breaktest(obj)) {
            if (obj->quan > 1L)
                obj = splitobj(obj, 1L);
            else
                setuwep((struct obj *) 0);
            freeinv(obj);
            breakflags |= BRK_KNOWN2BREAK;
        } else {
            breakflags |= BRK_KNOWN2NOTBREAK;
        }

        hit_bars(&obj, u.ux, u.uy, x, y, breakflags);
        return TRUE;
    }
    return FALSE;
}

/* force-fight a spider web with your weapon */
staticfn boolean
domove_fight_web(coordxy x, coordxy y)
{
    struct trap *trap = t_at(x, y);

    if (svc.context.forcefight && trap && trap->ttyp == WEB && trap->tseen) {
        int wtype = uwep_skill_type(),
            /* minus_2: restricted or unskilled: -1, basic: 0, skilled: 1,
               expert: 2, master: 3, grandmaster: 4 */
            wskill_minus_2 = max(P_SKILL(wtype), P_UNSKILLED) - 2,
            /* higher value is worse for player; for weaponless, adjust the
               chance to succeed rather than maybe make two tries */
            roll = rn2(uwep ? 20 : (45 - 5 * wskill_minus_2));

        if (uwep && (u_wield_art(ART_STING)
                     || (uwep->oartifact && attacks(AD_FIRE, uwep)))) {
            /* guaranteed success */
            pline("%s %s through the web!", bare_artifactname(uwep),
                  u_wield_art(ART_STING) ? "cuts" : "burns");

        /* is_blade() includes daggers (which are classified as PIERCE)
           but doesn't include axes and slashing polearms */
        } else if (uwep && !is_blade(uwep)
                   && (!u.twoweap || !is_blade(uswapwep))) {
            char *uwepstr = 0, *scndstr = 0, uwepbuf[BUFSZ], scndbuf[BUFSZ];
            boolean onewep;

            /* when dual wielding, second weapon will only be mentioned
               if it has a different type description from primary */
            Strcpy(uwepbuf, weapon_descr(uwep));
            Strcpy(scndbuf, u.twoweap ? weapon_descr(uswapwep) : "");
            onewep = !*scndbuf || !strcmp(uwepbuf, scndbuf);
            if (!strcmpi(uwepbuf, "armor") || !strcmpi(uwepbuf, "food")
                || !strcmpi(uwepbuf, "venom")) { /* as-is */
                /* non-weapon item wielded, of a type where an() would
                   result in weird phrasing; dual wield not possible */
                uwepstr = uwepbuf;
            } else if (uwep->quan == 1L /* singular */
                       /* unless secondary is suppressed due to same type */
                       && !(u.twoweap && onewep)) {
                uwepstr = an(uwepbuf);
            } else { /* plural */
                uwepstr = makeplural(uwepbuf);
            }
            if (!onewep) {
                assert(uswapwep != NULL);
                scndstr = (uswapwep->quan == 1L) ? an(scndbuf)
                                                 : makeplural(scndbuf);
            }
            You_cant("cut a web with %s%s%s!", uwepstr,
                     !onewep ? " or " : "", !onewep ? scndstr : "");
            return TRUE;

        /* weapon is ok; check whether hit is successful */
        } else if (roll > (acurrstr() - 2 /* 1..19 */
                           /* for weaponless, 'roll' was adjusted above */
                           + (uwep ? uwep->spe + wskill_minus_2 : 0))) {
            /* TODO: add failures, maybe make an occupation? */
            You("%s ineffectually at some of the strands.",
                uwep ? "hack" : "thrash");
            return TRUE;

        /* hit has succeeded */
        } else {
            You("%s through the web.", uwep ? "cut" : "punch");
            /* doesn't break "never hit with a wielded weapon" conduct */
            use_skill(wtype, 1);
        }

        deltrap(trap);
        newsym(x, y);
        return TRUE;
    }
    return FALSE;
}

/* maybe swap places with a pet? returns TRUE if swapped places */
staticfn boolean
domove_swap_with_pet(
    struct monst *mtmp,
    coordxy x, coordxy y)
{
    struct trap *trap;
    /* if it turns out we can't actually move */
    boolean didnt_move = FALSE;
    boolean u_with_boulder = (sobj_at(BOULDER, u.ux, u.uy) != 0);

    /* seemimic/newsym should be done before moving hero, otherwise
       the display code will draw the hero here before we possibly
       cancel the swap below (we can ignore steed mx,my here) */
    u.ux = u.ux0, u.uy = u.uy0;
    mtmp->mundetected = 0;
    if (M_AP_TYPE(mtmp))
        seemimic(mtmp);
    u.ux = mtmp->mx, u.uy = mtmp->my; /* resume swapping positions */

    trap = mtmp->mtrapped ? t_at(mtmp->mx, mtmp->my) : 0;
    if (!trap)
        mtmp->mtrapped = 0;

    if (mtmp->mtrapped && is_pit(trap->ttyp)
        && sobj_at(BOULDER, trap->tx, trap->ty)) {
        /* can't swap places with pet pinned in a pit by a boulder */
        didnt_move = TRUE;
    } else if (u.ux0 != x && u.uy0 != y && NODIAG(mtmp->data - mons)) {
        /* can't swap places when pet can't move to your spot */
        You("stop.  %s can't move diagonally.", YMonnam(mtmp));
        didnt_move = TRUE;
    } else if (u_with_boulder
               && !(verysmall(mtmp->data)
                    && (!mtmp->minvent || curr_mon_load(mtmp) <= 600))) {
        /* can't swap places when pet won't fit there with the boulder */
        You("stop.  %s won't fit into the same spot that you're at.",
            YMonnam(mtmp));
        didnt_move = TRUE;
    } else if (u.ux0 != x && u.uy0 != y && bad_rock(mtmp->data, x, u.uy0)
               && bad_rock(mtmp->data, u.ux0, y)
               && (bigmonst(mtmp->data) || (curr_mon_load(mtmp) > 600))) {
        /* can't swap places when pet won't fit thru the opening */
        You("stop.  %s won't fit through.", YMonnam(mtmp));
        didnt_move = TRUE;
    } else if (mtmp->mpeaceful && mtmp->mtrapped) {
        /* all mtame are also mpeaceful, so this affects pets too */
        assert(trap != NULL); /* implied by mtrapped */
        const char *what = trapname(trap->ttyp, FALSE), *which = "that ";
        char anbuf[10];

        if (!trap->tseen) {
            feeltrap(trap); /* show on map once mtmp is out of the way */
            which = just_an(anbuf, what); /* "a " or "an " */
        }
        You("stop.  %s can't move out of %s%s.", YMonnam(mtmp), which, what);
        (void) handle_tip(TIP_UNTRAP_MON);
        didnt_move = TRUE;
    } else if (mtmp->mpeaceful
               && (!goodpos(u.ux0, u.uy0, mtmp, 0)
                   || t_at(u.ux0, u.uy0) != NULL
                   || mundisplaceable(mtmp))) {
        /* displacing peaceful into unsafe or trapped space, or trying to
           displace quest leader, Oracle, shk, priest, or vault guard */
        You("stop.  %s doesn't want to swap places.", YMonnam(mtmp));
        didnt_move = TRUE;
    } else {
        mtmp->mtrapped = 0;
        remove_monster(x, y);
        place_monster(mtmp, u.ux0, u.uy0);
        newsym(x, y);
        newsym(u.ux0, u.uy0);

        You("%s %s.", mtmp->mpeaceful ? "swap places with" : "frighten",
            x_monnam(mtmp,
                     mtmp->mtame ? ARTICLE_YOUR
                     : (!has_mgivenname(mtmp)
                        && !type_is_pname(mtmp->data)) ? ARTICLE_THE
                     : ARTICLE_NONE,
                     (mtmp->mpeaceful && !mtmp->mtame) ? "peaceful" : 0,
                     has_mgivenname(mtmp) ? SUPPRESS_SADDLE : 0, FALSE));

        /* check for displacing it into pools and traps */
        switch (minliquid(mtmp) ? Trap_Killed_Mon
                : mintrap(mtmp, NO_TRAP_FLAGS)) {
        case Trap_Effect_Finished:
            break;
        case Trap_Caught_Mon: /* trapped */
        case Trap_Moved_Mon: /* changed levels */
            /* there's already been a trap message, reinforce it */
            abuse_dog(mtmp);
            adjalign(-3);
            break;
        case Trap_Killed_Mon:
            /* drowned or died...
             * you killed your pet by direct action, so get experience
             * and possibly penalties;
             * we want the level gain message, if it happens, to occur
             * before the guilt message below
             */
            {
                /* minliquid() and mintrap() call mondead() rather than
                   killed() so we duplicate some of the latter here */
                int tmp, mndx;

                if (!u.uconduct.killer++)
                    livelog_printf(LL_CONDUCT, "killed for the first time");
                mndx = monsndx(mtmp->data);
                tmp = experience(mtmp, (int) svm.mvitals[mndx].died);
                more_experienced(tmp, 0);
                newexplevel(); /* will decide if you go up */
            }
            /* That's no way to treat a pet!  Your god gets angry.
             *
             * [This has always been pretty iffy.  Why does your
             * patron deity care at all, let alone enough to get mad?]
             */
            if (rn2(4)) {
                You_feel("guilty about losing your pet like this.");
                u.ugangr++;
                adjalign(-15);
            }
            break;
        default:
            impossible("that's strange, unknown mintrap result!");
            break;
        }
    }
    return !didnt_move;
}

/* force-fight (x,y) which doesn't have anything to fight */
staticfn boolean
domove_fight_empty(coordxy x, coordxy y)
{
    static const char unknown_obstacle[] = "an unknown obstacle";
    boolean off_edge = !isok(x, y);
    int glyph = !off_edge ? glyph_at(x, y) : GLYPH_UNEXPLORED;

    if (off_edge)
        x = 0, y = 1; /* for forcefight against the edge of the map; make
                       * sure 'bad' coordinates are within array bounds in
                       * case a bounds check gets overlooked; avoid <0,0>
                       * because m_at() might find a vault guard there */

    /* specifying 'F' with no monster wastes a turn */
    if (svc.context.forcefight
        /* remembered an 'I' && didn't use a move command */
        || (glyph_is_invisible(glyph) && !m_at(x, y)
            && !svc.context.nopick)) {
        struct obj *boulder = 0;
        boolean explo = (Upolyd && attacktype(gy.youmonst.data, AT_EXPL)),
                solid = (off_edge || (!accessible(x, y)
                                      || IS_FURNITURE(levl[x][y].typ)));
        char buf[BUFSZ];

        if (off_edge) {
            /* treat as if solid rock, even on planes' levels */
            Strcpy(buf, unknown_obstacle);
            goto futile;
        }

        if (!Underwater) {
            boulder = sobj_at(BOULDER, x, y);
            /* if a statue is displayed at the target location,
               player is attempting to attack it [and boulder
               handling below is suitable for handling that] */
            if (glyph_is_statue(glyph)
                || (Hallucination && glyph_is_monster(glyph)))
                boulder = sobj_at(STATUE, x, y);

            /* force fight at boulder/statue or wall/door while wielding
               pick:  start digging to break the boulder or wall */
            if (svc.context.forcefight
                /* can we dig? */
                && uwep && dig_typ(uwep, x, y)
                /* should we dig? */
                && !glyph_is_invisible(glyph) && !glyph_is_monster(glyph)) {
                (void) use_pick_axe2(uwep);
                return TRUE;
            }

            /* Fountainbane can dry up water walls and fountains by force attacking them */
            if (svc.context.forcefight && uwep && is_art(uwep, ART_FOUNTAINBANE)
                && (IS_WATERWALL(levl[x][y].typ) || IS_FOUNTAIN(levl[x][y].typ))) {
                boolean isfountain = IS_FOUNTAIN(levl[x][y].typ);
                levl[x][y].typ = ROOM, levl[x][y].flags = 0;
                newsym(x, y);
                pline("You strike the %s with Fountainbane... the %s dries up!", isfountain ? "fountain" : "wall of water",
                                  isfountain ? "fountain" : "wall");
                return TRUE;
            }
        }

        /* about to become known empty -- remove 'I' if present */
        unmap_object(x, y);
        if (boulder)
            map_object(boulder, TRUE);
        newsym(x, y);
        glyph = glyph_at(x, y); /* might have just changed */
        nhUse(glyph);

        if (boulder) {
            Strcpy(buf, ansimpleoname(boulder));
        } else if (Underwater && !is_pool(x, y)) {
            /* Underwater, targeting non-water; the map just shows blank
               because you don't see remembered terrain while underwater;
               although the hero can attack an adjacent monster this way,
               assume he can't reach out far enough to distinguish terrain */
            Sprintf(buf, "%s",
                    (Is_waterlevel(&u.uz) && levl[x][y].typ == AIR)
                         ? "an air bubble"
                         : "nothing");
        } else if (solid) {
            /* glyph might indicate unseen terrain if hero is blind;
               unlike searching, this won't reveal what that terrain is;
               3.7: used to say "solid rock" for STONE, but that made it be
               different from unmapped walls outside of rooms (and was wrong
               on arboreal levels) */
            if (levl[x][y].seenv || IS_STWALL(levl[x][y].typ)
                || levl[x][y].typ == SDOOR || levl[x][y].typ == SCORR) {
                glyph = back_to_glyph(x, y);
                Strcpy(buf, the(defsyms[glyph_to_cmap(glyph)].explanation));
            } else {
                Strcpy(buf, unknown_obstacle);
            }
            /* note: 'solid' is misleadingly named and catches pools
               of water and lava as well as rock and walls;
               3.7: furniture too */
        } else {
            Strcpy(buf, "thin air");
        }

 futile:
        You("%s%s %s.",
            !(boulder || solid) ? "" : !explo ? "harmlessly " : "futilely ",
            explo ? "explode at" : "attack", buf);

        nomul(0);
        if (explo) {
            struct attack *attk
                       = attacktype_fordmg(gy.youmonst.data, AT_EXPL, AD_ANY);

            /* no monster has been attacked so we have bypassed explum() */
            wake_nearto(u.ux, u.uy, 7 * 7); /* same radius as explum() */
            if (attk)
                explum((struct monst *) 0, attk);
            u.mh = -1; /* dead in the current form */
            rehumanize();
        }
        return TRUE;
    }
    return FALSE;
}

/* does the plane of air disturb movement? */
staticfn boolean
air_turbulence(void)
{
    if (Is_airlevel(&u.uz) && rn2(4) && !Levitation && !Flying) {
        switch (rn2(3)) {
        case 0:
            You("tumble in place.");
            exercise(A_DEX, FALSE);
            break;
        case 1:
            You_cant("control your movements very well.");
            break;
        case 2:
            pline("It's hard to walk in thin air.");
            exercise(A_DEX, TRUE);
            break;
        }
        return TRUE;
    }
    return FALSE;
}

/* does water disturb the movement? */
staticfn boolean
water_turbulence(coordxy *x, coordxy *y)
{
    if (u.uinwater) {
        int wtcap;
        int wtmod = (Swimming ? MOD_ENCUMBER : SLT_ENCUMBER);

        water_friction();
        if (!u.dx && !u.dy) {
            nomul(0);
            return TRUE;
        }
        *x = u.ux + u.dx;
        *y = u.uy + u.dy;

        /* are we trying to move out of water while carrying too much? */
        if (isok(*x, *y) && !is_pool(*x, *y) && !Is_waterlevel(&u.uz)
            && (wtcap = near_capacity()) > wtmod) {
            /* when escaping from drowning you need to be unencumbered
               in order to crawl out of water, but when not drowning,
               doing so while encumbered is feasible; if in an aquatic
               form, stressed or less is allowed; otherwise (magical
               breathing), only burdened is allowed */
            You("are carrying too much to climb out of the water.");
            nomul(0);
            return TRUE;
        }
    }
    return FALSE;
}

staticfn void
slippery_ice_fumbling(void)
{
    boolean on_ice = !Levitation && is_ice(u.ux, u.uy);
    struct monst *iceskater = u.usteed ? u.usteed : &gy.youmonst;

    if (on_ice) {
        if ((uarmf && objdescr_is(uarmf, "snow boots"))
            || resists_cold(iceskater) || Flying
            || is_floater(iceskater->data) || is_clinger(iceskater->data)
            || is_whirly(iceskater->data)) {
            on_ice = FALSE;
        } else if (!rn2(Cold_resistance ? 3 : 2)) {
            HFumbling |= FROMOUTSIDE;
            HFumbling &= ~TIMEOUT;
            HFumbling += 1; /* slip on next move */
        }
    }
    if (!on_ice && (HFumbling & FROMOUTSIDE))
        HFumbling &= ~FROMOUTSIDE;
}

boolean
u_maybe_impaired(void)
{
    return (Stunned || (Confusion && !rn2(5)));
}

/* change movement dir if impaired. return TRUE if can't move */
staticfn boolean
impaired_movement(coordxy *x, coordxy *y)
{
    if (u_maybe_impaired()) {
        int tries = 0;

        do {
            if (tries++ > 50) {
                nomul(0);
                return TRUE;
            }
            confdir(TRUE);
            *x = u.ux + u.dx;
            *y = u.uy + u.dy;
        } while (!isok(*x, *y) || bad_rock(gy.youmonst.data, *x, *y));
    }
    return FALSE;
}

staticfn boolean
avoid_moving_on_trap(coordxy x, coordxy y, boolean msg)
{
    struct trap *trap;

    if ((trap = t_at(x, y)) && trap->tseen) {
        if (msg && flags.mention_walls) {
            set_msg_xy(x, y);
            You("stop in front of %s.",
                an(trapname(trap->ttyp, FALSE)));
        }
        return TRUE;
    }
    return FALSE;
}

staticfn boolean
avoid_moving_on_liquid(
    coordxy x, coordxy y,
    boolean msg)
{
    boolean in_air = (Levitation || Flying);

    /* don't stop if you're not on a transition between terrain types... */
    if ((levl[x][y].typ == levl[u.ux][u.uy].typ
         /* or you are using shift-dir running and the transition isn't
            dangerous... */
         || (svc.context.run < 2 && (!is_lava(x, y) || in_air))
         || svc.context.travel)
        /* and you know you won't fall in */
        && (in_air || Known_lwalking || (is_pool(x, y) && Known_wwalking))
        && !(IS_WATERWALL(levl[x][y].typ) || levl[x][y].typ == LAVAWALL)) {
        /* XXX: should send 'is_clinger(gy.youmonst.data)' here once clinging
           polyforms are allowed to move over water */
        return FALSE; /* liquid is safe to traverse */
    } else if (is_pool_or_lava(x, y) && levl[x][y].seenv) {
        if (msg && flags.mention_walls) {
            set_msg_xy(x, y);
            You("stop at the edge of the %s.",
                hliquid(is_pool(x,y) ? "water" : "lava"));
        }
        return TRUE;
    }
    return FALSE;
}

/* when running/rushing, avoid stepping on a known trap or pool of liquid.
   returns TRUE if avoided. */
staticfn boolean
avoid_running_into_trap_or_liquid(coordxy x, coordxy y)
{
    boolean would_stop = (svc.context.run >= 2);
    if (!svc.context.run)
        return FALSE;

    if (avoid_moving_on_trap(x,y, would_stop)
        || (Blind && avoid_moving_on_liquid(x,y, would_stop))) {
        nomul(0);
        if (would_stop)
            svc.context.move = 0;
        return would_stop;
    }
    return FALSE;
}

/* if paranoid_confirm:Trap is enabled, check whether the next step forward
   needs player confirmation due to visible region or discovered trap;
   result: True => stop moving, False => proceed */
staticfn boolean
avoid_trap_andor_region(coordxy x, coordxy y)
{
    char qbuf[QBUFSZ];
    NhRegion *newreg, *oldreg;
    struct trap *trap = NULL;

    /* treat entering a visible gas cloud region like entering a trap;
       there could be a known trap as well as a region at the target spot;
       if so, ask about entring the region first; even though this could
       lead to two consecutive confirmation prompts, the situation seems
       to be too uncommon to warrant a separate case with combined
       trap+region confirmation */
    if (ParanoidTrap && !Blind && !Stunned && !Confusion && !Hallucination
        /* skip if player used 'm' prefix or is moving recklessly */
        && (!svc.context.nopick || svc.context.run)
        /* check for region(s) */
        && (newreg = visible_region_at(x, y)) != 0
        && ((oldreg = visible_region_at(u.ux, u.uy)) == 0
            /* if moving from one region into another, only ask for
               confirmation if the one potentially being entered inflicts
               damage (poison gas) and the one being exited doesn't (vapor) */
            || (reg_damg(newreg) > 0 && reg_damg(oldreg) == 0))
        /* check whether attempted move will be viable */
        && test_move(u.ux, u.uy, u.dx, u.dy, TEST_MOVE)
        /* we don't override confirmation for poison resistance since the
           region also hinders hero's vision even if/when no damage is done */
    ) {
        Snprintf(qbuf, sizeof qbuf, "%s into that %s cloud?",
                 u_locomotion("step"),
                 (reg_damg(newreg) > 0) ? "poison gas" : "vapor");
        if (!paranoid_query(ParanoidConfirm, upstart(qbuf))) {
            nomul(0);
            svc.context.move = 0;
            return TRUE;
        }
    }

    /* maybe ask player for confirmation before walking into known trap */
    if (ParanoidTrap && !Stunned && !Confusion
        /* skip if player used 'm' prefix or is moving recklessly */
        && (!svc.context.nopick || svc.context.run)
        /* check for discovered trap */
        && (trap = t_at(x, y)) != 0 && trap->tseen
        /* check whether attempted move will be viable */
        && test_move(u.ux, u.uy, u.dx, u.dy, TEST_MOVE)
        /* override confirmation if the trap is harmless to the hero */
        && (immune_to_trap(&gy.youmonst, trap->ttyp) != TRAP_CLEARLY_IMMUNE
            /* Hallucination: all traps still show as ^, but the
               hero can't tell what they are, so treat as dangerous */
            || Hallucination)) {
        int traptype = (Hallucination ? rnd(TRAPNUM - 1) : (int) trap->ttyp);
        boolean into = into_vs_onto(traptype);

        Snprintf(qbuf, sizeof qbuf, "Really %s %s that %s?",
                 u_locomotion("step"), into ? "into" : "onto",
                 defsyms[trap_to_defsym(traptype)].explanation);
        /* handled like paranoid_confirm:pray; when paranoid_confirm:trap
           isn't set, don't ask at all but if it is set (checked above),
           ask via y/n if parnoid_confirm:confirm isn't also set or via
           yes/no if it is */
        if (!paranoid_query(ParanoidConfirm, qbuf)) {
            nomul(0);
            svc.context.move = 0;
            return TRUE;
        }
    }
    return FALSE;
}

/* trying to move out-of-bounds? */
staticfn boolean
move_out_of_bounds(coordxy x, coordxy y)
{
    if (!isok(x, y)) {
        if (svc.context.forcefight)
            return domove_fight_empty(x, y);

        if (flags.mention_walls) {
            coordxy dx = u.dx, dy = u.dy;

            if (dx && dy) { /* diagonal */
                /* only as far as possible diagonally if in very
                   corner; otherwise just report whichever of the
                   cardinal directions has reached its limit */
                if (isok(x, u.uy))
                    dx = 0;
                else if (isok(u.ux, y))
                    dy = 0;
            }
            You("have already gone as far %s as possible.",
                directionname(xytod(dx, dy)));
        }
        nomul(0);
        svc.context.move = 0;
        return TRUE;
    }
    return FALSE;
}

/* carrying too much to be able to move? */
staticfn boolean
carrying_too_much(void)
{
    int wtcap;

    if (((wtcap = near_capacity()) >= OVERLOADED
         || (wtcap > SLT_ENCUMBER
             && (Upolyd ? (u.mh < 5 && u.mh != u.mhmax)
                        : (u.uhp < 10 && u.uhp != u.uhpmax))))
        && !Is_airlevel(&u.uz)) {
        if (wtcap < OVERLOADED) {
            You("don't have enough stamina to move.");
            exercise(A_CON, FALSE);
        } else
            You("collapse under your load.");
        nomul(0);
        return TRUE;
    }
    return FALSE;
}

/* try to pull free from sticking monster, or you release a monster
   you're sticking to. returns TRUE if you lose your movement. */
staticfn boolean
escape_from_sticky_mon(coordxy x, coordxy y)
{
    if (u.ustuck && (x != u.ustuck->mx || y != u.ustuck->my)) {
        struct monst *mtmp;

        if (!m_next2u(u.ustuck)) {
            /* perhaps it fled (or was teleported or ... ) */
            set_ustuck((struct monst *) 0);
        } else if (sticks(gy.youmonst.data)) {
            /* When polymorphed into a sticking monster,
             * u.ustuck means it's stuck to you, not you to it.
             */
            mtmp = u.ustuck;
            set_ustuck((struct monst *) 0);
            You("release %s.", y_monnam(mtmp));
        } else {
            /* If holder is asleep or paralyzed:
             *      37.5% chance of getting away,
             *      12.5% chance of waking/releasing it;
             * otherwise:
             *       7.5% chance of getting away.
             * [strength ought to be a factor]
             * If holder is tame and there is no conflict,
             * guaranteed escape.
             */
            switch (rn2(!u.ustuck->mcanmove ? 8 : 40)) {
            case 3:
                if (!u.ustuck->mcanmove) {
                    /* it's free to move on next turn */
                    u.ustuck->mfrozen = 1;
                    u.ustuck->msleeping = 0;
                }
                FALLTHROUGH;
                /*FALLTHRU*/
            default:
                if (Conflict || u.ustuck->mconf || !u.ustuck->mtame) {
                    You("cannot escape from %s!", y_monnam(u.ustuck));
                    nomul(0);
                    return TRUE;
                }
                FALLTHROUGH;
                /*FALLTHRU*/
            case 0:
            case 1:
            case 2:
                mtmp = u.ustuck;
                set_ustuck((struct monst *) 0);
                You("pull free from %s.", y_monnam(mtmp));
                break;
            }
        }
    }
    return FALSE;
}

void
domove(void)
{
        coordxy ux1 = u.ux, uy1 = u.uy;

        gd.domove_succeeded = 0L;
        domove_core();
        /* gd.domove_succeeded is available to make assessments now */
        if ((gd.domove_succeeded & (DOMOVE_RUSH | DOMOVE_WALK)) != 0) {
            maybe_smudge_engr(ux1, uy1, u.ux, u.uy);
            maybe_adjust_hero_bubble();
        }
        gd.domove_attempting = 0L;

        gk.kickedloc.x = 0, gk.kickedloc.y = 0;
}

staticfn void
domove_core(void)
{
    struct monst *mtmp;
    struct rm *tmpr;
    coordxy x, y;
    int glyph;
    coordxy chainx = 0, chainy = 0,
            ballx = 0, bally = 0;       /* ball&chain new positions */
    int bc_control = 0;                 /* control for ball&chain */
    boolean cause_delay = FALSE,        /* dragging ball will skip a move */
            displaceu = FALSE;          /* involuntary swap */

    if (svc.context.travel) {
        if (!findtravelpath(TRAVP_TRAVEL))
            (void) findtravelpath(TRAVP_GUESS);
        svc.context.travel1 = 0;
    }

    if (carrying_too_much())
        return;

    if (u.uswallow) {
        u.dx = u.dy = 0;
        x = u.ustuck->mx, y = u.ustuck->my;
        u_on_newpos(x, y); /* set u.ux,uy and handle CLIPPING */
        mtmp = u.ustuck;
    } else {
        if (air_turbulence())
            return;

        /* check slippery ice */
        slippery_ice_fumbling();

        x = u.ux + u.dx;
        y = u.uy + u.dy;
        if (impaired_movement(&x, &y))
            return;

        /* turbulence might alter your actual destination */
        if (water_turbulence(&x, &y))
            return;

        if (move_out_of_bounds(x, y))
            return;

        if (avoid_running_into_trap_or_liquid(x, y))
            return;

        if (escape_from_sticky_mon(x, y))
            return;

        mtmp = m_at(x, y);
        if (mtmp && !is_safemon(mtmp)) {
            /* Don't attack if you're running, and can see it */
            /* It's fine to displace pets, though */
            /* We should never get here if forcefight */
            if (svc.context.run && ((!Blind && mon_visible(mtmp)
                                 && ((M_AP_TYPE(mtmp) != M_AP_FURNITURE
                                      && M_AP_TYPE(mtmp) != M_AP_OBJECT)
                                     || Protection_from_shape_changers))
                                || sensemon(mtmp))) {
                nomul(0);
                svc.context.move = 0;
                return;
            }
        }
    }

    u.ux0 = u.ux;
    u.uy0 = u.uy;
    gb.bhitpos.x = x;
    gb.bhitpos.y = y;
    tmpr = &levl[x][y];
    glyph = glyph_at(x, y);

    if (mtmp) {
        /* don't stop travel when displacing pets; if the
           displace fails for some reason, do_attack() in uhitm.c
           will stop travel rather than domove */
        if (!is_safemon(mtmp) || svc.context.forcefight)
            nomul(0);

        if (domove_bump_mon(mtmp, glyph))
            return;

        /* attack monster */
        if (domove_attackmon_at(mtmp, x, y, &displaceu))
            return;
    }

    if (!displaceu) {

        if (domove_fight_ironbars(x, y))
            return;

        if (domove_fight_web(x, y))
            return;

        if (domove_fight_empty(x, y))
            return;

        (void) unmap_invisible(x, y);
        /* not attacking an animal, so we try to move */
        if ((u.dx || u.dy) && u.usteed && stucksteed(FALSE)) {
            nomul(0);
            return;
        }

        if (u_rooted())
            return;

        /* handling for paranoid_confirm:Trap which doubles as
           paranoid_confirm:Region */
        if (ParanoidTrap) {
            if (avoid_trap_andor_region(x, y))
                return;
        }

        if (u.utrap) { /* when u.utrap is True, displaceu is False */
            boolean moved = trapmove(x, y, (struct trap *) NULL);

            if (!u.utrap) {
                disp.botl = TRUE;
                reset_utrap(TRUE); /* might resume levitation or flight */
            }
            /* might not have escaped, or did escape but remain in the same
               spot */
            if (!moved)
                return;
        }

        if (!test_move(u.ux, u.uy, x - u.ux, y - u.uy, DO_MOVE)) {
            if (!svc.context.door_opened) {
                svc.context.move = 0;
                nomul(0);
            }
            return;
        }

        /* Is it dangerous to swim in water or lava? */
        if (swim_move_danger(x, y)) {
            svc.context.move = 0;
            nomul(0);
            return;
        }

    } /* !dislacedu */

    /* Move ball and chain.  */
    if (Punished)
        if (!drag_ball(x, y, &bc_control, &ballx, &bally, &chainx, &chainy,
                       &cause_delay, TRUE))
            return;

    /* Check regions entering/leaving */
    if (!in_out_region(x, y))
        return;

    mtmp = m_at(x, y);
    /* mtmp can be null at this point */

    /* tentatively move the hero plus steed; leave CLIPPING til later */
    u.ux += u.dx;
    u.uy += u.dy;

    m_postmove_effect(&gy.youmonst);

    if (u.usteed) {
        u.usteed->mx = u.ux;
        u.usteed->my = u.uy;
        /* [if move attempt ends up being blocked, should training count?] */
        exercise_steed(); /* train riding skill */
    }

    if (mtmp) {
        if (displaceu) {
            boolean noticed_it = (canspotmon(mtmp)
                                  || glyph_is_invisible(glyph)
                                  || glyph_is_warning(glyph));

            remove_monster(u.ux, u.uy);
            place_monster(mtmp, u.ux0, u.uy0);
            newsym(u.ux, u.uy);
            newsym(u.ux0, u.uy0);
            /* monst still knows where hero is */
            mtmp->mux = u.ux, mtmp->muy = u.uy;

            pline("%s swaps places with you...",
                  !noticed_it ? Something : YMonnam(mtmp));
            if (!canspotmon(mtmp))
                map_invisible(u.ux0, u.uy0);
            /* monster chose to swap places; hero doesn't get any credit
               or blame if something bad happens to it */
            svc.context.mon_moving = 1;
            if (!minliquid(mtmp))
                (void) mintrap(mtmp, NO_TRAP_FLAGS);
            svc.context.mon_moving = 0;

        /*
         * If safepet at destination then move the pet to the hero's
         * previous location using the same conditions as in do_attack().
         * there are special extenuating circumstances:
         * (1) if the pet dies then your god angers,
         * (2) if the pet gets trapped then your god may disapprove.
         *
         * Ceiling-hiding pets are skipped by this section of code, to
         * be caught by the normal falling-monster code.
         */
        } else if (is_safemon(mtmp)
                   && !(is_hider(mtmp->data) && mtmp->mundetected)) {
            if (!domove_swap_with_pet(mtmp, x, y)) {
                u.ux = u.ux0, u.uy = u.uy0; /* didn't move after all */
                /* could skip this since we're about to call u_on_newpos() */
                if (u.usteed)
                    u.usteed->mx = u.ux, u.usteed->my = u.uy;
            }
        }
    }  /* mtmp != NULL */

    /* tentative move above didn't handle CLIPPING, in case there was a
       monster in the way and the move attempt ended up being blocked;
       do a full re-position now, possibly back to where hero started */
    u_on_newpos(u.ux, u.uy);

    reset_occupations();
    if (svc.context.run) {
        if (svc.context.run < 8)
            if (IS_DOOR(tmpr->typ) || IS_OBSTRUCTED(tmpr->typ)
                || IS_FURNITURE(tmpr->typ))
                nomul(0);
    }

    /* your tread on the ground may disturb the slumber of nearby zombies */
    if (!Levitation && !Flying && !Stealth
        && gy.youmonst.data->cwt >= (WT_ELF / 2))
        disturb_buried_zombies(u.ux, u.uy);

    if (hides_under(gy.youmonst.data) || gy.youmonst.data->mlet == S_EEL
        || u.dx || u.dy)
        (void) hideunder(&gy.youmonst);

    /*
     * Mimics (or whatever) become noticeable if they move and are
     * imitating something that doesn't move.  We could extend this
     * to non-moving monsters...
     */
    if ((u.dx || u.dy) && (U_AP_TYPE == M_AP_OBJECT
                           || U_AP_TYPE == M_AP_FURNITURE))
        gy.youmonst.m_ap_type = M_AP_NOTHING;

    check_leash(u.ux0, u.uy0);

    if (u.ux0 != u.ux || u.uy0 != u.uy) {
        /* let caller know so that an evaluation may take place */
        gd.domove_succeeded |=
                (gd.domove_attempting & (DOMOVE_RUSH | DOMOVE_WALK));
        u.umoved = TRUE;
        /* Clean old position -- vision_recalc() will print our new one. */
        newsym(u.ux0, u.uy0);
        /* Since the hero has moved, adjust what can be seen/unseen. */
        vision_recalc(1); /* Do the work now in the recover time. */
        invocation_message();
    }

    if (Punished) /* put back ball and chain */
        move_bc(0, bc_control, ballx, bally, chainx, chainy);

    if (u.umoved)
        spoteffects(TRUE);

    /* delay next move because of ball dragging */
    /* must come after we finished picking up, in spoteffects() */
    if (cause_delay) {
        nomul(-2);
        gm.multi_reason = "dragging an iron ball";
        gn.nomovemsg = "";
    }

    runmode_delay_output();
}

/* delay output based on value of runmode,
   if hero is running or doing a multi-turn action */
void
runmode_delay_output(void)
{
    if ((svc.context.run || gm.multi) && flags.runmode != RUN_TPORT) {
        /* for tport mode, don't display anything until we've stopped;
           for normal (leap) mode, update display every 7th step
           (relative to turn counter; ought to be to start of running);
           for walk and crawl (visual debugging) modes, update the
           display after every step */
        if (flags.runmode != RUN_LEAP || !(svm.moves % 7L)) {
            /* moveloop() suppresses time_botl when running */
            disp.time_botl = flags.time;
            curs_on_u();
            nh_delay_output();
            if (flags.runmode == RUN_CRAWL) {
                nh_delay_output();
                nh_delay_output();
                nh_delay_output();
                nh_delay_output();
            }
        }
    }
}

staticfn void
maybe_smudge_engr(coordxy x1, coordxy y1, coordxy x2, coordxy y2)
{
    struct engr *ep;

    if (can_reach_floor(TRUE)) {
        if ((ep = engr_at(x1, y1)) && ep->engr_type != HEADSTONE)
            wipe_engr_at(x1, y1, rnd(5), FALSE);
        if ((x2 != x1 || y2 != y1)
                && (ep = engr_at(x2, y2)) && ep->engr_type != HEADSTONE)
            wipe_engr_at(x2, y2, rnd(5), FALSE);
    }
}

/* HP loss or passing out from overexerting yourself */
void
overexert_hp(void)
{
    int *hp = (!Upolyd ? &u.uhp : &u.mh);

    if (*hp > 1) {
        *hp -= 1;
        disp.botl = TRUE;
    } else {
        You("pass out from exertion!");
        exercise(A_CON, FALSE);
        fall_asleep(-10, FALSE);
    }
}

/* combat increases metabolism */
boolean
overexertion(void)
{
    /* this used to be part of domove() when moving to a monster's
       position, but is now called by do_attack() so that it doesn't
       execute if you decline to attack a peaceful monster */
    gethungry();
    if ((svm.moves % 3L) != 0L && near_capacity() >= HVY_ENCUMBER) {
        overexert_hp();
    }
    return (boolean) (gm.multi < 0); /* might have fainted (forced to sleep) */
}

void
invocation_message(void)
{
    /* a special clue-msg when on the Invocation position */
    if (invocation_pos(u.ux, u.uy) && !On_stairs(u.ux, u.uy)) {
        char buf[BUFSZ];
        struct obj *otmp = carrying(CANDELABRUM_OF_INVOCATION);

        nomul(0); /* stop running or travelling */
        if (u.usteed)
            Sprintf(buf, "beneath %s", y_monnam(u.usteed));
        else if (Levitation || Flying)
            Strcpy(buf, "beneath you");
        else
            Sprintf(buf, "under your %s", makeplural(body_part(FOOT)));

        You_feel("a strange vibration %s.", buf);
        u.uevent.uvibrated = 1;
        if (otmp && otmp->spe == 7 && otmp->lamplit)
            pline("%s %s!", The(xname(otmp)),
                  Blind ? "throbs palpably" : "glows with a strange light");
    }
}

/* moving onto different terrain;
   might be going into solid rock, inhibiting levitation or flight,
   or coming back out of such, reinstating levitation/flying */
void
switch_terrain(void)
{
    struct rm *lev = &levl[u.ux][u.uy];
    boolean blocklev = (IS_OBSTRUCTED(lev->typ) || closed_door(u.ux, u.uy)
                        || IS_WATERWALL(lev->typ)
                        || lev->typ == LAVAWALL),
            was_levitating = !!Levitation, was_flying = !!Flying;

    if (blocklev) {
        /* called from spoteffects(), stop levitating but skip float_down() */
        if (Levitation)
            You_cant("levitate in here.");
        BLevitation |= FROMOUTSIDE;
    } else if (BLevitation) {
        BLevitation &= ~FROMOUTSIDE;
        /* we're probably levitating now; if not, we must be chained
           to a buried iron ball so get float_up() feedback for that */
        if (Levitation || BLevitation)
            float_up();
    }
    /* the same terrain that blocks levitation also blocks flight */
    if (blocklev) {
        if (Flying)
            You_cant("fly in here.");
        BFlying |= FROMOUTSIDE;
    } else if (BFlying) {
        BFlying &= ~FROMOUTSIDE;
        float_vs_flight(); /* maybe toggle (BFlying & I_SPECIAL) */
        /* [minor bug: we don't know whether this is beginning flight or
           resuming it; that could be tracked so that this message could
           be adjusted to "resume flying", but isn't worth the effort...] */
        if (Flying)
            You("start flying.");
    }
    if ((!!Levitation ^ was_levitating) || (!!Flying ^ was_flying))
        disp.botl = TRUE; /* update Lev/Fly status condition */
}

/* set or clear u.uinwater */
void
set_uinwater(int in_out)
{
    u.uinwater = in_out ? 1 : 0;
}

/* extracted from spoteffects; called by spoteffects to check for entering or
   leaving a pool of water/lava, and by moveloop to check for staying on one;
   returns true to skip rest of spoteffects */
boolean
pooleffects(
    boolean newspot) /* true if called by spoteffects */
{
    /* check for leaving water */
    if (u.uinwater) {
        boolean still_inwater = FALSE; /* assume we're getting out */

        if (!is_pool(u.ux, u.uy)) {
            if (Is_waterlevel(&u.uz)) {
                You("pop into an air bubble.");
                iflags.last_msg = PLNMSG_BACK_ON_GROUND;
            } else if (is_lava(u.ux, u.uy)) {
                You("leave the %s...", hliquid("water")); /* oops! */
            } else {
                back_on_ground(FALSE);
            }
        } else if (Is_waterlevel(&u.uz)) {
            still_inwater = TRUE;
        } else if (Levitation) {
            You("pop out of the %s like a cork!", hliquid("water"));
        } else if (Flying) {
            You("fly out of the %s.", hliquid("water"));
        } else if (Wwalking) {
            You("slowly rise above the surface.");
        } else {
            still_inwater = TRUE;
        }
        if (!still_inwater) {
            boolean was_underwater = (Underwater && !Is_waterlevel(&u.uz));

            set_uinwater(0); /* u.uinwater = 0; leave the water */
            if (was_underwater) { /* restore vision */
                docrt();
                gv.vision_full_recalc = 1;
            }
        }
    }

    /* check for entering water or lava */
    if (!u.ustuck && !Levitation && !Flying && is_pool_or_lava(u.ux, u.uy)) {
        if (u.usteed && !grounded(u.usteed->data)) {
            /* floating or clinging steed keeps hero safe (is_flyer() test
               is redundant; it can't be true since Flying yielded false) */
            return FALSE;
        } else if (u.usteed) {
            /* steed enters pool */
            dismount_steed(Underwater ? DISMOUNT_FELL : DISMOUNT_GENERIC);
            /* dismount_steed() -> float_down() -> pickup()
               (float_down doesn't do autopickup on Air or Water) */
            if (Is_airlevel(&u.uz) || Is_waterlevel(&u.uz))
                return FALSE;
            /* even if we actually end up at same location, float_down()
               has already done trap and pickup actions of spoteffects() */
            if (newspot)
                check_special_room(FALSE); /* spoteffects */
            return TRUE;
        }
        /* not mounted */

        /* if hiding on ceiling then don't automatically enter pool */
        if (Upolyd && ceiling_hider(&mons[u.umonnum]) && u.uundetected)
            return FALSE;

        /* drown(),lava_effects() return true if hero changes
           location while surviving the problem */
        if (is_lava(u.ux, u.uy)) {
            if (lava_effects())
                return TRUE;
        } else if ((!Wwalking || is_waterwall(u.ux,u.uy))
                   && (newspot || !u.uinwater
                       || !(Swimming || Amphibious || Breathless))) {
            if (drown())
                return TRUE;
        }
    }
    return FALSE;
}

void
spoteffects(boolean pick)
{
    static int inspoteffects = 0;
    static coord spotloc;
    static int spotterrain;
    static struct trap *spottrap = (struct trap *) 0;
    static unsigned spottraptyp = NO_TRAP;

    struct monst *mtmp;
    struct trap *trap = t_at(u.ux, u.uy);
    int trapflag = iflags.failing_untrap ? FAILEDUNTRAP : 0;

    /* prevent recursion from affecting the hero all over again
       [hero poly'd to iron golem enters water here, drown() inflicts
       damage that triggers rehumanize() which calls spoteffects()...] */
    if (inspoteffects && u_at(spotloc.x, spotloc.y)
        /* except when reason is transformed terrain (ice -> water) */
        && spotterrain == levl[u.ux][u.uy].typ
        /* or transformed trap (land mine -> pit) */
        && (!spottrap || !trap || trap->ttyp == spottraptyp))
        return;
    /* when float_down() puts hero into lava and she teleports out,
       defer spoteffects() until after "you are back on solid <surface>" */
    if (iflags.in_lava_effects)
        return;

    ++inspoteffects;
    spotterrain = levl[u.ux][u.uy].typ;
    spotloc.x = u.ux, spotloc.y = u.uy;

    /* moving onto different terrain might cause Lev or Fly to toggle */
    if (spotterrain != levl[u.ux0][u.uy0].typ || !on_level(&u.uz, &u.uz0))
        switch_terrain();

    if (pooleffects(TRUE))
        goto spotdone;

    check_special_room(FALSE);
    if (IS_SINK(levl[u.ux][u.uy].typ) && Levitation)
        dosinkfall();
    if (!gi.in_steed_dismounting) { /* if dismounting, check again later */
        boolean pit;

        /* if levitation is due to time out at the end of this
           turn, allowing it to do so could give the perception
           that a trap here is being triggered twice, so adjust
           the timeout to prevent that */
        if (trap && (HLevitation & TIMEOUT) == 1L
            && !(ELevitation || (HLevitation & ~(I_SPECIAL | TIMEOUT)))) {
            if (rn2(2)) { /* defer timeout */
                incr_itimeout(&HLevitation, 1L);
            } else { /* timeout early */
                if (float_down(I_SPECIAL | TIMEOUT, 0L)) {
                    /* levitation has ended; we've already triggered
                       any trap and [usually] performed autopickup */
                    trap = 0;
                    pick = FALSE;
                }
            }
        }
        /*
         * If not a pit, pickup before triggering trap.
         * If pit, trigger trap before pickup.
         */
        pit = (trap && is_pit(trap->ttyp));
        if (pick && !pit)
            (void) pickup(1);

        if (trap) {
            /*
             * dotrap on a fire trap calls melt_ice() which triggers
             * spoteffects() (again) which can trigger the same fire
             * trap (again).  Use static spottrap to prevent that.
             * We track spottraptyp because some traps morph (landmine
             * to pit) and any new trap type should get triggered.
             */
            if (!spottrap || spottraptyp != trap->ttyp) {
                spottrap = trap;
                spottraptyp = trap->ttyp;
                dotrap(trap, trapflag); /* fall into arrow trap, etc. */
                spottrap = (struct trap *) 0;
                spottraptyp = NO_TRAP;
            }
        }
        if (pick && pit)
            (void) pickup(1);
    }
    /* Warning alerts you to ice danger */
    if (Warning && is_ice(u.ux, u.uy)) {
        static const char *const icewarnings[] = {
            "The ice seems very soft and slushy.",
            "You feel the ice shift beneath you!",
            "The ice, is gonna BREAK!", /* The Dead Zone */
        };
        long time_left = spot_time_left(u.ux, u.uy, MELT_ICE_AWAY);

        if (time_left && time_left < 15L)
            pline("%s", icewarnings[(time_left < 5L) ? 2
                                    : (time_left < 10L) ? 1
                                      : 0]);
    }

    if ((mtmp = m_at(u.ux, u.uy)) != 0 && !u.uswallow) {
        mtmp->mundetected = mtmp->msleeping = 0;
        switch (mtmp->data->mlet) {
        case S_PIERCER:
            pline("%s suddenly drops from the %s!", Amonnam(mtmp),
                  ceiling(u.ux, u.uy));
            if (mtmp->mtame) { /* jumps to greet you, not attack */
                ;
            } else if (hard_helmet(uarmh)) {
                pline("Its blow glances off your %s.",
                      helm_simple_name(uarmh));
            } else if (u.uac + 3 <= rnd(20)) {
                You("are almost hit by %s!",
                    x_monnam(mtmp, ARTICLE_A, "falling", 0, TRUE));
            } else {
                int dmg;

                You("are hit by %s!",
                    x_monnam(mtmp, ARTICLE_A, "falling", 0, TRUE));
                dmg = d(4, 6);
                if (Half_physical_damage)
                    dmg = (dmg + 1) / 2;
                mdamageu(mtmp, dmg);
            }
            break;
        default: /* monster surprises you. */
            if (mtmp->mtame)
                pline("%s jumps near you from the %s.", Amonnam(mtmp),
                      ceiling(u.ux, u.uy));
            else if (mtmp->mpeaceful) {
                You("surprise %s!",
                    Blind && !sensemon(mtmp) ? something : a_monnam(mtmp));
                mtmp->mpeaceful = 0;
            } else
                pline("%s attacks you by surprise!", Amonnam(mtmp));
            break;
        }
        mnexto(mtmp, RLOC_NOMSG); /* have to move the monster */
    }
 spotdone:
    if (!--inspoteffects) {
        spotterrain = STONE; /* 0 */
        spotloc.x = spotloc.y = 0;
    }
    return;
}

/* returns first matching monster */
staticfn struct monst *
monstinroom(struct permonst *mdat, int roomno)
{
    struct monst *mtmp;

    for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
        if (DEADMONSTER(mtmp))
            continue;
        if (mtmp->data == mdat
            && strchr(in_rooms(mtmp->mx, mtmp->my, 0), roomno + ROOMOFFSET))
            return mtmp;
    }
    return (struct monst *) 0;
}

/* check whether room contains a particular type of furniture */
staticfn boolean
furniture_present(int furniture, int roomno)
{
    int x, y, lx, ly, hx, hy;
    struct mkroom *sroom = &svr.rooms[roomno];

    ly = sroom->ly, hy = sroom->hy;
    lx = sroom->lx; hx = sroom->hx;
    /* the inside_room() check handles irregularly shaped rooms */
    for (y = ly; y <= hy; ++y)
        for (x = lx; x <= hx; ++x)
            if (levl[x][y].typ == furniture && inside_room(sroom, x, y))
                return TRUE;
    return FALSE;
}

char *
in_rooms(coordxy x, coordxy y, int typewanted)
{
    static char buf[5];
    char rno, *ptr = &buf[4];
    int typefound, min_x, min_y, max_x, max_y_offset, step;
    struct rm *lev;

#define goodtype(rno) \
    (!typewanted                                                   \
     || (typefound = svr.rooms[rno - ROOMOFFSET].rtype) == typewanted  \
     || (typewanted == SHOPBASE && typefound > SHOPBASE))

    switch (rno = levl[x][y].roomno) {
    case NO_ROOM:
        return ptr;
    case SHARED:
        step = 2;
        break;
    case SHARED_PLUS:
        step = 1;
        break;
    default: /* i.e. a regular room # */
        if (goodtype(rno))
            *(--ptr) = rno;
        return ptr;
    }

    min_x = x - 1;
    max_x = x + 1;
    if (x < 1)
        min_x += step;
    else if (x >= COLNO)
        max_x -= step;

    min_y = y - 1;
    max_y_offset = 2;
    if (min_y < 0) {
        min_y += step;
        max_y_offset -= step;
    } else if ((min_y + max_y_offset) >= ROWNO)
        max_y_offset -= step;

    for (x = min_x; x <= max_x; x += step) {
        lev = &levl[x][min_y];
        y = 0;
        if ((rno = lev[y].roomno) >= ROOMOFFSET && !strchr(ptr, rno)
            && goodtype(rno))
            *(--ptr) = rno;
        y += step;
        if (y > max_y_offset)
            continue;
        if ((rno = lev[y].roomno) >= ROOMOFFSET && !strchr(ptr, rno)
            && goodtype(rno))
            *(--ptr) = rno;
        y += step;
        if (y > max_y_offset)
            continue;
        if ((rno = lev[y].roomno) >= ROOMOFFSET && !strchr(ptr, rno)
            && goodtype(rno))
            *(--ptr) = rno;
    }
    return ptr;
}

/* is (x,y) in a town? */
boolean
in_town(coordxy x, coordxy y)
{
    struct mkroom *sroom;
    boolean has_subrooms = FALSE;

    if (!svl.level.flags.has_town)
        return FALSE;

    /*
     * See if (x,y) is in a room with subrooms, if so, assume it's the
     * town.  If there are no subrooms, the whole level is in town.
     */
    for (sroom = &svr.rooms[0]; sroom->hx > 0; sroom++) {
        if (sroom->nsubrooms > 0) {
            has_subrooms = TRUE;
            if (inside_room(sroom, x, y))
                return TRUE;
        }
    }

    return !has_subrooms;
}

staticfn void
move_update(boolean newlev)
{
    char c, *ptr1, *ptr2, *ptr3, *ptr4;

    Strcpy(u.urooms0, u.urooms);
    Strcpy(u.ushops0, u.ushops);
    if (newlev) {
        (void) memset(u.urooms, '\0', sizeof u.urooms);
        (void) memset(u.uentered, '\0', sizeof u.uentered);
        (void) memset(u.ushops, '\0', sizeof u.ushops);
        (void) memset(u.ushops_entered, '\0', sizeof u.ushops_entered);
        Strcpy(u.ushops_left, u.ushops0);
        return;
    }
    Strcpy(u.urooms, in_rooms(u.ux, u.uy, 0));

    for (ptr1 = u.urooms, ptr2 = u.uentered,
         ptr3 = u.ushops, ptr4 = u.ushops_entered; *ptr1; ptr1++) {
        c = *ptr1;
        if (!strchr(u.urooms0, c))
            *ptr2++ = c;
        if (IS_SHOP(c - ROOMOFFSET)) {
            *ptr3++ = c;
            if (!strchr(u.ushops0, c))
                *ptr4++ = c;
        }
    }
    *ptr2 = '\0', *ptr3 = '\0', *ptr4 = '\0';

    /* filter u.ushops0 -> u.ushops_left */
    for (ptr1 = u.ushops0, ptr2 = u.ushops_left; *ptr1; ptr1++)
        if (!strchr(u.ushops, *ptr1))
            *ptr2++ = *ptr1;
    *ptr2 = '\0';
}

/* possibly deliver a one-time room entry message */
void
check_special_room(boolean newlev)
{
    struct monst *mtmp;
    char *ptr;

    move_update(newlev);

    if (*u.ushops0)
        u_left_shop(u.ushops_left, newlev);

    /*
     * Check for attaining 'entered Mine Town' achievement.
     * Most of the Mine Town variations have the town in one large room
     * containing a bunch of subrooms; we check for entering that large
     * room.  However, two of the variations cover the whole level rather
     * than include a room with subrooms.  We need to check for town entry
     * before the possible early return for not having entered a room in
     * case we have arrived in the town but have not entered any room.
     *
     * TODO: change the minetn variants which don't include any town
     * boundary to have such.
     */
    if (svl.level.flags.has_town && !svc.context.achieveo.minetn_reached
        && In_mines(&u.uz) && in_town(u.ux, u.uy)) {
        record_achievement(ACH_TOWN);
        svc.context.achieveo.minetn_reached = TRUE;
    }

    if (!*u.uentered && !*u.ushops_entered) /* implied by newlev */
        return; /* no entrance messages necessary */

    /* Did we just enter a shop? */
    if (*u.ushops_entered)
        u_entered_shop(u.ushops_entered);

    for (ptr = &u.uentered[0]; *ptr; ptr++) {
        int roomno = *ptr - ROOMOFFSET, rt = svr.rooms[roomno].rtype;
        boolean msg_given = TRUE;

        /* Did we just enter some other special room? */
        /* vault.c insists that a vault remain a VAULT,
         * and temples should remain TEMPLEs,
         * but everything else gives a message only the first time */
        switch (rt) {
        case ZOO:
            pline("Welcome to David's treasure zoo!");
            break;
        case SWAMP:
            pline("It %s rather %s down here.", Blind ? "feels" : "looks",
                  Blind ? "humid" : "muddy");
            break;
        case COURT:
            You("enter an opulent%s room!",
                /* the throne room in Sam quest home level lacks a throne */
                !furniture_present(THRONE, roomno) ? "" : " throne");
            break;
        case LEPREHALL:
            You("enter a leprechaun hall!");
            break;
        case MORGUE:
            if (midnight()) {
                const char *run = u_locomotion("Run");

                pline("%s away!  %s away!", run, run);
            } else
                You("have an uncanny feeling...");
            break;
        case BEEHIVE:
            You("enter a giant beehive!");
            break;
        case COCKNEST:
            You("enter a disgusting nest!");
            break;
        case ANTHOLE:
            You("enter an anthole!");
            break;
        case BARRACKS:
            if (monstinroom(&mons[PM_SOLDIER], roomno)
                || monstinroom(&mons[PM_SERGEANT], roomno)
                || monstinroom(&mons[PM_LIEUTENANT], roomno)
                || monstinroom(&mons[PM_CAPTAIN], roomno))
                You("enter a military barracks!");
            else
                You("enter an abandoned barracks.");
            break;
        case DELPHI: {
            struct monst *oracle = monstinroom(&mons[PM_ORACLE], roomno);

            if (oracle) {
                SetVoice(oracle, 0, 80, 0);
                if (!oracle->mpeaceful)
                    verbalize("You're in Delphi, %s.", svp.plname);
                else
                    verbalize("%s, %s, welcome to Delphi!",
                              Hello((struct monst *) 0), svp.plname);
            } else
                msg_given = FALSE;
            break;
        }
        case TEMPLE:
            intemple(roomno + ROOMOFFSET);
            FALLTHROUGH;
        /*FALLTHRU*/
        default:
            msg_given = (rt == TEMPLE || rt >= SHOPBASE);
            rt = 0;
            break;
        }
        if (msg_given)
            room_discovered(roomno);

        if (rt != 0) {
            svr.rooms[roomno].rtype = OROOM;
            if (!search_special(rt)) {
                /* No more room of that type */
                switch (rt) {
                case COURT:
                    svl.level.flags.has_court = 0;
                    break;
                case SWAMP:
                    svl.level.flags.has_swamp = 0;
                    break;
                case MORGUE:
                    svl.level.flags.has_morgue = 0;
                    break;
                case ZOO:
                    svl.level.flags.has_zoo = 0;
                    break;
                case BARRACKS:
                    svl.level.flags.has_barracks = 0;
                    break;
                case TEMPLE:
                    svl.level.flags.has_temple = 0;
                    break;
                case BEEHIVE:
                    svl.level.flags.has_beehive = 0;
                    break;
                }
            }
            if (rt == COURT || rt == SWAMP || rt == MORGUE || rt == ZOO)
                for (mtmp = fmon; mtmp; mtmp = mtmp->nmon) {
                    if (DEADMONSTER(mtmp))
                        continue;
                    if (!isok(mtmp->mx,mtmp->my)
                        || roomno != (int) levl[mtmp->mx][mtmp->my].roomno)
                        continue;
                    if (!Stealth && !rn2(3)) {
                        wake_msg(mtmp, FALSE);
                        mtmp->msleeping = 0;
                    }
                }
        }
    }
    return;
}

/* returns
   1 = cannot pickup, time taken
   0 = cannot pickup, no time taken
  -1 = do normal pickup
  -2 = loot the monster */
staticfn int
pickup_checks(void)
{
    struct trap *traphere;

    /* uswallow case added by GAN 01/29/87 */
    if (u.uswallow) {
        if (!u.ustuck->minvent) {
            if (digests(u.ustuck->data)) {
                You("pick up %s tongue.", s_suffix(mon_nam(u.ustuck)));
                pline("But it's kind of slimy, so you drop it.");
            } else
                You("don't %s anything in here to pick up.",
                    Blind ? "feel" : "see");
            return 1;
        } else {
            return -2; /* loot the monster inventory */
        }
    }
    if (is_pool(u.ux, u.uy)) {
        if (Wwalking || is_floater(gy.youmonst.data)
            || is_clinger(gy.youmonst.data) || (Flying && !Breathless)) {
            You("cannot dive into the %s to pick things up.",
                hliquid("water"));
            return 0;
        } else if (!Underwater) {
            You_cant("even see the bottom, let alone pick up %s.", something);
            return 0;
        }
    }
    if (is_lava(u.ux, u.uy)) {
        if (Wwalking || is_floater(gy.youmonst.data)
            || is_clinger(gy.youmonst.data) || (Flying && !Breathless)) {
            You_cant("reach the bottom to pick things up.");
            return 0;
        } else if (!likes_lava(gy.youmonst.data)) {
            You("would burn to a crisp trying to pick things up.");
            return 0;
        }
    }
    if (!OBJ_AT(u.ux, u.uy)) {
        struct rm *lev = &levl[u.ux][u.uy];

        if (IS_THRONE(lev->typ))
            pline("It must weigh%s a ton!", lev->looted ? " almost" : "");
        else if (IS_SINK(lev->typ))
            pline_The("plumbing connects it to the floor.");
        else if (IS_GRAVE(lev->typ))
            You("don't need a gravestone.  Yet.");
        else if (IS_FOUNTAIN(lev->typ))
            You("could drink the %s...", hliquid("water"));
        else if (IS_DOOR(lev->typ) && (lev->doormask & D_ISOPEN))
            pline("It won't come off the hinges.");
        else if (IS_ALTAR(lev->typ))
            pline("Moving the altar would be a very bad idea.");
        else if (lev->typ == STAIRS)
            pline_The("stairs are solidly affixed.");
        else
            There("is nothing here to pick up.");
        return 0;
    }
    traphere = t_at(u.ux, u.uy);
    if (!can_reach_floor(traphere && is_pit(traphere->ttyp))) {
        /* if there's a hole here, any objects here clearly aren't at
           the bottom so only check for pits */
        if (traphere && uteetering_at_seen_pit(traphere)) {
            You("cannot reach the bottom of the pit.");
        } else if (u.usteed && P_SKILL(P_RIDING) < P_BASIC) {
            rider_cant_reach();
        } else if (Blind) {
            You("cannot reach anything here.");
        } else {
            const char *surf = surface(u.ux, u.uy);

            if (traphere) {
                if (traphere->ttyp == HOLE)
                    surf = "edge of the hole";
                else if (traphere->ttyp == TRAPDOOR)
                    surf = "trap door";
            }
            You("cannot reach the %s.", surf);
        }
        return 0;
    }
    return -1; /* can do normal pickup */
}

/* the #pickup command */
int
dopickup(void)
{
    int count, tmpcount, ret;

    count = (int) gc.command_count;
    gm.multi = 0; /* always reset */

    if ((ret = pickup_checks()) >= 0) {
        return ret ? ECMD_TIME : ECMD_OK;
    } else if (ret == -2) {
        tmpcount = -count;
        return loot_mon(u.ustuck, &tmpcount, (boolean *) 0) ? ECMD_TIME
                                                            : ECMD_OK;
    } /* else ret == -1 */

    return pickup(-count) ? ECMD_TIME : ECMD_OK;
}

/* stop running if we see something interesting next to us */
/* turn around a corner if that is the only way we can proceed */
/* do not turn left or right twice */
void
lookaround(void)
{
    coordxy x, y;
    coordxy i, x0 = 0, y0 = 0, m0 = 1, i0 = 9;
    int corrct = 0, noturn = 0;
    struct monst *mtmp;

    /* Grid bugs stop if trying to move diagonal, even if blind.  Maybe */
    /* they polymorphed while in the middle of a long move. */
    if (NODIAG(u.umonnum) && u.dx && u.dy) {
        You("cannot move diagonally.");
        nomul(0);
        return;
    }

    if (Blind || svc.context.run == 0)
        return;
    for (x = u.ux - 1; x <= u.ux + 1; x++)
        for (y = u.uy - 1; y <= u.uy + 1; y++) {
            boolean infront = (x == u.ux + u.dx && y == u.uy + u.dy);

            /* ignore out of bounds, and our own location */
            if (!isok(x, y) || u_at(x, y))
                continue;
            /* (grid bugs) ignore diagonals */
            if (NODIAG(u.umonnum) && x != u.ux && y != u.uy)
                continue;

            /* can we see a monster there? */
            if ((mtmp = m_at(x, y)) != 0
                && M_AP_TYPE(mtmp) != M_AP_FURNITURE
                && M_AP_TYPE(mtmp) != M_AP_OBJECT
                && mon_visible(mtmp)) {
                /* running movement and not a hostile monster */
                /* OR it blocks our move direction and we're not traveling */
                if ((svc.context.run != 1 && !is_safemon(mtmp))
                    || (infront && !svc.context.travel)) {
                    if (flags.mention_walls)
                        pline_xy(x, y, "%s blocks your path.",
                                 upstart(a_monnam(mtmp)));
                    goto stop;
                }
            }

            /* stone is never interesting */
            if (levl[x][y].typ == STONE)
                continue;
            /* ignore the square we're moving away from */
            if (x == u.ux - u.dx && y == u.uy - u.dy)
                continue;

            /* stop for traps, sometimes */
            if (avoid_moving_on_trap(x, y,
                                     (infront && svc.context.run > 1))) {
                if (svc.context.run == 1)
                    goto bcorr; /* if you must */
                if (infront)
                    goto stop;
            }

            /* more uninteresting terrain */
            if (IS_OBSTRUCTED(levl[x][y].typ) || levl[x][y].typ == ROOM
                || IS_AIR(levl[x][y].typ) || levl[x][y].typ == ICE) {
                continue;
            } else if (closed_door(x, y) || (mtmp && is_door_mappear(mtmp))) {
                /* a closed door? */
                /* ignore if diagonal */
                if (x != u.ux && y != u.uy)
                    continue;
                if (svc.context.run != 1 && !svc.context.travel) {
                    if (flags.mention_walls) {
                        set_msg_xy(x, y);
                        You("stop in front of the door.");
                    }
                    goto stop;
                }
                /* orthogonal to a closed door, consider it a corridor */
                goto bcorr;
            } else if (levl[x][y].typ == CORR) {
                /* corridor */
 bcorr:
                if (levl[u.ux][u.uy].typ != ROOM) {
                    /* running or traveling */
                    if (svc.context.run == 1 || svc.context.run == 3
                        || svc.context.run == 8) {
                        /* distance from x,y to location we're moving to */
                        i = dist2(x, y, u.ux + u.dx, u.uy + u.dy);
                        /* ignore if not on or directly adjacent to it */
                        if (i > 2)
                            continue;
                        /* x,y is (adjacent to) the location we're moving to;
                           if we've seen one corridor, and x,y is not directly
                           orthogonally next to it, mark noturn */
                        if (corrct == 1 && dist2(x, y, x0, y0) != 1)
                            noturn = 1;
                        /* if previous x,y was diagonal, now x,y is
                           orthogonal (or this is first time we're here) */
                        if (i < i0) {
                            i0 = i;
                            x0 = x;
                            y0 = y;
                            m0 = mtmp ? 1 : 0;
                        }
                    }
                    corrct++;
                }
                continue;
            } else if (is_pool_or_lava(x, y)) {
                if (infront && avoid_moving_on_liquid(x, y, TRUE))
                    goto stop;
                continue;
            } else { /* e.g. objects or trap or stairs */
                if (svc.context.run == 1)
                    goto bcorr;
                if (svc.context.run == 8)
                    continue;
                if (mtmp)
                    continue; /* d */
                if (((x == u.ux - u.dx) && (y != u.uy + u.dy))
                    || ((y == u.uy - u.dy) && (x != u.ux + u.dx)))
                    continue;
            }
 stop:
            nomul(0);
            return;
        } /* end for loops */

    if (corrct > 1 && svc.context.run == 2) {
        if (flags.mention_walls)
            pline_The("corridor widens here.");
        goto stop;
    }
    if ((svc.context.run == 1 || svc.context.run == 3 || svc.context.run == 8)
        && !noturn && !m0 && i0
        && (corrct == 1 || (corrct == 2 && i0 == 1))) {
        /* make sure that we do not turn too far */
        if (i0 == 2) {
            if (u.dx == y0 - u.uy && u.dy == u.ux - x0)
                i = 2; /* straight turn right */
            else
                i = -2; /* straight turn left */
        } else if (u.dx && u.dy) {
            if ((u.dx == u.dy && y0 == u.uy) || (u.dx != u.dy && y0 != u.uy))
                i = -1; /* half turn left */
            else
                i = 1; /* half turn right */
        } else {
            if ((x0 - u.ux == y0 - u.uy && !u.dy)
                || (x0 - u.ux != y0 - u.uy && u.dy))
                i = 1; /* half turn right */
            else
                i = -1; /* half turn left */
        }

        i += u.last_str_turn;
        if (i <= 2 && i >= -2) {
            u.last_str_turn = i;
            u.dx = x0 - u.ux;
            u.dy = y0 - u.uy;
        }
    }
}

/* check for a doorway which lacks its door (NODOOR or BROKEN) */
boolean
doorless_door(coordxy x, coordxy y)
{
    struct rm *lev_p = &levl[x][y];

    if (!IS_DOOR(lev_p->typ))
        return FALSE;
    /* all rogue level doors are doorless but disallow diagonal access, so
       we treat them as if their non-existent doors were actually present */
    if (Is_rogue_level(&u.uz))
        return FALSE;
    return !(lev_p->doormask & ~(D_NODOOR | D_BROKEN));
}

/* used by drown() to check whether hero can crawl from water to <x,y>;
   also used by findtravelpath() when destination is one step away */
boolean
crawl_destination(coordxy x, coordxy y)
{
    /* is location ok in general? */
    if (!goodpos(x, y, &gy.youmonst, 0))
        return FALSE;

    /* orthogonal movement is unrestricted when destination is ok */
    if (x == u.ux || y == u.uy)
        return TRUE;

    /* diagonal movement has some restrictions */
    if (NODIAG(u.umonnum))
        return FALSE; /* poly'd into a grid bug... */
    if (Passes_walls)
        return TRUE; /* or a xorn... */
    /* pool could be next to a door, conceivably even inside a shop */
    if (IS_DOOR(levl[x][y].typ) && (!doorless_door(x, y) || block_door(x, y)))
        return FALSE;
    /* finally, are we trying to squeeze through a too-narrow gap? */
    return !(bad_rock(gy.youmonst.data, u.ux, y)
             && bad_rock(gy.youmonst.data, x, u.uy)
             && cant_squeeze_thru(&gy.youmonst));
}

/* something like lookaround, but we are not running */
/* react only to monsters that might hit us */
int
monster_nearby(void)
{
    coordxy x, y;
    struct monst *mtmp;

    /* Also see the similar check in dochugw() in monmove.c */
    for (x = u.ux - 1; x <= u.ux + 1; x++)
        for (y = u.uy - 1; y <= u.uy + 1; y++) {
            if (!isok(x, y) || u_at(x, y))
                continue;
            if ((mtmp = m_at(x, y)) != 0
                && M_AP_TYPE(mtmp) != M_AP_FURNITURE
                && M_AP_TYPE(mtmp) != M_AP_OBJECT
                && (Hallucination
                    || (!mtmp->mpeaceful && !noattacks(mtmp->data)))
                && (!is_hider(mtmp->data) || !mtmp->mundetected)
                && !helpless(mtmp)
                && !onscary(u.ux, u.uy, mtmp) && canspotmon(mtmp))
                return 1;
        }
    return 0;
}

void
end_running(boolean and_travel)
{
    /* moveloop() suppresses time_botl when context.run is non-zero; when
       running stops, update 'time' even if other botl status is unchanged */
    if (flags.time && svc.context.run)
        disp.time_botl = TRUE;
    svc.context.run = 0;
    /* 'context.mv' isn't travel but callers who want to end travel
       all clear it too */
    if (and_travel)
        svc.context.travel = svc.context.travel1 = svc.context.mv = 0;
    if (gt.travelmap) {
        selection_free(gt.travelmap, TRUE);
        gt.travelmap = NULL;
    }
    /* cancel multi */
    if (gm.multi > 0)
        gm.multi = 0;
}

void
nomul(int nval)
{
    if (gm.multi < nval)
        return;              /* This is a bug fix by ab@unido */
    disp.botl |= (gm.multi >= 0);
    u.uinvulnerable = FALSE; /* Kludge to avoid ctrl-C bug -dlc */
    u.usleep = 0;
    gm.multi = nval;
    if (nval == 0)
        gm.multi_reason = NULL, gm.multireasonbuf[0] = '\0';
    end_running(TRUE);
    cmdq_clear(CQ_CANNED);
}

/* called when a non-movement, multi-turn action has completed */
void
unmul(const char *msg_override)
{
    disp.botl = TRUE;
    gm.multi = 0; /* caller will usually have done this already */
    if (msg_override)
        gn.nomovemsg = msg_override;
    else if (!gn.nomovemsg)
        gn.nomovemsg = You_can_move_again;
    if (*gn.nomovemsg) {
        pline("%s", gn.nomovemsg);
        /* follow "you survived that attempt on your life" with a message
           about current form if it's not the default; primarily for
           life-saving while turning into green slime but is also a reminder
           if life-saved while poly'd and Unchanging (explore or wizard mode
           declining to die since can't be both Unchanging and Lifesaved) */
        if (Upolyd && !strncmpi(gn.nomovemsg, "You survived that ", 18))
            You("are %s.",
                an(pmname(&mons[u.umonnum], Ugender))); /* (ignore Hallu) */
    }
    gn.nomovemsg = 0;
    u.usleep = 0;
    gm.multi_reason = NULL, gm.multireasonbuf[0] = '\0';

    if (ga.afternmv) {
        int (*f)(void) = ga.afternmv;

        /* clear afternmv before calling it (to override the
           encumbrance hack for levitation--see weight_cap()) */
        ga.afternmv = (int (*)(void)) 0;
        (void) (*f)();
    }
}

staticfn void
maybe_wail(void)
{
    static short powers[] = { TELEPORT, SEE_INVIS, POISON_RES, COLD_RES,
                              SHOCK_RES, FIRE_RES, SLEEP_RES, DISINT_RES,
                              TELEPORT_CONTROL, STEALTH, FAST, INVIS };

    if (svm.moves <= gw.wailmsg + 50)
        return;

    gw.wailmsg = svm.moves;
    if (Role_if(PM_WIZARD) || Race_if(PM_ELF) || Role_if(PM_VALKYRIE)) {
        const char *who;
        int i, powercnt;

        who = (Role_if(PM_WIZARD) || Role_if(PM_VALKYRIE)) ? gu.urole.name.m
                                                           : "Elf";
        if (u.uhp == 1) {
            pline("%s is about to die.", who);
        } else {
            for (i = 0, powercnt = 0; i < SIZE(powers); ++i)
                if (u.uprops[powers[i]].intrinsic & INTRINSIC)
                    ++powercnt;

            pline((powercnt >= 4) ? "%s, all your powers will be lost..."
                                  : "%s, your life force is running out.",
                  who);
        }
    } else {
        Soundeffect(se_wailing_of_the_banshee, 75);
        You_hear(u.uhp == 1 ? "the wailing of the Banshee..."
                            : "the howling of the CwnAnnwn...");
    }
}

/* once per game, if receiving a killing blow from above 90% HP,
   allow the hero to survive with 1 HP */
int
saving_grace(int dmg)
{
    if (dmg < 0) {
        impossible("saving_grace check for negative damage? (%d)", dmg);
        return 0;
    }

    if (!u.usaving_grace && dmg >= u.uhp && (u.uhp * 100 / u.uhpmax) > 90) {
        /* saving_grace doesn't have it's own livelog classification;
           we might invent one, or perhaps use LL_LIFESAVE, but surviving
           certain death (or preserving worn amulet of life saving) via
           saving-grace feels like breaking a conduct; not sure how best
           to phrase this though; classifying it as a spoiler will hide it
           from #chronicle during play but show it to livelog observers */
        livelog_printf(LL_CONDUCT | LL_SPOILER, "%s (%d damage, %d/%d HP)",
                       "survived one-shot death via saving-grace",
                       dmg, u.uhp, u.uhpmax);

        /* note: this could reduce dmg to 0 if u.uhpmax==1 */
        dmg = u.uhp - 1;
        u.usaving_grace = 1; /* used up */
        end_running(TRUE);
        if (u.usleep)
            unmul("Suddenly you wake up!");
        if (is_fainted())
            reset_faint();
    }
    return dmg;
}

/* show a message how much damage you received */
void
showdamage(int dmg)
{
    if (!iflags.showdamage || !dmg)
        return;

    pline("[HP %i, %i left]", -dmg, Upolyd ? u.mh : u.uhp);
}

void
losehp(int n, const char *knam, schar k_format)
{
#if 0   /* code below is prepared to handle negative 'loss' so don't add this
         * until we've verified that no callers intentionally rely on that */
    if (n <= 0) {
        impossible("hero losing %d hit points due to \"%s\"?", n, knam);
        return;
    }
#endif
    disp.botl = TRUE; /* u.uhp or u.mh is changing */
    end_running(TRUE);
    if (Upolyd) {
        u.mh -= n;
        showdamage(n);
        if (u.mhmax < u.mh)
            u.mhmax = u.mh;
        if (u.mh < 1)
            rehumanize();
        else if (n > 0 && u.mh * 10 < u.mhmax && Unchanging)
            maybe_wail();
        return;
    }

    n = saving_grace(n);
    u.uhp -= n;
    showdamage(n);
    if (u.uhp > u.uhpmax)
        u.uhpmax = u.uhp; /* perhaps n was negative */
    if (u.uhp < 1) {
        svk.killer.format = k_format;
        if (svk.killer.name != knam) /* the thing that killed you */
            Strcpy(svk.killer.name, knam ? knam : "");
        urgent_pline("You die...");
        done(DIED);
    } else if (n > 0 && u.uhp * 10 < u.uhpmax) {
        maybe_wail();
    }
}

int
weight_cap(void)
{
    long carrcap, save_ELev = ELevitation, save_BLev = BLevitation;

    /* boots take multiple turns to wear but any properties they
       confer are enabled at the start rather than the end; that
       causes message sequencing issues for boots of levitation
       so defer their encumbrance benefit until they're fully worn */
    if (ga.afternmv == Boots_on && (ELevitation & W_ARMF) != 0L) {
        ELevitation &= ~W_ARMF;
        float_vs_flight(); /* in case Levitation is blocking Flying */
    }
    /* levitation is blocked by being trapped in the floor, but it still
       functions enough in that situation to enhance carrying capacity */
    BLevitation &= ~I_SPECIAL;

    carrcap = (WT_WEIGHTCAP_STRCON * (ACURRSTR + ACURR(A_CON)))
               + WT_WEIGHTCAP_SPARE;
    if (Upolyd) {
        /* consistent with can_carry() in mon.c */
        if (gy.youmonst.data->mlet == S_NYMPH)
            carrcap = MAX_CARR_CAP;
        else if (!gy.youmonst.data->cwt)
            carrcap = (carrcap * (long) gy.youmonst.data->msize) / MZ_HUMAN;
        else if (!strongmonst(gy.youmonst.data)
                 || (strongmonst(gy.youmonst.data)
                     && (gy.youmonst.data->cwt > WT_HUMAN)))
            carrcap = (carrcap * (long) gy.youmonst.data->cwt / WT_HUMAN);
    }

    if (Levitation || Is_airlevel(&u.uz) /* pugh@cornell */
        || (u.usteed && strongmonst(u.usteed->data))) {
        carrcap = MAX_CARR_CAP;
    } else {
        if (carrcap > MAX_CARR_CAP)
            carrcap = MAX_CARR_CAP;
        if (!Flying) {
            if (EWounded_legs & LEFT_SIDE)
                carrcap -= WT_WOUNDEDLEG_REDUCT;
            if (EWounded_legs & RIGHT_SIDE)
                carrcap -= WT_WOUNDEDLEG_REDUCT;
        }
    }

    if (ELevitation != save_ELev || BLevitation != save_BLev) {
        ELevitation = save_ELev;
        BLevitation = save_BLev;
        float_vs_flight();
    }

    return (int) max(carrcap, 1L); /* never return 0 */
}

/* returns how far beyond the normal capacity the player is currently. */
/* inv_weight() is negative if the player is below normal capacity. */
int
inv_weight(void)
{
    struct obj *otmp = gi.invent;
    int wt = 0;

    while (otmp) {
        if (otmp->oclass == COIN_CLASS)
            wt += (int) (((long) otmp->quan + 50L) / 100L);
        else if (otmp->otyp != BOULDER || !throws_rocks(gy.youmonst.data))
            wt += otmp->owt;
        otmp = otmp->nobj;
    }
    gw.wc = weight_cap();
    return (wt - gw.wc);
}

/*
 * Returns 0 if below normal capacity, or the number of "capacity units"
 * over the normal capacity the player is loaded.  Max is 5.
 */
int
calc_capacity(int xtra_wt)
{
    int cap, wt = inv_weight() + xtra_wt;

    if (wt <= 0)
        return UNENCUMBERED;
    if (gw.wc <= 1)
        return OVERLOADED;
    cap = (wt * 2 / gw.wc) + 1;
    return min(cap, OVERLOADED);
}

int
near_capacity(void)
{
    return calc_capacity(0);
}

int
max_capacity(void)
{
    int wt = inv_weight();

    return (wt - (2 * gw.wc));
}

boolean
check_capacity(const char *str)
{
    if (near_capacity() >= EXT_ENCUMBER) {
        if (str)
            pline1(str);
        else
            You_cant("do that while carrying so much stuff.");
        return 1;
    }
    return 0;
}

struct weight_table_entry {
    unsigned wtyp; /* 1 = monst, 2 = obj */
    const char *nm; /* 0-6 = weight in ascii; 7 - end = name */
    int wt, idx;
    boolean unique;
};

static struct weight_table_entry *weightlist;

void
dump_weights(void)
{
    int i, cnt = 0, nmwidth = 49, mcount = NUMMONS, ocount = NUM_OBJECTS;
    char nmbuf[BUFSZ], nmbufbase[BUFSZ];

    weightlist = (struct weight_table_entry *)
                alloc(sizeof (struct weight_table_entry) * (mcount + ocount));
    decl_globals_init();
    init_objects();
    for (i = 0; i < mcount; ++i) {
        if (i != PM_LONG_WORM_TAIL) {
            boolean cm;

            weightlist[cnt].wt = (int) mons[i].cwt;
            weightlist[cnt].idx = i;
            weightlist[cnt].wtyp = 1;
            weightlist[cnt].unique = ((mons[i].geno & G_UNIQ) != 0);
            Snprintf(nmbuf, sizeof nmbuf, "%07u", weightlist[cnt].wt);
            cm = CapitalMon(mons[i].pmnames[NEUTRAL]);
            Snprintf(&nmbuf[7], sizeof nmbuf - 7, "%s%s", "the body of ",
                     (cm)                     ? the(mons[i].pmnames[NEUTRAL])
                     : weightlist[cnt].unique ? mons[i].pmnames[NEUTRAL]
                                              : an(mons[i].pmnames[NEUTRAL]));
            weightlist[cnt].nm = dupstr(nmbuf);
            cnt++;
        }
    }
    for (i = 0; i < ocount; ++i) {
        const char *oc_name = (i == SLIME_MOLD) ? "slime mold"
                              : obj_descr[i].oc_name;
        int wt = (int) objects[i].oc_weight;

        if (wt && oc_name) {
            weightlist[cnt].idx = i;
            weightlist[cnt].wt = wt;
            weightlist[cnt].wtyp = 2;
            weightlist[cnt].unique = (objects[i].oc_unique != 0);
            objects[i].oc_name_known = 1;
            Strcpy(nmbufbase, simple_typename(i));
            Snprintf(nmbuf, sizeof nmbuf, "%07u%s", wt,
                     (weightlist[cnt].unique) ? the(nmbufbase)
                                              : an(nmbufbase));
            weightlist[cnt].nm = dupstr(nmbuf);
            cnt++;
        }
    }
    qsort((genericptr_t) weightlist, cnt,
          sizeof (struct weight_table_entry), cmp_weights);
    raw_printf("int all_weights[] = {");
    for (i = 0; i < cnt; ++i) {
        if (weightlist[i].nm) {
            raw_printf("    %7u%s /* %*s */", weightlist[i].wt,
                       (i == cnt - 1) ? " " : ",", -nmwidth,
                       &weightlist[i].nm[7]);
            free((genericptr_t) weightlist[i].nm), weightlist[i].nm = 0;
        }
    }
    raw_print("};");
    raw_print("");
    free((genericptr_t) weightlist);
    freedynamicdata();
}

staticfn int QSORTCALLBACK
cmp_weights(const void *p1, const void *p2)
{
    struct weight_table_entry *i1 = (struct weight_table_entry *) p1,
                              *i2 = (struct weight_table_entry *) p2;

   /* return (i1->wt - i2->wt); */
    return strcmp(i1->nm, i2->nm);
}

int
inv_cnt(boolean incl_gold)
{
    struct obj *otmp = gi.invent;
    int ct = 0;

    while (otmp) {
        if (incl_gold || otmp->invlet != GOLD_SYM)
            ct++;
        otmp = otmp->nobj;
    }
    return ct;
}

/* Counts the money in an object chain. */
/* Intended use is for your or some monster's inventory, */
/* now that u.gold/m.gold is gone.*/
/* Counting money in a container might be possible too. */
long
money_cnt(struct obj *otmp)
{
    while (otmp) {
        if (otmp->oclass == COIN_CLASS)
            return otmp->quan;
        otmp = otmp->nobj;
    }
    return 0L;
}

void
spot_checks(coordxy x, coordxy y, schar old_typ)
{
    schar new_typ = levl[x][y].typ;
    boolean db_ice_now = FALSE;

    switch (old_typ) {
    case DRAWBRIDGE_UP:
        db_ice_now = ((levl[x][y].drawbridgemask & DB_UNDER) == DB_ICE);
        FALLTHROUGH;
        /*FALLTHRU*/
    case ICE:
        if ((new_typ != old_typ)
            || (old_typ == DRAWBRIDGE_UP && !db_ice_now)) {
            /* make sure there's no MELT_ICE_AWAY timer */
            if (spot_time_left(x, y, MELT_ICE_AWAY)) {
                spot_stop_timers(x, y, MELT_ICE_AWAY);
            }
            /* adjust things affected by the ice */
            obj_ice_effects(x, y, FALSE);
        }
        break;
    }
}

/* calculate x/y, rounding as appropriate */
int
rounddiv(long x, int y)
{
    int r, m;
    int divsgn = 1;

    if (y == 0)
        panic("division by zero in rounddiv");
    else if (y < 0) {
        divsgn = -divsgn;
        y = -y;
    }
    if (x < 0) {
        divsgn = -divsgn;
        x = -x;
    }
    r = (int) (x / y);
    m = x % y;
    if (2 * m >= y)
        r++;

    return divsgn * r;
}


/*hack.c*/
