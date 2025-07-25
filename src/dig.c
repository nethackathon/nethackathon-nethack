/* NetHack 3.7	dig.c	$NHDT-Date: 1740629713 2025/02/26 20:15:13 $  $NHDT-Branch: NetHack-3.7 $:$NHDT-Revision: 1.227 $ */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/*-Copyright (c) Michael Allison, 2012. */
/* NetHack may be freely redistributed.  See license for details. */

#include "hack.h"

staticfn boolean rm_waslit(void);
staticfn void mkcavepos(coordxy, coordxy, int, boolean, boolean);
staticfn void mkcavearea(boolean);
staticfn boolean pick_can_reach(struct obj *, coordxy, coordxy) NONNULLARG1;
staticfn int dig(void);
staticfn void dig_up_grave(coord *);
staticfn boolean watchman_canseeu(struct monst *) NONNULLARG1;
staticfn int adj_pit_checks(coord *, char *) NONNULLARG2;
staticfn void pit_flow(struct trap *, schar);
staticfn boolean furniture_handled(coordxy, coordxy, boolean);

/* Indices returned by dig_typ() */
enum dig_types {
    DIGTYP_UNDIGGABLE = 0,
    DIGTYP_ROCK,
    DIGTYP_STATUE,
    DIGTYP_BOULDER,
    DIGTYP_DOOR,
    DIGTYP_TREE
};

staticfn boolean
rm_waslit(void)
{
    coordxy x, y;

    if (levl[u.ux][u.uy].typ == ROOM && levl[u.ux][u.uy].waslit)
        return TRUE;
    for (x = u.ux - 2; x < u.ux + 3; x++)
        for (y = u.uy - 1; y < u.uy + 2; y++)
            if (isok(x, y) && levl[x][y].waslit)
                return TRUE;
    return FALSE;
}

/* Change level topology.  Messes with vision tables and ignores things like
 * boulders in the name of a nice effect.  Vision will get fixed up again
 * immediately after the effect is complete.
 */
staticfn void
mkcavepos(coordxy x, coordxy y, int dist, boolean waslit, boolean rockit)
{
    struct rm *lev;

    if (!isok(x, y))
        return;
    lev = &levl[x][y];

    if (rockit) {
        struct monst *mtmp;

        if (IS_OBSTRUCTED(lev->typ))
            return;
        if (t_at(x, y))
            return;                   /* don't cover the portal */
        if ((mtmp = m_at(x, y)) != 0) /* make sure crucial monsters survive */
            if (!passes_walls(mtmp->data))
                (void) rloc(mtmp, RLOC_NOMSG);
    } else if (lev->typ == ROOM)
        return;

    unblock_point(x, y); /* make sure vision knows this location is open */

    /* fake out saved state */
    lev->seenv = 0;
    lev->doormask = 0;
    if (dist < 3)
        lev->lit = (rockit ? FALSE : TRUE);
    if (waslit)
        lev->waslit = (rockit ? FALSE : TRUE);
    lev->horizontal = FALSE;
    /* short-circuit vision recalc */
    gv.viz_array[y][x] = (dist < 3) ? (IN_SIGHT | COULD_SEE) : COULD_SEE;
    lev->typ = (rockit ? STONE : ROOM); /* flags set via doormask above */
    if (dist >= 3)
        impossible("mkcavepos called with dist %d", dist);
    feel_newsym(x, y);
}

staticfn void
mkcavearea(boolean rockit)
{
    int dist;
    coordxy xmin = u.ux, xmax = u.ux;
    coordxy ymin = u.uy, ymax = u.uy;
    coordxy i;
    boolean waslit = rm_waslit();

    if (rockit) {
        Soundeffect(se_crashing_rock, 100);
        pline("Crash!  The ceiling collapses around you!");
    } else {
        pline("A mysterious force %s cave around you!",
              (levl[u.ux][u.uy].typ == CORR) ? "creates a" : "extends the");
    }
    display_nhwindow(WIN_MESSAGE, TRUE);

    for (dist = 1; dist <= 2; dist++) {
        xmin--;
        xmax++;

        /* top and bottom */
        if (dist < 2) { /* the area is wider that it is high */
            ymin--;
            ymax++;
            for (i = xmin + 1; i < xmax; i++) {
                mkcavepos(i, ymin, dist, waslit, rockit);
                mkcavepos(i, ymax, dist, waslit, rockit);
            }
        }

        /* left and right */
        for (i = ymin; i <= ymax; i++) {
            mkcavepos(xmin, i, dist, waslit, rockit);
            mkcavepos(xmax, i, dist, waslit, rockit);
        }

        flush_screen(1); /* make sure the new glyphs shows up */
        nh_delay_output();
    }

    if (!rockit && levl[u.ux][u.uy].typ == CORR) {
        levl[u.ux][u.uy].typ = ROOM; /* flags for CORR already 0 */
        if (waslit)
            levl[u.ux][u.uy].waslit = TRUE;
        newsym(u.ux, u.uy); /* in case player is invisible */
    }

    gv.vision_full_recalc = 1; /* everything changed */
}

/* called when attempting to break a statue or boulder with a pick */
staticfn boolean
pick_can_reach(struct obj *pick, coordxy x, coordxy y)
{
    struct trap *t = t_at(x, y);
    /* tseen: pit only affects item positioning when it is known */
    boolean target_in_pit = t && is_pit(t->ttyp) && t->tseen;

    /* if hero is in a pit, pick can only reach if the statue is too and
       the two pits are conjoined or the statue isn't and pick is two-handed;
       this applies to hero in pit trying to reach an adjcacent boulder too */
    if (u.utrap && u.utraptype == TT_PIT) {
        if (target_in_pit)
            return conjoined_pits(t, t_at(u.ux, u.uy), FALSE);
        return bimanual(pick);
    }

    /* when hero isn't in a pit, a mattock or flying hero w/ pick can reach
       whether or not the statue is in a pit */
    if (bimanual(pick) || Flying)
        return TRUE;
    /* one-handed pick-axe can reach if statue isn't in a pit */
    if (!target_in_pit)
        return TRUE;

    return FALSE;
}

/* When digging into location <x,y>, what are you actually digging into? */
int
dig_typ(struct obj *otmp, coordxy x, coordxy y)
{
    int ltyp;

    if (!isok(x, y) || !otmp || (!is_pick(otmp) && !is_axe(otmp)))
        return DIGTYP_UNDIGGABLE;

    ltyp = levl[x][y].typ;
    if (is_axe(otmp))
        return closed_door(x, y) ? DIGTYP_DOOR
               : IS_TREE(ltyp) ? DIGTYP_TREE /* axe vs tree */
                 : DIGTYP_UNDIGGABLE;
    /*assert(is_pick(otmp));*/
    return (sobj_at(STATUE, x, y) && pick_can_reach(otmp, x, y))
           ? DIGTYP_STATUE
           : (sobj_at(BOULDER, x, y) && pick_can_reach(otmp, x, y))
             ? DIGTYP_BOULDER
             : closed_door(x, y) ? DIGTYP_DOOR
               : IS_TREE(ltyp) ? DIGTYP_UNDIGGABLE /* pick vs tree */
                 : (IS_OBSTRUCTED(ltyp)
                    && (!svl.level.flags.arboreal || IS_WALL(ltyp)))
                   ? DIGTYP_ROCK
                   : DIGTYP_UNDIGGABLE;
}

boolean
is_digging(void)
{
    if (go.occupation == dig) {
        return TRUE;
    }
    return FALSE;
}

#define BY_YOU (&gy.youmonst)
#define BY_OBJECT ((struct monst *) 0)

enum digcheck_result
dig_check(struct monst *madeby, coordxy x, coordxy y)
{
    struct trap *ttmp = t_at(x, y);

    if (On_stairs(x, y)) {
        stairway *stway = stairway_at(x, y);
        if (stway->isladder) {
            return DIGCHECK_FAIL_ONLADDER;
        } else {
            return DIGCHECK_FAIL_ONSTAIRS;
        }
    } else if (IS_THRONE(levl[x][y].typ) && madeby != BY_OBJECT) {
        return DIGCHECK_FAIL_THRONE;
    } else if (IS_ALTAR(levl[x][y].typ)
               && (madeby != BY_OBJECT
                   || (altarmask_at(x, y) & AM_SANCTUM) != 0)) {
        return DIGCHECK_FAIL_ALTAR;
    } else if (Is_airlevel(&u.uz)) {
        return DIGCHECK_FAIL_AIRLEVEL;
    } else if (Is_waterlevel(&u.uz)) {
        return DIGCHECK_FAIL_WATERLEVEL;
    } else if ((IS_OBSTRUCTED(levl[x][y].typ) && levl[x][y].typ != SDOOR
                && (levl[x][y].wall_info & W_NONDIGGABLE) != 0)) {
        return DIGCHECK_FAIL_TOOHARD;
    } else if (ttmp && undestroyable_trap(ttmp->ttyp)) {
        return DIGCHECK_FAIL_UNDESTROYABLETRAP;
    } else if (!Can_dig_down(&u.uz) && !levl[x][y].candig) {
        if (ttmp) {
            if (!is_hole(ttmp->ttyp) && !is_pit(ttmp->ttyp))
                return DIGCHECK_PASSED_DESTROY_TRAP;
            else
                return DIGCHECK_FAIL_CANTDIG;
        } else {
            return DIGCHECK_PASSED_PITONLY;
        }
    } else if (sobj_at(BOULDER, x, y)) {
        return DIGCHECK_FAIL_BOULDER;
    } else if (madeby == BY_OBJECT
               /* the block against existing traps is mainly to
                  prevent broken wands from turning holes into pits */
               && (ttmp || is_pool_or_lava(x, y))) {
        /* digging by player handles pools separately */
        return DIGCHECK_FAIL_OBJ_POOL_OR_TRAP;
    }
    return DIGCHECK_PASSED;
}

void
digcheck_fail_message(enum digcheck_result digresult, struct monst *madeby,
                      coordxy x, coordxy y)
{
    const char *verb =
        (madeby == BY_YOU && uwep && is_axe(uwep)) ? "chop" : "dig in";

    if (digresult < DIGCHECK_FAILED)
        return;

    switch (digresult) {
    case DIGCHECK_FAIL_AIRLEVEL:
        You("cannot %s thin air.", verb);
        break;
    case DIGCHECK_FAIL_ALTAR:
        pline_The("altar is too hard to break apart.");
        break;
    case DIGCHECK_FAIL_BOULDER:
        There("isn't enough room to %s here.", verb);
        break;
    case DIGCHECK_FAIL_ONLADDER:
        pline_The("ladder resists your effort.");
        break;
    case DIGCHECK_FAIL_ONSTAIRS:
        pline_The("stairs are too hard to %s.", verb);
        break;
    case DIGCHECK_FAIL_THRONE:
        pline_The("throne is too hard to break apart.");
        break;
    case DIGCHECK_FAIL_CANTDIG:
    case DIGCHECK_FAIL_TOOHARD:
    case DIGCHECK_FAIL_UNDESTROYABLETRAP:
        pline_The("%s here is too hard to %s.", surface(x, y), verb);
        break;
    case DIGCHECK_FAIL_WATERLEVEL:
        pline_The("%s splashes and subsides.", hliquid("water"));
        break;
    case DIGCHECK_FAIL_OBJ_POOL_OR_TRAP:
    case DIGCHECK_PASSED:
    case DIGCHECK_PASSED_PITONLY:
    case DIGCHECK_PASSED_DESTROY_TRAP:
        break;
    }
}

staticfn int
dig(void)
{
    struct rm *lev;
    coordxy dpx = svc.context.digging.pos.x, dpy = svc.context.digging.pos.y;
    boolean ispick = uwep && is_pick(uwep);
    const char *verb = (!uwep || is_pick(uwep)) ? "dig into" : "chop through";
    enum digcheck_result dcresult = DIGCHECK_PASSED;

    lev = &levl[dpx][dpy];
    /* perhaps a nymph stole your pick-axe while you were busy digging */
    /* or perhaps you teleported away */
    if (u.uswallow || !uwep || (!ispick && !is_axe(uwep))
        || !on_level(&svc.context.digging.level, &u.uz)
        || ((svc.context.digging.down ? (dpx != u.ux || dpy != u.uy)
                                  : !next2u(dpx, dpy))))
        return 0;

    if (svc.context.digging.down) {
        dcresult = dig_check(BY_YOU, u.ux, u.uy);
        if (dcresult >= DIGCHECK_FAILED) {
            digcheck_fail_message(dcresult, BY_YOU, u.ux, u.uy);
            return 0;
        }
    } else { /* !svc.context.digging.down */
        if (IS_TREE(lev->typ) && !may_dig(dpx, dpy)
            && dig_typ(uwep, dpx, dpy) == DIGTYP_TREE) {
            pline("This tree seems to be petrified.");
            return 0;
        }
        if (IS_OBSTRUCTED(lev->typ) && !may_dig(dpx, dpy)
            && dig_typ(uwep, dpx, dpy) == DIGTYP_ROCK) {
            pline("This %s is too hard to %s.",
                  is_db_wall(dpx, dpy) ? "drawbridge" : "wall", verb);
            return 0;
        }
    }
    if (Fumbling && !rn2(3)) {
        switch (rn2(3)) {
        case 0:
            if (!welded(uwep)) {
                You("fumble and drop %s.", yname(uwep));
                dropx(uwep);
            } else {
                if (u.usteed)
                    pline("%s and %s %s!", Yobjnam2(uwep, "bounce"),
                          otense(uwep, "hit"), mon_nam(u.usteed));
                else
                    pline("Ouch!  %s and %s you!", Yobjnam2(uwep, "bounce"),
                          otense(uwep, "hit"));
                set_wounded_legs(RIGHT_SIDE, 5 + rnd(5));
            }
            break;
        case 1:
            Soundeffect(se_bang_weapon_side, 100);
            pline("Bang!  You hit with the broad side of %s!",
                  the(xname(uwep)));
            wake_nearby(FALSE);
            break;
        default:
            Your("swing misses its mark.");
            break;
        }
        return 0;
    }

    svc.context.digging.effort +=
        10 + rn2(5) + abon() + uwep->spe - greatest_erosion(uwep) + u.udaminc;
    if (Race_if(PM_DWARF))
        svc.context.digging.effort *= 2;
    if (svc.context.digging.down) {
        struct trap *ttmp = t_at(dpx, dpy);

        if (svc.context.digging.effort > 250
            || (ttmp && ttmp->ttyp == HOLE)) {
            (void) dighole(FALSE, FALSE, (coord *) 0);
            (void) memset((genericptr_t) &svc.context.digging, 0,
                          sizeof svc.context.digging);
            return 0; /* done with digging */
        }

        if (svc.context.digging.effort <= 50
            || (ttmp && (ttmp->ttyp == TRAPDOOR || is_pit(ttmp->ttyp)))) {
            return 1;
        } else if (ttmp && (ttmp->ttyp == LANDMINE
                            || (ttmp->ttyp == BEAR_TRAP && !u.utrap))) {
            /* digging onto a set object trap triggers it;
               hero should have used #untrap first */
            dotrap(ttmp, FORCETRAP);
            /* restart completely from scratch if we resume digging */
            (void) memset((genericptr_t) &svc.context.digging, 0,
                          sizeof svc.context.digging);
            return 0;
        } else if (ttmp && ttmp->ttyp == BEAR_TRAP && u.utrap) {
            if (rnl(7) > (Fumbling ? 1 : 4)) {
                char kbuf[BUFSZ];
                int dmg = dmgval(uwep, &gy.youmonst) + dbon();

                if (dmg < 1)
                    dmg = 1;
                else if (uarmf)
                    dmg = (dmg + 1) / 2;
                You("hit yourself in the %s.", body_part(FOOT));
                Sprintf(kbuf, "chopping off %s own %s", uhis(),
                        body_part(FOOT));
                losehp(Maybe_Half_Phys(dmg), kbuf, KILLED_BY);
            } else {
                You("destroy the bear trap with %s.",
                    yobjnam(uwep, (const char *) 0));
                deltrap(ttmp);
                reset_utrap(TRUE); /* release from trap, maybe Lev or Fly */
            }
            /* we haven't made any progress toward a pit yet */
            svc.context.digging.effort = 0;
            return 0;
        } else if (ttmp && dcresult == DIGCHECK_PASSED_DESTROY_TRAP) {
            const char *ttmpname = trapname(ttmp->ttyp, FALSE);

            if (ispick)
                You("destroy %s with %s.",
                    ttmp->tseen ? the(ttmpname) : an(ttmpname),
                    yobjnam(uwep, (const char *) 0));
            deltrap(ttmp);
            /* we haven't made any progress toward a pit yet */
            svc.context.digging.effort = 0;
            return 0;
        }

        if (IS_ALTAR(lev->typ)) {
            altar_wrath(dpx, dpy);
            angry_priest();
        }

        /* make pit at <u.ux,u.uy> */
        if (dighole(TRUE, FALSE, (coord *) 0)) {
            svc.context.digging.level.dnum = 0;
            svc.context.digging.level.dlevel = -1;
        }
        return 0;
    }

    if (svc.context.digging.effort > 100) {
        char digbuf[BUFSZ];
        const char *digtxt, *dmgtxt = (const char *) 0;
        struct obj *obj, *bobj;
        boolean shopedge = *in_rooms(dpx, dpy, SHOPBASE);
        int digtyp = dig_typ(uwep, dpx, dpy);

        if (digtyp == DIGTYP_STATUE
            && (obj = sobj_at(STATUE, dpx, dpy)) != 0) {
            if (break_statue(obj))
                digtxt = "The statue shatters.";
            else
                /* it was a statue trap; break_statue()
                   printed a message and updated the screen */
                digtxt = (char *) 0;
        } else if (digtyp == DIGTYP_BOULDER
                   && (obj = sobj_at(BOULDER, dpx, dpy)) != 0) {
            fracture_rock(obj);
            /*[3.7: this probably isn't necessary anymore]*/
            if ((bobj = sobj_at(BOULDER, dpx, dpy)) != 0) {
                /* another boulder here, restack it to the top */
                obj_extract_self(bobj);
                place_object(bobj, dpx, dpy);
            }
            digtxt = "The boulder falls apart.";
        } else if (lev->typ == STONE || lev->typ == SCORR
                   || IS_TREE(lev->typ)) {
            if (Is_earthlevel(&u.uz)) {
                if (uwep->blessed && !rn2(3)) {
                    mkcavearea(FALSE);
                    goto cleanup;
                } else if ((uwep->cursed && !rn2(4))
                           || (!uwep->blessed && !rn2(6))) {
                    mkcavearea(TRUE);
                    goto cleanup;
                }
            }
            if (digtyp == DIGTYP_TREE) {
                digtxt = "You cut down the tree.";
                lev->typ = ROOM, lev->flags = 0;
                if (!rn2(5))
                    (void) rnd_treefruit_at(dpx, dpy);
                if (Race_if(PM_ELF) || Role_if(PM_RANGER))
                    adjalign(-1);
            } else {
                digtxt = "You succeed in cutting away some rock.";
                lev->typ = CORR, lev->flags = 0;
            }
        } else if (IS_WALL(lev->typ)) {
            if (shopedge) {
                add_damage(dpx, dpy, SHOP_WALL_DMG);
                dmgtxt = "damage";
            }
            if (svl.level.flags.is_maze_lev) {
                lev->typ = ROOM, lev->flags = 0;
            } else if (svl.level.flags.is_cavernous_lev
                       && !in_town(dpx, dpy)) {
                lev->typ = CORR, lev->flags = 0;
            } else {
                lev->typ = DOOR, lev->doormask = D_NODOOR;
            }
            digtxt = "You make an opening in the wall.";
        } else if (lev->typ == SDOOR) {
            cvt_sdoor_to_door(lev); /* ->typ = DOOR */
            digtxt = "You break through a secret door!";
            if (!(lev->doormask & D_TRAPPED))
                lev->doormask = D_BROKEN;
        } else if (closed_door(dpx, dpy)) {
            Sprintf(digbuf, "You break through the door with your %s.",
                    simpleonames(uwep));
            digtxt = digbuf;
            if (shopedge) {
                add_damage(dpx, dpy, SHOP_DOOR_COST);
                dmgtxt = "break";
            }
            if (!(lev->doormask & D_TRAPPED))
                lev->doormask = D_BROKEN;
        } else
            return 0; /* statue or boulder got taken */

        if (!does_block(dpx, dpy, &levl[dpx][dpy]))
            unblock_point(dpx, dpy); /* vision:  can see through */
        feel_newsym(dpx, dpy);
        if (digtxt && !svc.context.digging.quiet)
            pline1(digtxt); /* after newsym */
        if (dmgtxt)
            pay_for_damage(dmgtxt, FALSE);

        if (Is_earthlevel(&u.uz) && !rn2(3)) {
            int mndx = rn2(2) ? PM_EARTH_ELEMENTAL : PM_XORN;

            if (makemon(&mons[mndx], dpx, dpy, MM_NOMSG))
                pline_The("debris from your digging comes to life!");
        }
        if (IS_DOOR(lev->typ) && (lev->doormask & D_TRAPPED)) {
            lev->doormask = D_NODOOR;
            b_trapped("door", NO_PART);
            recalc_block_point(dpx, dpy);
            newsym(dpx, dpy);
        }
 cleanup:
        svc.context.digging.lastdigtime = svm.moves;
        svc.context.digging.quiet = FALSE;
        svc.context.digging.level.dnum = 0;
        svc.context.digging.level.dlevel = -1;
        return 0;
    } else { /* not enough effort has been spent yet */
        static const char *const d_target[6] = { "",        "rock", "statue",
                                                 "boulder", "door", "tree" };
        int dig_target = dig_typ(uwep, dpx, dpy);

        if (IS_WALL(lev->typ) || dig_target == DIGTYP_DOOR) {
            if (*in_rooms(dpx, dpy, SHOPBASE)) {
                pline("This %s seems too hard to %s.",
                      IS_DOOR(lev->typ) ? "door" : "wall", verb);
                return 0;
            }
        } else if (dig_target == DIGTYP_UNDIGGABLE
                   || (dig_target == DIGTYP_ROCK && !IS_OBSTRUCTED(lev->typ)))
            return 0; /* statue or boulder got taken */

        if (!gd.did_dig_msg) {
            You("hit the %s with all your might.", d_target[dig_target]);
            wake_nearby(FALSE);
            gd.did_dig_msg = TRUE;
        }
    }
    return 1;
}

staticfn boolean
furniture_handled(coordxy x, coordxy y, boolean madeby_u)
{
    struct rm *lev = &levl[x][y];

    if (IS_FOUNTAIN(lev->typ)) {
        dogushforth(FALSE);
        SET_FOUNTAIN_WARNED(x, y); /* force dryup */
        dryup(x, y, madeby_u, FALSE);
    } else if (IS_SINK(lev->typ)) {
        breaksink(x, y);
    } else if (lev->typ == DRAWBRIDGE_DOWN
               || (is_drawbridge_wall(x, y) >= 0)) {
        coordxy bx = x, by = y;

        /* if under the portcullis, the bridge is adjacent */
        (void) find_drawbridge(&bx, &by);
        destroy_drawbridge(bx, by);
    } else {
        return FALSE;
    }
    return TRUE;
}


/* When will hole be finished? Very rough indication used by shopkeeper. */
int
holetime(void)
{
    if (go.occupation != dig || !*u.ushops)
        return -1;
    return ((250 - svc.context.digging.effort) / 20);
}

/* Return typ of liquid to fill a hole with, or ROOM, if no liquid nearby */
schar
fillholetyp(coordxy x, coordxy y,
            boolean fill_if_any) /* force filling if it exists at all */
{
    coordxy x1, y1;
    coordxy lo_x = max(1, x - 1), hi_x = min(x + 1, COLNO - 1),
            lo_y = max(0, y - 1), hi_y = min(y + 1, ROWNO - 1);
    int pool_cnt = 0, moat_cnt = 0, lava_cnt = 0;

    for (x1 = lo_x; x1 <= hi_x; x1++)
        for (y1 = lo_y; y1 <= hi_y; y1++)
            if (is_moat(x1, y1))
                moat_cnt++;
            else if (is_pool(x1, y1))
                /* This must come after is_moat since moats are pools
                 * but not vice-versa. */
                pool_cnt++;
            else if (is_lava(x1, y1))
                lava_cnt++;

    if (!fill_if_any)
        pool_cnt /= 3; /* not as much liquid as the others */

    if ((lava_cnt > moat_cnt + pool_cnt && rn2(lava_cnt + 1))
        || (lava_cnt && fill_if_any))
        return LAVAPOOL;
    else if ((moat_cnt > 0 && rn2(moat_cnt + 1)) || (moat_cnt && fill_if_any))
        return MOAT;
    else if ((pool_cnt > 0 && rn2(pool_cnt + 1)) || (pool_cnt && fill_if_any))
        return POOL;
    else
        return ROOM;
}

void
digactualhole(coordxy x, coordxy y, struct monst *madeby, int ttyp)
{
    struct obj *oldobjs, *newobjs;
    struct trap *ttmp;
    const char *surface_type, *tname, *in_thru;
    char furniture[BUFSZ];
    struct rm *lev = &levl[x][y];
    struct monst *mtmp = m_at(x, y); /* may be madeby */
    boolean madeby_u = (madeby == BY_YOU), madeby_obj = (madeby == BY_OBJECT),
            /* BY_OBJECT means the hero broke a wand, so blame her for it */
            heros_fault = (madeby_u || madeby_obj);
    boolean shopdoor, at_u = u_at(x, y), wont_fall = Levitation || Flying;
    int old_typ, old_aligntyp = A_NONE;

    if (at_u && u.utrap) {
        if (u.utraptype == TT_BURIEDBALL)
            buried_ball_to_punishment();
        else if (u.utraptype == TT_INFLOOR)
            reset_utrap(FALSE);
    }

    if (furniture_handled(x, y, madeby_u))
        return;

    if (ttyp != PIT && (!Can_dig_down(&u.uz) && !lev->candig)) {
        impossible("digactualhole: can't dig %s on this level.",
                   trapname(ttyp, TRUE));
        ttyp = PIT;
    }

    /* maketrap() might change terrain type but we deliver messages after
       that, so prepare in advance */
    old_typ = lev->typ;
    furniture[0] = '\0';
    if (IS_FURNITURE(lev->typ)) {
        /* should mirror the word used by surface() for normal floor */
        surface_type = (IS_ROOM(lev->typ) && !Is_earthlevel(&u.uz)
                         ? "floor" : "ground");
        if (IS_ALTAR(lev->typ)) {
            old_aligntyp = Amask2align(levl[x][y].altarmask & AM_MASK);
            Strcpy(furniture, align_str(old_aligntyp));
            Strcat(furniture, " ");
        }
        Strcat(furniture, surface(x, y));
    } else {
        surface_type = surface(x, y);
    }
    shopdoor = IS_DOOR(lev->typ) && *in_rooms(x, y, SHOPBASE);
    oldobjs = svl.level.objects[x][y];

    ttmp = maketrap(x, y, ttyp);
    if (!ttmp)
        return;
    newobjs = svl.level.objects[x][y];
    ttmp->madeby_u = heros_fault;
    ttmp->tseen = 0;
    if (cansee(x, y))
        seetrap(ttmp);
    else if (madeby_u)
        feeltrap(ttmp);

    tname = trapname(ttyp, TRUE);
    in_thru = (ttyp == HOLE ? "through" : "in");
    if (madeby_u) {
        if (x != u.ux || y != u.uy)
            You("dig an adjacent %s.", tname);
        else
            You("dig %s %s the %s.", an(tname), in_thru, surface_type);
    } else if (!madeby_obj && canseemon(madeby)) {
        pline("%s digs %s %s the %s.", Monnam(madeby), an(tname), in_thru,
              surface_type);
    } else if (cansee(x, y) && flags.verbose) {
        if (IS_STWALL(old_typ))
            pline_The("%s crumbles into %s.", surface_type, an(tname));
        else
            pline("%s appears in the %s.", An(tname), surface_type);
    }
    if (IS_FURNITURE(old_typ) && cansee(x, y))
        pline_The("%s falls into the %s!", furniture, tname);
    /* wrath should immediately follow altar destruction message */
    if (heros_fault && old_typ == ALTAR)
        desecrate_altar(FALSE, old_aligntyp);

    /* now deal with actual post-trap creation effects */
    if (ttyp == PIT) {
        if (shopdoor && heros_fault)
            pay_for_damage("ruin", FALSE);
        else
            add_damage(x, y, heros_fault ? SHOP_PIT_COST : 0L);
        if (madeby_u)
            wake_nearby(FALSE);
        /* in case we're digging down while encased in solid rock
           which is blocking levitation or flight */
        switch_terrain();
        if (Levitation || Flying)
            wont_fall = TRUE;

        if (at_u) {
            if (!wont_fall) {
                set_utrap(rn1(4, 2), TT_PIT);
                gv.vision_full_recalc = 1; /* vision limits change */
            } else
                reset_utrap(TRUE);
            if (oldobjs != newobjs) /* something unearthed */
                (void) pickup(1);   /* detects pit */
        } else if (mtmp) {
            if (is_flyer(mtmp->data) || is_floater(mtmp->data)) {
                if (canseemon(mtmp))
                    pline("%s %s over the pit.", Monnam(mtmp),
                          (is_flyer(mtmp->data)) ? "flies" : "floats");
            } else if (mtmp != madeby)
                (void) mintrap(mtmp, NO_TRAP_FLAGS);
        }
    } else { /* was TRAPDOOR now a HOLE*/
        if (at_u) {
            /* in case we're digging down while encased in solid rock
               which is blocking levitation or flight */
            switch_terrain();
            if (Levitation || Flying)
                wont_fall = TRUE;

            /* check for leashed pet that can't fall right now */
            if (!u.ustuck && !wont_fall && !next_to_u()) {
                You("are jerked back by your pet!");
                wont_fall = TRUE;
            }

            /* Floor objects get a chance of falling down.  The case where
             * the hero does NOT fall down is treated here.  The case
             * where the hero does fall down is treated in goto_level().
             */
            if (u.ustuck || wont_fall) {
                if (newobjs)
                    impact_drop((struct obj *) 0, x, y, 0);
                if (oldobjs != newobjs)
                    (void) pickup(1);
                if (shopdoor && heros_fault)
                    pay_for_damage("ruin", FALSE);
            } else {
                d_level newlevel;

                if (*u.ushops && heros_fault)
                    shopdig(1); /* shk might snatch pack */
                else /* handle any earlier hero-caused damage */
                    pay_for_damage("dig into", TRUE);
                You("fall through...");
                /* Earlier checks must ensure that the destination
                 * level exists and is in the present dungeon.
                 */
                newlevel.dnum = u.uz.dnum;
                newlevel.dlevel = u.uz.dlevel + 1;
                goto_level(&newlevel, FALSE, TRUE, FALSE);
                /* messages for arriving in special rooms */
                spoteffects(FALSE);
            }
        } else {
            if (shopdoor && heros_fault)
                pay_for_damage("ruin", FALSE);
            if (newobjs)
                impact_drop((struct obj *) 0, x, y, 0);
            if (mtmp) {
                /*[don't we need special sokoban handling here?]*/
                if (!grounded(mtmp->data)
                    || (mtmp->wormno && count_wsegs(mtmp) > 5)
                    || mtmp->data->msize >= MZ_HUGE)
                    return;
                if (mtmp == u.ustuck) /* probably a vortex */
                    return;           /* temporary? kludge */

                if (teleport_pet(mtmp, FALSE)) {
                    d_level tolevel;

                    if (Is_stronghold(&u.uz)) {
                        assign_level(&tolevel, &valley_level);
                    } else if (Is_botlevel(&u.uz)) {
                        if (canseemon(mtmp))
                            pline("%s avoids the trap.", Monnam(mtmp));
                        return;
                    } else {
                        get_level(&tolevel, depth(&u.uz) + 1);
                    }
                    if (mtmp->isshk)
                        make_angry_shk(mtmp, 0, 0);
                    migrate_to_level(mtmp, ledger_no(&tolevel),
                                     MIGR_RANDOM, (coord *) 0);
                }
            }
        }
    }
}

DISABLE_WARNING_FORMAT_NONLITERAL

/*
 * Called from dighole(); also from do_break_wand() in apply.c
 * and do_earthquake() in music.c.
 */
void
liquid_flow(
    coordxy x, coordxy y,
    schar typ,
    struct trap *ttmp,
    const char *fillmsg)
{
    struct obj *objchain;
    struct monst *mon;
    boolean u_spot = u_at(x, y);

    /* caller should have changed levl[x][y].typ to POOL, MOAT, or LAVA */
    if (!is_pool_or_lava(x, y)) {
        if (iflags.sanity_check) {
            impossible("Insane liquid_flow(%d,%d,%s,%s).", x, y,
                       ttmp ? trapname(ttmp->ttyp, TRUE) : "no trap",
                       fillmsg ? fillmsg : "no mesg");
        }
        return;
    }

    if (ttmp)
        (void) delfloortrap(ttmp); /* will untrap monster if one is here */
    /* if any objects were frozen here, they're released now */
    obj_ice_effects(x, y, TRUE);
    unearth_objs(x, y);

    if (fillmsg)
        pline(fillmsg, hliquid(typ == LAVAPOOL ? "lava" : "water"));
    /* handle object damage before hero damage; affects potential bones */
    if ((objchain = svl.level.objects[x][y]) != 0) {
        if (typ == LAVAPOOL)
            fire_damage_chain(objchain, TRUE, TRUE, x, y);
        else
            water_damage_chain(objchain, TRUE);
    }
    /* damage to the hero */
    if (u_spot) {
        (void) pooleffects(FALSE);
    } else if ((mon = m_at(x, y)) != 0) {
        (void) minliquid(mon);
    }
}

RESTORE_WARNING_FORMAT_NONLITERAL

/* return TRUE if digging succeeded, FALSE otherwise */
boolean
dighole(boolean pit_only, boolean by_magic, coord *cc)
{
    struct trap *ttmp;
    struct rm *lev;
    struct obj *boulder_here;
    schar typ, old_typ;
    coordxy dig_x, dig_y;
    boolean nohole, retval = FALSE;
    enum digcheck_result dig_check_result;

    if (!cc) {
        dig_x = u.ux;
        dig_y = u.uy;
    } else {
        dig_x = cc->x;
        dig_y = cc->y;
        if (!isok(dig_x, dig_y))
            return FALSE;
    }

    ttmp = t_at(dig_x, dig_y);
    lev = &levl[dig_x][dig_y];
    dig_check_result = dig_check(BY_YOU, dig_x, dig_y);
    /* nohole = (!Can_dig_down(&u.uz) && !lev->candig); */
    nohole = (dig_check_result == DIGCHECK_FAIL_CANTDIG
                  || dig_check_result == DIGCHECK_FAIL_TOOHARD);
    old_typ = lev->typ;

    if ((ttmp && (undestroyable_trap(ttmp->ttyp) || nohole))
        || (IS_OBSTRUCTED(old_typ) && old_typ != SDOOR
            && (lev->wall_info & W_NONDIGGABLE) != 0)) {
        pline_The("%s %shere is too hard to dig in.", surface(dig_x, dig_y),
                  (dig_x != u.ux || dig_y != u.uy) ? "t" : "");
    } else if (ttmp && is_magical_trap(ttmp->ttyp)) {
        explode(dig_x, dig_y, 0, 20 + d(3, 6), TRAP_EXPLODE, EXPL_MAGICAL);
        deltrap(ttmp);
        newsym(dig_x, dig_y);
    } else if (is_pool_or_lava(dig_x, dig_y)) {
        pline_The("%s sloshes furiously for a moment, then subsides.",
                  hliquid(is_lava(dig_x, dig_y) ? "lava" : "water"));
        wake_nearby(FALSE); /* splashing */

    } else if (old_typ == DRAWBRIDGE_DOWN
               || (is_drawbridge_wall(dig_x, dig_y) >= 0)) {
        /* drawbridge_down is the platform crossing the moat when the
           bridge is extended; drawbridge_wall is the open "doorway" or
           closed "door" where the portcullis/mechanism is located */
        if (pit_only) {
            pline_The("drawbridge seems too hard to dig through.");
        } else {
            coordxy x = dig_x, y = dig_y;
            /* if under the portcullis, the bridge is adjacent */
            (void) find_drawbridge(&x, &y);
            destroy_drawbridge(x, y);
            retval = TRUE;
        }

    } else if ((boulder_here = sobj_at(BOULDER, dig_x, dig_y)) != 0) {
        if (ttmp && is_pit(ttmp->ttyp)
            && rn2(2)) {
            pline_The("boulder settles into the %spit.",
                      (dig_x != u.ux || dig_y != u.uy) ? "adjacent " : "");
            ttmp->ttyp = PIT; /* crush spikes */
        } else {
            /*
             * digging makes a hole, but the boulder immediately
             * fills it.  Final outcome:  no hole, no boulder.
             */
            Soundeffect(se_kadoom_boulder_falls_in, 60);
            pline("KADOOM!  The boulder falls in!");
            wake_nearby(FALSE);
            (void) delfloortrap(ttmp);
        }
        delobj(boulder_here);
    } else if (IS_GRAVE(old_typ)) {
        digactualhole(dig_x, dig_y, BY_YOU, PIT);
        dig_up_grave(cc);
        retval = TRUE;
    } else if (old_typ == DRAWBRIDGE_UP) {
        /* must be floor or ice, other cases handled above */
        /* dig "pit" and let fluid flow in (if possible) */
        typ = fillholetyp(dig_x, dig_y, FALSE);

        if (typ == ROOM) {
            /*
             * We can't dig a hole here since that will destroy
             * the drawbridge.  The following is a cop-out. --dlc
             */
            pline_The("%s %shere is too hard to dig in.",
                      surface(dig_x, dig_y),
                      (dig_x != u.ux || dig_y != u.uy) ? "t" : "");
        } else {
            lev->drawbridgemask &= ~DB_UNDER;
            lev->drawbridgemask |= (typ == LAVAPOOL) ? DB_LAVA : DB_MOAT;
            liquid_flow(dig_x, dig_y, typ, ttmp,
                        "As you dig, the hole fills with %s!");
            retval = TRUE;
        }

    /* the following two are here for the wand of digging */
    } else if (IS_THRONE(old_typ)) {
        pline_The("throne is too hard to break apart.");

    } else if (IS_ALTAR(old_typ)) {
        pline_The("altar is too hard to break apart.");

    } else {
        typ = fillholetyp(dig_x, dig_y, FALSE);

        lev->flags = 0;
        if (typ != ROOM) {
            if (!furniture_handled((int) dig_x, (int) dig_y, TRUE)) {
                lev->typ = typ;
                liquid_flow(dig_x, dig_y, typ, ttmp,
                            "As you dig, the hole fills with %s!");
            }
            retval = TRUE;
        } else {
            /* magical digging disarms settable traps */
            if (by_magic && ttmp
                && (ttmp->ttyp == LANDMINE || ttmp->ttyp == BEAR_TRAP)) {
                int otyp = (ttmp->ttyp == LANDMINE) ? LAND_MINE : BEARTRAP;

                /* convert trap into buried object (deletes trap) */
                cnv_trap_obj(otyp, 1, ttmp, TRUE);
            }

            /* finally we get to make a hole */
            if (nohole || pit_only
                || dig_check_result == DIGCHECK_PASSED_DESTROY_TRAP
                || dig_check_result == DIGCHECK_PASSED_PITONLY)
                digactualhole(dig_x, dig_y, BY_YOU, PIT);
            else
                digactualhole(dig_x, dig_y, BY_YOU, HOLE);
            retval = TRUE;
        }
    }
    spot_checks(dig_x, dig_y, old_typ);
    return retval;
}

staticfn void
dig_up_grave(coord *cc)
{
    struct obj *otmp;
    int what_happens;
    coordxy dig_x, dig_y;

    if (!cc) {
        dig_x = u.ux;
        dig_y = u.uy;
    } else {
        dig_x = cc->x;
        dig_y = cc->y;
        if (!isok(dig_x, dig_y))
            return;
    }

    /* Grave-robbing is frowned upon... */
    exercise(A_WIS, FALSE);
    if (Role_if(PM_ARCHEOLOGIST)) {
        adjalign(-sgn(u.ualign.type) * 3);
        You_feel("like a despicable grave-robber!");
    } else if (Role_if(PM_SAMURAI)) {
        adjalign(-sgn(u.ualign.type));
        You("disturb the honorable dead!");
    } else if (u.ualign.type == A_LAWFUL) {
        if (u.ualign.record > -10)
            adjalign(-1);
        You("have violated the sanctity of this grave!");
    }

    /* -1: force default case for empty grave */
    what_happens = levl[dig_x][dig_y].emptygrave ? -1 : rn2(5);
    switch (what_happens) {
    case 0:
    case 1:
        You("unearth a corpse.");
        if ((otmp = mk_tt_object(CORPSE, dig_x, dig_y)) != 0)
            otmp->age -= (TAINT_AGE + 1); /* this is an *OLD* corpse */
        break;
    case 2:
        if (!Blind)
            pline("%s!", Hallucination ? "Dude!  The living dead"
                                       : "The grave's owner is very upset");
        (void) makemon(mkclass(S_ZOMBIE, 0), dig_x, dig_y, MM_NOMSG);
        break;
    case 3:
        if (!Blind)
            pline("%s!", Hallucination ? "I want my mummy"
                                       : "You've disturbed a tomb");
        (void) makemon(mkclass(S_MUMMY, 0), dig_x, dig_y, MM_NOMSG);
        break;
    default:
        /* No corpse */
        pline_The("grave is unoccupied.  Strange...");
        break;
    }
    levl[dig_x][dig_y].typ = ROOM;
    levl[dig_x][dig_y].emptygrave = 0; /* clear 'flags' */
    levl[dig_x][dig_y].disturbed = 0; /* clear 'horizontal' */
    del_engr_at(dig_x, dig_y);
    newsym(dig_x, dig_y);
    return;
}

int
use_pick_axe(struct obj *obj)
{
    const char *verb;
    char *dsp, dirsyms[12], qbuf[BUFSZ];
    boolean ispick;
    int rx, ry, downok, res = ECMD_OK;
    int dir;

    /* Check tool */
    if (obj != uwep) {
        if (wield_tool(obj, "swing")) {
            /* we're now wielding it. next turn, apply to dig. */
            cmdq_add_ec(CQ_CANNED, doapply);
            cmdq_add_key(CQ_CANNED, obj->invlet);
            return ECMD_TIME;
        }
        return ECMD_OK;
    }
    ispick = is_pick(obj);
    verb = ispick ? "dig" : "chop";

    if (u.utrap && u.utraptype == TT_WEB) {
        pline("%s you can't %s while entangled in a web.",
              /* res==0 => no prior message;
                 res==1 => just got "You now wield a pick-axe." message */
              !res ? "Unfortunately," : "But", verb);
        return res;
    }

    /* construct list of directions to show player for likely choices */
    downok = !!can_reach_floor(FALSE);
    dsp = dirsyms;
    for (dir = 0; dir < N_DIRS_Z; dir++) {
        char dirch = cmd_from_dir(dir, MV_WALK);

        /* filter out useless directions */
        if (u.uswallow) {
            ; /* all directions are viable when swallowed */
        } else if (movecmd(dirch, MV_WALK)) {
            /* normal direction, within plane of the level map */
            if (!dxdy_moveok())
                continue; /* handle NODIAG */
            rx = u.ux + u.dx;
            ry = u.uy + u.dy;
            if (!isok(rx, ry) || dig_typ(obj, rx, ry) == DIGTYP_UNDIGGABLE)
                continue;
        } else {
            /* up or down; we used to always include down, so that
               there would always be at least one choice shown, but
               it shouldn't be a likely candidate when floating high
               above the floor; include up instead in that situation
               (as a silly candidate rather than a likely one...) */
            if ((u.dz > 0) ^ downok)
                continue;
        }
        /* include this direction */
        *dsp++ = dirch;
    }
    *dsp = 0;
    Sprintf(qbuf, "In what direction do you want to %s? [%s]", verb, dirsyms);
    if (!getdir(qbuf))
        return (res|ECMD_CANCEL);

    return use_pick_axe2(obj);
}

/* MRKR: use_pick_axe() is split in two to allow autodig to bypass */
/*       the "In what direction do you want to dig?" query.        */
/*       use_pick_axe2() uses the existing u.dx, u.dy and u.dz    */
int
use_pick_axe2(struct obj *obj)
{
    coordxy rx, ry;
    struct rm *lev;
    struct trap *trap, *trap_with_u;
    int dig_target;
    boolean ispick = is_pick(obj);
    const char *verbing = ispick ? "digging" : "chopping";

    if (u.uswallow && do_attack(u.ustuck)) {
        ; /* return 1 */
    } else if (Underwater) {
        pline("Turbulence torpedoes your %s attempts.", verbing);
    } else if (u.dz < 0) {
        if (Levitation)
            You("don't have enough leverage.");
        else
            You_cant("reach the %s.", ceiling(u.ux, u.uy));
    } else if (!u.dx && !u.dy && !u.dz) {
        char buf[BUFSZ];
        int dam;

        dam = rnd(2) + dbon() + obj->spe;
        if (dam <= 0)
            dam = 1;
        You("hit yourself with %s.", yname(uwep));
        Sprintf(buf, "%s own %s", uhis(), OBJ_NAME(objects[obj->otyp]));
        losehp(Maybe_Half_Phys(dam), buf, KILLED_BY);
        disp.botl = TRUE;
        return ECMD_TIME;
    } else if (u.dz == 0) {
        confdir(FALSE);
        rx = u.ux + u.dx;
        ry = u.uy + u.dy;
        if (!isok(rx, ry)) {
            Soundeffect(se_clash, 40);
            pline("Clash!");
            return ECMD_TIME;
        }
        lev = &levl[rx][ry];
        if (MON_AT(rx, ry) && do_attack(m_at(rx, ry)))
            return ECMD_TIME;
        dig_target = dig_typ(obj, rx, ry);
        if (dig_target == DIGTYP_UNDIGGABLE) {
            struct obj *boulder;

            /* ACCESSIBLE or POOL */
            trap = t_at(rx, ry);
            if (trap && trap->ttyp == WEB) {
                if (!trap->tseen) {
                    seetrap(trap);
                    There("is a spider web there!");
                }
                pline("%s entangled in the web.", Yobjnam2(obj, "become"));
                /* you ought to be able to let go; tough luck */
                /* (maybe `move_into_trap()' would be better) */
                nomul(-d(2, 2));
                gm.multi_reason = "stuck in a spider web";
                gn.nomovemsg = "You pull free.";
            } else if (lev->typ == IRONBARS) {
                pline("Clang!");
                wake_nearby(FALSE);
            } else if (IS_WATERWALL(lev->typ)) {
                pline("Splash!");
            } else if (lev->typ == LAVAWALL) {
                pline("Splash!");
                (void) fire_damage(uwep, FALSE, rx, ry);
            } else if (IS_TREE(lev->typ)) {
                You("need an axe to cut down a tree.");
            } else if (IS_OBSTRUCTED(lev->typ)) {
                You("need a pick to dig rock.");
            } else if ((boulder = sobj_at(BOULDER, rx, ry)) != 0
                       || sobj_at(STATUE, rx, ry)) {
                /* if both boulders and statues are present, the topmost
                   boulder will be shown on the map so treat it as target */
                const char *what = boulder ? "boulder" : "statue";

                if (!ispick) {
                    boolean vibrate = !rn2(3);

                    pline("Sparks fly as you whack the %s.%s", what,
                          vibrate ? "  The axe-handle vibrates violently!"
                                  : "");
                    if (vibrate)
                        losehp(Maybe_Half_Phys(2), "axing a hard object",
                               KILLED_BY);
                    wake_nearby(FALSE);
                } else {
                    /* using a pick but dig_target is DIGTYPE_UNDIGGABLE
                       and there is at least one boulder or statue or both
                       present; pick_can_reach() returned false */
                    You_cant("reach the %s.", what);
                }
            } else if (u.utrap && u.utraptype == TT_PIT && trap
                       && (trap_with_u = t_at(u.ux, u.uy))
                       && is_pit(trap->ttyp)
                       && !conjoined_pits(trap, trap_with_u, FALSE)) {
                int idx = xytod(u.dx, u.dy);

                if (idx != DIR_ERR) {
                    int adjidx = DIR_180(idx);

                    trap_with_u->conjoined |= (1 << idx);
                    trap->conjoined |= (1 << adjidx);
                    You("clear some debris from between the pits.");
                }
            } else if (u.utrap && u.utraptype == TT_PIT
                       && (trap_with_u = t_at(u.ux, u.uy)) != 0) {
                You("swing %s, but the rubble has no place to go.",
                    yobjnam(obj, (char *) 0));
            } else {
                You("swing %s through thin air.", yobjnam(obj, (char *) 0));
            }
        } else {
            static const char *const d_action[6] = { "swinging", "digging",
                                                     "chipping the statue",
                                                     "hitting the boulder",
                                                     "chopping at the door",
                                                     "cutting the tree" };

            gd.did_dig_msg = FALSE;
            svc.context.digging.quiet = FALSE;
            if (svc.context.digging.pos.x != rx
                || svc.context.digging.pos.y != ry
                || !on_level(&svc.context.digging.level, &u.uz)
                || svc.context.digging.down) {
                if (flags.autodig && dig_target == DIGTYP_ROCK
                    && !svc.context.digging.down
                    && u_at(svc.context.digging.pos.x,
                            svc.context.digging.pos.y)
                    && (svm.moves <= svc.context.digging.lastdigtime + 2
                        && svm.moves >= svc.context.digging.lastdigtime)) {
                    /* avoid messages if repeated autodigging */
                    gd.did_dig_msg = TRUE;
                    svc.context.digging.quiet = TRUE;
                }
                svc.context.digging.down = svc.context.digging.chew = FALSE;
                svc.context.digging.warned = FALSE;
                svc.context.digging.pos.x = rx;
                svc.context.digging.pos.y = ry;
                assign_level(&svc.context.digging.level, &u.uz);
                svc.context.digging.effort = 0;
                if (!svc.context.digging.quiet)
                    You("start %s.", d_action[dig_target]);
            } else {
                You("%s %s.", svc.context.digging.chew ? "begin" : "continue",
                    d_action[dig_target]);
                svc.context.digging.chew = FALSE;
            }
            set_occupation(dig, verbing, 0);
        }
    } else if (Is_airlevel(&u.uz) || Is_waterlevel(&u.uz)) {
        /* it must be air -- water checked above */
        You("swing %s through thin air.", yobjnam(obj, (char *) 0));
    } else if (!can_reach_floor(FALSE)) {
        cant_reach_floor(u.ux, u.uy, FALSE, FALSE, FALSE);
    } else if (is_pool_or_lava(u.ux, u.uy)) {
        /* Monsters which swim also happen not to be able to dig */
        You("cannot stay under%s long enough.",
            is_pool(u.ux, u.uy) ? "water" : " the lava");
    } else if ((trap = t_at(u.ux, u.uy)) != 0
               && (uteetering_at_seen_pit(trap) || uescaped_shaft(trap))) {
        dotrap(trap, FORCEBUNGLE);
        /* might escape trap and still be teetering at brink */
        if (!u.utrap)
            cant_reach_floor(u.ux, u.uy, FALSE, TRUE, FALSE);
    } else if (!ispick
               /* can only dig down with an axe when doing so will
                  trigger or disarm a trap here */
               && (!trap || (trap->ttyp != LANDMINE
                             && trap->ttyp != BEAR_TRAP))) {
        pline("%s merely scratches the %s.", Yobjnam2(obj, (char *) 0),
              surface(u.ux, u.uy));
        u_wipe_engr(3);
    } else {
        if (svc.context.digging.pos.x != u.ux
            || svc.context.digging.pos.y != u.uy
            || !on_level(&svc.context.digging.level, &u.uz)
            || !svc.context.digging.down) {
            svc.context.digging.chew = FALSE;
            svc.context.digging.down = TRUE;
            svc.context.digging.warned = FALSE;
            svc.context.digging.pos.x = u.ux;
            svc.context.digging.pos.y = u.uy;
            assign_level(&svc.context.digging.level, &u.uz);
            svc.context.digging.effort = 0;
            You("start %s downward.", verbing);
            if (*u.ushops) {
                shopdig(0);
                add_damage(u.ux, u.uy, SHOP_PIT_COST);
            }
        } else
            You("continue %s downward.", verbing);
        gd.did_dig_msg = FALSE;
        set_occupation(dig, verbing, 0);
    }
    return ECMD_TIME;
}

staticfn boolean
watchman_canseeu(struct monst *mtmp)
{
    if (is_watch(mtmp->data) && mtmp->mcansee && m_canseeu(mtmp)
        && mtmp->mpeaceful)
        return TRUE;
    return FALSE;
}

/*
 * Town Watchmen frown on damage to the town walls, trees or fountains.
 * It's OK to dig holes in the ground, however.
 * If mtmp is assumed to be a watchman, a watchman is found if mtmp == 0
 * zap == TRUE if wand/spell of digging, FALSE otherwise (chewing)
 */
void
watch_dig(struct monst *mtmp, coordxy x, coordxy y, boolean zap)
{
    struct rm *lev = &levl[x][y];

    if (in_town(x, y)
        && (closed_door(x, y) || lev->typ == SDOOR || IS_WALL(lev->typ)
            || IS_FOUNTAIN(lev->typ) || IS_TREE(lev->typ))) {
        if (!mtmp)
            mtmp = get_iter_mons(watchman_canseeu);

        if (mtmp) {
            SetVoice(mtmp, 0, 80, 0);
            if (zap || svc.context.digging.warned) {
                verbalize("Halt, vandal!  You're under arrest!");
                (void) angry_guards(!!Deaf);
            } else {
                const char *str;

                if (IS_DOOR(lev->typ))
                    str = "door";
                else if (IS_TREE(lev->typ))
                    str = "tree";
                else if (IS_OBSTRUCTED(lev->typ))
                    str = "wall";
                else
                    str = "fountain";
                verbalize("Hey, stop damaging that %s!", str);
                svc.context.digging.warned = TRUE;
            }
            if (is_digging())
                stop_occupation();
        }
    }
}

/* Return TRUE if monster died, FALSE otherwise.  Called from m_move(). */
boolean
mdig_tunnel(struct monst *mtmp)
{
    struct rm *here;
    boolean sawit, seeit, trapped;
    int pile = rnd(12);

    here = &levl[mtmp->mx][mtmp->my];
    if (here->typ == SDOOR)
        cvt_sdoor_to_door(here); /* ->typ = DOOR */

    /* Eats away door if present & closed or locked */
    if (closed_door(mtmp->mx, mtmp->my)) {
        if (*in_rooms(mtmp->mx, mtmp->my, SHOPBASE))
            add_damage(mtmp->mx, mtmp->my, 0L);
        /* sawit: closed door location is more visible than an open one */
        sawit = canseemon(mtmp); /* before door state change and unblock_pt */
        trapped = (here->doormask & D_TRAPPED) ? TRUE : FALSE;
        here->doormask = trapped ? D_NODOOR : D_BROKEN;
        recalc_block_point(mtmp->mx, mtmp->my); /* vision */
        newsym(mtmp->mx, mtmp->my);
        if (trapped) {
            seeit = canseemon(mtmp);
            if (mb_trapped(mtmp, sawit || seeit)) { /* mtmp is killed */
                newsym(mtmp->mx, mtmp->my);
                return TRUE;
            }
        } else {
            if (flags.verbose) {
                if (!Unaware && !rn2(3)) /* not too often.. */
                    draft_message(TRUE); /* "You feel an unexpected draft." */
            }
        }
        return FALSE;
    } else if (here->typ == SCORR) {
        here->typ = CORR, here->flags = 0;
        unblock_point(mtmp->mx, mtmp->my);
        newsym(mtmp->mx, mtmp->my);
        draft_message(FALSE); /* "You feel a draft." */
        return FALSE;
    } else if (!IS_OBSTRUCTED(here->typ) && !IS_TREE(here->typ)) { /* no dig */
        return FALSE;
    }

    /* Only rock, trees, and walls fall through to this point. */
    if ((here->wall_info & W_NONDIGGABLE) != 0) {
        impossible("mdig_tunnel:  %s at (%d,%d) is undiggable",
                   (IS_WALL(here->typ) ? "wall"
                    : IS_TREE(here->typ) ? "tree" : "stone"),
                   (int) mtmp->mx, (int) mtmp->my);
        return FALSE; /* still alive */
    }

    if (IS_WALL(here->typ)) {
        /* KMH -- Okay on arboreal levels (room walls are still stone) */
        if (flags.verbose && !rn2(5)) {
            Soundeffect(se_crashing_rock, 75);
            You_hear("crashing rock.");
        }
        if (*in_rooms(mtmp->mx, mtmp->my, SHOPBASE))
            add_damage(mtmp->mx, mtmp->my, 0L);
        if (svl.level.flags.is_maze_lev) {
            here->typ = ROOM, here->flags = 0;
        } else if (svl.level.flags.is_cavernous_lev
                   && !in_town(mtmp->mx, mtmp->my)) {
            here->typ = CORR, here->flags = 0;
        } else {
            here->typ = DOOR, here->doormask = D_NODOOR;
        }
    } else if (IS_TREE(here->typ)) {
        here->typ = ROOM, here->flags = 0;
        if (pile && pile < 5)
            (void) rnd_treefruit_at(mtmp->mx, mtmp->my);
    } else {
        here->typ = CORR, here->flags = 0;
        if (pile && pile < 5)
            (void) mksobj_at((pile == 1) ? BOULDER : ROCK, mtmp->mx, mtmp->my,
                             TRUE, FALSE);
    }
    newsym(mtmp->mx, mtmp->my);
    if (!sobj_at(BOULDER, mtmp->mx, mtmp->my))
        unblock_point(mtmp->mx, mtmp->my); /* vision */

    return FALSE;
}

#define STRIDENT 4 /* from pray.c */

/* draft refers to air currents, but can be a pun on "draft" as conscription
   for military service (probably not a good pun if it has to be explained) */
void
draft_message(boolean unexpected)
{
    /*
     * [Bug or TODO?  Have caller pass coordinates and use the travel
     * mechanism to determine whether there is a path between
     * destroyed door (or exposed secret corridor) and hero's location.
     * When there is no such path, no draft should be felt.]
     */

    if (unexpected) {
        if (!Hallucination)
            You_feel("an unexpected draft.");
        else
            /* U.S. classification system uses 1-A for eligible to serve
               and 4-F for ineligible due to physical or mental defect;
               some intermediate values exist but are rarely seen */
            You_feel("like you are %s.",
                     (ACURR(A_STR) < 6 || ACURR(A_DEX) < 6
                      || ACURR(A_CON) < 6 || ACURR(A_CHA) < 6
                      || ACURR(A_INT) < 6 || ACURR(A_WIS) < 6) ? "4-F"
                                                               : "1-A");
    } else {
        if (!Hallucination) {
            You_feel("a draft.");
        } else {
            /* "marching" is deliberately ambiguous; it might mean drills
                after entering military service or mean engaging in protests */
            static const char *const draft_reaction[] = {
                "enlisting", "marching", "protesting", "fleeing",
            };
            int dridx;

            /* Lawful: 0..1, Neutral: 1..2, Chaotic: 2..3 */
            dridx = rn1(2, 1 - sgn(u.ualign.type));
            if (u.ualign.record < STRIDENT)
                /* L: +(0..2), N: +(-1..1), C: +(-2..0); all: 0..3 */
                dridx += rn1(3, sgn(u.ualign.type) - 1);
            You_feel("like %s.", draft_reaction[dridx]);
        }
    }
}

/* digging via wand zap or spell cast */
void
zap_dig(void)
{
    struct rm *room;
    struct monst *mtmp;
    struct obj *otmp;
    struct trap *trap_with_u = (struct trap *) 0;
    coordxy zx, zy, flow_x = -1, flow_y = -1;
    int diridx = 8, digdepth;
    boolean shopdoor, shopwall, maze_dig, pitdig = FALSE, pitflow = FALSE;

    /*
     * Original effect (approximately):
     * from CORR: dig until we pierce a wall
     * from ROOM: pierce wall and dig until we reach
     * an ACCESSIBLE place.
     * Currently: dig for digdepth positions;
     * also down on request of Lennart Augustsson.
     * 3.6.0: from a PIT: dig one adjacent pit.
     */

    if (u.uswallow) {
        mtmp = u.ustuck;

        if (!is_whirly(mtmp->data)) {
            if (digests(mtmp->data))
                You("pierce %s %s wall!", s_suffix(mon_nam(mtmp)),
                    mbodypart(mtmp, STOMACH));
            if (unique_corpstat(mtmp->data))
                mtmp->mhp = (mtmp->mhp + 1) / 2;
            else
                mtmp->mhp = 1; /* almost dead */
            expels(mtmp, mtmp->data, !digests(mtmp->data));
        }
        return;
    } /* swallowed */

    if (u.dz) {
        if (!Is_airlevel(&u.uz) && !Is_waterlevel(&u.uz) && !Underwater) {
            if (u.dz < 0 || On_stairs(u.ux, u.uy)) {
                int dmg;
                if (On_stairs(u.ux, u.uy)) {
                    stairway *stway = stairway_at(u.ux, u.uy);
                    pline_The("beam bounces off the %s and hits the %s.",
                              stway->isladder ? "ladder" : "stairs",
                              ceiling(u.ux, u.uy));
                }
                You("loosen a rock from the %s.", ceiling(u.ux, u.uy));
                pline("It falls on your %s!", body_part(HEAD));
                dmg = rnd(hard_helmet(uarmh) ? 2 : 6);
                losehp(Maybe_Half_Phys(dmg), "falling rock", KILLED_BY_AN);
                otmp = mksobj_at(ROCK, u.ux, u.uy, FALSE, FALSE);
                if (otmp) {
                    (void) xname(otmp); /* set dknown, maybe bknown */
                    stackobj(otmp);
                }
                newsym(u.ux, u.uy);
            } else {
                watch_dig((struct monst *) 0, u.ux, u.uy, TRUE);
                (void) dighole(FALSE, TRUE, (coord *) 0);
            }
        }
        return;
    } /* up or down */

    /* normal case: digging across the level */
    shopdoor = shopwall = FALSE;
    maze_dig = svl.level.flags.is_maze_lev && !Is_earthlevel(&u.uz);
    zx = u.ux + u.dx;
    zy = u.uy + u.dy;
    if (u.utrap && u.utraptype == TT_PIT
        && (trap_with_u = t_at(u.ux, u.uy))) {
        pitdig = TRUE;
        diridx = xytod(u.dx, u.dy);
    }
    digdepth = rn1(18, 8);
    tmp_at(DISP_BEAM, cmap_to_glyph(S_digbeam));
    while (--digdepth >= 0) {
        if (!isok(zx, zy))
            break;
        room = &levl[zx][zy];
        tmp_at(zx, zy);
        nh_delay_output(); /* wait a little bit */

        if (pitdig) { /* we are already in a pit if this is true */
            coord cc;
            struct trap *adjpit = t_at(zx, zy);

            if (diridx != DIR_ERR
                && !conjoined_pits(adjpit, trap_with_u, FALSE)) {
                digdepth = 0; /* limited to the adjacent location only */
                nhUse(digdepth);
                if (!(adjpit && is_pit(adjpit->ttyp))) {
                    char buf[BUFSZ];

                    cc.x = zx;
                    cc.y = zy;
                    if (!adj_pit_checks(&cc, buf)) {
                        if (buf[0])
                            pline1(buf);
                    } else {
                        /* this can also result in a pool at zx,zy */
                        dighole(TRUE, TRUE, &cc);
                        adjpit = t_at(zx, zy);
                    }
                }
                if (adjpit && is_pit(adjpit->ttyp)) {
                    int adjidx = DIR_180(diridx);

                    trap_with_u->conjoined |= (1 << diridx);
                    adjpit->conjoined |= (1 << adjidx);
                    flow_x = zx;
                    flow_y = zy;
                    pitflow = TRUE;
                }
                if (is_pool(zx, zy) || is_lava(zx, zy)) {
                    flow_x = zx - u.dx;
                    flow_y = zy - u.dy;
                    pitflow = TRUE;
                }
                break;
            }
        } else if (closed_door(zx, zy) || room->typ == SDOOR) {
            if (*in_rooms(zx, zy, SHOPBASE)) {
                add_damage(zx, zy, SHOP_DOOR_COST);
                shopdoor = TRUE;
            }
            if (room->typ == SDOOR)
                room->typ = DOOR; /* doormask set below */
            else if (cansee(zx, zy))
                pline_The("door is razed!");
            watch_dig((struct monst *) 0, zx, zy, TRUE);
            room->doormask = D_NODOOR;
            recalc_block_point(zx, zy); /* vision */
            digdepth -= 2;
            if (maze_dig)
                break;
        } else if (maze_dig) {
            if (IS_WALL(room->typ)) {
                if (!(room->wall_info & W_NONDIGGABLE)) {
                    if (*in_rooms(zx, zy, SHOPBASE)) {
                        add_damage(zx, zy, SHOP_WALL_COST);
                        shopwall = TRUE;
                    }
                    room->typ = ROOM, room->flags = 0;
                    unblock_point(zx, zy); /* vision */
                } else if (!Blind)
                    pline_The("wall glows then fades.");
                break;
            } else if (IS_TREE(room->typ)) { /* check trees before stone */
                if (!(room->wall_info & W_NONDIGGABLE)) {
                    room->typ = ROOM, room->flags = 0;
                    unblock_point(zx, zy); /* vision */
                } else if (!Blind)
                    pline_The("tree shudders but is unharmed.");
                break;
            } else if (room->typ == STONE || room->typ == SCORR) {
                if (!(room->wall_info & W_NONDIGGABLE)) {
                    room->typ = CORR, room->flags = 0;
                    unblock_point(zx, zy); /* vision */
                } else if (!Blind)
                    pline_The("rock glows then fades.");
                break;
            }
        } else if (IS_OBSTRUCTED(room->typ)) {
            if (!may_dig(zx, zy))
                break;
            if (IS_WALL(room->typ) || room->typ == SDOOR) {
                if (*in_rooms(zx, zy, SHOPBASE)) {
                    add_damage(zx, zy, SHOP_WALL_COST);
                    shopwall = TRUE;
                }
                watch_dig((struct monst *) 0, zx, zy, TRUE);
                if (svl.level.flags.is_cavernous_lev && !in_town(zx, zy)) {
                    room->typ = CORR, room->flags = 0;
                } else {
                    room->typ = DOOR, room->doormask = D_NODOOR;
                }
                digdepth -= 2;
            } else if (IS_TREE(room->typ)) {
                room->typ = ROOM, room->flags = 0;
                digdepth -= 2;
            } else { /* IS_OBSTRUCTED but not IS_WALL or SDOOR */
                room->typ = CORR, room->flags = 0;
                digdepth--;
            }
            unblock_point(zx, zy); /* vision */
        }
        zx += u.dx;
        zy += u.dy;
    }                    /* while */
    tmp_at(DISP_END, 0); /* closing call */

    if (pitflow && isok(flow_x, flow_y)) {
        struct trap *ttmp = t_at(flow_x, flow_y);

        if (ttmp && is_pit(ttmp->ttyp)) {
            schar filltyp = fillholetyp(ttmp->tx, ttmp->ty, TRUE);

            if (filltyp != ROOM)
                pit_flow(ttmp, filltyp);
        }
    }

    if (shopdoor || shopwall)
        pay_for_damage(shopdoor ? "destroy" : "dig into", FALSE);
    return;
}

/*
 * This checks what is on the surface above the
 * location where an adjacent pit might be created if
 * you're zapping a wand of digging laterally while
 * down in the pit.
 */
staticfn int
adj_pit_checks(coord *cc, char *msg)
{
    int ltyp;
    struct rm *room;
    const char *foundation_msg =
                 "The foundation is too hard to dig through from this angle.";

    if (!cc)
        return FALSE;
    if (!isok(cc->x, cc->y))
        return FALSE;
    *msg = '\0';
    room = &levl[cc->x][cc->y];
    ltyp = room->typ, room->flags = 0;

    if (is_pool(cc->x, cc->y) || is_lava(cc->x, cc->y)) {
        /* this is handled by the caller after we return FALSE */
        return FALSE;
    } else if (closed_door(cc->x, cc->y) || room->typ == SDOOR) {
        /* We reject this here because dighole() isn't
           prepared to deal with this case */
        Strcpy(msg, foundation_msg);
        return FALSE;
    } else if (IS_WALL(ltyp)) {
        /* if (room->wall_info & W_NONDIGGABLE) */
        Strcpy(msg, foundation_msg);
        return FALSE;
    } else if (IS_TREE(ltyp)) { /* check trees before stone */
        /* if (room->wall_info & W_NONDIGGABLE) */
        Strcpy(msg, "The tree's roots glow then fade.");
        return FALSE;
    } else if (ltyp == STONE || ltyp == SCORR) {
        if (room->wall_info & W_NONDIGGABLE) {
            Strcpy(msg, "The rock glows then fades.");
            return FALSE;
        }
    } else if (ltyp == IRONBARS) {
        /* "set of iron bars" */
        Strcpy(msg, "The bars go much deeper than your pit.");
        return FALSE;
#if 0
    } else if (is_lava(cc->x, cc->y)) {
    } else if (is_ice(cc->x, cc->y)) {
    } else if (is_pool(cc->x, cc->y)) {
    } else if (IS_GRAVE(ltyp)) {
#endif
    } else if (IS_SINK(ltyp)) {
        Strcpy(msg, "A tangled mass of plumbing remains below the sink.");
        return FALSE;
    } else if (On_ladder(cc->x, cc->y)) {
        Strcpy(msg, "The ladder is unaffected.");
        return FALSE;
    } else {
        const char *supporting = (const char *) 0;

        if (IS_FOUNTAIN(ltyp))
            supporting = "fountain";
        else if (IS_THRONE(ltyp))
            supporting = "throne";
        else if (IS_ALTAR(ltyp))
            supporting = "altar";
        else if (On_stairs(cc->x, cc->y))
            /* staircase up or down. On_ladder handled above. */
            supporting = "stairs";
        else if (ltyp == DRAWBRIDGE_DOWN   /* "lowered drawbridge" */
                 || ltyp == DBWALL)        /* "raised drawbridge" */
            supporting = "drawbridge";

        if (supporting) {
            Sprintf(msg, "The %s supporting structures remain intact.",
                    s_suffix(supporting));
            return FALSE;
        }
    }
    return TRUE;
}

/*
 * Ensure that all conjoined pits fill up.
 */
staticfn void
pit_flow(struct trap *trap, schar filltyp)
{
    /*
     * FIXME?
     *  liquid_flow() -> pooleffects() -> {drown(),lava_effects()}
     *  might kill the hero; the game will end and if that leaves bones,
     *  remaining conjoined pits will be left unprocessed.
     */
    if (trap && filltyp != ROOM && is_pit(trap->ttyp)) {
        struct trap t;
        int idx;

        t = *trap;
        levl[t.tx][t.ty].typ = filltyp, levl[t.tx][t.ty].flags = 0;
        liquid_flow(t.tx, t.ty, filltyp, trap,
                    u_at(t.tx, t.ty)
                        ? "Suddenly %s flows in from the adjacent pit!"
                        : (char *) 0);
        for (idx = 0; idx < N_DIRS; ++idx) {
            if (t.conjoined & (1 << idx)) {
                coordxy x, y;
                struct trap *t2;

                x = t.tx + xdir[idx];
                y = t.ty + ydir[idx];
                t2 = t_at(x, y);
#if 0
                /* cannot do this back-check; liquid_flow()
                 * called deltrap() which cleaned up the
                 * conjoined fields on both pits.
                 */
                if (t2 && (t2->conjoined & (1 << DIR_180(idx))))
#endif
                /* recursion */
                pit_flow(t2, filltyp);
            }
        }
    }
}

struct obj *
buried_ball(coord *cc)
{
    int odist, bdist = COLNO;
    struct obj *otmp, *ball = 0;

    /* FIXME:
     *  This is just approximate; if multiple buried balls meet the
     *  criterium (within 2 steps of tethered hero's present location)
     *  it will find an arbitrary one rather than the one which used
     *  to be uball.  Once 3.6.{0,1} save file compatibility is broken,
     *  we should add svc.context.buriedball_oid and then we can find the
     *  actual former uball, which might be extra heavy or christened
     *  or not the one buried directly underneath the target spot.
     *
     *  [Why does this search within a radius of two when trapmove()
     *  only lets hero get one step away from the buried ball?]
     */

    /* u.utrap might have already been cleared, in which case the value
       of u.utraptype is no longer meaningful; if u.utrap is still set
       then u.utraptype needs to be for buried ball */
    if (!u.utrap || u.utraptype == TT_BURIEDBALL) {
        for (otmp = svl.level.buriedobjlist; otmp; otmp = otmp->nobj) {
            if (otmp->otyp != HEAVY_IRON_BALL)
                continue;
            /* if found at the target spot, we're done */
            if (otmp->ox == cc->x && otmp->oy == cc->y)
                return otmp;
            /* find nearest within allowable vicinity: +/-2
             *  4 5 8
             *  1 2 5
             *  0 1 4
             */
            odist = dist2(otmp->ox, otmp->oy, cc->x, cc->y);
            if (odist <= 8 && (!ball || odist < bdist)) {
                /* remember nearest buried ball but keep checking others */
                ball = otmp;
                bdist = odist;
            }
        }
    }
    if (ball) {
        /* found, but not at < cc->x, cc->y > */
        cc->x = ball->ox;
        cc->y = ball->oy;
    }
    return ball;
}

void
buried_ball_to_punishment(void)
{
    coord cc;
    struct obj *ball;

    cc.x = u.ux;
    cc.y = u.uy;
    ball = buried_ball(&cc);
    if (ball) {
        obj_extract_self(ball);
#if 0
        /* rusting buried metallic objects is not implemented yet */
        if (ball->timed)
            (void) stop_timer(RUST_METAL, obj_to_any(ball));
#endif
        punish(ball); /* use ball as flag for unearthed buried ball */
        reset_utrap(FALSE);
        del_engr_at(cc.x, cc.y);
        newsym(cc.x, cc.y);
    }
}

void
buried_ball_to_freedom(void)
{
    coord cc;
    struct obj *ball;

    cc.x = u.ux;
    cc.y = u.uy;
    ball = buried_ball(&cc);
    if (ball) {
        obj_extract_self(ball);
#if 0
        /* rusting buried metallic objects is not implemented yet */
        if (ball->timed)
            (void) stop_timer(RUST_METAL, obj_to_any(ball));
#endif
        place_object(ball, cc.x, cc.y);
        stackobj(ball);
        reset_utrap(TRUE);
        del_engr_at(cc.x, cc.y);
        newsym(cc.x, cc.y);
    }
}

/* move objects from fobj/nexthere lists to buriedobjlist, keeping position
   information */
struct obj *
bury_an_obj(struct obj *otmp, boolean *dealloced)
{
    struct obj *otmp2;
    boolean under_ice;

    debugpline1("bury_an_obj: %s", xname(otmp));
    if (dealloced)
        *dealloced = FALSE;
    if (otmp == uball) {
        unpunish();
        set_utrap((unsigned) rn1(50, 20), TT_BURIEDBALL);
        pline_The("iron ball gets buried!");
    }
    /* after unpunish(), or might get deallocated chain */
    otmp2 = otmp->nexthere;
    /*
     * obj_resists(,0,0) prevents Rider corpses from being buried.
     * It also prevents The Amulet and invocation tools from being
     * buried.  Since they can't be confined to bags and statues,
     * it makes sense that they can't be buried either, even though
     * the real reason there (direct accessibility when carried) is
     * completely different.
     */
    if (otmp == uchain || obj_resists(otmp, 0, 0))
        return otmp2;

    if (otmp->otyp == LEASH && otmp->leashmon != 0)
        o_unleash(otmp);

    if (otmp->lamplit && otmp->otyp != POT_OIL)
        end_burn(otmp, TRUE);

    obj_extract_self(otmp);

    under_ice = is_ice(otmp->ox, otmp->oy);
    if ((otmp->otyp == ROCK && !under_ice) || otmp->otyp == BOULDER) {
        /* merges into burying material; boulder removal is for #wizbury */
        if (dealloced)
            *dealloced = TRUE;
        obfree(otmp, (struct obj *) 0);
        return otmp2;
    }
    /*
     * Start a rot on organic material.  Not corpses -- they
     * are already handled.
     */
    if (otmp->otyp == CORPSE) {
        ; /* should cancel timer if under_ice */
    } else if ((under_ice ? (otmp->oclass == POTION_CLASS) : is_organic(otmp))
               && !obj_resists(otmp, 5, 95)) {
        (void) start_timer((under_ice ? 0L : 250L) + (long) rnd(250),
                           TIMER_OBJECT, ROT_ORGANIC, obj_to_any(otmp));
#if 0
    /* rusting of buried metal not yet implemented */
    } else if (is_rustprone(otmp)) {
        (void) start_timer((long) rnd((otmp->otyp == HEAVY_IRON_BALL)
                                         ? 1500
                                         : 250),
                           TIMER_OBJECT, RUST_METAL, obj_to_any(otmp));
#endif
    }
    add_to_buried(otmp);
    return  otmp2;
}

void
bury_objs(int x, int y)
{
    struct obj *otmp, *otmp2;
    struct monst *shkp;
    long loss = 0L;
    boolean costly;

    costly = ((shkp = shop_keeper(*in_rooms(x, y, SHOPBASE)))
              && costly_spot(x, y));

    if (svl.level.objects[x][y] != (struct obj *) 0) {
        debugpline2("bury_objs: at <%d,%d>", x, y);
    }
    for (otmp = svl.level.objects[x][y]; otmp; otmp = otmp2) {
        if (costly && !svc.context.mon_moving) {
            loss += stolen_value(otmp, x, y, (boolean) shkp->mpeaceful, TRUE);
            if (otmp->oclass != COIN_CLASS)
                otmp->no_charge = 1;
        }
        otmp2 = bury_an_obj(otmp, (boolean *) 0);
    }

    /* don't expect any engravings here, but just in case */
    del_engr_at(x, y);
    newsym(x, y);
    maybe_unhide_at(x, y);

    if (costly && loss) {
        You("owe %s %ld %s for burying merchandise.", shkname(shkp), loss,
            currency(loss));
    }
}

/* move objects from buriedobjlist to fobj/nexthere lists; if caller
   converts terrain from ice to something, it should call obj_ice_effects() */
void
unearth_objs(int x, int y)
{
    struct obj *otmp, *otmp2, *bball;
    coord cc;

    debugpline2("unearth_objs: at <%d,%d>", x, y);
    cc.x = x;
    cc.y = y;
    bball = buried_ball(&cc);
    for (otmp = svl.level.buriedobjlist; otmp; otmp = otmp2) {
        otmp2 = otmp->nobj;
        if (otmp->ox == x && otmp->oy == y) {
            if (bball && otmp == bball
                && u.utrap && u.utraptype == TT_BURIEDBALL) {
                buried_ball_to_punishment();
            } else {
                obj_extract_self(otmp);
                if (otmp->timed)
                    (void) stop_timer(ROT_ORGANIC, obj_to_any(otmp));
                place_object(otmp, x, y);
                stackobj(otmp);
            }
        }
    }
    del_engr_at(x, y);
    newsym(x, y);
}

/*
 * The organic material has rotted away while buried.  As an expansion,
 * we could add partial damage.  A damage count is kept in the object
 * and every time we are called we increment the count and reschedule another
 * timeout.  Eventually the object rots away.
 *
 * This is used by buried objects other than corpses.  When a container rots
 * away, any contents become newly buried objects.
 */
/* ARGSUSED */
void
rot_organic(anything *arg, long timeout UNUSED)
{
    struct obj *obj = arg->a_obj;

    while (Has_contents(obj)) {
        /* We don't need to place contained object on the floor
           first, but we do need to update its map coordinates. */
        obj->cobj->ox = obj->ox, obj->cobj->oy = obj->oy;
        /* Everything which can be held in a container can also be
           buried, so bury_an_obj's use of obj_extract_self insures
           that Has_contents(obj) will eventually become false. */
        (void) bury_an_obj(obj->cobj, (boolean *) 0);
    }
    obj_extract_self(obj);
    obfree(obj, (struct obj *) 0);
}

/*
 * Called when a corpse has rotted completely away.
 */
void
rot_corpse(anything *arg, long timeout)
{
    coordxy x = 0, y = 0;
    struct obj *obj = arg->a_obj;
    boolean on_floor = obj->where == OBJ_FLOOR,
            in_invent = obj->where == OBJ_INVENT;

    if (on_floor) {
        x = obj->ox;
        y = obj->oy;
    } else if (in_invent) {
        if (flags.verbose) {
            char *cname = corpse_xname(obj, (const char *) 0, CXN_NO_PFX);

            Your("%s%s %s away%c", obj == uwep ? "wielded " : "", cname,
                 otense(obj, "rot"), obj == uwep ? '!' : '.');
        }
        if (obj->owornmask) {
            remove_worn_item(obj, TRUE);
            stop_occupation();
        }
    } else if (obj->where == OBJ_MINVENT) {
        if (obj->owornmask && obj == MON_WEP(obj->ocarry))
            setmnotwielded(obj->ocarry, obj); /* clears owornmask */
    } else if (obj->where == OBJ_MIGRATING) {
        /* clear destination flag so that obfree()'s check for
           freeing a worn object doesn't get a false hit */
        obj->owornmask = 0L;
    }
    rot_organic(arg, timeout);
    if (on_floor) {
        struct monst *mtmp = m_at(x, y);

        /* a hiding monster may be exposed */
        if (mtmp && !OBJ_AT(x, y) && mtmp->mundetected
            && hides_under(mtmp->data)) {
            mtmp->mundetected = 0;
        } else if (u_at(x, y)
                   && u.uundetected && hides_under(gy.youmonst.data))
            (void) hideunder(&gy.youmonst);
        newsym(x, y);
    } else if (in_invent)
        update_inventory();
}

#if 0
void
bury_monst(struct monst *mtmp)
{
    debugpline1("bury_monst: %s", mon_nam(mtmp));
    if (canseemon(mtmp)) {
        if (is_flyer(mtmp->data) || is_floater(mtmp->data)) {
            pline_The("%s opens up, but %s is not swallowed!",
                      surface(mtmp->mx, mtmp->my), mon_nam(mtmp));
            return;
        } else
            pline_The("%s opens up and swallows %s!",
                      surface(mtmp->mx, mtmp->my), mon_nam(mtmp));
    }

    mtmp->mburied = TRUE;
    wakeup(mtmp, FALSE);       /* at least give it a chance :-) */
    newsym(mtmp->mx, mtmp->my);
}

void
bury_you(void)
{
    debugpline0("bury_you");
    if (!Levitation && !Flying) {
        if (u.uswallow)
            You_feel("a sensation like falling into a trap!");
        else
            pline_The("%s opens beneath you and you fall in!",
                      surface(u.ux, u.uy));

        u.uburied = TRUE;
        if (!Strangled && !Breathless)
            Strangled = 6;
        under_ground(1);
    }
}

void
unearth_you(void)
{
    debugpline0("unearth_you");
    u.uburied = FALSE;
    under_ground(0);
    if (!uamul || uamul->otyp != AMULET_OF_STRANGULATION)
        Strangled = 0;
    vision_recalc(0);
}

void
escape_tomb(void)
{
    debugpline0("escape_tomb");
    if ((Teleportation || can_teleport(gy.youmonst.data))
        && (Teleport_control || rn2(3) < Luck+2)) {
        You("attempt a teleport spell.");
        (void) dotele(FALSE);        /* calls unearth_you() */
    } else if (u.uburied) { /* still buried after 'port attempt */
        boolean good;

        if (amorphous(gy.youmonst.data) || Passes_walls
            || noncorporeal(gy.youmonst.data)
            || (unsolid(gy.youmonst.data)
                && gy.youmonst.data != &mons[PM_WATER_ELEMENTAL])
            || (tunnels(gy.youmonst.data) && !needspick(gy.youmonst.data))) {
            You("%s up through the %s.",
                (tunnels(gy.youmonst.data) && !needspick(gy.youmonst.data))
                   ? "try to tunnel"
                   : (amorphous(gy.youmonst.data))
                      ? "ooze"
                      : "phase",
                surface(u.ux, u.uy));

            good = (tunnels(gy.youmonst.data) && !needspick(gy.youmonst.data))
                      ? dighole(TRUE, FALSE, (coord *) 0) : TRUE;
            if (good)
                unearth_you();
        }
    }
}

void
bury_obj(otmp)
struct obj *otmp;
{
    debugpline0("bury_obj");
    if (cansee(otmp->ox, otmp->oy))
        pline_The("objects on the %s tumble into a hole!",
                  surface(otmp->ox, otmp->oy));

    bury_objs(otmp->ox, otmp->oy);
}
#endif /*0*/

#ifdef DEBUG
/* the #wizbury command - bury everything at your loc and around */
int
wiz_debug_cmd_bury(void)
{
    struct obj *otmp;
    int x, y, before = 0, after = 0, diff;

    for (x = u.ux - 1; x <= u.ux + 1; x++)
        for (y = u.uy - 1; y <= u.uy + 1; y++) {
            if (!isok(x, y))
                continue;
            for (otmp = svl.level.objects[x][y]; otmp; otmp = otmp->nexthere)
                ++before;

            bury_objs(x, y);

            for (otmp = svl.level.objects[x][y]; otmp; otmp = otmp->nexthere)
                ++after;
        }

    diff = before - after;
    if (before == 0)
        /* there was nothing here */
        pline("No objects here or adjacent to bury.");
    else if (diff == 0)
        /* before and after will be the same if only unburiable objects are
           present (The Amulet, invocation items, Rider corpses, uchain when
           uball doesn't get buried: carried or floor beyond burial range) */
        pline("No objects buried.");
    else
        /* usual case; if uball got buried, uchain went away and won't be
           counted as buried */
        pline("%d object%s buried.", diff, plur(diff));
    return ECMD_OK;
}
#endif /* DEBUG */

#undef BY_YOU
#undef BY_OBJECT
/* for 'onefile' testing, leave STRIDENT defined so that the other instance
   of it in pray.c will trigger a complaint if someone changes its value */
/*#undef STRIDENT*/

/*dig.c*/
