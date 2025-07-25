/* NetHack 3.7	wintty.c	$NHDT-Date: 1737691300 2025/01/23 20:01:40 $  $NHDT-Branch: NetHack-3.7 $:$NHDT-Revision: 1.420 $ */
/* Copyright (c) David Cohrs, 1991                                */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Neither a standard out nor character-based control codes should be
 * part of the "tty look" windowing implementation.
 * h+ 930227
 */

/* It's still not clear I've caught all the cases for H2344.  #undef this
 * to back out the changes. */
#define H2344_BROKEN

#include "hack.h"

#ifdef TTY_GRAPHICS
#include "dlb.h"

/* leave this undefined; it produces bad screen output with rxvt-unicode */
/*#define DECgraphicsOptimization*/

#ifdef MAC
#define MICRO /* The Mac is a MICRO only for this file, not in general! */
#ifdef THINK_C
extern void msmsg(const char *, ...);
#endif
#endif

#ifndef NO_TERMS
#include "tcap.h"
#endif

#include "wintty.h"

#if defined(CLIPPING) && !defined(NO_SIGNAL)
#include <signal.h>
#ifdef SIGWINCH
#define RESIZABLE
#endif
#endif

#ifdef DEF_PAGER
    /* DEF_PAGER implies UNIX; when dlb is in use, the only file accessible
       to an external pager is 'license'; override 'DEF_PAGER' for that
       situation rather than using code to fallback to DLB plus internal
       pager after open() failure */
#ifdef DLB
#undef DEF_PAGER
#else
#ifndef O_RDONLY /* (same logic as unixmain.c) */
#include <fcntl.h>
#endif
#endif /* DLB */
#endif /* DEF_PAGER */

#if defined(TTY_TILES_ESCCODES) || defined(TTY_SOUND_ESCCODES)
#define VT_ANSI_COMMAND 'z'
#endif
#ifdef TTY_TILES_ESCCODES
#define AVTC_GLYPH_START   0
#define AVTC_GLYPH_END     1
#define AVTC_SELECT_WINDOW 2
#define AVTC_INLINE_SYNC   3
#endif
#ifdef TTY_SOUND_ESCCODES
#define AVTC_SOUND_PLAY    4
#endif

#ifdef HANGUPHANDLING
/*
 * NetHack's core switches to a dummy windowing interface when it
 * detects SIGHUP, but that's no help for any interface routine which
 * is already in progress at the time, and there have been reports of
 * runaway disconnected processes which use up all available CPU time.
 * HUPSKIP() and HUPSKIP_RETURN(x) are used to try to cut them off so
 * that they return to the core instead attempting more terminal I/O.
 */
#define HUPSKIP() \
    do {                                        \
        if (program_state.done_hup) {        \
            morc = '\033';                      \
            return;                             \
        }                                       \
    } while (0)
    /* morc=ESC - in case we bypass xwaitforspace() which sets that */
#define HUPSKIP_RESULT(RES) \
    do {                                        \
        if (program_state.done_hup)          \
            return (RES);                       \
    } while (0)
#else /* !HANGUPHANDLING */
#define HUPSKIP() /*empty*/
#define HUPSKIP_RESULT(RES) /*empty*/
#endif /* ?HANGUPHANDLING */

/* Interface definition, for windows.c */
struct window_procs tty_procs = {
    WPID(tty),
    (0
#ifdef TTY_PERM_INVENT
     | WC_PERM_INVENT
#endif
#ifdef MSDOS
     | WC_TILED_MAP | WC_ASCII_MAP
#endif
#if defined(WIN32CON)
     | WC_MOUSE_SUPPORT
#endif
     | WC_COLOR | WC_HILITE_PET | WC_INVERSE | WC_EIGHT_BIT_IN),
    (0
#if defined(SELECTSAVED)
     | WC2_SELECTSAVED
#endif
#if defined(STATUS_HILITES)
     | WC2_HILITE_STATUS | WC2_HITPOINTBAR | WC2_FLUSH_STATUS
     | WC2_RESET_STATUS
#endif
     | WC2_DARKGRAY | WC2_SUPPRESS_HIST | WC2_URGENT_MESG | WC2_STATUSLINES
     | WC2_U_UTF8STR | WC2_PETATTR
#if !defined(NO_TERMS) || defined(WIN32CON)
     | WC2_EXTRACOLORS
#endif
    ),
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, /* color availability */
    tty_init_nhwindows, tty_player_selection, tty_askname, tty_get_nh_event,
    tty_exit_nhwindows, tty_suspend_nhwindows, tty_resume_nhwindows,
    tty_create_nhwindow, tty_clear_nhwindow, tty_display_nhwindow,
    tty_destroy_nhwindow, tty_curs, tty_putstr,
    tty_putmixed,
    tty_display_file, tty_start_menu, tty_add_menu, tty_end_menu,
    tty_select_menu, tty_message_menu, tty_mark_synch,
    tty_wait_synch,
#ifdef CLIPPING
    tty_cliparound,
#endif
#ifdef POSITIONBAR
    tty_update_positionbar,
#endif
    tty_print_glyph, tty_raw_print, tty_raw_print_bold, tty_nhgetch,
    tty_nh_poskey, tty_nhbell, tty_doprev_message, tty_yn_function,
    tty_getlin, tty_get_ext_cmd, tty_number_pad, tty_delay_output,
#ifdef CHANGE_COLOR /* the Mac uses a palette device */
    tty_change_color,
#ifdef MAC
    tty_change_background, set_tty_font_name,
#endif
    tty_get_color_string,
#endif

    genl_outrip,
    tty_preference_update,
    tty_getmsghistory, tty_putmsghistory,
    tty_status_init,
    genl_status_finish, tty_status_enablefield,
#ifdef STATUS_HILITES
    tty_status_update,
#else
    genl_status_update,
#endif
    genl_can_suspend_yes,
    tty_update_inventory,
    tty_ctrl_nhwindow,
};

winid BASE_WINDOW;
struct WinDesc *wins[MAXWIN];
struct DisplayDesc *ttyDisplay; /* the tty display descriptor */

extern void cmov(int, int);   /* from termcap.c */
extern void nocmov(int, int); /* from termcap.c */

static volatile int erasing_tty_screen; /* volatile: SIGWINCH */

#if defined(UNIX) || defined(VMS)
static char obuf[BUFSIZ]; /* BUFSIZ is defined in stdio.h */
#endif

static const char winpanicstr[] = "Bad window Id %d (%s)";
#define ttywindowpanic() panic(winpanicstr, window, __func__)
char defmorestr[] = "--More--";

#ifdef CLIPPING
#if defined(TILES_IN_GLYPHMAP) && defined(MSDOS)
boolean clipping = FALSE; /* clipping on? */
int clipx = 0, clipxmax = 0;
int clipy = 0, clipymax = 0;
#else
static boolean clipping = FALSE; /* clipping on? */
static int clipx = 0, clipxmax = 0;
static int clipy = 0, clipymax = 0;
#endif
#endif /* CLIPPING */

#if defined(TILES_IN_GLYPHMAP) && defined(MSDOS)
extern void adjust_cursor_flags(struct WinDesc *);
#endif

#if defined(ASCIIGRAPH)
boolean GFlag = FALSE;
boolean HE_resets_AS; /* see termcap.c */
#endif

static void bail(const char *); /* __attribute__((noreturn)) */
static void newclipping(coordxy, coordxy);
static void new_status_window(void);
static void getret(void);
static void erase_menu_or_text(winid, struct WinDesc *, boolean);
static void free_window_info(struct WinDesc *, boolean);
static boolean toggle_menu_curr(winid, tty_menu_item *, int, boolean,
                                boolean, long);
static void dmore(struct WinDesc *, const char *);
static void set_item_state(winid, int, tty_menu_item *);
static void set_all_on_page(winid, tty_menu_item *, tty_menu_item *);
static void unset_all_on_page(winid, tty_menu_item *, tty_menu_item *);
static void invert_all_on_page(winid, tty_menu_item *, tty_menu_item *,
                               char, long);
static void invert_all(winid, tty_menu_item *, tty_menu_item *, char, long);
static void toggle_menu_attr(boolean, int, int);
static void process_menu_window(winid, struct WinDesc *);
static void process_text_window(winid, struct WinDesc *);
static tty_menu_item *reverse(tty_menu_item *);
static const char *compress_str(const char *);
#ifndef STATUS_HILITES
static void tty_putsym(winid, int, int, char);
#endif
#define MAX_STATUS_ROWS 3
#define StatusRows() ((iflags.wc2_statuslines <= 2) ? 2 : MAX_STATUS_ROWS)
#ifdef STATUS_HILITES
static boolean check_fields(boolean forcefields, int sz[MAX_STATUS_ROWS]);
static void render_status(void);
static void tty_putstatusfield(const char *, int, int);
static boolean check_windowdata(void);
static void set_condition_length(void);
static int make_things_fit(boolean);
static void shrink_enc(int);
static void shrink_dlvl(int);
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
static void status_sanity_check(void);
#endif /* NH_DEVEL_STATUS */
#endif
#ifdef ENHANCED_SYMBOLS
void g_pututf8(uint8 *utf8str);
#endif

static boolean calling_from_update_inventory = FALSE;
#ifdef TTY_PERM_INVENT
static struct tty_perminvent_cell emptyttycell = {
    0, 0, 0, { 0 }, NO_COLOR + 1
};
static glyph_info zerogi = { 0 };
static struct to_core zero_tocore = { 0 };
enum { border_left, border_middle, border_right, border_elements };
static int bordercol[border_elements] = { 0, 0, 0 }; /* left, middle, right */
static int ttyinvmode = InvNormal; /* enum is in wintype.h */
static int inuse_only_start = 0; /* next slot to use for in-use-only mode */
static boolean done_tty_perm_invent_init = FALSE;
enum { tty_slots = invlet_basic + invlet_gold + invlet_overflow }; /* 54 */
static boolean slot_tracker[tty_slots];
static int ttyinv_slots_used = 0; /* 1-based, slot_trackter[0..slots-1] */
static long last_glyph_reset_when;
#ifndef NOINVSYM /* invent.c */
#define NOINVSYM '#'
#endif
static int ttyinv_create_window(int, struct WinDesc *);
static void ttyinv_remove_data(struct WinDesc *, boolean);
static void ttyinv_add_menu(winid, struct WinDesc *, char ch, int attr,
                            int clr, const char *str);
static int selector_to_slot(char ch, const int invflags, boolean *ignore);
static char slot_to_invlet(int, boolean);
static void ttyinv_inuse_fulllines(struct WinDesc *, int);
static void ttyinv_inuse_twosides(struct WinDesc *, int);
static void ttyinv_end_menu(int, struct WinDesc *);
static void ttyinv_render(winid window, struct WinDesc *cw);
static void tty_invent_box_glyph_init(struct WinDesc *cw);
static boolean assesstty(enum inv_modes, short *, short *,
                         long *, long *, long *, long *, long *);
static void ttyinv_populate_slot(struct WinDesc *, int, int,
                                 const char *, uint32, int);
#endif /* TTY_PERM_INVENT */

/*
 * A string containing all the default commands -- to add to a list
 * of acceptable inputs.
 */
static const char default_menu_cmds[] = {
    MENU_FIRST_PAGE,    MENU_LAST_PAGE,   MENU_NEXT_PAGE,
    MENU_PREVIOUS_PAGE, MENU_SELECT_ALL,  MENU_UNSELECT_ALL,
    MENU_INVERT_ALL,    MENU_SELECT_PAGE, MENU_UNSELECT_PAGE,
    MENU_INVERT_PAGE,   MENU_SEARCH,      0 /* null terminator */
};

#ifdef TTY_TILES_ESCCODES
static int vt_tile_current_window = -2;

static void
print_vt_code(int i, int c, int d)
{
    HUPSKIP();
    if (iflags.vt_tiledata) {
        if (c >= 0) {
            if (i == AVTC_SELECT_WINDOW) {
                if (c == vt_tile_current_window)
                    return;
                vt_tile_current_window = c;
            }
            if (d >= 0)
                printf("\033[1;%d;%d;%d%c", i, c, d, VT_ANSI_COMMAND);
            else
                printf("\033[1;%d;%d%c", i, c, VT_ANSI_COMMAND);
        } else {
            printf("\033[1;%d%c", i, VT_ANSI_COMMAND);
        }
    }
}
#else
#define print_vt_code(i, c, d) /*empty*/
#endif /* !TTY_TILES_ESCCODES */
#define print_vt_code1(i)     print_vt_code((i), -1, -1)
#define print_vt_code2(i,c)   print_vt_code((i), (c), -1)
#define print_vt_code3(i,c,d) print_vt_code((i), (c), (d))

#if defined(USER_SOUNDS) && defined(TTY_SOUND_ESCCODES)
static void
print_vt_soundcode_idx(int idx, int v)
{
    HUPSKIP();
    if (iflags.vt_sounddata) {
        if (v >= 0)
            printf("\033[1;%d;%d;%d%c", AVTC_SOUND_PLAY,
                   idx, v, VT_ANSI_COMMAND);
        else
            printf("\033[1;%d;%d%c", AVTC_SOUND_PLAY,
                   idx, VT_ANSI_COMMAND);
    }
}
#else /* !(USER_SOUNDS && TTY_SOUND_ESCCODES) */
#define print_vt_soundcode_idx(idx, v) ;
#endif /* ?(USER_SOUNDS && TTY_SOUND_ESCCODES) */

/* clean up and quit */
static void
bail(const char *mesg)
{
    clearlocks();
    tty_exit_nhwindows(mesg);
    nh_terminate(EXIT_SUCCESS);
    /*NOTREACHED*/
}

#ifdef RESIZABLE
static void winch_handler(int);
static void resize_tty(void);

static volatile int resize_mesg = 0;

/* signal handler is called with at least 1 arg */
/*ARGUSED*/
static void
winch_handler(int sig_unused UNUSED)
{
#ifdef WINCHAIN
    {
#define WINCH_MESSAGE "(SIGWINCH)"
        if (wc_tracelogf)
            (void) write(fileno(wc_tracelogf), WINCH_MESSAGE,
                         sizeof WINCH_MESSAGE - sizeof "");
#undef WINCH_MESSAGE
    }
#endif

    program_state.resize_pending++; /* resize_tty() will reset it */
    /* if nethack is waiting for input, which is the most likely scenario,
       we will go ahead and respond to the resize immediately; otherwise,
       tty_nhgetch() will do so the next time it's called */
    if (program_state.getting_char) {
        resize_tty();
#if 0   /* [this doesn't work as intended and seems to be unnecessary] */
        if (resize_mesg) {
            /* resize_tty() put "Press a key to continue: " on top line */
            (void) tty_nhgetch(); /* recursion... */
        }
#endif
    }
    return;
}

/* query the system for tty size (vis getwindowsz()), and adapt the game's
   windows to match */
static void
resize_tty(void)
{
    int oldLI = LI, oldCO = CO, mapx, mapy, oldtoplin, i;
    boolean map_active;
    struct WinDesc *cw;

    /* reset to 0 rather than just decrement */
    program_state.resize_pending = 0;
    resize_mesg = 0;

    getwindowsz(); /* update LI and CO */
    if (!ttyDisplay || (LI == oldLI && CO == oldCO))
        return;

    ttyDisplay->rows = LI;
    ttyDisplay->cols = CO;

    cw = wins[BASE_WINDOW];
    cw->rows = ttyDisplay->rows;
    cw->cols = ttyDisplay->cols;

    if (!iflags.window_inited)
        return;

    /* try to figure out where we'll want to leave the cursor */
    cw = (WIN_MAP != WIN_ERR) ? wins[WIN_MAP] : NULL;
    map_active = (cw && cw->active);
    if (!map_active)
        mapx = 1, mapy = 0;
    else if (gg.getposx < 1)
        mapx = u.ux, mapy = u.uy;
    else
        mapx = gg.getposx, mapy = gg.getposy;

    oldtoplin = ttyDisplay->toplin;
    ttyDisplay->toplin = TOPLINE_EMPTY; /* (before term_clear_screen()) */
    /* whatever used to be on the screen has become suspect;
       blank it all out, then redo the display */
    term_clear_screen();
    new_status_window();

    /* if the map window is shown, redisplay it (also status, perminv) */
    if (map_active) {
        docrt_flags(docrtRefresh);
        bot(); /* context.botlx=1 gets set by docrt() */
        for (i = 0; i < MAXWIN; ++i) {
            if (i == BASE_WINDOW
                || i == WIN_MAP /* docrt() updates it */
                || i == WIN_STATUS /* docrt()+bot() updates it */
#ifdef TTY_PERM_INVENT
                || i == WIN_INVEN /* docrt() again */
#endif
                || i == WIN_MESSAGE) /* we fake it here */
                continue;
            if (wins[i] && wins[i]->active) {
                /* cop-out */
                oldtoplin = TOPLINE_EMPTY; /* don't restore it below */
                ttyDisplay->toplin = TOPLINE_NON_EMPTY;
                addtopl("Press a key to continue: ");
                resize_mesg++;
                break;
            }
        }
    } /* wins[WIN_MAP].active */

    if (oldtoplin != TOPLINE_EMPTY) {
        ttyDisplay->toplin = oldtoplin;
        addtopl(gt.toplines);
    }
    if (map_active) {
        newclipping(mapx, mapy);
        tty_curs(WIN_MAP, mapx, mapy);
        tty_display_nhwindow(WIN_MAP, FALSE); /*fflush(stdout)*/
    }
    return;
}
#endif /* RESIZABLE */

static void
newclipping(coordxy x, coordxy y)
{
#ifdef CLIPPING
    if (CO < COLNO || LI < 1 + ROWNO + iflags.wc2_statuslines) {
        setclipped(); /* sets clipping=TRUE */
        if (x)
            tty_cliparound(x, y);
    } else {
        clipping = FALSE;
        clipx = clipy = 0;
    }
#else
    nhUse(x + y);
#endif
    return;
}

/* destroy and recreate status window; extracted from winch_handler()
   and augmented for use by tty_preference_update() */
static void
new_status_window(void)
{
    if (WIN_STATUS != WIN_ERR) {
        /* in case it's shrinking, clear it before destroying so that
           dropped portion won't show anything that's now becoming stale */
        tty_clear_nhwindow(WIN_STATUS);
        tty_destroy_nhwindow(WIN_STATUS), WIN_STATUS = WIN_ERR;
    }
    /* frees some status tracking data */
    genl_status_finish();
    /* creates status window and allocates tracking data */
    tty_status_init();
    tty_clear_nhwindow(WIN_STATUS); /* does some init, sets context.botlx */
#ifdef STATUS_HILITES
    status_initialize(REASSESS_ONLY);
#endif
}

/*ARGSUSED*/
void
tty_init_nhwindows(int *argcp UNUSED, char **argv UNUSED)
{
    int wid, hgt, i;

    /* options aren't processed yet so wc2_statuslines might be 0;
       make sure that it has a reasonable value during tty setup */
    iflags.wc2_statuslines = StatusRows(); /* 2 or 3; 0 => 2 */
    /*
     *  Remember tty modes, to be restored on exit.
     *
     *  gettty() must be called before term_startup()
     *    due to ordering of LI/CO settings
     *  term_startup() must be called before initoptions()
     *    due to ordering of graphics settings
     */
#if defined(UNIX) || defined(VMS)
    setbuf(stdout, obuf);
#endif
    gettty();

    /* to port dependant tty setup */
    term_startup(&wid, &hgt);
    setftty(); /* calls term_start_screen */
    term_curs_set(0);

    /* set up tty descriptor */
    ttyDisplay = (struct DisplayDesc *) alloc(sizeof (struct DisplayDesc));
    ttyDisplay->toplin = TOPLINE_EMPTY;
    ttyDisplay->topl_utf8 = 0;  /* putmixed may set this */
    ttyDisplay->rows = hgt;
    ttyDisplay->cols = wid;
    ttyDisplay->curx = ttyDisplay->cury = 0;
    ttyDisplay->inmore = ttyDisplay->inread = ttyDisplay->intr = 0;
    ttyDisplay->dismiss_more = 0;
    ttyDisplay->color = NO_COLOR;
    ttyDisplay->attrs = 0;
    ttyDisplay->topl_utf8 = 0;
    ttyDisplay->mixed = 0;

    /* set up the default windows */
    BASE_WINDOW = tty_create_nhwindow(NHW_BASE);
    wins[BASE_WINDOW]->active = 1;

    ttyDisplay->lastwin = WIN_ERR;

#ifdef RESIZABLE
    (void) signal(SIGWINCH, (SIG_RET_TYPE) winch_handler);
#endif

    tty_clear_nhwindow(BASE_WINDOW);

    /* Once pline() is functional, error-related prompts such as
     * those relating to save files etc. can intrude on the
     * copyright information display because their prompts are
     * up at the very top in the message window.
     * Move the copyright information a little further down to
     * row 3, out of the way. */

    tty_curs(BASE_WINDOW, 1, 4);
    for (i = 1; i <= 4; ++i)
        tty_putstr(BASE_WINDOW, 0, copyright_banner_line(i));
    tty_putstr(BASE_WINDOW, 0, "");
    tty_display_nhwindow(BASE_WINDOW, FALSE);

    /* Move to a default location for the "Shall I pick .." player
     * selection prompts, which also use the BASE_WINDOW. Leave
     * room for as many as 3 unexpected raw_prints early startup
     * messages above that.
     * If there is a topline message prompt, before the
     * "Shall I pick ..." prompt, the latter will end up appearing
     * immediately after the topline message prompt. There should
     * now be room. */
    tty_curs(BASE_WINDOW, 1, 11);

    /* 'statuslines' defaults to set_in_config, allowed but invisible;
       make it dynamically settable if feasible, otherwise visible */
    if ((tty_procs.wincap2 & WC2_STATUSLINES) != 0)
        set_wc2_option_mod_status(WC2_STATUSLINES,
#ifndef CLIPPING
                                  (LI <= 1 + ROWNO + 2) ? set_gameview :
#endif
                                      set_in_game);
}

void
tty_preference_update(const char *pref)
{
    boolean newstatuslines = (!strcmp(pref, "statuslines")
                              && iflags.window_inited);

    if (newstatuslines) {
        new_status_window();
        newclipping(u.ux, u.uy);
    }

#if defined(WIN32CON)
    consoletty_preference_update(pref);
#else
    genl_preference_update(pref);
#endif
#ifdef TTY_PERM_INVENT
    /* the boundary box around persistent inventory is drawn with wall
       symbols, so if player changes to a different symbol set (other
       than temporary switch to the rogue one), redraw perm_invent; not
       only might individual symbols change (punctuation vs line drawing),
       the way to render them might change too (Handling: DEC/UTF8/&c) */
    if ((!strcmp(pref, "symset") || !strcmp(pref, "perm_invent"))
        && iflags.window_inited) {
        if (WIN_INVEN != WIN_ERR)
           tty_invent_box_glyph_init(wins[WIN_INVEN]);
    }
    /* if newstatuslines has been toggled between 2 and 3 or vice versa
       then we want to reposition the perm_invent window to match */
    if (newstatuslines && WIN_INVEN != WIN_ERR) {
        perm_invent_toggled(TRUE);      /*TEMP?*/
        tty_destroy_nhwindow(WIN_INVEN), WIN_INVEN = WIN_ERR;
        perm_invent_toggled(FALSE);     /*TEMP?*/
    }
#endif
    return;
}

void
tty_player_selection(void)
{
    if (genl_player_setup(ttyDisplay->rows))
        return;

    bail((char *) 0);
    /*NOTREACHED*/
}

/*
 * svp.plname is filled either by an option (-u Player  or  -uPlayer) or
 * explicitly (by being the wizard) or by askname.
 * It may still contain a suffix denoting the role, etc.
 * Always called after init_nhwindows() and before
 * init_sound_disp_gamewindows().
 */
void
tty_askname(void)
{
    static const char who_are_you[] = "Who are you? ";
    int c, ct, tryct = 0;

#ifdef SELECTSAVED
    if (iflags.wc2_selectsaved && !iflags.renameinprogress)
        switch (restore_menu(BASE_WINDOW)) {
        case -1:
            bail("Until next time then..."); /* quit */
            /*NOTREACHED*/
            break;
        case 0:
            break; /* no game chosen; start new game */
        case 1:
            return; /* svp.plname[] has been set */
        }
#endif /* SELECTSAVED */

    tty_putstr(BASE_WINDOW, 0, "");
    do {
        if (++tryct > 1) {
            if (tryct > 10)
                bail("Giving up after 10 tries.\n");
            tty_curs(BASE_WINDOW, 1, wins[BASE_WINDOW]->cury - 1);
            tty_putstr(BASE_WINDOW, 0, "Enter a name for your character...");
            /* erase previous prompt (in case of ESC after partial response) */
            tty_curs(BASE_WINDOW, 1, wins[BASE_WINDOW]->cury), cl_end();
        }
        tty_putstr(BASE_WINDOW, 0, who_are_you);
        tty_curs(BASE_WINDOW, (int) (sizeof who_are_you),
                 wins[BASE_WINDOW]->cury - 1);
        ct = 0;
        while ((c = tty_nhgetch()) != '\n') {
            if (c == EOF)
                c = '\033';
            if (c == '\r')
                break;
            if (c == '\033') {
                ct = 0;
                break;
            } /* continue outer loop */
#if defined(WIN32CON)
            if (c == '\003')
                bail("^C abort.\n");
#endif
            /* some people get confused when their erase char is not ^H */
            if (c == '\b' || c == '\177') {
                if (ct) {
                    ct--;
#ifdef WIN32CON
                    ttyDisplay->curx--;
#endif
#if defined(MICRO) || defined(WIN32CON)
#if defined(WIN32CON) || defined(MSDOS)
                    backsp(); /* \b is visible on NT */
                    (void) putchar(' ');
                    backsp();
#else
                    msmsg("\b \b");
#endif
#else
                    (void) putchar('\b');
                    (void) putchar(' ');
                    (void) putchar('\b');
#endif
                }
                continue;
            }
#if defined(UNIX) || defined(VMS)
            if (c != '-' && c != '@')
                if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z')
                    /* reject leading digit but allow digits elsewhere
                       (avoids ambiguity when character name gets
                       appended to uid to construct save file name) */
                    && !(c >= '0' && c <= '9' && ct > 0))
                    c = '_';
#endif
            if (ct < (int) (sizeof svp.plname) - 1) {
#if defined(MICRO)
#if defined(MSDOS)
                if (iflags.grmode) {
                    (void) putchar(c);
                } else
#endif
                    msmsg("%c", c);
#else
                (void) putchar(c);
#endif
                svp.plname[ct++] = c;
#ifdef WIN32CON
                ttyDisplay->curx++;
#endif
            }
        }
        svp.plname[ct] = 0;
    } while (ct == 0);

    /* move to next line to simulate echo of user's <return> */
    tty_curs(BASE_WINDOW, 1, wins[BASE_WINDOW]->cury + 1);

    /* since we let user pick an arbitrary name now, he/she can pick
       another one during role selection */
    iflags.renameallowed = TRUE;
}

void
tty_get_nh_event(void)
{
    return;
}

static void
getret(void)
{
#if defined(MICRO) || defined(WIN32CON)
    getreturn("to continue");
#else
    HUPSKIP();
    xputs("\n");
    if (flags.standout)
        standoutbeg();
    xputs("Hit ");
    xputs(iflags.cbreak ? "space" : "return");
    xputs(" to continue: ");
    if (flags.standout)
        standoutend();
    xwaitforspace(" ");
#endif
    iflags.raw_printed = 0;
}

void
tty_suspend_nhwindows(const char *str)
{
    term_curs_set(1);
    settty(str); /* calls end_screen, perhaps raw_print */
    if (!str)
        tty_raw_print(""); /* calls fflush(stdout) */
}

void
tty_resume_nhwindows(void)
{
    gettty();
    setftty(); /* calls term_start_screen */
    term_curs_set(0);
    docrt();
}

#ifdef CHANGE_COLOR
char *
tty_get_color_string(void)
{
    return (char *) 0;
}
#endif /* CHANGE_COLOR */

void
tty_exit_nhwindows(const char *str)
{
    winid i;

    tty_suspend_nhwindows(str);
    /*
     * Disable windows to avoid calls to window routines.
     */
    free_pickinv_cache(); /* reset its state as well as tear down window */
    for (i = 0; i < MAXWIN; i++) {
        if (i == BASE_WINDOW)
            continue; /* handle wins[BASE_WINDOW] last */
        if (wins[i]) {
#ifdef FREE_ALL_MEMORY
            free_window_info(wins[i], TRUE);
            free((genericptr_t) wins[i]);
#endif
            wins[i] = (struct WinDesc *) 0;
        }
    }
    WIN_MAP = WIN_MESSAGE = WIN_INVEN = WIN_ERR; /* these are all gone now */
    WIN_STATUS = WIN_ERR;
#ifdef FREE_ALL_MEMORY
    if (BASE_WINDOW != WIN_ERR && wins[BASE_WINDOW]) {
        free_window_info(wins[BASE_WINDOW], TRUE);
        free((genericptr_t) wins[BASE_WINDOW]);
        wins[BASE_WINDOW] = (struct WinDesc *) 0;
        BASE_WINDOW = WIN_ERR;
    }
    free((genericptr_t) ttyDisplay);
    ttyDisplay = (struct DisplayDesc *) 0;
#endif

    term_shutdown(); /* cleanup termcap/terminfo/whatever */
    iflags.window_inited = 0;
}

DISABLE_WARNING_UNREACHABLE_CODE

winid
tty_create_nhwindow(int type)
{
    struct WinDesc *newwin;
    int i, rowoffset;
    int newid;

    for (newid = 0; newid < MAXWIN; ++newid)
        if (wins[newid] == 0)
            break;
    if (newid == MAXWIN) {
        panic("No window slots!");
        /*NOTREACHED*/
        return WIN_ERR;
    }

    newwin = (struct WinDesc *) alloc(sizeof (struct WinDesc));
    wins[newid] = newwin;

    newwin->type = type;
    newwin->flags = 0;
    newwin->active = FALSE;
    newwin->curx = newwin->cury = 0;
    newwin->morestr = 0;
    newwin->mlist = (tty_menu_item *) 0;
    newwin->plist = (tty_menu_item **) 0;
    newwin->npages = newwin->plist_size = newwin->nitems = newwin->how = 0;
    newwin->mbehavior = 0U;
    switch (type) {
    case NHW_BASE:
        /* base window, used for absolute movement on the screen */
        newwin->offx = newwin->offy = 0;
        newwin->rows = ttyDisplay->rows;
        newwin->cols = ttyDisplay->cols;
        newwin->maxrow = newwin->maxcol = 0;
        break;
    case NHW_MESSAGE:
        /* message window, 1 line long, very wide, top of screen */
        newwin->offx = newwin->offy = 0;
        /* sanity check */
        if (iflags.msg_history < 20)
            iflags.msg_history = 20;
        else if (iflags.msg_history > MAX_MSG_HISTORY)
            iflags.msg_history = MAX_MSG_HISTORY;
        newwin->maxrow = newwin->rows = iflags.msg_history;
        newwin->maxcol = newwin->cols = 0;
        break;
    case NHW_STATUS:
        /* status window, 2 or 3 lines long, full width, bottom of screen */
        if (iflags.wc2_statuslines < 2
#ifndef CLIPPING
            || (LI < 1 + ROWNO + 3)
#endif
            || iflags.wc2_statuslines > 3)
            iflags.wc2_statuslines = 2;
        newwin->offx = 0;
        rowoffset = ttyDisplay->rows - iflags.wc2_statuslines;
#if defined(TILES_IN_GLYPHMAP) && defined(MSDOS)
        if (iflags.grmode) {
            newwin->offy = rowoffset;
        } else
#endif
            newwin->offy = min(rowoffset, ROWNO + 1);
        newwin->rows = newwin->maxrow = iflags.wc2_statuslines;
        newwin->cols = newwin->maxcol = ttyDisplay->cols;
        break;
    case NHW_MAP:
        /* map window, ROWNO lines long, full width, below message window */
        newwin->offx = 0;
        newwin->offy = 1;
        newwin->rows = ROWNO;
        newwin->cols = COLNO;
        newwin->maxrow = 0; /* no buffering done -- let gbuf do it */
        newwin->maxcol = 0;
        break;
    case NHW_MENU:
    case NHW_TEXT:
        /* inventory/menu window, variable length, full width, top of screen;
           help window, the same, different semantics for display, etc */
        newwin->offx = newwin->offy = 0;
        newwin->rows = 0;
        newwin->cols = ttyDisplay->cols;
        newwin->maxrow = newwin->maxcol = 0;
        break;
    default:
        panic("Tried to create window type %d\n", (int) type);
        /*NOTREACHED*/
        return WIN_ERR;
    }

    if (newwin->maxrow) {
        newwin->data = (char **) alloc(
                               (unsigned) (newwin->maxrow * sizeof (char *)));
        newwin->datlen = (short *) alloc(
                                (unsigned) (newwin->maxrow * sizeof (short)));
        for (i = 0; i < newwin->maxrow; i++) {
            if (newwin->maxcol) { /* WIN_STATUS */
                newwin->data[i] = (char *) alloc((unsigned) newwin->maxcol);
                newwin->datlen[i] = (short) newwin->maxcol;
            } else {
                newwin->data[i] = (char *) 0;
                newwin->datlen[i] = 0;
            }
        }
        if (newwin->type == NHW_MESSAGE)
            newwin->maxrow = 0;
    } else {
        newwin->data = (char **) 0;
        newwin->datlen = (short *) 0;
    }

    return newid;
}

RESTORE_WARNING_UNREACHABLE_CODE

static void
erase_menu_or_text(
    winid window,
    struct WinDesc *cw,
    boolean clear)
{
    if (cw->offx == 0) {
        if (cw->offy) {
            tty_curs(window, 1, 0);
            cl_eos();
        } else if (clear) {
            term_clear_screen();
        } else {
            docrt();
            flush_screen(1);
        }
    } else {
        docorner((int) cw->offx, cw->maxrow + 1, 0);
    }
}

static void
free_window_info(struct WinDesc *cw, boolean free_data)
{
    int i;

    if (cw->data) {
        if (WIN_MESSAGE != WIN_ERR && cw == wins[WIN_MESSAGE]
            && cw->rows > cw->maxrow)
            cw->maxrow = cw->rows; /* topl data */
        for (i = 0; i < cw->maxrow; i++)
            if (cw->data[i]) {
                free((genericptr_t) cw->data[i]);
                cw->data[i] = (char *) 0;
                if (cw->datlen)
                    cw->datlen[i] = 0;
            }
        if (free_data) {
            free((genericptr_t) cw->data);
            cw->data = (char **) 0;
            if (cw->datlen)
                free((genericptr_t) cw->datlen);
            cw->datlen = (short *) 0;
            cw->rows = 0;
        }
    }
    cw->maxrow = cw->maxcol = 0;
    if (cw->mlist) {
        tty_menu_item *temp;

        while ((temp = cw->mlist) != 0) {
            cw->mlist = temp->next;
            if (temp->str)
                free((genericptr_t) temp->str);
            free((genericptr_t) temp);
        }
    }
    if (cw->plist) {
        free((genericptr_t) cw->plist);
        cw->plist = 0;
    }
    cw->plist_size = cw->npages = cw->nitems = cw->how = 0;
    if (cw->morestr) {
        free((genericptr_t) cw->morestr);
        cw->morestr = 0;
    }
}

void
tty_clear_nhwindow(winid window)
{
    int i, j, m, n;
    struct WinDesc *cw = 0;

    HUPSKIP();
    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0)
        ttywindowpanic();
    ttyDisplay->lastwin = window;

    print_vt_code2(AVTC_SELECT_WINDOW, window);

    switch (cw->type) {
    case NHW_MESSAGE:
        if (ttyDisplay->toplin != TOPLINE_EMPTY) {
            if (!erasing_tty_screen) {
                home();
                cl_end();
                if (cw->cury)
                    docorner(1, cw->cury + 1, 0);
            }
            cw->curx = cw->cury = 0;
            ttyDisplay->toplin = TOPLINE_EMPTY;
        }
        break;
    case NHW_STATUS:
        m = cw->maxrow;
        n = cw->cols;
        for (i = 0; i < m; ++i) {
            if (!erasing_tty_screen) {
                tty_curs(window, 1, i);
                cl_end();
            }
            for (j = 0; j < n - 1; ++j)
                cw->data[i][j] = ' ';
            cw->data[i][n - 1] = '\0';
            /*finalx[i][NOW] = finalx[i][BEFORE] = 0;*/
        }
        disp.botlx = TRUE;
        break;
    case NHW_MAP:
        /* cheap -- clear the whole thing and tell nethack to redraw botl */
        disp.botlx = TRUE;
        FALLTHROUGH;
        /*FALLTHRU*/
    case NHW_BASE:
        /* if erasing_tty_screen is True, calling sequence is
           term_clear_screen -> erase_tty_screen -> tty_clear_nhwindow
           so we don't call term_clear_screen again */
        if (!erasing_tty_screen) {
            term_clear_screen();
        }
        /*
         * Neither map nor base window tracks what is currently shown so
         * there's no stale data that would need to be changed to spaces.
         */
        break;
    case NHW_MENU:
    case NHW_TEXT:
#ifdef TTY_PERM_INVENT
    case NHW_PERMINVENT:
#endif
        if (!erasing_tty_screen) {
            if (cw->active)
                erase_menu_or_text(window, cw, TRUE);
#ifdef TTY_PERM_INVENT
            if (cw->type == NHW_MENU && cw->mbehavior == MENU_BEHAVE_PERMINV)
                ttyinv_remove_data(cw, FALSE);
            else
#endif
                free_window_info(cw, FALSE);
        }
        break;
    }
    cw->curx = cw->cury = 0;
    /* cw->blanked = TRUE; -- this isn't used */
}

/* toggle a specific entry */
static boolean
toggle_menu_curr(
    winid window,
    tty_menu_item *curr,
    int lineno,
    boolean in_view,
    boolean counting,
    long count)
{
    if (curr->selected) {
        if (counting && count > 0) {
            curr->count = count;
            if (in_view)
                set_item_state(window, lineno, curr);
            return TRUE;
        } else { /* change state */
            curr->selected = FALSE;
            curr->count = -1L;
            if (in_view)
                set_item_state(window, lineno, curr);
            return TRUE;
        }
    } else { /* !selected */
        if (counting && count > 0) {
            curr->count = count;
            curr->selected = TRUE;
            if (in_view)
                set_item_state(window, lineno, curr);
            return TRUE;
        } else if (!counting) {
            curr->selected = TRUE;
            if (in_view)
                set_item_state(window, lineno, curr);
            return TRUE;
        }
        /* do nothing counting&&count==0 */
    }
    return FALSE;
}

static void
dmore(
    struct WinDesc *cw,
    const char *s) /* valid responses */
{
    const char *prompt = cw->morestr ? cw->morestr : defmorestr;
    int offset = (cw->type == NHW_TEXT) ? 1 : 2;

    HUPSKIP();
    tty_curs(BASE_WINDOW, (int) ttyDisplay->curx + offset,
             (int) ttyDisplay->cury);
    if (flags.standout)
        standoutbeg();
    xputs(prompt);
    ttyDisplay->curx += strlen(prompt);
    if (flags.standout)
        standoutend();

    xwaitforspace(s);
}

/* change screen display for selection state of an item;
   not used or wanted for items that aren't shown by the current page */
static void
set_item_state(
    winid window,
    int lineno,
    tty_menu_item *item)
{
    char ch = item->selected ? (item->count == -1L ? '+' : '#') : '-';

    HUPSKIP();
    tty_curs(window, 4, lineno);
    term_start_attr(item->attr);
    if (item->color != NO_COLOR)
        term_start_color(item->color);
    (void) putchar(ch);
    ttyDisplay->curx++;
    if (item->color != NO_COLOR)
        term_end_color();
    term_end_attr(item->attr);
}

/* select all [ignores pending count, if any] */
static void
set_all_on_page(
    winid window,
    tty_menu_item *page_start,
    tty_menu_item *page_end)
{
    tty_menu_item *curr;
    int n;

    for (n = 0, curr = page_start; curr != page_end; n++, curr = curr->next) {
        if (!curr->identifier.a_void /* not selectable */
            || curr->selected /* already selected */
            || !menuitem_invert_test(1, curr->itemflags, FALSE))
            continue;
        curr->selected = TRUE;
        set_item_state(window, n, curr);
    }
}

/* unselect all */
static void
unset_all_on_page(
    winid window,
    tty_menu_item *page_start,
    tty_menu_item *page_end)
{
    tty_menu_item *curr;
    int n;

    for (n = 0, curr = page_start; curr != page_end; n++, curr = curr->next) {
        if (!curr->identifier.a_void /* skip if not selectable */
            || !curr->selected /* skip if already de-selected */
            || !menuitem_invert_test(2, curr->itemflags, TRUE))
            continue;
        curr->selected = FALSE;
        curr->count = -1L;
        set_item_state(window, n, curr);
    }
}

/* invert current page */
static void
invert_all_on_page(
    winid window,
    tty_menu_item *page_start,
    tty_menu_item *page_end,
    char acc, /* group accelerator, 0 => all */
    long count) /* pending count; -1L for non-group toggling */
{
    tty_menu_item *curr;
    int n;

    for (n = 0, curr = page_start; curr != page_end; n++, curr = curr->next) {
        if (!curr->identifier.a_void /* not selectable */
            || (acc ? curr->gselector != acc /* skip if not group 'acc' */
                /* inverting all rather than a group; skip if flagged */
                : !menuitem_invert_test(0, curr->itemflags, curr->selected)))
            continue;

        if (curr->selected) {
            curr->selected = FALSE;
            curr->count = -1L;
        } else {
            curr->selected = TRUE;
            if (count > 0)
                curr->count = count;
        }
        set_item_state(window, n, curr);
    }
}

/* invert all entries that match given group accelerator (or all if zero) */
static void
invert_all(
    winid window,
    tty_menu_item *page_start,
    tty_menu_item *page_end,
    char acc, /* group accelerator, 0 => all */
    long count) /* pending count; -1L for non-group toggling */
{
    tty_menu_item *curr;
    boolean on_curr_page;
    struct WinDesc *cw = wins[window];

    /* handle current page separately (it will need screen updating) */
    invert_all_on_page(window, page_start, page_end, acc, count);

    /* invert the rest (no screen updating for them) */
    for (on_curr_page = FALSE, curr = cw->mlist; curr; curr = curr->next) {
        if (curr == page_start)
            on_curr_page = TRUE;
        else if (curr == page_end)
            on_curr_page = FALSE;

        /* skip if on current page (already handled above) or not
           selectable (header line or similar) or if group toggling
           is taking place and this item isn't in specified group or
           group toggling is not taking place and this item is off
           limits to bulk toggling (assumes that an item won't be
           both in a group and also subject to bulk restrictions) */
        if (on_curr_page || !curr->identifier.a_void
            || (acc ? curr->gselector != acc
                : !menuitem_invert_test(0, curr->itemflags, curr->selected)))
            continue;

        if (curr->selected) {
            curr->selected = FALSE;
            curr->count = -1;
        } else {
            curr->selected = TRUE;
            if (count > 0)
                curr->count = count;
        }
    }
}

/* support menucolor in addition to caller-supplied attribute */
static void
toggle_menu_attr(boolean on, int color, int attr)
{
    if (on) {
        term_start_attr(attr);
        if (color != NO_COLOR)
            term_start_color(color);
    } else {
        if (color != NO_COLOR)
            term_end_color();
        term_end_attr(attr);
    }
}

static void
process_menu_window(winid window, struct WinDesc *cw)
{
    tty_menu_item *page_start, *page_end, *curr;
    long count;
    int n, attr_n, curr_page, page_lines, resp_len, previous_page_lines;
    boolean finished, counting, reset_count, show_obj_syms,
            only_if_no_headers = (iflags.menuobjsyms & 4) != 0;
    char *cp, *rp, resp[QBUFSZ], gacc[QBUFSZ], *msave, *morestr, really_morc;
#define MENU_EXPLICIT_CHOICE 0x7f /* pseudo menu manipulation char */

    curr_page = page_lines = 0;
    page_start = page_end = 0;
    msave = cw->morestr; /* save the morestr */
    cw->morestr = morestr = (char *) alloc((unsigned) QBUFSZ);
    counting = FALSE;
    count = 0L;
    reset_count = TRUE;
    finished = FALSE;
    previous_page_lines = 0;

    /* collect group accelerators; for PICK_NONE, they're ignored;
       for PICK_ONE, only those which match exactly one entry will be
       accepted; for PICK_ANY, those which match any entry are okay */
    gacc[0] = '\0';
    if (cw->how != PICK_NONE) {
        int i, gcnt[128];
#define GSELIDX(c) ((c) & 127) /* guard against `signed char' */

        for (i = 0; i < SIZE(gcnt); i++)
            gcnt[i] = 0;
        for (n = 0, curr = cw->mlist; curr; curr = curr->next)
            if (curr->gselector && curr->gselector != curr->selector) {
                ++n;
                ++gcnt[GSELIDX(curr->gselector)];
            }

        if (n > 0) /* at least one group accelerator found */
            for (rp = gacc, curr = cw->mlist; curr; curr = curr->next)
                if (curr->gselector
                    && (curr->gselector != curr->selector
                        /* '$' is both a selector "letter" and a group
                           accelerator; including it in gacc allows gold to
                           be selected via group when not on current page */
                        || curr->gselector == GOLD_SYM)
                    && !strchr(gacc, curr->gselector)
                    && (cw->how == PICK_ANY
                        || gcnt[GSELIDX(curr->gselector)] == 1)) {
                    *rp++ = curr->gselector;
                    *rp = '\0'; /* re-terminate for strchr() */
                }
#undef GSELIDX
    }
    resp_len = 0; /* lint suppression */

    show_obj_syms = iflags.use_menu_glyphs;
    if (only_if_no_headers) {
        for (curr = cw->mlist; curr; curr = curr->next)
            if (curr->identifier.a_void == 0) {
                show_obj_syms = FALSE;
                break;
            }
    }

    /* loop until finished */
    while (!finished) {
        HUPSKIP();
        if (reset_count) {
            counting = FALSE;
            count = 0L;
        } else
            reset_count = TRUE;

        if (!page_start) {
            /* new page to be displayed */
            if (curr_page < 0 || (cw->npages > 0 && curr_page >= cw->npages))
                panic("bad menu screen page #%d", curr_page);

            /* clear screen */
            if (!cw->offx) { /* if not corner, do clearscreen */
                if (cw->offy) {
                    tty_curs(window, 1, 0);
                    cl_eos();
                } else
                    term_clear_screen();
            }

            rp = resp;
            if (cw->npages > 0) {
                /* collect accelerators */
                page_start = cw->plist[curr_page];
                page_end = cw->plist[curr_page + 1];
                for (page_lines = 0, curr = page_start; curr != page_end;
                     page_lines++, curr = curr->next) {
                    int attr, color = NO_COLOR;

                    if (curr->selector)
                        *rp++ = curr->selector;

                    tty_curs(window, 1, page_lines);
                    if (cw->offx)
                        cl_end();

                    (void) putchar(' ');
                    ++ttyDisplay->curx;

                    attr = curr->attr;
                    color = curr->color;

                    /* which character to start attribute highlighting;
                       whole line for headers and such, after the selector
                       character and space and selection indicator for menu
                       lines (including fake ones that simulate grayed-out
                       entries, so we don't rely on curr->identifier here) */
                    attr_n = 0; /* whole line */
                    if (curr->str[0] && curr->str[1] == ' '
                        && curr->str[2] && strchr("-+#", curr->str[2])
                        && curr->str[3] == ' ')
                        /* [0]=letter, [1]=space, [2]=[-+#], [3]=space */
                        attr_n = 4; /* [4:N]=entry description */

                    /*
                     * Don't use xputs() because (1) under unix it calls
                     * tputstr() which will interpret a '*' as some kind
                     * of padding information and (2) it calls xputc to
                     * actually output the character.  We're faster doing
                     * this.
                     */
                    for (n = 0, cp = curr->str;
                         *cp &&
#ifndef WIN32CON
                            (int) ++ttyDisplay->curx < (int) ttyDisplay->cols;
#else
                            (int) ttyDisplay->curx < (int) ttyDisplay->cols;
                         ttyDisplay->curx++,
#endif
                         cp++, n++) {
                        if (n == attr_n && (color != NO_COLOR
                                            || attr != ATR_NONE))
                            toggle_menu_attr(TRUE, color, attr);
                        if (n == 2 && curr->identifier.a_void != 0
                            && curr->selected) {
                            char c = (curr->count == -1L) ? '*' : '#';

                            /* all selected: '*' vs count selected: '#' */
                            (void) putchar(c);
                        } else if (n == 2 && curr->identifier.a_void != 0
                                   && show_obj_syms
                                   && curr->glyphinfo.glyph != NO_GLYPH) {
                            int gcolor = curr->glyphinfo.gm.sym.color;

                            /* tty_print_glyph could be used, but is overkill
                               and requires referencing the cursor location */
                            toggle_menu_attr(TRUE, gcolor, ATR_NONE);
                            (void) putchar(curr->glyphinfo.ttychar);
                            toggle_menu_attr(FALSE, gcolor, ATR_NONE);
                        } else {
                            (void) putchar(*cp);
                        }
                    } /* for *cp */
                    if (n > attr_n && (color != NO_COLOR || attr != ATR_NONE))
                        toggle_menu_attr(FALSE, color, attr);
                } /* if npages > 0 */
            } else {
                page_start = 0;
                page_end = 0;
                page_lines = 0;
            }
            *rp = 0;
            /* remember how many explicit menu choices there are */
            resp_len = (int) strlen(resp);

            /* corner window - clear extra lines from last page */
            if (cw->offx) {
                for (n = page_lines + 1; n < cw->maxrow; n++) {
                    tty_curs(window, 1, n);
                    cl_end();
                }
                /*
                 * If this corner menu was big, there are likely large
                 * portions of the map, status window, and tty perm_invent
                 * window (is there is one), that are all missing a lot of
                 * information. Let's repair the blacked-out rows now
                 * because it looks better.
                 */
                if (previous_page_lines != 0
                        && page_lines < previous_page_lines) {
                    /*
                     * +3 to leave a couple of blank rows
                     * under the menu to make it contrast well.
                     */
                    int row_startoffset = page_lines + 3;

                    if (row_startoffset > cw->maxrow - 1)
                        row_startoffset = cw->maxrow - 1;
                    docorner((int) cw->offx, cw->maxrow + 1, row_startoffset);
                }
            }
            /* set extra chars.. */
            if (*gacc) {
                Strcat(resp, gacc);             /* group accelerators */
                if (cw->how == PICK_ONE)
                    resp_len = (int) strlen(resp);
            }
            Strcat(resp, " ");                  /* next page or end */
            Strcat(resp, "0123456789\033\n\r"); /* counts, quit */
            Strcat(resp, gm.mapped_menu_cmds);
            Strcat(resp, default_menu_cmds);

            if (cw->npages > 1)
                Sprintf(cw->morestr, "(%d of %d)", curr_page + 1,
                        (int) cw->npages);
            else if (msave)
                Strcpy(cw->morestr, msave);
            else
                Strcpy(cw->morestr, defmorestr);

            tty_curs(window, 1, page_lines);
            cl_end();
            dmore(cw, resp);
        } else {
            /* just put the cursor back... */
            tty_curs(window, (int) strlen(cw->morestr) + 2, page_lines);
            xwaitforspace(resp);
        }

        really_morc = morc; /* (only used with MENU_EXPLICIT_CHOICE) */
        if ((rp = strchr(resp, morc)) != 0 && rp < resp + resp_len)
            /* explicit menu selection; don't override it if it also
               happens to match a mapped menu command (such as ':' to
               look inside a container vs ':' to search) */
            morc = MENU_EXPLICIT_CHOICE;
        else
            morc = map_menu_cmd(morc);

        switch (morc) {
        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            /* special case: '0' is also the default ball class;
               some menus use digits as potential group accelerators
               but their entries don't rely on counts */
            if (!counting && strchr(gacc, morc))
                goto group_accel;

            {
                long dgt = (long) (morc - '0');

                /* count = (10 * count) + (morc - '0'); */
                count = AppendLongDigit(count, dgt);
                if (count < 0L) /* overflow */
                    continue; /* reset_count is True */
            }
            /*
             * It is debatable whether we should allow 0 to
             * start a count.  There is no difference if the
             * item is selected.  If not selected, then
             * "0b" could mean:
             *
             *  count starting zero:    "zero b's"
             *  ignore starting zero:   "select b"
             *
             * At present I don't know which is better.
             */
            if (count != 0L) { /* ignore leading zeros */
                counting = TRUE;
                reset_count = FALSE;
            }
            break;
        case '\033': /* cancel - from counting or loop */
            if (!counting) {
                /* deselect everything */
                for (curr = cw->mlist; curr; curr = curr->next) {
                    curr->selected = FALSE;
                    curr->count = -1L;
                }
                cw->flags |= WIN_CANCELLED;
                finished = TRUE;
            }
            /* else only stop count */
            break;
        case '\0': /* finished (commit) */
        case '\n':
        case '\r':
            finished = TRUE;
            break;
        case ' ':
        case MENU_NEXT_PAGE:
            if (cw->npages > 0 && curr_page != cw->npages - 1) {
                previous_page_lines = page_lines;
                curr_page++;
                page_start = 0;
            } else if (morc == ' ') {
                /* ' ' finishes menus here, but stop '>' doing the same. */
                finished = TRUE;
            }
            break;
        case MENU_PREVIOUS_PAGE:
            if (cw->npages > 0 && curr_page != 0) {
                --curr_page;
                page_start = 0;
            }
            break;
        case MENU_FIRST_PAGE:
            if (cw->npages > 0 && curr_page != 0) {
                page_start = 0;
                curr_page = 0;
            }
            break;
        case MENU_LAST_PAGE:
            if (cw->npages > 0 && curr_page != cw->npages - 1) {
                page_start = 0;
                curr_page = cw->npages - 1;
            }
            break;
        case MENU_SELECT_PAGE:
            if (cw->how == PICK_ANY)
                set_all_on_page(window, page_start, page_end);
            break;
        case MENU_UNSELECT_PAGE:
            unset_all_on_page(window, page_start, page_end);
            break;
        case MENU_INVERT_PAGE:
            if (cw->how == PICK_ANY)
                invert_all_on_page(window, page_start, page_end, 0, -1L);
            break;
        case MENU_SELECT_ALL:
            if (cw->how == PICK_ANY) {
                /* entries on the current page need screen updating */
                set_all_on_page(window, page_start, page_end);
                /* set the rest; entries on current page will be skipped
                   because all of those will be 'already selected' now
                   (some won't be if they failed menuitem_invert_test()
                   in set_all_on_page() but those will fail it again here) */
                for (curr = cw->mlist; curr; curr = curr->next) {
                    if (!curr->identifier.a_void /* not selectable */
                        || curr->selected /* already selected */
                        /* FALSE: not currently selected */
                        || !menuitem_invert_test(1, curr->itemflags, FALSE))
                        continue;
                    curr->selected = TRUE;
                }
            } /* if PICK_ANY */
            break;
        case MENU_UNSELECT_ALL:
            /* entries on the current page need screen updating */
            unset_all_on_page(window, page_start, page_end);
            /* unset the rest; entries on current page will be skipped
               because none of those will still be in 'selected' state
               (unless they failed the menuitem_invert_test() in
               unset_all_on_page() but those will fail it again here) */
            for (curr = cw->mlist; curr; curr = curr->next) {
                if (!curr->identifier.a_void /* not selectable */
                    || !curr->selected /* already de-selected */
                    /* TRUE: currently selected */
                    || !menuitem_invert_test(2, curr->itemflags, TRUE))
                    continue;
                curr->selected = FALSE;
                curr->count = -1;
            }
            break;
        case MENU_INVERT_ALL:
            if (cw->how == PICK_ANY)
                invert_all(window, page_start, page_end, 0, -1L);
            break;
        case MENU_SEARCH:
            if (cw->how == PICK_NONE) {
                tty_nhbell();
                break;
            } else {
                char searchbuf[BUFSZ + 2], tmpbuf[BUFSZ] = DUMMY;
                boolean on_curr_page = FALSE;
                int lineno = 0;

                tty_getlin("Search for:", tmpbuf);
                if (!tmpbuf[0] || tmpbuf[0] == '\033')
                    break;
                Sprintf(searchbuf, "*%s*", tmpbuf);

                for (curr = cw->mlist; curr; curr = curr->next) {
                    if (on_curr_page)
                        lineno++;
                    if (curr == page_start)
                        on_curr_page = TRUE;
                    else if (curr == page_end)
                        on_curr_page = FALSE;
                    if (curr->identifier.a_void
                        && pmatchi(searchbuf, curr->str)) {
                        toggle_menu_curr(window, curr, lineno, on_curr_page,
                                         counting, count);
                        if (cw->how == PICK_ONE) {
                            finished = TRUE;
                            break;
                        }
                    }
                }
            }
            break;
        case MENU_EXPLICIT_CHOICE:
            morc = really_morc;
            FALLTHROUGH;
        /*FALLTHRU*/
        default:
            if (cw->how == PICK_NONE || !strchr(resp, morc)) {
                /* unacceptable input received */
                tty_nhbell();
                break;
            } else if (strchr(gacc, morc)) {
 group_accel:
                /* group accelerator; for the PICK_ONE case, we know that
                   it matches exactly one item in order to be in gacc[] */
                invert_all(window, page_start, page_end, morc,
                           counting ? count : -1L);
                if (cw->how == PICK_ONE)
                    finished = TRUE;
                break;
            }
            /* find, toggle, and possibly update */
            for (n = 0, curr = page_start; curr != page_end;
                 n++, curr = curr->next)
                if (morc == curr->selector) {
                    toggle_menu_curr(window, curr, n, TRUE, counting, count);
                    if (cw->how == PICK_ONE)
                        finished = TRUE;
                    break; /* from `for' loop */
                }
            break;
        }

    } /* while */
    cw->morestr = msave;
    free((genericptr_t) morestr);
#undef MENU_EXPLICIT_CHOICE
}

static void
process_text_window(winid window, struct WinDesc *cw)
{
    int i, n, attr;
    boolean linestart;
    char *cp;

    for (n = 0, i = 0; i < cw->maxrow; i++) {
        HUPSKIP();
        if (!cw->offx && (n + cw->offy == ttyDisplay->rows - 1)) {
            tty_curs(window, 1, n);
            cl_end();
            dmore(cw, quitchars);
            if (morc == '\033') {
                cw->flags |= WIN_CANCELLED;
                break;
            }
            if (cw->offy) {
                tty_curs(window, 1, 0);
                cl_eos();
            } else
                term_clear_screen();
            n = 0;
        }
        tty_curs(window, 1, n++);
#ifdef H2344_BROKEN
        cl_end();
#else
        if (cw->offx)
            cl_end();
#endif
        if (cw->data[i]) {
            attr = cw->data[i][0] - 1;
            if (cw->offx) {
                (void) putchar(' ');
                ++ttyDisplay->curx;
            }
            term_start_attr(attr);
            for (cp = &cw->data[i][1], linestart = TRUE;
#ifndef WIN32CON
                 *cp && (int) ++ttyDisplay->curx < (int) ttyDisplay->cols;
                 cp++
#else
                 *cp && (int) ttyDisplay->curx < (int) ttyDisplay->cols;
                 cp++, ttyDisplay->curx++
#endif
                 ) {
                /* message recall for msg_window:full/combination/reverse
                   might have output from '/' in it (see redotoplin()) */
                if (linestart) {
                    if (SYMHANDLING(H_UTF8)) {
                        /* FIXME: what is actually in that line? is it the \GNNNNNNNN or UTF-8? */
                        g_putch(*cp);
                    } else if ((*cp & 0x80) != 0) {
                        g_putch(*cp);
                        end_glyphout();
                    } else {
                        (void) putchar(*cp);
                    }
                    linestart = FALSE;
                } else {
                    (void) putchar(*cp);
                }
            }
            term_end_attr(attr);
        }
    }
    if (i == cw->maxrow) {
#ifdef H2344_BROKEN
        if (cw->type == NHW_TEXT) {
            tty_curs(BASE_WINDOW, 1, (int) ttyDisplay->cury + 1);
            cl_eos();
        }
#endif
        tty_curs(BASE_WINDOW, (int) cw->offx + 1,
                 (cw->type == NHW_TEXT) ? (int) ttyDisplay->rows - 1 : n);
        cl_end();
        dmore(cw, quitchars);
        if (morc == '\033')
            cw->flags |= WIN_CANCELLED;
    }
}

/*ARGSUSED*/
void
tty_display_nhwindow(
    winid window,
    boolean blocking) /* with ttys, all windows are blocking */
{
    struct WinDesc *cw = 0;
    short s_maxcol;

    HUPSKIP();
    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0)
        ttywindowpanic();
    if (cw->flags & WIN_CANCELLED)
        return;
    ttyDisplay->lastwin = window;
    ttyDisplay->rawprint = 0;

    print_vt_code2(AVTC_SELECT_WINDOW, window);

    switch (cw->type) {
    case NHW_MESSAGE:
        if (ttyDisplay->toplin == TOPLINE_NEED_MORE) {
            more();
            ttyDisplay->toplin = TOPLINE_NEED_MORE; /* more resets this */
            tty_clear_nhwindow(window);
            nhassert(ttyDisplay->toplin == TOPLINE_EMPTY);
        } else
            ttyDisplay->toplin = TOPLINE_EMPTY;
        cw->curx = cw->cury = 0;
        if (!cw->active)
            iflags.window_inited = TRUE;
        break;
    case NHW_MAP:
        end_glyphout();
        if (blocking) {
            if (ttyDisplay->toplin != TOPLINE_EMPTY)
                ttyDisplay->toplin = TOPLINE_NEED_MORE;
            tty_display_nhwindow(WIN_MESSAGE, TRUE);
            return;
        }
        FALLTHROUGH;
        /*FALLTHRU*/
    case NHW_BASE:
        (void) fflush(stdout);
        break;
    case NHW_TEXT:
        cw->maxcol = ttyDisplay->cols; /* force full-screen mode */
        FALLTHROUGH;
        /*FALLTHRU*/
    case NHW_MENU:
        cw->active = 1;
        /* cw->maxcol is a long, but its value is constrained to
           be <= ttyDisplay->cols, so is sure to fit within a short */
        s_maxcol = (short) cw->maxcol;
#ifdef H2344_BROKEN
        cw->offx = (cw->type == NHW_TEXT)
                       ? 0
                       : min(min(82, ttyDisplay->cols / 2),
                             ttyDisplay->cols - s_maxcol - 1);
#else
        /* avoid converting to uchar before calculations are finished */
        cw->offx = (uchar) max((int) 10,
                               (int) (ttyDisplay->cols - s_maxcol - 1));
#endif
        if (cw->offx < 0)
            cw->offx = 0;
        if (cw->type == NHW_MENU)
            cw->offy = 0;
        if (ttyDisplay->toplin == TOPLINE_NEED_MORE)
            tty_display_nhwindow(WIN_MESSAGE, TRUE);
#ifdef H2344_BROKEN
        if (cw->maxrow >= (int) ttyDisplay->rows
            || !iflags.menu_overlay)
#else
        if (cw->offx == 10 || cw->maxrow >= (int) ttyDisplay->rows
            || !iflags.menu_overlay)
#endif
        {
            cw->offx = 0;
            if (cw->offy || iflags.menu_overlay) {
                tty_curs(window, 1, 0);
                cl_eos();
            } else
                term_clear_screen();
            ttyDisplay->toplin = TOPLINE_EMPTY;
        } else {
            if (WIN_MESSAGE != WIN_ERR)
                tty_clear_nhwindow(WIN_MESSAGE);
        }

        if (cw->data || !cw->maxrow)
            process_text_window(window, cw);
        else
            process_menu_window(window, cw);
        break;
    }
    cw->active = 1;
}

void
tty_dismiss_nhwindow(winid window)
{
    struct WinDesc *cw = 0;

    HUPSKIP();
    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0)
        ttywindowpanic();

    print_vt_code2(AVTC_SELECT_WINDOW, window);

    switch (cw->type) {
    case NHW_MESSAGE:
        if (ttyDisplay->toplin != TOPLINE_EMPTY)
            tty_display_nhwindow(WIN_MESSAGE, TRUE);
        nhassert(ttyDisplay->toplin == TOPLINE_EMPTY);
        FALLTHROUGH;
        /*FALLTHRU*/
    case NHW_STATUS:
    case NHW_BASE:
    case NHW_MAP:
        /*
         * these should only get dismissed when the game is going away
         * or suspending
         */
        tty_curs(BASE_WINDOW, 1, (int) ttyDisplay->rows - 1);
        cw->active = 0;
        break;
    case NHW_MENU:
    case NHW_TEXT:
    case NHW_PERMINVENT:
        if (cw->active) {
            /* skip erasure if window_inited has been reset to 0 during
               final run-down in case this is the end-of-game window;
               the contents of that window should remain shown even when
               the window itself has gone away */
            if (iflags.window_inited && !erasing_tty_screen) {
                boolean clearscreen = FALSE; /* just erase the menu */

                /* during role/race/&c selection, menus are put up on top
                   of the base window; we don't track what was there so
                   can't refresh--force the screen to be cleared instead
                   (affects dismissal of 'reset role filtering' menu if
                   screen height forces that to need a second page) */
                if (program_state.in_role_selection)
                    clearscreen = TRUE;

                erase_menu_or_text(window, cw, clearscreen);
            }
            cw->active = 0;
        }
        break;
    }
    cw->flags = 0;
}

void
tty_destroy_nhwindow(winid window)
{
    struct WinDesc *cw = 0;

    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0) {
        if (window == WIN_INVEN) /* Null wins[WIN_INVEN] is tolerated */
            return;
        ttywindowpanic();
    }

    if (cw->active)
        tty_dismiss_nhwindow(window);
    if (cw->type == NHW_MESSAGE)
        iflags.window_inited = 0;
    if (cw->type == NHW_MAP)
        term_clear_screen();
#ifdef TTY_PERM_INVENT
    if (cw->type == NHW_MENU && cw->mbehavior == MENU_BEHAVE_PERMINV)
        ttyinv_remove_data(cw, TRUE);
#endif
    free_window_info(cw, TRUE);
    free((genericptr_t) cw);
    wins[window] = 0; /* available for re-use */
}

/* when screen is cleared, reset stored data to spaces */
void
erase_tty_screen(void)
{
    struct WinDesc *cw;
    int i;

    HUPSKIP();
    if (erasing_tty_screen++)
        return;
#if 0   /* originally we called term_clear_screen() but now it calls us */
    term_clear_screen(); /* erase the screen */
#endif
    /* update window data to reflect that each active window is all blanks */
    for (i = 0; i < MAXWIN; ++i) {
        cw = wins[i];
        if (cw && cw->active)
            tty_clear_nhwindow(i);
    }
    tty_curs(BASE_WINDOW, 1, 0);
    erasing_tty_screen = 0;
}

void
tty_curs(
    winid window,
    int x, int y)  /* coordinates (and curx-x) must be signed */
{
    struct WinDesc *cw = 0;
    int cx = ttyDisplay->curx;
    int cy = ttyDisplay->cury;

    HUPSKIP();
    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0)
        ttywindowpanic();
    ttyDisplay->lastwin = window;

    print_vt_code2(AVTC_SELECT_WINDOW, window);

#if defined(TILES_IN_GLYPHMAP) && defined(MSDOS)
    adjust_cursor_flags(cw);
#endif

#ifdef DEBUG
    if (x < 1 || y < 0 || y >= cw->rows || x > cw->cols) {
        const char *s = "[unknown type]";

        switch (cw->type) {
        case NHW_MESSAGE:
            s = "[topl window]";
            break;
        case NHW_STATUS:
            s = "[status window]";
            break;
        case NHW_MAP:
            s = "[map window]";
            break;
        case NHW_MENU:
            s = "[corner window]";
            break;
        case NHW_TEXT:
            s = "[text window]";
            break;
        case NHW_BASE:
            s = "[base window]";
            break;
        }
        /* avoid sending a line to the message window if we're complaining
           about cursor positioning in message window; perhaps raw_printf?
           this ought to be using impossible() so that someone might
           actually see it */
        if (cw->type != NHW_MESSAGE)
            debugpline4("tty_curs: bad positioning win %d %s <%d,%d>",
                        window, s, x, y);
        /* don't try to fix up x,y here because then tty_curs() would
           behave differently depending on whether DEBUG is defined;
           garbage in, garbage out is the order of the day... */
    }
#endif /* DEBUG */
    cw->curx = --x; /* column 0 is not used */
    cw->cury = y;

    x += cw->offx;
    y += cw->offy;

#ifdef CLIPPING
    if (clipping && window == WIN_MAP) {
        x -= clipx;
        y -= clipy;
    }
#endif

    if (y == cy && x == cx)
        return;

    if (cw->type == NHW_MAP)
        end_glyphout();

#ifndef NO_TERMS
    if (!nh_ND && (cx != x || x <= 3)) { /* Extremely primitive */
        cmov(x, y);                      /* bunker!wtm */
        return;
    }
#endif

    if ((cy -= y) < 0)
        cy = -cy;
    if ((cx -= x) < 0)
        cx = -cx;
    if (cy <= 3 && cx <= 3) {
        nocmov(x, y);
#ifndef NO_TERMS
    } else if ((x <= 3 && cy <= 3) || (!nh_CM && x < cx)) {
        (void) putchar('\r');
        ttyDisplay->curx = 0;
        nocmov(x, y);
    } else if (!nh_CM) {
        nocmov(x, y);
#endif
    } else
        cmov(x, y);

    ttyDisplay->curx = x;
    ttyDisplay->cury = y;
}

#ifndef STATUS_HILITES
static void
tty_putsym(winid window, int x, int y, char ch)
{
    struct WinDesc *cw = 0;

    HUPSKIP();
    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0)
        ttywindowpanic();

    print_vt_code2(AVTC_SELECT_WINDOW, window);

    switch (cw->type) {
#ifndef STATUS_HILITES
    case NHW_STATUS:
#endif
#ifdef TTY_PERM_INVENT
    case NHW_PERMINVENT:
#endif
    case NHW_MAP:
    case NHW_BASE:
        tty_curs(window, x, y);
        (void) putchar(ch);
        ttyDisplay->curx++;
        cw->curx++;
        break;
    case NHW_MESSAGE:
    case NHW_MENU:
    case NHW_TEXT:
        impossible("Can't putsym to window type %d", cw->type);
        break;
    }
}
#endif

static const char *
compress_str(const char *str)
{
    static char cbuf[BUFSZ];

    /* compress out consecutive spaces if line is too long;
       topline wrapping converts space at wrap point into newline,
       we reverse that here */
    if ((int) strlen(str) >= CO || strchr(str, '\n')) {
        const char *in_str = str;
        char c, *outstr = cbuf, *outend = &cbuf[sizeof cbuf - 1];
        boolean was_space = TRUE; /* True discards all leading spaces;
                                     False would retain one if present */

        while ((c = *in_str++) != '\0' && outstr < outend) {
            if (c == '\n')
                c = ' ';
            if (was_space && c == ' ')
                continue;
            *outstr++ = c;
            was_space = (c == ' ');
        }
        if ((was_space && outstr > cbuf) || outstr == outend)
            --outstr; /* remove trailing space or make room for terminator */
        *outstr = '\0';
        str = cbuf;
    }
    return str;
}

void
tty_putstr(winid window, int attr, const char *str)
{
    struct WinDesc *cw = 0;
    char *ob;
    long i, n0;
#ifndef STATUS_HILITES
    const char *nb;
    long j;
#endif

    HUPSKIP();
    /* Assume there's a real problem if the window is missing --
     * probably a panic message
     */
    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0) {
        tty_raw_print(str);
        return;
    }

    if (str == (const char *) 0
        || ((cw->flags & WIN_CANCELLED) && (cw->type != NHW_MESSAGE)))
        return;
    if (cw->type != NHW_MESSAGE
#ifdef TTY_PERM_INVENT
        && window != WIN_INVEN
#endif
       )
        str = compress_str(str);

    ttyDisplay->lastwin = window;

    print_vt_code2(AVTC_SELECT_WINDOW, window);

    switch (cw->type) {
    case NHW_MESSAGE: {
        int suppress_history = (attr & ATR_NOHISTORY),
            urgent_message = (attr & ATR_URGENT);

#if defined(ASCIIGRAPH) && !defined(NO_TERMS)
        /* if ^C occurs, player is prompted with "Really quit?" and that
           prompt is issued via tty_putstr(WIN_MESSAGE); if ^C happens
           while writing DECgraphics chars, the prompt text would be
           rendered as VT line-drawing characters unless we do this;
           also, if color was in progress it wouldn't be switched off */
        end_glyphout();
#endif
        /* if message is designated 'urgent' don't suppress it if user has
           typed ESC at --More-- prompt when dismissing an earlier message;
           besides turning off WIN_STOP, we need to prevent current message
           from provoking --More-- and giving the user another chance at
           using ESC to suppress, otherwise this message wouldn't get shown */
        if (urgent_message) {
            if ((cw->flags & WIN_STOP) != 0) {
                tty_clear_nhwindow(WIN_MESSAGE);
                cw->flags &= ~WIN_STOP;
            }
            cw->flags |= WIN_NOSTOP;
        }

        /* in case we ever support display attributes for topline
           messages, clear flag mask leaving only display attr */
        /*attr &= ~(ATR_URGENT | ATR_NOHISTORY);*/

        if (!suppress_history) {
            /* normal output; add to current top line if room, else flush
               whatever is there to history and then write this */
            update_topl(str);
        } else {
            /* put anything already on top line into history */
            remember_topl();
            /* write to top line without remembering what we're writing */
            show_topl(str);
        }

        cw->flags &= ~WIN_NOSTOP; /* NOSTOP is a one-shot operation */
        break;
    }
#ifndef STATUS_HILITES
    case NHW_STATUS:
        ob = &cw->data[cw->cury][j = cw->curx];
        if (disp.botlx)
            *ob = '\0';
        if (!cw->cury && (int) strlen(str) >= CO) {
            /* the characters before "St:" are unnecessary */
            nb = strchr(str, ':');
            if (nb && nb > str + 2)
                str = nb - 2;
        }
        nb = str;
        for (i = cw->curx + 1, n0 = cw->cols; i < n0; i++, nb++) {
            if (!*nb) {
                if (*ob || disp.botlx) {
                    /* last char printed may be in middle of line */
                    tty_curs(WIN_STATUS, i, cw->cury);
                    cl_end();
                }
                break;
            }
            if (*ob != *nb)
                tty_putsym(WIN_STATUS, i, cw->cury, *nb);
            if (*ob)
                ob++;
        }

        (void) strncpy(&cw->data[cw->cury][j], str, cw->cols - j - 1);
        cw->data[cw->cury][cw->cols - 1] = '\0'; /* null terminate */
        cw->cury = (cw->cury + 1) % cw->maxrow;
        cw->curx = 0;
        break;
#endif /* STATUS_HILITES */
    case NHW_MAP:
        tty_curs(window, cw->curx + 1, cw->cury);
        term_start_attr(attr);
        while (*str && (int) ttyDisplay->curx < (int) ttyDisplay->cols - 1) {
            (void) putchar(*str);
            str++;
            ttyDisplay->curx++;
        }
        cw->curx = 0;
        cw->cury++;
        term_end_attr(attr);
        break;
    case NHW_BASE:
        tty_curs(window, cw->curx + 1, cw->cury);
        term_start_attr(attr);
        while (*str) {
            if ((int) ttyDisplay->curx >= (int) ttyDisplay->cols - 1) {
                cw->curx = 0;
                cw->cury++;
                tty_curs(window, cw->curx + 1, cw->cury);
            }
            (void) putchar(*str);
            str++;
            ttyDisplay->curx++;
        }
        cw->curx = 0;
        cw->cury++;
        term_end_attr(attr);
        break;
    case NHW_MENU:
    case NHW_TEXT:
#ifdef H2344_BROKEN
        if (cw->type == NHW_TEXT
            && (cw->cury + cw->offy) == ttyDisplay->rows - 1)
#else
        if (cw->type == NHW_TEXT && cw->cury == ttyDisplay->rows - 1)
#endif
        {
            /* not a menu, so save memory and output 1 page at a time */
            cw->maxcol = ttyDisplay->cols; /* force full-screen mode */
            tty_display_nhwindow(window, TRUE);
            for (i = 0; i < cw->maxrow; i++)
                if (cw->data[i]) {
                    free((genericptr_t) cw->data[i]);
                    cw->data[i] = 0;
                }
            cw->maxrow = cw->cury = 0;
        }
        /* always grows one at a time, but alloc 12 at a time */
        if (cw->cury >= cw->rows) {
            char **tmp;

            cw->rows += 12;
            tmp = (char **) alloc(sizeof(char *) * (unsigned) cw->rows);
            for (i = 0; i < cw->maxrow; i++)
                tmp[i] = cw->data[i];
            if (cw->data)
                free((genericptr_t) cw->data);
            cw->data = tmp;

            for (i = cw->maxrow; i < cw->rows; i++)
                cw->data[i] = 0;
        }
        if (cw->data[cw->cury])
            free((genericptr_t) cw->data[cw->cury]);
        n0 = (long) strlen(str) + 1L;
        ob = cw->data[cw->cury] = (char *) alloc((unsigned) n0 + 1);
        *ob++ = (char) (attr + 1); /* avoid nuls, for convenience */
        Strcpy(ob, str);

        if (n0 > cw->maxcol)
            cw->maxcol = n0;
        if (++cw->cury > cw->maxrow)
            cw->maxrow = cw->cury;
        if (n0 > CO) {
            /* attempt to break the line */
            for (i = CO - 1; i && str[i] != ' ' && str[i] != '\n';)
                i--;
            if (i) {
                cw->data[cw->cury - 1][++i] = '\0';
                tty_putstr(window, attr, &str[i]);
            }
        }
        break;
    }
    return;
}

void
tty_display_file(
    const char *fname, /* name of file to display */
    boolean complain)  /* whether to report problem if file can't be opened */
{
#ifdef DEF_PAGER /* this implies that UNIX is defined */
    /* FIXME:  this won't work if fname is inside a dlb container */
    {
        /* use external pager; this may give security problems */
        int fd = open(fname, O_RDONLY);

        if (fd < 0) {
            if (complain)
                pline("Cannot open %s.", fname);
            else /* [is this refresh actually necessary?] */
                docrt();
            return;
        }
        if (child(1)) {
            /* Now that child() does a setuid(getuid()) and a chdir(),
               we may not be able to open file fname anymore, so make
               it stdin. */
            (void) close(0);
            if (dup(fd)) {
                if (complain)
                    raw_printf("Cannot open %s as stdin.", fname);
            } else {
                (void) execlp(gc.catmore, "page", (char *) 0);
                if (complain)
                    raw_printf("Cannot exec %s.", gc.catmore);
            }
            if (complain)
                sleep(10); /* want to wait_synch() but stdin is gone */
            nh_terminate(EXIT_FAILURE);
        }
        (void) close(fd);
    }
#else /* DEF_PAGER */
    {
        dlb *f;
        char buf[BUFSZ];
        char *cr;

        tty_clear_nhwindow(WIN_MESSAGE);
        f = dlb_fopen(fname, "r");
        if (!f) {
            if (complain) {
                home();
                tty_mark_synch();
                tty_raw_print("");
                perror(fname);
                tty_wait_synch(); /* "Hit <space> to continue: " */
                if (u.ux) /* if hero is on map, refresh the screen */
                    docrt();
                pline("Cannot open \"%s\".", fname);
            }
        } else {
            winid datawin = tty_create_nhwindow(NHW_TEXT);
            boolean empty = TRUE;

            if (complain
#ifndef NO_TERMS
                && nh_CD
#endif
                ) {
                /* attempt to scroll text below map window if there's room */
                wins[datawin]->offy = wins[WIN_STATUS]->offy + StatusRows();
                if ((int) wins[datawin]->offy + 12 > (int) ttyDisplay->rows)
                    wins[datawin]->offy = 0;
            }
            while (dlb_fgets(buf, BUFSZ, f)) {
                if ((cr = strchr(buf, '\n')) != 0)
                    *cr = 0;
#ifdef MSDOS
                if ((cr = strchr(buf, '\r')) != 0)
                    *cr = 0;
#endif
                if (strchr(buf, '\t') != 0)
                    (void) tabexpand(buf);
                empty = FALSE;
                tty_putstr(datawin, 0, buf);
                if (wins[datawin]->flags & WIN_CANCELLED)
                    break;
            }
            if (!empty)
                tty_display_nhwindow(datawin, FALSE);
            tty_destroy_nhwindow(datawin);
            (void) dlb_fclose(f);
        }
    }
#endif /* DEF_PAGER */
    return;
}

void
tty_start_menu(winid window, unsigned long mbehavior)
{
    struct WinDesc *cw = 0;

    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0)
        ttywindowpanic();

#ifdef TTY_PERM_INVENT
    if (cw->mbehavior == MENU_BEHAVE_PERMINV) {
        /* PERMINV is ready to go already; not much to do here */
        inuse_only_start = 0;
        return;
    }
    if (mbehavior == MENU_BEHAVE_PERMINV
        && (iflags.perm_invent
            || gp.perm_invent_toggling_direction == toggling_on)) {
        winid w = ttyinv_create_window(window, wins[window]);

        if (w == WIN_ERR) {
            ; /* something went wrong, so add clean up code here */
        } else {
            cw->mbehavior = mbehavior;
            WIN_INVEN = w;
        }
        return;
    }
#else
    nhUse(mbehavior);
#endif

    tty_clear_nhwindow(window);
    return;
}

    /*ARGSUSED*/
/*
 * Add a menu item to the beginning of the menu list.  This list is reversed
 * later.
 */
void
tty_add_menu(
    winid window,  /* window to use, must be of type NHW_MENU */
    const glyph_info *glyphinfo, /* glyph info with glyph to
                                  * display with item */
    const anything *identifier, /* what to return if selected */
    char ch,                /* selector letter (0 = pick our own) */
    char gch,               /* group accelerator (0 = no group) */
    int attr,               /* attribute for string (like tty_putstr()) */
    int clr,                /* color for string */
    const char *str,        /* menu string */
    unsigned int itemflags) /* itemflags such as MENU_ITEMFLAGS_SELECTED */
{
    boolean preselected = ((itemflags & MENU_ITEMFLAGS_SELECTED) != 0);
    struct WinDesc *cw = 0;
    tty_menu_item *item;
    const char *newstr;
    char buf[4 + BUFSZ];

    HUPSKIP();
    if (str == (const char *) 0)
        return;

    if (window == WIN_ERR
        || (cw = wins[window]) == (struct WinDesc *) 0
        || cw->type != NHW_MENU)
        ttywindowpanic();

#ifdef TTY_PERM_INVENT
    if (cw->mbehavior == MENU_BEHAVE_PERMINV) {
        ttyinv_add_menu(window, cw, ch, attr, clr, str);
        return;
    }
#endif

    cw->nitems++;
    if (identifier->a_void) {
        int len = (int) strlen(str);

        if (len >= BUFSZ) {
            /* We *think* everything's coming in off at most BUFSZ bufs... */
            impossible("Menu item too long (%d).", len);
            len = BUFSZ - 1;
        }
        Sprintf(buf, "%c - ", ch ? ch : '?');
        (void) strncpy(buf + 4, str, len);
        buf[4 + len] = '\0';
        newstr = buf;
    } else
        newstr = str;

    item = (tty_menu_item *) alloc(sizeof *item);
    item->identifier = *identifier;
    item->count = -1L;
    item->selected = preselected;
    item->itemflags = itemflags;
    item->selector = ch;
    item->gselector = gch;
    item->attr = attr;
    item->color = clr;
    item->str = dupstr(newstr);
    item->glyphinfo = *glyphinfo;

    item->next = cw->mlist;
    cw->mlist = item;
}

/* Invert the given list, can handle NULL as an input. */
static tty_menu_item *
reverse(tty_menu_item *curr)
{
    tty_menu_item *next, *head = 0;

    while (curr) {
        next = curr->next;
        curr->next = head;
        head = curr;
        curr = next;
    }
    return head;
}

static color_attr tty_menu_promptstyle = { NO_COLOR, ATR_NONE };

/*
 * End a menu in this window, window must a type NHW_MENU.  This routine
 * processes the string list.  We calculate the # of pages, then assign
 * keyboard accelerators as needed.  Finally we decide on the width and
 * height of the window.
 */
void
tty_end_menu(
    winid window,       /* menu to use */
    const char *prompt) /* prompt to for menu */
{
    struct WinDesc *cw = 0;
    tty_menu_item *curr;
    short len;
    int lmax, n;
    char menu_ch;
    int clr = NO_COLOR;

    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0
        || cw->type != NHW_MENU) {
        /* this can happen if start_menu failed due to size requirements
           for the tty perm inventory window. It isn't a situation that
           requires a panic, just an early return. */
        if (window == WIN_INVEN && !cw)
            return;
        ttywindowpanic();
    }
#ifdef TTY_PERM_INVENT
    /* (probably don't need to check both of these conditions) */
    if (cw->mbehavior == MENU_BEHAVE_PERMINV && window == WIN_INVEN) {
        ttyinv_end_menu(window, cw);
        return;
    }
#endif

    /* Reverse the list so that items are in correct order. */
    cw->mlist = reverse(cw->mlist);

    /* Put the prompt at the beginning of the menu. */
    if (prompt) {
        anything any;

        any = cg.zeroany; /* not selectable */
        tty_add_menu(window, &nul_glyphinfo, &any, 0, 0,
                     ATR_NONE, clr, "", MENU_ITEMFLAGS_NONE);
        tty_add_menu(window, &nul_glyphinfo, &any, 0, 0,
                     tty_menu_promptstyle.attr, tty_menu_promptstyle.color,
                     prompt, MENU_ITEMFLAGS_NONE);
    }

    /* 52: 'a'..'z' and 'A'..'Z'; avoids selector duplication within a page */
    lmax = min(52, (int) ttyDisplay->rows - 1);    /* # lines per page */
    cw->npages = (cw->nitems + (lmax - 1)) / lmax; /* # of pages */
    /*
     * TODO?
     *  For really tall page, allow 53 if '$' or '#' is present and
     *  54 if both are.  [Only for single page menu, otherwise pages
     *  without those could try to use too many letters.]
     *  Probably not worth bothering with; anyone with a display big
     *  for this to matter will likely switch from tty to curses for
     *  multi-line message window and/or persistent inventory window.
     */

    /* make sure page list is large enough */
    if (cw->plist_size < cw->npages + 1) { /* +1: need one slot beyond last */
        if (cw->plist)
            free((genericptr_t) cw->plist);
        cw->plist_size = cw->npages + 1;
        cw->plist = (tty_menu_item **) alloc(cw->plist_size
                                             * sizeof (tty_menu_item *));
    }

    cw->cols = 0;  /* cols is set when the win is initialized... (why?) */
    menu_ch = '?'; /* lint suppression */
    for (n = 0, curr = cw->mlist; curr; n++, curr = curr->next) {
        /* set page boundaries and character accelerators */
        if ((n % lmax) == 0) {
            menu_ch = 'a';
            cw->plist[n / lmax] = curr;
        }
        if (curr->identifier.a_void && !curr->selector) {
            curr->str[0] = curr->selector = menu_ch;
            if (menu_ch++ == 'z')
                menu_ch = 'A';
        }

        /* cut off any lines that are too long */
        len = strlen(curr->str) + 2; /* extra space at beg & end */
        if (len > (int) ttyDisplay->cols) {
            curr->str[ttyDisplay->cols - 2] = 0;
            len = ttyDisplay->cols;
        }
        if (len > cw->cols)
            cw->cols = len;
    }
    cw->plist[cw->npages] = 0; /* plist terminator */

    /*
     * If greater than 1 page, morestr is "(x of y) " otherwise, "(end) "
     */
    if (cw->npages > 1) {
        char buf[QBUFSZ];
        /* produce the largest demo string */
        Sprintf(buf, "(%ld of %ld) ", cw->npages, cw->npages);
        len = strlen(buf);
        cw->morestr = dupstr("");
    } else {
        cw->morestr = dupstr("(end) ");
        len = strlen(cw->morestr);
    }

    if (len > (int) ttyDisplay->cols) {
        /* truncate the prompt if it's too long for the screen */
        if (cw->npages <= 1) /* only str in single page case */
            cw->morestr[ttyDisplay->cols] = 0;
        len = ttyDisplay->cols;
    }
    if (len > cw->cols)
        cw->cols = len;

    cw->maxcol = cw->cols;

    /*
     * The number of lines in the first page plus the morestr will be the
     * maximum size of the window.
     */
    if (cw->npages > 1)
        cw->maxrow = cw->rows = lmax + 1;
    else
        cw->maxrow = cw->rows = cw->nitems + 1;
}

int
tty_select_menu(winid window, int how, menu_item **menu_list)
{
    struct WinDesc *cw = 0;
    tty_menu_item *curr;
    menu_item *mi;
    int n, cancelled;

    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0
        || cw->type != NHW_MENU)
        ttywindowpanic();

    if (cw->mbehavior == MENU_BEHAVE_PERMINV) {
        return 0;
    }
    *menu_list = (menu_item *) 0;
    cw->how = (short) how;
    morc = 0;
    tty_display_nhwindow(window, TRUE);
    cancelled = !!(cw->flags & WIN_CANCELLED);
    tty_dismiss_nhwindow(window); /* does not destroy window data */

    if (cancelled) {
        n = -1;
    } else {
        for (n = 0, curr = cw->mlist; curr; curr = curr->next)
            if (curr->selected)
                n++;
    }

    if (n > 0) {
        *menu_list = (menu_item *) alloc(n * sizeof(menu_item));
        for (mi = *menu_list, curr = cw->mlist; curr; curr = curr->next)
            if (curr->selected) {
                mi->item = curr->identifier;
                mi->count = curr->count;
                mi++;
            }
    }

    return n;
}

/* special hack for treating top line --More-- as a one item menu */
char
tty_message_menu(char let, int how, const char *mesg)
{
    HUPSKIP_RESULT('\033');
    /* "menu" without selection; use ordinary pline, no more() */
    if (how == PICK_NONE) {
        pline("%s", mesg);
        return 0;
    }

    ttyDisplay->dismiss_more = let;
    morc = 0;
    /* barebones pline(); since we're only supposed to be called after
       response to a prompt, we'll assume that the display is up to date */
    tty_putstr(WIN_MESSAGE, 0, mesg);
    /* if `mesg' didn't wrap (triggering --More--), force --More-- now */
    if (ttyDisplay->toplin == TOPLINE_NEED_MORE) {
        more();
        ttyDisplay->toplin = TOPLINE_NEED_MORE; /* more resets this */
        tty_clear_nhwindow(WIN_MESSAGE);
        nhassert(ttyDisplay->toplin == TOPLINE_EMPTY);
    }
    /* normally <ESC> means skip further messages, but in this case
       it means cancel the current prompt; any other messages should
       continue to be output normally */
    wins[WIN_MESSAGE]->flags &= ~WIN_CANCELLED;
    ttyDisplay->dismiss_more = 0;

    return ((how == PICK_ONE && morc == let) || morc == '\033') ? morc : '\0';
}

win_request_info *
tty_ctrl_nhwindow(
    winid window UNUSED,
    int request,
    win_request_info *wri)
{
#if defined(TTY_PERM_INVENT)
    boolean tty_ok /*, show_gold */, inuse_only;
    int maxslot;
    /* these types are set match the wintty.h field declarations */
    long minrow; /* long to match maxrow declaration in wintty.h */
    short offx, offy;
    long rows, cols, maxrow, maxcol;
#endif

    if (!wri)
        return (win_request_info *) 0;

    switch (request) {
    case set_mode:
    case request_settings:
#if defined(TTY_PERM_INVENT)
        ttyinvmode = wri->fromcore.invmode;
        if (request == set_mode)
            break;
        /* show_gold = (ttyinvmode & InvShowGold) != 0; */
        inuse_only = ((ttyinvmode & InvInUse) != 0);
        wri->tocore = zero_tocore;
        tty_ok = assesstty(ttyinvmode, &offx, &offy, &rows, &cols, &maxcol,
                           &minrow, &maxrow);
        if (!tty_ok && rows == 0 && cols == 0) {
            /* something is terribly wrong, possibly too early in startup */
            wri->tocore.tocore_flags |= too_early;
        } else {
            wri->tocore.needrows = (int) (minrow + 1 + ROWNO + StatusRows());
            wri->tocore.needcols = (int) tty_perminv_mincol;
            wri->tocore.haverows = (int) ttyDisplay->rows;
            wri->tocore.havecols = (int) ttyDisplay->cols;
            if (!tty_ok) {
#ifdef RESIZABLE
                /* terminal isn't big enough right now but player might
                 * resize it and then use 'm O' to try to set 'perm_invent'
                 * again
                 */
                wri->tocore.tocore_flags |= too_small;
#else
                wri->tocore.tocore_flags |= prohibited;
#endif
            } else {
                maxslot = (maxrow - 2) * (!inuse_only ? 2 : 1);
                wri->tocore.maxslot = maxslot;
            }
        }
#endif  /* TTY_PERM_INVENT */
        break;
    case set_menu_promptstyle:
        tty_menu_promptstyle = wri->fromcore.menu_promptstyle;
        break;
    default:
        break;
    }
    return wri;
}

#ifdef TTY_PERM_INVENT

static int
ttyinv_create_window(int newid, struct WinDesc *newwin)
{
    int r, c;
    long minrow; /* long to match maxrow declaration */
    unsigned n;

    /* Is there enough real estate to do this beyond the status line?
     * Rows:
     * Top border line (1)
     * 26 inventory rows (26)
     * [should be 27 to have room for '$' and '#']
     * Bottom border line (1)
     * 1 + 26 + 1 = 28
     *
     * Cols:
     * Left border (1)
     * Left inventory items (38)
     * Middle separation (1)
     * Right inventory items (38)
     * Right border (1)
     * 1 + 38 + 1 + 38 + 1 = 79
     *
     * The topline + map rows + status lines require:
     * 1 + 21 + 2 (or 3) = 24 (or 25 depending on status line count).
     * So we can only present a full inventory on tty if there are
     * 28 + 24 (or 25) available (52 or 53 rows on the terminal).
     * Correspondingly ttyDisplay->rows has to be at least 52 (or 53).
     * [The top and bottom borderlines aren't necessary.  Suppressing
     * them would reduce the number of rows needed by 2.]
     *
     */

    /* preliminary init in case tty_destroy_nhwindow() gets called */
    newwin->data = (char **) 0;
    newwin->datlen = (short *) 0;
    newwin->cells = (struct tty_perminvent_cell **) 0;

    if (!assesstty(ttyinvmode, &newwin->offx, &newwin->offy, &newwin->rows,
                   &newwin->cols, &newwin->maxcol, &minrow,
                   &newwin->maxrow)) {
        tty_destroy_nhwindow(newid);
        WIN_INVEN = WIN_ERR;
        pline("%s.", "tty perm_invent could not be enabled");
        pline("tty perm_invent needs a terminal that is at least %dx%d, "
              "yours is %dx%d.",
              (int) (minrow + 1 + ROWNO + StatusRows()), tty_perminv_mincol,
              ttyDisplay->rows, ttyDisplay->cols);
        tty_wait_synch();
#ifndef RESIZABLE
        set_option_mod_status("perm_invent", set_gameview);
#endif
        iflags.perm_invent = FALSE;
        return WIN_ERR;
    }

    /*
     * Terminal/window/screen is big enough.
     */
    /*newwin->maxrow = minrow;*/
    newwin->maxcol = newwin->cols;
    /* establish the borders */
    bordercol[border_left] = 0;
    bordercol[border_middle] = (newwin->maxcol + 1) / 2;
    bordercol[border_right] = newwin->maxcol - 1;
    /* for in-use mode, use full lines; it will switch to two panels if
       there are more items than the number of full lines */
    if ((ttyinvmode & InvInUse) != 0)
        bordercol[border_middle] = bordercol[border_right];
    ttyinv_slots_used = 0;

    n = (unsigned) (newwin->maxrow * sizeof (struct tty_perminvent_cell *));
    newwin->cells = (struct tty_perminvent_cell **) alloc(n);

    n = (unsigned) (newwin->maxcol * sizeof (struct tty_perminvent_cell));
    for (r = 0; r < newwin->maxrow; r++) {
        newwin->cells[r] = (struct tty_perminvent_cell *) alloc(n);
        for (c = 0; c < newwin->maxcol; c++)
            newwin->cells[r][c] = emptyttycell;
    }
    newwin->active = 1;
    tty_invent_box_glyph_init(newwin);

    return newid;
}

/* discard perminvent window or erase it and set remembered data to spaces */
static void
ttyinv_remove_data(struct WinDesc *cw, boolean destroy)
{
    if (!cw) {
        impossible("Removing ttyinv data for nonexistent perm invent window?");
        return;
    }

    if (cw->cells) {
        int r, c;

        for (r = 0; r < cw->maxrow; r++) {
            if (cw->cells[r]) {
                for (c = 0; c < cw->maxcol; c++) {
                    struct tty_perminvent_cell *invcell = &cw->cells[r][c];

                    /* glyph is a flag indicating whether content union
                       contains a glyph_info structure or just a char */
                    if (invcell->glyph)
                        free((genericptr_t) invcell->content.gi);
                    *invcell = emptyttycell; /* sets glyph flag to 0 */
                    if (!destroy) { /* erasing */
                        invcell->content.ttychar = ' ';
                        invcell->text = 1;
                        invcell->refresh = 1; /* full redraw wanted */
                    }
                }
                if (destroy)
                    free((genericptr_t) cw->cells[r]),
                        cw->cells[r] = (struct tty_perminvent_cell *) 0;
            }
        }
        if (destroy) {
            free((genericptr_t) cw->cells),
                cw->cells = (struct tty_perminvent_cell **) 0;
            cw->rows = cw->cols = 0;
        }
    }
    if (destroy) {
        cw->maxrow = cw->maxcol = 0;
        /*WIN_INVEN = WIN_ERR;*/ /* caller's responsibility */
        done_tty_perm_invent_init = FALSE;
    }
    ttyinv_slots_used = 0; /* reset: inuse_only isn't using any slots */
}

static void
ttyinv_add_menu(
    winid window UNUSED,
    struct WinDesc *cw,
    char ch,
    int attr UNUSED, int clr,
    const char *str)
{
    char invbuf[BUFSZ];
    const char *text;
    boolean show_gold = (ttyinvmode & InvShowGold) != 0,
            inuse_only = (ttyinvmode & InvInUse) != 0,
            ignore = FALSE;
    int row, side, slot, startcolor_at = 0,
        rows_per_side = (inuse_only ? (cw->maxrow - 2)
                         : !show_gold ? 26
                           : 27);

    if (!program_state.in_moveloop)
        return;
    slot = selector_to_slot(ch, ttyinvmode, &ignore);
    if (inuse_only && slot > 2 * rows_per_side)
        ignore = TRUE; /* left & right sides full; no 3rd 'side' available */
    if (!ignore) {
        slot_tracker[slot] = TRUE;
        /* if we need to expand inuse_only from one side to two, do so now;
           entries are shown in row 1 thru rows_per_side (with rows 0 and
           rows_per_side+1 containing boundary lines) but stored in
           slots 0 thru rows_per_side-1, so zero-based slot==rows_per_side
           is the entry that will be shown on first row of the second side
           (and one-base ttyinv_slots_used==rows_per_side means that on the
           previous inventory_update(), full lines filled all the rows;
           ttyinv_slots_used==0 means that we've just enabled perm_invent) */
        if (inuse_only && slot == rows_per_side
            && ttyinv_slots_used % rows_per_side == 0)
            ttyinv_inuse_twosides(cw, rows_per_side);

        text = str; /* 'text' will switch to invbuf[] below */
        /* strip away "a"/"an"/"the" prefix to show a bit more of
            the interesting part of the object's description; this
            is inline version of pi_article_skip() from cursinvt.c;
            should move that to hacklib.c and use it here */
        if (text[0] == 'a') {
            if (text[1] == ' ')
                text += 2;
            else if (text[1] == 'n' && text[2] == ' ')
                text += 3;
        } else if (text[0] == 't') {
            if (text[1] == 'h' && text[2] == 'e' && text[3] == ' ')
                text += 4;
        }
        /*
         * TODO?
         *  Replace "c - " prefix with "c: " or just "c " to have a bit more
         *  room for 'text' (the item description).  If this prefix gets
         *  changed, the indentation for empty inventory in ttyinv_render()
         *  should be changed to match.
         */
        Snprintf(invbuf, sizeof invbuf, "%c - %s", ch, text);
        text = invbuf;
        startcolor_at = (int) (sizeof "a - " - sizeof ""); /* 4 */
        row = (slot % rows_per_side) + 1; /* +1: top border */
        /* side: left side panel or right side panel, not a window column */
        side = slot / rows_per_side;
        ttyinv_populate_slot(cw, row, side, text,
                             (uint32) clr, startcolor_at);
    }
    return;
}

/* convert a..zA..Z to 0..51 or for 'show_gold' $a..zA..Z# to 0..53 */
static int
selector_to_slot(
    char ch,
    const int invflags,
    boolean *ignore)
{
    int slot = 0;
    boolean show_gold = (invflags & InvShowGold) != 0,
            inuse_only = (invflags & InvInUse) != 0;

    *ignore = FALSE;
    if (inuse_only) {
        /*
         * Inuse_only uses slots 0 to N-1 instead of fixed positions.
         * Caller--or caller's caller's ... caller--only passes us
         * relevant items so no filtering is necessary here.
         * '!show_gold' gets ignored here since gold might be quivered
         * and become an in-use item.
         * Inuse_only items might include one (or more than one) with
         * invlet '#' but it isn't special for Inuse_only.
         * When nothing is in use, the core sends a menu header line to
         * to indicate such in order to avoid having an empty menu; we
         * ignore that and reconstruct one in ttyinv_render().
         */
        if (!ch)
            *ignore = TRUE;
        else
            slot = inuse_only_start++;
    } else {
        /* normal perminv */
        switch (ch) {
        case '$':
            if (!show_gold)
                *ignore = TRUE;
            else
                slot = 0;
            break;
        case '#':
            /* overflow item won't be gold but there is only room allocated
               for it when gold is also eligible to be shown (hero doesn't
               have to be carrying any, we just need the extra row it gets) */
            if (!show_gold)
                *ignore = TRUE;
            else
                /* fixed location, bottom of right-hand panel; if there is
                   more than 1 overflow item, we are only able to show 1 */
                slot = 0 + 52 + 1; /* 0 for gold, 2 * 26 letters, then '#' */
            break;
        case '\0':
            *ignore = TRUE;
            break;
        default:
            if (ch >= 'a' && ch <= 'z')
                slot = (ch - 'a') + (show_gold ? 1 : 0);
            else if (ch >= 'A' && ch <= 'Z')
                slot = (ch - 'A') + (show_gold ? 1 : 0) + 26;
            break;
        }
    } /* normal vs inuse_only */
    return slot;
}

static char
slot_to_invlet(int slot, boolean incl_gold)
{
    char res = '\0';

    switch (slot) {
    case 0:
        res = incl_gold ? '$' : 'a';
        break;
    case 53:
        /* if (!incl_gold) impossible("ttyinv_render: slot %d?", slot); */
        res = '#';
        break;
    default:
        if (incl_gold)
            --slot; /* simplify 1..52 (0 and 53 won't get here) to 0..51 */
        res = (slot < 26) ? ('a' + slot)
              : (slot < 52) ? ('A' + slot - 26)
                : '?'; /* won't happen */
        break;
    }
    return res;
}

/* called if inuse_only contracts from two sides to one; there won't be
   full lines shown until the next inventory update because the data past
   the middle boundary was clipped rather than saved; caller or caller's
   caller or somewhere up the call chain will call update_inventory() to
   redraw it all and rectify that */
static void
ttyinv_inuse_fulllines(
    struct WinDesc *cw,
    int rows_per_side UNUSED)
{
    bordercol[border_middle] = bordercol[border_right];
    tty_invent_box_glyph_init(cw);
}

/* called when inuse_only expands from full lines (one side) to two sides */
static void
ttyinv_inuse_twosides(
    struct WinDesc *cw,
    int rows_per_side)
{
    int row, col;

    bordercol[border_middle] = (cw->maxcol + 1) / 2;
    tty_invent_box_glyph_init(cw);
    /* draw the middle boundary */
    col = bordercol[border_middle];
    for (row = 0; row <= rows_per_side; ++row)
        tty_refresh_inventory(col, col, row);
}

/* split out of tty_end_menu(); persistent inventory is ready to display */
static void
ttyinv_end_menu(int window, struct WinDesc *cw)
{
    if (iflags.perm_invent
        || gp.perm_invent_toggling_direction == toggling_on) {
        if (program_state.in_moveloop) {
            boolean inuse_only = ((ttyinvmode & InvInUse) != 0);
            int rows_per_side = inuse_only ? cw->maxrow - 2 : 0;
            int old_slots_used = ttyinv_slots_used; /* value before render */

            ttyinv_render(window, cw);

            /* if inuse_only was using two sides and has just shrunk to one,
               it will switch to full rows instead of side-by-side panels
               but current data only holds the left-hand panel's portion;
               rerun the whole thing to regenerate previously clipped data;
               fortunately this should be a fairly rare occurrence */
            if (inuse_only && old_slots_used > rows_per_side
                /* 'ttyinv_slots_used' was just updated by ttyinv_render()
                   to the number of entries currently shown */
                && ttyinv_slots_used <= rows_per_side)
                tty_update_inventory(0); /* will call back to core for data */
        }
    }
}

/* display persistent inventory from data collected by add_menu() */
static void
ttyinv_render(winid window, struct WinDesc *cw)
{
    int row, col, slot, side, filled_count = 0, slot_limit;
    uint32 current_row_color = NO_COLOR;
    struct tty_perminvent_cell *cell;
    char invbuf[BUFSZ];
    boolean force_redraw = program_state.in_docrt ? TRUE : FALSE,
            inuse_only = (ttyinvmode & InvInUse) != 0,
            show_gold = (ttyinvmode & InvShowGold) != 0 && !inuse_only,
            sparse = (ttyinvmode & InvSparse) != 0 && !inuse_only;
    int rows_per_side = (inuse_only ? (cw->maxrow - 2)
                         : !show_gold ? 26
                           : 27);

    slot_limit = SIZE(slot_tracker);
    if (inuse_only) {
        slot_limit = rows_per_side; /* assume one side */
        if (ttyinv_slots_used == 0 || ttyinv_slots_used >= rows_per_side)
            slot_limit *= 2; /* second side is populated */
    } else if (!show_gold) {
        slot_limit -= 2; /* there are two extra slots for gold and overflow;
                          * blanking them for !show_gold would wrap back to
                          * rows 1 and 2 and need non-existent third side */
    }

    for (slot = 0; slot < slot_limit; ++slot)
        if (slot_tracker[slot])
            filled_count++;
    /* clear unused slots */
    for (slot = 0; slot < slot_limit; ++slot) {
        if (slot_tracker[slot])
           continue;
        if (slot == 0 && !filled_count) {
            Sprintf(invbuf, "%-4s[%s]", "",
                    inuse_only ? "no items are in use"
                    : (!show_gold && money_cnt(gi.invent)) ? "only gold"
                      : "empty");
        } else if (sparse && filled_count) {
            Sprintf(invbuf, "%c", slot_to_invlet(slot, show_gold));
        } else {
            invbuf[0] = '\0'; /* "" => fill slot with spaces */
        }
        row = (slot % rows_per_side) + 1; /* +1: top border */
        /* side: left side panel or right side panel, not a window column */
        side = slot / rows_per_side;
        ttyinv_populate_slot(cw, row, side, invbuf, NO_COLOR, 0);
    }

    /* inuse_only might switch from one panel to two or vice versa */
    if (inuse_only && filled_count != ttyinv_slots_used) {
        if (filled_count > rows_per_side
            && ttyinv_slots_used <= rows_per_side) {
            /* need second side; set up the middle border */
            /* done earlier, in add_menu():
            ttyinv_inuse_twosides(cw, rows_per_side);
            */
        } else if (filled_count <= rows_per_side
                   && ttyinv_slots_used > rows_per_side) {
            /* have second side but don't need/want it anymore */
            ttyinv_inuse_fulllines(cw, rows_per_side);
        }
        ttyinv_slots_used = filled_count;
    }

    /* has there been a glyph reset since we last got here? */
    if (gg.glyph_reset_timestamp > last_glyph_reset_when) {
        /* // tty_invent_box_glyph_init(wins[WIN_INVEN]); */
        last_glyph_reset_when = gg.glyph_reset_timestamp;
        force_redraw = TRUE;
    }
    /* render to the display */
    calling_from_update_inventory = TRUE;
    for (row = 0; row < cw->maxrow; ++row) {
        for (col = 0; col < cw->maxcol; ++col) {
            cell = &cw->cells[row][col];
            if (cell->refresh || force_redraw) {
                if (cell->color && (current_row_color != cell->color - 1)) {
                    current_row_color = cell->color - 1;
#if 0
                    if (current_row_color == NO_COLOR)
                        term_end_color();
                    else
#endif
                        term_start_color(current_row_color);
                }
                if (cell->glyph) {
                    tty_print_glyph(window, col + 1, row, cell->content.gi,
                                    &nul_glyphinfo);
                    end_glyphout();
                } else {
                    if (col != cw->curx || row != cw->cury)
                        tty_curs(window, col + 1, row);
                    (void) putchar(cell->content.ttychar);
                    ttyDisplay->curx++;
                    cw->curx++;
                }
                cell->refresh = 0;
            }
        }
        if (current_row_color != NO_COLOR)
            term_end_color();
    }
    tty_curs(window, 1, 0);
    for (slot = 0; slot < SIZE(slot_tracker); ++slot)
        slot_tracker[slot] = FALSE;
    calling_from_update_inventory = FALSE;
    return;
}

/* put the formatted object description for one item into a particular row
   and left/right panel, truncating if long or padding with spaces if short */
static void
ttyinv_populate_slot(
    struct WinDesc *cw,
    int row,  /* 'row' within the window, not within screen */
    int side, /* 'side'==0 is left panel or ==1 is right panel */
    const char *text,
    uint32 color,
    int clroffset)
{
    struct tty_perminvent_cell *cell;
    char c;
    int ccnt, col, endcol;
    boolean oops, inuse_only = (ttyinvmode & InvInUse) != 0;

    oops = (row < 0 || (long) row >= cw->maxrow || side < 0);
    if (inuse_only && side > 1 && !oops)
        return; /* there might be more in-use than fits; ignore excess */
    if (oops || side > 1)
        panic("ttyinv_populate_slot row=%d, side=%d", row, side);

    col = bordercol[side] + 1;
    endcol = bordercol[side + 1] - 1;
    cell = &cw->cells[row][col];
    for (ccnt = col; ccnt <= endcol; ++ccnt, ++cell) {
        /* [don't expect this to happen] if there was a glyph here, release
           memory allocated for it; gi pointer and ttychar character overlay
           each other in a union, so clear gi before assigning ttychar */
        if (cell->glyph) {
            free((genericptr_t) cell->content.gi);
            *cell = emptyttycell; /* clears cell->glyph and cell->content */
        }

        if ((c = *text) != '\0')
            ++text;
        else
            c = ' ';

        if (cell->content.ttychar != c) {
            cell->content.ttychar = c;
            cell->refresh = 1;
        }
        if (cell->color != color + 1) {
            /* offset color by 1 so 0 is not valid */
            if (ccnt >= (col + clroffset))
                cell->color = color + 1;
            else
                cell->color = NO_COLOR + 1;
            cell->refresh = 1;
        }
        cell->text = 1; /* cell->content.ttychar is current */
    }
}

void
tty_refresh_inventory(int start, int stop, int y)
{
    int row = y, col, col_limit = stop;
    struct WinDesc *cw = 0;
    winid window = WIN_INVEN;
    struct tty_perminvent_cell *cell;
    boolean printing_glyphs;

    if (window == WIN_ERR || !iflags.perm_invent
        || gp.perm_invent_toggling_direction == toggling_off)
        return;

    if ((cw = wins[window]) == (struct WinDesc *) 0)
        ttywindowpanic();

    if (col_limit > cw->maxcol)
        col_limit = cw->maxcol;

    /* we've been asked to redisplay a portion of the screen, one row */
    printing_glyphs = FALSE;
    for (col = start - 1; col < col_limit; ++col) {
        cell = &cw->cells[row][col];
        if (cell->color != ttyDisplay->color + 1) {
            term_start_color(cell->color - 1);
            ttyDisplay->color = cell->color - 1;
        }
        if (cell->glyph) {
            tty_print_glyph(window, col + 1, row, cell->content.gi,
                            &nul_glyphinfo);
            printing_glyphs = TRUE;
        } else {
            if (printing_glyphs)
                end_glyphout();
            if (col != cw->curx || row != cw->cury)
                tty_curs(window, col + 1, row);
            (void) putchar(cell->content.ttychar);
            ttyDisplay->curx++;
            cw->curx++;
        }
        cell->refresh = 0;
    }
    if (printing_glyphs)
        end_glyphout();
    if (ttyDisplay->color != NO_COLOR) {
        term_end_color();
        ttyDisplay->color = NO_COLOR;
    }
}

static void
tty_invent_box_glyph_init(struct WinDesc *cw)
{
    int row, col, glyph, bordercolor = NO_COLOR;
    uchar sym;
    struct tty_perminvent_cell *cell;
    boolean inuse_only = (ttyinvmode & InvInUse) != 0,
            show_gold = (ttyinvmode & InvShowGold) != 0 && !inuse_only;
    int rows_per_side = (inuse_only ? (cw->maxrow - 2)
                         : !show_gold ? 26
                           : 27),
        bottomrow = rows_per_side + 1;

    if (cw == 0 || !cw->active)
        return;

    for (row = 0; row < cw->maxrow; ++row)
        for (col = 0; col < cw->maxcol; ++col) {
            cell = &cw->cells[row][col];
            sym = S_crwall; /* placeholder */
            /* note: for top and bottom, check [border_right] before
               [border_middle] because they could be the same column (for
               InvInUse) and if so we want corner rather than tee there */
            if (row == 0) {
                sym = (col == bordercol[border_left]) ? S_tlcorn
                      : (col == bordercol[border_right]) ?  S_trcorn
                        : (col == bordercol[border_middle]) ? S_tdwall
                          : S_hwall;
            } else if (row < bottomrow) {
                if (col == bordercol[border_left]
                    || col == bordercol[border_middle]
                    || col == bordercol[border_right])
                    sym = S_vwall;
                /* else sym keeps placeholder value */
            } else if (row == bottomrow) {
                sym = (col == bordercol[border_left]) ? S_blcorn
                      : (col == bordercol[border_right]) ? S_brcorn
                        : (col == bordercol[border_middle]) ? S_tuwall
                          : S_hwall;
            }

            if (sym == S_crwall) { /* not a boundary (but might have been) */
                if (cell->glyph) {
                    /* presumably is no longer wanted middle vertical line */
                    free((genericptr_t) cell->content.gi);
                    *cell = emptyttycell; /* clears cell->glyph */
                    cell->content.ttychar = ' ';
                    cell->text = 1;
                    cell->refresh = 1;
                }
                continue;
            }
            /* a boundary; if it doesn't hold a glyph yet, allocate one */
            if (!cell->glyph) {
                cell->content.gi = (glyph_info *) alloc(sizeof (glyph_info));
                *(cell->content.gi) = zerogi;
                cell->glyph = 1, cell->text = 0;
            }

            /* to get here, cell->glyph is 1 and cell->content union has gi */
            glyph = cmap_D0walls_to_glyph(sym);
            map_glyphinfo(0, 0, glyph, 0, cell->content.gi);
            cell->glyph = 1; /* (redundant) */
            cell->text = 0;
            /* originally this conditionally set refresh depending upon
               whether the glyph was already shown, but that optimization
               is for something that rarely happens (boundary lines aren't
               redrawn very often, and most of the time when they are it's
               because they were erased or overwritten by something so
               won't match the prior value anymore) so skip it */
            cell->refresh = 1;
            cell->color = bordercolor + 1;
        }
    done_tty_perm_invent_init = TRUE;
}

/*
 * returns TRUE if things are ok
 */
static boolean
assesstty(
    enum inv_modes invmode,
    short *offx, short *offy, long *rows, long *cols,
    long *maxcol, long *minrow, long *maxrow)
{
    boolean inuse_only = (invmode & InvInUse) != 0,
            show_gold = (invmode & InvShowGold) != 0 && !inuse_only;
    int perminv_minrow = tty_perminv_minrow + (show_gold ? 1 : 0);

    if (!ttyDisplay) {
        /* too early */
        *offx = *offy = *rows = *cols = 0;
        *maxcol = 0;
        *minrow = *maxrow = 0;
        return !(*rows < perminv_minrow || *cols < tty_perminv_mincol);
    }

    *offx = 0;
    /* topline + map rows + status lines */
    *offy = 1 + ROWNO + StatusRows(); /* 1 + 21 + (2 or 3) */
    *rows = (ttyDisplay->rows - *offy);
    *cols = ttyDisplay->cols;
    *minrow = perminv_minrow;
#define SMALL_INUSE_WINDOW
#ifdef SMALL_INUSE_WINDOW
#undef SMALL_INUSE_WINDOW
    /* simplify testing by not requiring a small font to have enough room */
    if (inuse_only)
        *minrow = 1 + 8 + 1;
#else
    /* "normal" max for items in use would be 3 weapon + 7 armor + 4
       accessories == 14, but lit lamps/candles and attached leashes are
       also included; if hero ends up with more than 15 in-use items,
       some will be left out */
    if (inuse_only)
        *minrow = 1 + 15 + 1; /* top border + 15 lines + bottom border */
#endif
    *maxrow = min(*rows, perminv_minrow);
    *maxcol = *cols;
    return !(*rows < *minrow || *cols < tty_perminv_mincol);
}


#endif  /* TTY_PERM_INVENT */

/* update persistent inventory window */
void
tty_update_inventory(int arg UNUSED)
{
#ifdef TTY_PERM_INVENT
    sync_perminvent(); /* call back to core */
#else
    /* do nothing */
#endif
    return;
}

void
tty_mark_synch(void)
{
    HUPSKIP();
    (void) fflush(stdout);
}

void
tty_wait_synch(void)
{
    HUPSKIP();
    /* we just need to make sure all windows are synch'd */
    if (WIN_MAP == WIN_ERR || !ttyDisplay || ttyDisplay->rawprint) {
        getret();
        if (ttyDisplay)
            ttyDisplay->rawprint = 0;
    } else {
        tty_display_nhwindow(WIN_MAP, FALSE);
        if (ttyDisplay->inmore) {
            addtopl("--More--");
            (void) fflush(stdout);
        } else if (ttyDisplay->inread > program_state.gameover) {
            /* this can only happen if we were reading and got interrupted */
            ttyDisplay->toplin = TOPLINE_SPECIAL_PROMPT;
            /* do this twice; 1st time gets the Quit? message again */
            (void) tty_doprev_message();
            (void) tty_doprev_message();
            ttyDisplay->intr++;
            (void) fflush(stdout);
        }
    }
}

void
docorner(
    int xmin, int ymax,
    int ystart_between_menu_pages)
{
    int y;
    struct WinDesc *cw = wins[WIN_MAP];
    int ystart = 0;
#ifdef TTY_PERM_INVENT
    struct WinDesc *icw = 0;

    if (WIN_INVEN != WIN_ERR && iflags.perm_invent)
        icw = wins[WIN_INVEN];
#endif

    HUPSKIP();
#if 0   /* this optimization is not valuable enough to justify
         * abusing core internals... */
    if (u.uswallow) { /* Can be done more efficiently */
        swallowed(1);
        /* without this flush, if we happen to follow --More-- displayed in
           leftmost column, the cursor gets left in the wrong place after
           <docorner<more<update_topl<tty_putstr calls unwind back to core */
        flush_screen(0);
        return;
    }
#endif /*0*/

#ifdef RESIZABLE
    if (ymax > LI)
        ymax = LI; /* can happen if window gets smaller */
#endif
    if (ystart_between_menu_pages)
        ystart = ystart_between_menu_pages;

    for (y = ystart; y < ymax; y++) {
        tty_curs(BASE_WINDOW, xmin, y); /* move cursor */
        if (!ystart_between_menu_pages)
            cl_end();                   /* clear to end of line */
#ifdef TTY_PERM_INVENT
        /* the whole thing is beyond the board but not necessarily all the
           way to the bottom of the screen */
        if (icw && y >= (int) icw->offy && y < icw->offy + icw->maxrow)
            tty_refresh_inventory(xmin - (int) icw->offx, icw->maxcol,
                                  y - (int) icw->offy);
#endif
#ifdef CLIPPING
        if (y < (int) cw->offy || y + clipy > ROWNO)
            continue; /* only refresh board */
#if defined(TILES_IN_GLYPHMAP) && defined(MSDOS)
        if (iflags.tile_view)
            row_refresh((xmin / 2) + clipx - ((int) cw->offx / 2), COLNO - 1,
                        y + clipy - (int) cw->offy);
        else
#endif
            row_refresh(xmin + clipx - (int) cw->offx, COLNO - 1,
                        y + clipy - (int) cw->offy);
#else
        if (y < cw->offy || y > ROWNO)
            continue; /* only refresh board  */
        row_refresh(xmin - (int) cw->offx, COLNO - 1, y - (int) cw->offy);
#endif

    }

    end_glyphout();
    if (ymax >= (int) wins[WIN_STATUS]->offy
        && !ystart_between_menu_pages) {
        /* we have wrecked the bottom line */
        disp.botlx = TRUE;
        bot();
    }
}

void
end_glyphout(void)
{
    HUPSKIP();
#if defined(ASCIIGRAPH) && !defined(NO_TERMS)
    if (GFlag) {
        GFlag = FALSE;
        graph_off();
    }
#endif
    if (ttyDisplay->color != NO_COLOR) {
        term_end_color();
        ttyDisplay->color = NO_COLOR;
    }
}

void
g_putch(int in_ch)
{
    char ch = (char) in_ch;

#ifndef WIN32CON
    HUPSKIP();

#if defined(ASCIIGRAPH)
    if (SYMHANDLING(H_UTF8)) {
        (void) putchar(ch);
    } else if (SYMHANDLING(H_IBM)
        /* for DECgraphics, lower-case letters with high bit set mean
           switch character set and render with high bit clear;
           user might want 8-bits for other characters */
        || (iflags.eight_bit_tty && (!SYMHANDLING(H_DEC)
                                     || (in_ch & 0x7f) < 0x60))) {
        /* IBM-compatible displays don't need other stuff */
        (void) putchar(ch);
    } else if (ch & 0x80) {
        if (!GFlag || HE_resets_AS) {
            graph_on();
            GFlag = TRUE;
        }
        (void) putchar((ch ^ 0x80)); /* Strip 8th bit */
    } else {
        if (GFlag
#ifdef DECgraphicsOptimization
        /* for DECgraphics, we only need to switch back from the line
           drawing character set to the normal one if 'ch' is a lowercase
           letter or one of a handful of punctuation characters (the
           range is contiguous but somewhat odd); deferring graph_off()
           now might allow skipping both it and next potential graph_on() */
            && ch >= 0x5f && ch <= 0x7e
#endif
            ) {
            graph_off();
            GFlag = FALSE;
        }
        (void) putchar(ch);
    }

#else  /* ?ASCIIGRAPH */
    (void) putchar(ch);

#endif /* ASCIIGRAPH */
#else  /* WIN32CON */
    console_g_putch(in_ch);
    nhUse(ch);
#endif /* WIN32CON */
    return;
}

#if defined(UNIX) || defined(VMS)
#if defined(ENHANCED_SYMBOLS)
void
g_pututf8(uint8 *utf8str)
{
    HUPSKIP();
    while (*utf8str) {
        (void) putchar(*utf8str);
        utf8str++;
    }
    return;
}
#endif /* ENHANCED_SYMBOLS */
#endif /* UNIX || VMS */

#ifdef CLIPPING
void
setclipped(void)
{
    clipping = TRUE;
    clipx = clipy = 0;
    clipxmax = CO;
    clipymax = LI - 1 - iflags.wc2_statuslines;
}

void
tty_cliparound(int x, int y)
{
    int oldx = clipx, oldy = clipy;

    HUPSKIP();
    if (!clipping)
        return;
    if (x < clipx + 5) {
        clipx = max(0, x - 20);
        clipxmax = clipx + CO;
    } else if (x > clipxmax - 5) {
        clipxmax = min(COLNO, clipxmax + 20);
        clipx = clipxmax - CO;
    }
    if (y < clipy + 2) {
        clipy = max(0, y - (clipymax - clipy) / 2);
        clipymax = clipy + (LI - 1 - iflags.wc2_statuslines);
    } else if (y > clipymax - 2) {
        clipymax = min(ROWNO, clipymax + (clipymax - clipy) / 2);
        clipy = clipymax - (LI - 1 - iflags.wc2_statuslines);
    }
    if (clipx != oldx || clipy != oldy) {
        redraw_map(TRUE); /* ask the core to resend the map window's data */
    }
}
#endif /* CLIPPING */

/*
 *  tty_print_glyph
 *
 *  Print the glyph to the output device.  Don't flush the output device.
 *
 *  Since this is only called from show_glyph(), it is assumed that the
 *  position and glyph are always correct (checked there)!
 */

void
tty_print_glyph(
    winid window,
    coordxy x, coordxy y,
    const glyph_info *glyphinfo,
    const glyph_info *bkglyphinfo)
{
    boolean inverse_on = FALSE, colordone = FALSE, glyphdone = FALSE;
    boolean petattr = FALSE;
    int ch;
    uint32 color;
    unsigned special;

    HUPSKIP();
#ifdef CLIPPING
    if (clipping) {
        if (x <= clipx || y < clipy || x >= clipxmax || y >= clipymax)
            return;
    }
#endif
    /* get glyph ttychar, color, and special flags */
    ch = glyphinfo->ttychar;
    color = glyphinfo->gm.sym.color;
    special = glyphinfo->gm.glyphflags;

    print_vt_code2(AVTC_SELECT_WINDOW, window);

    /* Move the cursor. */
    tty_curs(window, x, y);

    print_vt_code3(AVTC_GLYPH_START, glyphinfo->gm.tileidx, special);

#ifndef NO_TERMS
    if (ul_hack && ch == '_') { /* non-destructive underscore */
        (void) putchar((char) ' ');
        backsp();
    }
#endif
    if (iflags.use_color) {
        ttyDisplay->colorflags = NH_BASIC_COLOR;
        if (color != ttyDisplay->color) {
            if (ttyDisplay->color != NO_COLOR)
                term_end_color();
        }
        /* we don't link with termcap.o if NO_TERMS is defined */
        if ((tty_procs.wincap2 & WC2_EXTRACOLORS)
            && glyphinfo->gm.customcolor != 0
            && iflags.colorcount >= 256
            && !calling_from_update_inventory) {
            if ((glyphinfo->gm.customcolor & NH_BASIC_COLOR) == 0) {
                term_start_extracolor(glyphinfo->gm.customcolor,
                                      glyphinfo->gm.color256idx);
                ttyDisplay->colorflags = 0;
                colordone = TRUE;
            } else {
                color = COLORVAL(glyphinfo->gm.customcolor);
            }
        }
        if (!colordone) {
            ttyDisplay->color = color;
            if (color != NO_COLOR)
                term_start_color(color);
        }
    }   /* iflags.use_color aka iflags.wc_color */

    /* must be after color check; term_end_color may turn off inverse too;
       BW_LAVA, BW_ICE, BW_SINK, BW_ENGR won't ever be set when color is on;
       (tried bold for ice but it didn't look very good; inverse is easier
       to see although the Valkyrie quest ends up being hard on the eyes) */
    if (iflags.use_color
        && bkglyphinfo && bkglyphinfo->framecolor != NO_COLOR) {
        ttyDisplay->framecolor = bkglyphinfo->framecolor;
        term_start_bgcolor(bkglyphinfo->framecolor);
    } else if ((special & MG_PET) != 0 && iflags.hilite_pet) {
        term_start_attr(iflags.wc2_petattr);
        petattr = TRUE;
    } else if ((((special & MG_OBJPILE) != 0 && iflags.hilite_pile)
                || ((special & MG_FEMALE) != 0 && wizard && iflags.wizmgender)
                || ((special & (MG_DETECT | MG_BW_LAVA | MG_BW_ICE
                                | MG_BW_SINK | MG_BW_ENGR)) != 0))
               && iflags.use_inverse) {
        term_start_attr(ATR_INVERSE);
        inverse_on = TRUE;
    }

#if defined(TILES_IN_GLYPHMAP) && defined(MSDOS)
    if (iflags.grmode && iflags.tile_view) {
        xputg(glyphinfo, bkglyphinfo);
        glyphdone = TRUE;
    }
#endif
#ifdef ENHANCED_SYMBOLS
    if (!glyphdone
        && (tty_procs.wincap2 & WC2_U_UTF8STR) && SYMHANDLING(H_UTF8)
            && glyphinfo->gm.u && glyphinfo->gm.u->utf8str) {
        /* we have a sequence to do */
        g_pututf8(glyphinfo->gm.u->utf8str);
        glyphdone = TRUE;
    }
#endif
    if (!glyphdone)
        g_putch(ch); /* print the character */

    if (inverse_on)
        term_end_attr(ATR_INVERSE);
    else if (petattr)
        term_end_attr(iflags.wc2_petattr);
    if (iflags.use_color) {
        /* turn off color as well, turning off ATR_INVERSE may have done
          this already and if so, we won't know the current state unless
          we do it explicitly */
        if (ttyDisplay->color != NO_COLOR
            || ttyDisplay->framecolor != NO_COLOR) {
            term_end_color();
            ttyDisplay->color = ttyDisplay->framecolor = NO_COLOR;
        }
        if (ttyDisplay->colorflags != NH_BASIC_COLOR)
            term_end_extracolor();
    }
    print_vt_code1(AVTC_GLYPH_END);

    wins[window]->curx++; /* one character over */
    ttyDisplay->curx++;   /* the real cursor moved too */
}

#ifdef NO_TERMS  /* termcap.o isn't linked in */
#if !defined(MSDOS) && !defined(WIN32)
void
term_start_bgcolor(int color)
{
    /* placeholder for now */
}
void
term_curs_set(int visibility UNUSED)
{
    /* nothing */
}
#endif

#ifdef CHANGE_COLOR
void
tty_change_color(int color UNUSED, long rgb UNUSED, int reverse UNUSED)
{
    /* nothing */
}
#endif /* CHANGE_COLOR */

#endif  /* NO_TERMS */

void
tty_raw_print(const char *str)
{
    HUPSKIP();
    if (ttyDisplay)
        ttyDisplay->rawprint++;
    else if (*str)
        iflags.raw_printed++;
    print_vt_code2(AVTC_SELECT_WINDOW, NHW_BASE);
#if defined(MICRO) || defined(WIN32CON)
    msmsg("%s\n", str);
#else
    puts(str);
    (void) fflush(stdout);
#endif
}

void
tty_raw_print_bold(const char *str)
{
    HUPSKIP();
    if (ttyDisplay)
        ttyDisplay->rawprint++;
    else if (*str)
        iflags.raw_printed++;
    print_vt_code2(AVTC_SELECT_WINDOW, NHW_BASE);
    term_start_raw_bold();
#if defined(MICRO) || defined(WIN32CON)
    msmsg("%s", str);
#else
    (void) fputs(str, stdout);
#endif
    term_end_raw_bold();
#if defined(MICRO) || defined(WIN32CON)
    msmsg("\n");
#else
    puts("");
    (void) fflush(stdout);
#endif
}

int
tty_nhgetch(void)
{
    int i;
#ifdef UNIX
    /* kludge alert: Some Unix variants return funny values if getc() is
     * called, interrupted, and then called again.  There is non-reentrant
     * code in the internal _filbuf() routine, called by getc().
     */
    char nestbuf;
#endif

    HUPSKIP_RESULT('\033');
    print_vt_code1(AVTC_INLINE_SYNC);
    term_curs_set(1);
    (void) fflush(stdout);
    /* Note: if raw_print() and wait_synch() get called to report terminal
     * initialization problems, then wins[] and ttyDisplay might not be
     * available yet.  Such problems will probably be fatal before we get
     * here, but validate those pointers just in case...
     */
    if (WIN_MESSAGE != WIN_ERR && wins[WIN_MESSAGE])
        wins[WIN_MESSAGE]->flags &= ~WIN_STOP;
    if (iflags.debug_fuzzer) {
        i = randomkey();
    } else {
#ifdef RESIZABLE
        if (program_state.resize_pending)
            resize_tty();
#endif
        program_state.getting_char++;
#ifdef UNIX
        i = (program_state.getting_char == 1)
              ? tgetch()
              : ((read(fileno(stdin), (genericptr_t) &nestbuf, 1) == 1)
                 ? (int) nestbuf : EOF);
#else
        i = tgetch();
#endif
        program_state.getting_char--;
#ifdef RESIZABLE
        if (resize_mesg) {
            resize_mesg = 0;
            tty_clear_nhwindow(WIN_MESSAGE);
            i = '\033';
        }
#endif
    }
    term_curs_set(0);
    if (!i)
        i = '\033'; /* map NUL to ESC since nethack doesn't expect NUL */
    else if (i == EOF) {
        iflags.term_gone = 1;
        i = '\033'; /* same for EOF */
    }
    /* topline has been seen - we can clear the need for --More-- */
    if (ttyDisplay && ttyDisplay->toplin == TOPLINE_NEED_MORE)
        ttyDisplay->toplin = TOPLINE_NON_EMPTY;
#ifdef TTY_TILES_ESCCODES
    {
        /* hack to force output of the window select code */
        int tmp = vt_tile_current_window;

        vt_tile_current_window++;
        print_vt_code2(AVTC_SELECT_WINDOW, tmp);
    }
#endif /* TTY_TILES_ESCCODES */
    return i;
}

/*
 * return a key, or 0, in which case a mouse button was pressed
 * mouse events should be returned as character postitions in the map window.
 * Since normal tty's don't have mice, just return a key.
 */
/*ARGSUSED*/
int
#if defined(WIN32CON)
tty_nh_poskey(coordxy *x, coordxy *y, int *mod)
#else
tty_nh_poskey(coordxy *x UNUSED, coordxy *y UNUSED, int *mod UNUSED)
#endif
{
    int i;

    HUPSKIP_RESULT('\033');
#if defined(WIN32CON)
    (void) fflush(stdout);
    /* Note: if raw_print() and wait_synch() get called to report terminal
     * initialization problems, then wins[] and ttyDisplay might not be
     * available yet.  Such problems will probably be fatal before we get
     * here, but validate those pointers just in case...
     */
    if (WIN_MESSAGE != WIN_ERR && wins[WIN_MESSAGE])
        wins[WIN_MESSAGE]->flags &= ~WIN_STOP;
    i = console_poskey(x, y, mod);
    if (!i && mod && (*mod == 0 || *mod == EOF))
        i = '\033'; /* map NUL or EOF to ESC, nethack doesn't expect either */
    /* topline has been seen - we can clear the need for --More-- */
    if (ttyDisplay && ttyDisplay->toplin == TOPLINE_NEED_MORE)
        ttyDisplay->toplin = TOPLINE_NON_EMPTY;
#else /* !WIN32CON */
    i = tty_nhgetch();
#endif /* ?WIN32CON */
    return i;
}

void
win_tty_init(int dir)
{
    if (dir != WININIT)
        return;
    return;
}

#ifdef POSITIONBAR
void
tty_update_positionbar(char *posbar)
{
#ifdef MSDOS
    video_update_positionbar(posbar);
#endif
}
#endif /* POSITIONBAR */

void
tty_putmixed(winid window, int attr, const char *str)
{
    struct WinDesc *cw;
    char buf[BUFSZ];
#ifdef ENHANCED_SYMBOLS
    int utf8flag = 0;
#endif

    if (window == WIN_ERR || (cw = wins[window]) == (struct WinDesc *) 0) {
        tty_raw_print(str);
        return;
    }
    ttyDisplay->mixed = 1;
#ifdef ENHANCED_SYMBOLS
    if ((windowprocs.wincap2 & WC2_U_UTF8STR) && SYMHANDLING(H_UTF8)) {
        mixed_to_utf8(buf, sizeof buf, str, &utf8flag);
        if (cw->type == NHW_MESSAGE)
            ttyDisplay->topl_utf8 = utf8flag;
    } else
#endif
        decode_mixed(buf, str);
    /* now send it to the normal tty_putstr */
    tty_putstr(window, attr, buf);
    ttyDisplay->topl_utf8 = 0;
    ttyDisplay->mixed = 0;
}

/*
 * +------------------+
 * |  STATUS CODE     |
 * +------------------+
 */

/*
 * ----------------------------------------------------------------
 * tty_status_update
 *
 *      Called from the NetHack core and receives info hand-offs
 *      from the core, processes them, storing some information
 *      for rendering to the tty.
 *          -> make_things_fit()
 *          -> render_status()
 *
 * make_things_fit
 *
 *      Called from tty_status_update(). It calls check_fields()
 *      (described next) to get the required number of columns
 *      and tries a few different ways to squish a representation
 *      of the status window values onto the 80 column tty display.
 *          ->check_fields()
 *          ->set_condition_length()  - update the width of conditions
 *          ->shrink_enc()      - shrink encumbrance message word
 *          ->shrink_dlvl()     - reduce the width of Dlvl:42
 *
 *  check_fields
 *
 *      Verifies that all the fields are ready for display, and
 *      returns FALSE if called too early in the startup
 *      processing sequence. It also figures out where everything
 *      needs to go, taking the current shrinking attempts into
 *      account. It returns number of columns needed back to
 *      make_things_fit(), so make_things_fit() can make attempt
 *      to make adjustments.
 *
 *  render_status
 *
 *      Goes through each of the status row's fields and
 *      calls tty_putstatusfield() to place them on the display.
 *          ->tty_putstatusfield()
 *      At the end of the for-loop, the NOW values get copied
 *      to BEFORE values.
 *
 *  tty_putstatusfield()
 *
 *      Move the cursor to the target spot, and output the field
 *      asked for by render_status().
 *
 * ----------------------------------------------------------------
 */

/*
 * The following data structures come from the genl_ routines in
 * src/windows.c and as such are considered to be on the window-port
 * "side" of things, rather than the NetHack-core "side" of things.
 */
extern const char *status_fieldfmt[MAXBLSTATS];
extern char *status_vals[MAXBLSTATS];
extern boolean status_activefields[MAXBLSTATS];
extern winid WIN_STATUS;

#ifdef STATUS_HILITES
static int condcolor(long, unsigned long *);
static int condattr(long, unsigned long *);
static unsigned long *tty_colormasks;
static long tty_condition_bits;
static struct tty_status_fields tty_status[2][MAXBLSTATS]; /* 2: NOW,BEFORE */
static int hpbar_percent, hpbar_crit_hp;
extern const struct conditions_t conditions[CONDITION_COUNT];

static const char *const encvals[3][6] = {
    { "", "Burdened", "Stressed", "Strained", "Overtaxed", "Overloaded" },
    { "", "Burden",   "Stress",   "Strain",   "Overtax",   "Overload"   },
    { "", "Brd",      "Strs",     "Strn",     "Ovtx",      "Ovld"       }
};
#define blPAD BL_FLUSH
#define MAX_PER_ROW 16
/* 2 or 3 status lines */
static const enum statusfields
    twolineorder[3][MAX_PER_ROW] = {
    { BL_TITLE, BL_STR, BL_DX, BL_CO, BL_IN, BL_WI, BL_CH, BL_ALIGN,
      BL_SCORE, BL_FLUSH, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD },
    { BL_LEVELDESC, BL_GOLD, BL_HP, BL_HPMAX, BL_ENE, BL_ENEMAX,
      BL_AC, BL_XP, BL_EXP, BL_HD, BL_TIME, BL_HUNGER,
      BL_CAP, BL_CONDITION, BL_VERS, BL_FLUSH },
    /* third row of array isn't used for twolineorder */
    { BL_FLUSH, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD,
      blPAD, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD }
},
    /* Align moved from 1 to 2, Leveldesc+Time+Cond+Vers moved from 2 to 3 */
    threelineorder[3][MAX_PER_ROW] = {
    { BL_TITLE, BL_STR, BL_DX, BL_CO, BL_IN, BL_WI, BL_CH,
      BL_SCORE, BL_FLUSH, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD },
    { BL_ALIGN, BL_GOLD, BL_HP, BL_HPMAX, BL_ENE, BL_ENEMAX,
      BL_AC, BL_XP, BL_EXP, BL_HD, BL_HUNGER,
      BL_CAP, BL_FLUSH, blPAD, blPAD, blPAD },
    { BL_LEVELDESC, BL_TIME, BL_CONDITION, BL_VERS, BL_FLUSH, blPAD,
      blPAD, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD, blPAD }
};
static const enum statusfields (*fieldorder)[MAX_PER_ROW];
#undef MAX_PER_ROW
#undef blPAD

static int finalx[3][2];    /* [rows][NOW or BEFORE] */
static boolean windowdata_init = FALSE;
static int cond_shrinklvl = 0;
static int enclev = 0, enc_shrinklvl = 0;
static int dlvl_shrinklvl = 0;
static boolean truncation_expected = FALSE;
#define FORCE_RESET TRUE
#define NO_RESET FALSE

/* This controls whether to skip fields that aren't
 * flagged as requiring updating during the current
 * render_status().
 *
 * Hopefully that can be confirmed as working correctly
 * for all platforms eventually and the conditional
 * setting below can be removed.
 */
static int do_field_opt =
#if defined(DISABLE_TTY_FIELD_OPT)
    0;
#else
    1;
#endif

#endif  /* STATUS_HILITES */

/*
 *  tty_status_init()
 *      -- initialize the tty-specific data structures.
 *      -- call genl_status_init() to initialize the general data.
 */
void
tty_status_init(void)
{
#ifdef STATUS_HILITES
    int i, num_rows;

    num_rows = StatusRows(); /* 2 or 3 */
    fieldorder = (num_rows != 3) ? twolineorder : threelineorder;

    for (i = 0; i < MAXBLSTATS; ++i) {
        tty_status[NOW][i].idx = BL_FLUSH;
        tty_status[NOW][i].color = NO_COLOR; /* no color */
        tty_status[NOW][i].attr = ATR_NONE;
        tty_status[NOW][i].x = tty_status[NOW][i].y = 0;
        tty_status[NOW][i].valid  = FALSE;
        tty_status[NOW][i].dirty  = FALSE;
        tty_status[NOW][i].redraw = FALSE;
        tty_status[NOW][i].sanitycheck = FALSE;
        tty_status[BEFORE][i] = tty_status[NOW][i];
    }
    tty_condition_bits = 0L;
    hpbar_percent = hpbar_crit_hp = 0;
#endif /* STATUS_HILITES */

    /* let genl_status_init do most of the initialization */
    genl_status_init();
}

void
tty_status_enablefield(
    int fieldidx,
    const char *nm,
    const char *fmt,
    boolean enable)
{
    genl_status_enablefield(fieldidx, nm, fmt, enable);
}

#ifdef STATUS_HILITES

/*
 *  *_status_update()
 *      -- update the value of a status field.
 *      -- the fldindex identifies which field is changing and
 *         is an integer index value from botl.h
 *      -- fldindex could be any one of the following from botl.h:
 *         BL_TITLE, BL_STR, BL_DX, BL_CO, BL_IN, BL_WI, BL_CH,
 *         BL_ALIGN, BL_SCORE, BL_CAP, BL_GOLD, BL_ENE, BL_ENEMAX,
 *         BL_XP, BL_AC, BL_HD, BL_TIME, BL_HUNGER, BL_HP, BL_HPMAX,
 *         BL_LEVELDESC, BL_EXP, BL_CONDITION, BL_VERS
 *      -- fldindex could also be BL_FLUSH (-1), which is not really
 *         a field index, but is a special trigger to tell the
 *         windowport that it should output all changes received
 *         to this point. It marks the end of a bot() cycle.
 *      -- fldindex could also be BL_RESET (-3), which is not really
 *         a field index, but is a special advisory to tell the
 *         windowport that it should redisplay all its status fields,
 *         even if no changes have been presented to it.
 *      -- ptr is usually a "char *", unless fldindex is BL_CONDITION.
 *         If fldindex is BL_CONDITION, then ptr is a long value with
 *         any or none of the following bits set (from botl.h):
 *               BL_MASK_BAREH        0x00000001L
 *               BL_MASK_BLIND        0x00000002L
 *               BL_MASK_BUSY         0x00000004L
 *               BL_MASK_CONF         0x00000008L
 *               BL_MASK_DEAF         0x00000010L
 *               BL_MASK_ELF_IRON     0x00000020L
 *               BL_MASK_FLY          0x00000040L
 *               BL_MASK_FOODPOIS     0x00000080L
 *               BL_MASK_GLOWHANDS    0x00000100L
 *               BL_MASK_GRAB         0x00000200L
 *               BL_MASK_HALLU        0x00000400L
 *               BL_MASK_HELD         0x00000800L
 *               BL_MASK_ICY          0x00001000L
 *               BL_MASK_INLAVA       0x00002000L
 *               BL_MASK_LEV          0x00004000L
 *               BL_MASK_PARLYZ       0x00008000L
 *               BL_MASK_RIDE         0x00010000L
 *               BL_MASK_SLEEPING     0x00020000L
 *               BL_MASK_SLIME        0x00040000L
 *               BL_MASK_SLIPPERY     0x00080000L
 *               BL_MASK_STONE        0x00100000L
 *               BL_MASK_STRNGL       0x00200000L
 *               BL_MASK_STUN         0x00400000L
 *               BL_MASK_SUBMERGED    0x00800000L
 *               BL_MASK_TERMILL      0x01000000L
 *               BL_MASK_TETHERED     0x02000000L
 *               BL_MASK_TRAPPED      0x04000000L
 *               BL_MASK_UNCONSC      0x08000000L
 *               BL_MASK_WOUNDEDL     0x10000000L
 *               BL_MASK_HOLDING      0x20000000L
 *
 *      -- The value passed for BL_GOLD usually includes an encoded leading
 *         symbol for GOLD "\GXXXXNNNN:nnn". If the window port needs to use
 *         the textual gold amount without the leading "$:" the port will
 *         have to skip past ':' in the passed "ptr" for the BL_GOLD case.
 *      -- color is an unsigned int.
 *               color_index = color & 0x00FF;         CLR_* value
 *               attribute   = (color >> 8) & 0x00FF;  HL_ATTCLR_* mask
 *         This holds the color and attribute that the field should
 *         be displayed in.
 *         This is relevant for everything except BL_CONDITION fldindex.
 *         If fldindex is BL_CONDITION, this parameter should be ignored,
 *         as condition highlighting is done via the next colormasks
 *         parameter instead.
 *
 *      -- colormasks - pointer to cond_hilites[] array of colormasks.
 *         Only relevant for BL_CONDITION fldindex. The window port
 *         should ignore this parameter for other fldindex values.
 *         Each condition bit must only ever appear in one of the
 *         CLR_ array members, but can appear in multiple HL_ATTCLR_
 *         offsets (because more than one attribute can co-exist).
 *         See doc/window.txt for more details.
 */

DISABLE_WARNING_FORMAT_NONLITERAL

void
tty_status_update(
    int fldidx,
    genericptr_t ptr,
    int chg UNUSED,
    int percent,
    int color,
    unsigned long *colormasks)
{
    int attrmask;
    long *condptr = (long *) ptr;
    char *text = (char *) ptr;
    char goldbuf[40], *lastchar, *p;
    const char *fmt;
    boolean reset_state = NO_RESET;

    if ((fldidx < BL_RESET) || (fldidx >= MAXBLSTATS))
        return;

    if ((fldidx >= 0 && fldidx < MAXBLSTATS) && !status_activefields[fldidx])
        return;

    switch (fldidx) {
    case BL_RESET:
        reset_state = FORCE_RESET;
        FALLTHROUGH;
        /*FALLTHRU*/
    case BL_FLUSH:
        if (make_things_fit(reset_state) || truncation_expected) {
            render_status();
#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
            status_sanity_check();
#endif
        }
        return;
    case BL_CONDITION:
        tty_status[NOW][fldidx].idx = fldidx;
        tty_condition_bits = *condptr;
        tty_colormasks = colormasks;
        tty_status[NOW][fldidx].valid = TRUE;
        tty_status[NOW][fldidx].dirty = TRUE;
        tty_status[NOW][fldidx].sanitycheck = TRUE;
        truncation_expected = FALSE;
        break;
    case BL_GOLD:
        text = decode_mixed(goldbuf, text);
        FALLTHROUGH;
        /*FALLTHRU*/
    default:
        attrmask = (color >> 8) & 0x00FF;
        fmt = status_fieldfmt[fldidx];
        if (!fmt)
            fmt = "%s";
        /* should be checking for first enabled field here rather than
           just first field, but 'fieldorder' doesn't start any rows
           with fields which can be disabled so [any_row][0] suffices */
        if (*fmt == ' ' && (fldidx == fieldorder[0][0]
                            || fldidx == fieldorder[1][0]
                            || fldidx == fieldorder[2][0]))
            ++fmt; /* skip leading space for first field on line */
        Sprintf(status_vals[fldidx], fmt, text);
        tty_status[NOW][fldidx].idx = fldidx;
        tty_status[NOW][fldidx].color = (color & 0x00FF);
        tty_status[NOW][fldidx].attr = term_attr_fixup(attrmask);
        tty_status[NOW][fldidx].lth = strlen(status_vals[fldidx]);
        tty_status[NOW][fldidx].valid = TRUE;
        tty_status[NOW][fldidx].dirty = TRUE;
        tty_status[NOW][fldidx].sanitycheck = TRUE;
        break;
    }

    /* The core botl engine sends a single blank to the window port
       for carrying-capacity when it's unused. Let's suppress that */
    if (fldidx >= 0 && fldidx < MAXBLSTATS
        && tty_status[NOW][fldidx].lth == 1
        && status_vals[fldidx][0] == ' ') {
        status_vals[fldidx][0] = '\0';
        tty_status[NOW][fldidx].lth = 0;
    }

    /* default processing above was required before these */
    switch (fldidx) {
    case BL_HP:
        if (iflags.wc2_hitpointbar) {
            /* Special additional processing for hitpointbar */
            hpbar_percent = percent;
            hpbar_crit_hp = critically_low_hp(TRUE) ? 1 : 0;
            tty_status[NOW][BL_TITLE].color = (color & 0x00FF);
            attrmask = HL_INVERSE | (hpbar_crit_hp ? HL_BLINK : 0);
            tty_status[NOW][BL_TITLE].attr = term_attr_fixup(attrmask);
            tty_status[NOW][BL_TITLE].dirty = TRUE;
        }
        break;
    case BL_LEVELDESC:
        dlvl_shrinklvl = 0; /* caller is passing full length string */
        FALLTHROUGH;
        /*FALLTHRU*/
    case BL_HUNGER:
        /* The core sends trailing blanks for some fields.
           Let's suppress the trailing blanks */
        if (tty_status[NOW][fldidx].lth > 0) {
            p = status_vals[fldidx];
            for (lastchar = eos(p); lastchar > p && *--lastchar == ' '; ) {
                *lastchar = '\0';
                tty_status[NOW][fldidx].lth--;
            }
        }
        break;
    case BL_TITLE:
        /* when hitpointbar is enabled, rendering will enforce a length
           of 30 on title, padding with spaces or truncating if necessary */
        if (iflags.wc2_hitpointbar)
            tty_status[NOW][fldidx].lth = 30 + 2; /* '[' and ']' */
        break;
    case BL_GOLD:
        /* \GXXXXNNNN counts as 1 [moot since we use decode_mixed() above] */
        if ((p = strchr(status_vals[fldidx], '\\')) != 0 && p[1] == 'G')
            tty_status[NOW][fldidx].lth -= (10 - 1);
        break;
    case BL_CAP:
        enc_shrinklvl = 0; /* caller is passing full length string */
        enclev = stat_cap_indx();
        break;
    }
    /* As of 3.6.2 we only render on BL_FLUSH (or BL_RESET) */
    return;
}

RESTORE_WARNING_FORMAT_NONLITERAL

static int
make_things_fit(boolean force_update)
{
    int trycnt, fitting = 0, requirement;
    int rowsz[MAX_STATUS_ROWS], num_rows, condrow, otheroptions = 0;

    num_rows = StatusRows();
    condrow = num_rows - 1; /* always last row, 1 for 0..1 or 2 for 0..2 */
    cond_shrinklvl = 0;
    if (enc_shrinklvl > 0 && num_rows == 2)
        shrink_enc(0);
    if (dlvl_shrinklvl > 0)
        shrink_dlvl(0);
    set_condition_length();
    for (trycnt = 0; trycnt < 6 && !fitting; ++trycnt) {
        /* FIXME: this remeasures each line every time even though it
           is only attempting to shrink one of them and the other one
           (or two) remains the same */
        if (!check_fields(force_update, rowsz)) {
            fitting = 0;
            break;
        }

        requirement = rowsz[condrow] - 1;
        if (requirement <= wins[WIN_STATUS]->cols - 1) {
            fitting = requirement;
            break;  /* we're good */
        }
        if (trycnt < 2) {
            if (cond_shrinklvl < trycnt + 1) {
                cond_shrinklvl = trycnt + 1;
                set_condition_length();
            }
            continue;
        }
        if (cond_shrinklvl >= 2) {
            /* We've exhausted the condition identifiers shrinkage,
             * so let's try shrinking other things...
             */
            if (otheroptions < 2) {
                /* try shrinking the encumbrance word, but
                   only when it's on the same line as conditions */
                if (num_rows == 2)
                    shrink_enc(otheroptions + 1);
            } else if (otheroptions == 2) {
                shrink_dlvl(1);
            } else {
                /* Last resort - turn on trunction */
                truncation_expected = TRUE;
                break;
            }
            ++otheroptions;
        }
    }
    return fitting;
}

/*
 * This is the routine where we figure out where each field
 * should be placed, and flag whether the on-screen details
 * must be updated because they need to change.
 * This is now done at an individual field case-by-case level.
 */
static boolean
check_fields(boolean forcefields, int sz[MAX_STATUS_ROWS])
{
    int c, i, row, col, num_rows, idx;
    boolean valid = TRUE, matchprev, update_right;

    if (!windowdata_init && !check_windowdata())
        return FALSE;

    num_rows = StatusRows(); /* 2 or 3 */
    for (row = 0; row < num_rows; ++row) {
        sz[row] = 0;
        col = 1;
        update_right = FALSE;
        for (i = 0; (idx = fieldorder[row][i]) != BL_FLUSH; ++i) {
            if (!status_activefields[idx])
                continue;
            if (!tty_status[NOW][idx].valid)
                valid = FALSE;
            /* might be called more than once for shrink tests, so need
               to reset these (redraw and x at any rate) each time */
            tty_status[NOW][idx].redraw = FALSE;
            tty_status[NOW][idx].y = row;
            tty_status[NOW][idx].x = col;

            /* On a change to the field location, everything further
               to the right must be updated as well.  (Not necessarily
               everything; it's possible for complementary changes across
               multiple fields to put stuff further right back in sync.) */
            if (tty_status[NOW][idx].x + tty_status[NOW][idx].lth
                != tty_status[BEFORE][idx].x + tty_status[BEFORE][idx].lth)
                update_right = TRUE;
            else if (tty_status[NOW][idx].lth != tty_status[BEFORE][idx].lth
                     || tty_status[NOW][idx].x != tty_status[BEFORE][idx].x)
                tty_status[NOW][idx].redraw = TRUE;
            else /* in case update_right is set, we're back in sync now */
                update_right = FALSE;

            matchprev = FALSE; /* assume failure */
            if (valid && !update_right && !forcefields
                && !tty_status[NOW][idx].redraw) {
                /*
                 * Check values against those already on the display.
                 *  - Is the additional processing time for this worth it?
                 */
                if (do_field_opt
                    /* color/attr checks aren't right for 'condition'
                       and neither is examining status_vals[BL_CONDITION]
                       so skip same-contents optimization for conditions */
                    && idx != BL_CONDITION
                    && (tty_status[NOW][idx].color
                        == tty_status[BEFORE][idx].color)
                    && (tty_status[NOW][idx].attr
                        == tty_status[BEFORE][idx].attr)) {
                    matchprev = TRUE; /* assume success */
                    if (tty_status[NOW][idx].dirty) {
                        /* compare values */
                        const char *ob, *nb; /* old byte, new byte */
                        struct WinDesc *cw = wins[WIN_STATUS];

                        c = col - 1;
                        ob = &cw->data[row][c];
                        nb = status_vals[idx];
                        while (*nb && c < cw->cols) {
                            if (*nb != *ob)
                                break;
                            nb++;
                            ob++;
                            c++;
                        }
                        /* if we're not at the end of new string, no match;
                           we don't need to worry about whether there might
                           be leftover old string; that could only happen
                           if they have different lengths, in which case
                           'update_right' will be set and we won't get here */
                        if (*nb)
                            matchprev = FALSE;
#if 0
                        if (*nb || (*ob && *ob != ' '
                                    && (*ob != '/' || idx != BL_XP)
                                    && (*ob != '(' || (idx != BL_HP
                                                       && idx != BL_ENE))))
                            matchprev = FALSE;
#endif
                    }
                }
            }

            if (forcefields || update_right
                || (tty_status[NOW][idx].dirty && !matchprev))
                tty_status[NOW][idx].redraw = TRUE;

            col += tty_status[NOW][idx].lth;
        }
        sz[row] = col;
    }
    return valid;
}

#if (NH_DEVEL_STATUS != NH_STATUS_RELEASED)
static void
status_sanity_check(void)
{
    static const char *const idxtext[] = {
        "BL_TITLE", "BL_STR", "BL_DX", "BL_CO", "BL_IN", "BL_WI", /*  0.. 5 */
        "BL_CH","BL_ALIGN", "BL_SCORE", "BL_CAP", "BL_GOLD",      /*  6..10 */
        "BL_ENE", "BL_ENEMAX", "BL_XP", "BL_AC", "BL_HD",         /* 11..15 */
        "BL_TIME", "BL_HUNGER", "BL_HP", "BL_HPMAX",              /* 16..19 */
        "BL_LEVELDESC", "BL_EXP", "BL_CONDITION",                 /* 20..22 */
        "BL_VERS",                                                /*   23   */
    };
    static boolean in_sanity_check = FALSE;
    int i;

    if (in_sanity_check)
        return;
    in_sanity_check = TRUE;
    /*
     * Make sure that every field made it down to the
     * bottom of the render_status() for-loop.
     */
    for (i = 0; i < MAXBLSTATS; ++i) {
        if (tty_status[NOW][i].sanitycheck) {
            char panicmsg[BUFSZ], indxtxt[40];

            /* guard against an increase in MAXBLSTATS in botl.h without
               a corresponding expansion of idxtext[] here */
            if (i < SIZE(idxtext))
                Strcpy(indxtxt, idxtext[i]);
            else
                Sprintf(indxtxt, "%d", i);
            Sprintf(panicmsg, "failed on tty_status[NOW][%s].", indxtxt);
            paniclog("status_sanity_check", panicmsg);
            tty_status[NOW][i].sanitycheck = FALSE;
            /*
             * Attention developers: If you encounter the above
             * message in paniclog, it almost certainly means that
             * a recent code change has caused a failure to reach
             * the bottom of render_status(), at least for the BL_
             * field identified in the impossible() message.
             *
             * That could be because of the addition of a continue
             * statement within the render_status() for-loop, or a
             * premature return from render_status() before it finished
             * its housekeeping chores.
             */
        }
    }
    in_sanity_check = FALSE;
}
#endif /* NHDEVEL_STATUS */

/*
 * This is what places a field on the tty display.
 */
static void
tty_putstatusfield(const char *text, int x, int y)
{
    int i, n, ncols, nrows, lth = 0;
    struct WinDesc *cw = 0;

    if (WIN_STATUS == WIN_ERR
        || (cw = wins[WIN_STATUS]) == (struct WinDesc *) 0)
        panic("tty_putstatusfield: Invalid WinDesc\n");

    ncols = cw->cols;
    nrows = cw->maxrow;
    lth = (int) strlen(text);

    print_vt_code2(AVTC_SELECT_WINDOW, NHW_STATUS);

    if (x < ncols && y < nrows) {
        if (x != cw->curx || y != cw->cury)
            tty_curs(NHW_STATUS, x, y);
        for (i = 0; i < lth; ++i) {
            n = i + x;
            if (n < ncols && *text) {
                (void) putchar(*text);
                ttyDisplay->curx++;
                cw->curx++;
                cw->data[y][n - 1] = *text;
                text++;
            }
        }
    }
#if 0
    else {
        if (truncation_expected) {
        /* Now we're truncating */
            ; /* but we knew in advance */
        }
    }
#endif
}

/* caller must set cond_shrinklvl (0..2) before calling us */
static void
set_condition_length(void)
{
    long mask;
    int c, lth = 0;

    if (tty_condition_bits) {
        for (c = 0; c < SIZE(conditions); ++c) {
            mask = conditions[c].mask;
            if ((tty_condition_bits & mask) == mask)
                lth += 1 + (int) strlen(conditions[c].text[cond_shrinklvl]);
        }
    }
    tty_status[NOW][BL_CONDITION].lth = lth;
}

static void
shrink_enc(int lvl)
{
    /* shrink or restore the encumbrance word */
    if (lvl <= 2) {
        enc_shrinklvl = lvl;
        Sprintf(status_vals[BL_CAP], " %s", encvals[lvl][enclev]);
    }
    tty_status[NOW][BL_CAP].lth = strlen(status_vals[BL_CAP]);
}

static void
shrink_dlvl(int lvl)
{
    /* try changing Dlvl: to Dl: */
    char buf[BUFSZ];
    char *levval = strchr(status_vals[BL_LEVELDESC], ':');

    if (levval) {
        dlvl_shrinklvl = lvl;
        Strcpy(buf, (lvl == 0) ? "Dlvl" : "Dl");
        Strcat(buf, levval);
        Strcpy(status_vals[BL_LEVELDESC], buf);
        tty_status[NOW][BL_LEVELDESC].lth = strlen(status_vals[BL_LEVELDESC]);
    }
}

/*
 * Ensure the underlying status window data start out
 * blank and null-terminated.
 */
static boolean
check_windowdata(void)
{
    if (WIN_STATUS == WIN_ERR || wins[WIN_STATUS] == (struct WinDesc *) 0) {
        paniclog("check_windowdata", " null status window.");
        return FALSE;
    } else if (!windowdata_init) {
        tty_clear_nhwindow(WIN_STATUS); /* also sets cw->data[] to spaces */
        windowdata_init = TRUE;
    }
    return TRUE;
}

/*
 * Return what color this condition should
 * be displayed in based on user settings.
 */
static int
condcolor(long bm, unsigned long *bmarray)
{
    int i;

    if (bm && bmarray)
        for (i = 0; i < CLR_MAX; ++i) {
            if ((bm & bmarray[i]) != 0)
                return i;
        }
    return NO_COLOR;
}

static int
condattr(long bm, unsigned long *bmarray)
{
    int attr = 0;
    int i;

    if (bm && bmarray) {
        for (i = HL_ATTCLR_BOLD; i < BL_ATTCLR_MAX; ++i) {
            if ((bm & bmarray[i]) != 0) {
                switch (i) {
                case HL_ATTCLR_BOLD:
                    attr |= HL_BOLD;
                    break;
                case HL_ATTCLR_DIM:
                    attr |= HL_DIM;
                    break;
                case HL_ATTCLR_ITALIC:
                    attr |= HL_ITALIC;
                    break;
                case HL_ATTCLR_ULINE:
                    attr |= HL_ULINE;
                    break;
                case HL_ATTCLR_BLINK:
                    attr |= HL_BLINK;
                    break;
                case HL_ATTCLR_INVERSE:
                    attr |= HL_INVERSE;
                    break;
                }
            }
        }
    }
    return attr;
}

#define Begin_Attr(m) \
    do {                                        \
        if (m) {                                \
            if ((m) & HL_BOLD)                  \
                term_start_attr(ATR_BOLD);      \
            if ((m) & HL_DIM)                   \
                term_start_attr(ATR_DIM);       \
            if ((m) & HL_ITALIC)                \
                term_start_attr(ATR_ITALIC);    \
            if ((m) & HL_ULINE)                 \
                term_start_attr(ATR_ULINE);     \
            if ((m) & HL_BLINK)                 \
                term_start_attr(ATR_BLINK);     \
            if ((m) & HL_INVERSE)               \
                term_start_attr(ATR_INVERSE);   \
        }                                       \
    } while (0)

#define End_Attr(m) \
    do {                                        \
        if (m) {                                \
            if ((m) & HL_INVERSE)               \
                term_end_attr(ATR_INVERSE);     \
            if ((m) & HL_BLINK)                 \
                term_end_attr(ATR_BLINK);       \
            if ((m) & HL_ULINE)                 \
                term_end_attr(ATR_ULINE);       \
            if ((m) & HL_ITALIC)                \
                term_end_attr(ATR_ITALIC);      \
            if ((m) & HL_DIM)                   \
                term_end_attr(ATR_DIM);         \
            if ((m) & HL_BOLD)                  \
                term_end_attr(ATR_BOLD);        \
        }                                       \
    } while (0)

static void
render_status(void)
{
    long mask, bits;
    int i, x, y, idx, c, ci, row, tlth, num_rows,
        coloridx = 0, attrmask = 0;
    char *text;
    struct WinDesc *cw = 0;

    if (WIN_STATUS == WIN_ERR
        || (cw = wins[WIN_STATUS]) == (struct WinDesc *) 0) {
        paniclog("render_status", "WIN_ERR on status window.");
        return;
    }

    num_rows = StatusRows(); /* 2 or 3 */
    for (row = 0; row < num_rows; ++row) {
        HUPSKIP();
        y = row;
        tty_curs(WIN_STATUS, 1, y);
        for (i = 0; (idx = fieldorder[row][i]) != BL_FLUSH; ++i) {
            if (!status_activefields[idx])
                continue;
            x = tty_status[NOW][idx].x;
            text = status_vals[idx]; /* always "" for BL_CONDITION */
            tlth = (int) tty_status[NOW][idx].lth; /* valid for BL_CONDITION */

            if (tty_status[NOW][idx].redraw || !do_field_opt) {
                boolean hitpointbar = (idx == BL_TITLE
                                       && iflags.wc2_hitpointbar);

                if (idx == BL_CONDITION) {
                    /*
                     * +-----------------+
                     * | Condition Codes |
                     * +-----------------+
                     */
                    bits = tty_condition_bits;
                    /*
                     * If no bits are set, we can fall through condition
                     * rendering code to finalx[] handling (and subsequent
                     * rest-of-line erasure if line is shorter than before).
                     *
                     * First, when conditions are on 3rd row, they might
                     * be indented to line up with a position on 2nd row.
                     */
                    if (row == MAX_STATUS_ROWS - 1 && bits != 0L) {
                        int cstart, last_col = cw->cols;
                        char *dat = &cw->data[y][0];

                        /* 'version' might follow conditions; if so, adjust
                           expectations for where conditions should end;
                           only matters when conditions are being indented */
                        if (status_activefields[BL_VERS]
                            && fieldorder[row][i + 1] == BL_VERS)
                            last_col -= (int) tty_status[NOW][BL_VERS].lth;
                        /* line up with hunger (or where it would have
                           been when currently omitted); if there isn't
                           enough room for that, right justify; or place
                           as-is if not even enough room for /that/; we
                           expect hunger to be on preceding row, in which
                           case its current data has been moved to [BEFORE] */
                        if (tty_status[BEFORE][BL_HUNGER].y < row
                            && x < tty_status[BEFORE][BL_HUNGER].x
                            && (tty_status[BEFORE][BL_HUNGER].x + tlth
                                < last_col - 1))
                            cstart = tty_status[BEFORE][BL_HUNGER].x;
                        else if (x + tlth < cw->cols - 1)
                            cstart = last_col - tlth;
                        else
                            cstart = x;
                        /* indent conditions to line them up with 2nd row */
                        if (x < cstart) {
                            do {
                                if (dat[x - 1] != ' ')
                                    tty_putstatusfield(" ", x, y);
                            } while (++x < cstart);
                            tty_status[NOW][BL_CONDITION].x = x;
                            tty_curs(WIN_STATUS, x, y);
                        }
                    }
                    /* actually draw condition words */
                    for (c = 0; c < SIZE(conditions) && bits != 0L; ++c) {
                        ci = cond_idx[c];
                        mask = conditions[ci].mask;
                        if (bits & mask) {
                            const char *condtext;

                            tty_putstatusfield(" ", x++, y);
                            if (iflags.hilite_delta) {
                                attrmask = condattr(mask, tty_colormasks);
                                Begin_Attr(attrmask);
                                coloridx = condcolor(mask, tty_colormasks);
                                if (coloridx != NO_COLOR)
                                    term_start_color(coloridx);
                            }
                            condtext = conditions[ci].text[cond_shrinklvl];
                            if (x >= cw->cols && !truncation_expected) {
                                impossible(
                         "Unexpected condition placement overflow for \"%s\"",
                                           condtext);
                                condtext = "";
                                bits = 0L; /* skip any remaining conditions */
                            }
                            tty_putstatusfield(condtext, x, y);
                            x += (int) strlen(condtext);
                            if (iflags.hilite_delta) {
                                if (coloridx != NO_COLOR)
                                    term_end_color();
                                End_Attr(attrmask);
                            }
                            bits &= ~mask;
                        }
                    }
                    /* 'x' is 1-based and 'cols' and 'data' are 0-based,
                       so x==cols means we just stored in data[N-2] and
                       are now positioned at data[N-1], the terminator;
                       that's ok as long as we don't write there */
                    if (x > cw->cols) {
                        static unsigned once_only = 0;

                        if (!truncation_expected && !once_only++)
                            paniclog("render_status()",
                                     " unexpected truncation.");
                        x = cw->cols;
                    }
                } else if (hitpointbar) {
                    /*
                     * +-------------------------+
                     * | Title with Hitpoint Bar |
                     * +-------------------------+
                     */
                    /* hitpointbar using hp percent calculation */
                    int bar_len, bar_pos = 0;
                    char bar[30 + 1], *bar2 = (char *) 0, savedch = '\0';
                    boolean twoparts = (hpbar_percent < 100);

                    /* force exactly 30 characters, padded with spaces
                       if shorter or truncated if longer */
                    if (strlen(text) != 30) {
                        Sprintf(bar, "%-30.30s", text);
                        Strcpy(status_vals[BL_TITLE], bar);
                    } else {
                        Strcpy(bar, text);
                    }
                    if (hpbar_crit_hp)
                        repad_with_dashes(bar);
                    bar_len = (int) strlen(bar); /* always 30 */
                    /*tlth = bar_len + 2; // not needed within this 'if'*/
                    attrmask = 0; /* for the second part only case: dead */
                    /* when at full HP, the whole title will be highlighted;
                       when injured or dead, there will be a second portion
                       which is not highlighted */
                    if (twoparts) {
                        /* figure out where to separate the two parts */
                        bar_pos = (bar_len * hpbar_percent) / 100;
                        if (bar_pos < 1 && hpbar_percent > 0)
                            bar_pos = 1;
                        if (bar_pos >= bar_len && hpbar_percent < 100)
                            bar_pos = bar_len - 1;
                        bar2 = &bar[bar_pos];
                        savedch = *bar2;
                        *bar2 = '\0';
                    }
                    tty_putstatusfield("[", x++, y);
                    if (*bar) { /* always True, unless twoparts+dead (0 HP) */
                        coloridx = tty_status[NOW][BL_TITLE].color;
                        attrmask = tty_status[NOW][BL_TITLE].attr;
                        Begin_Attr(attrmask);
                        if (iflags.hilite_delta && coloridx != NO_COLOR)
                            term_start_color(coloridx);
                        tty_putstatusfield(bar, x, y);
                        x += (int) strlen(bar);
                        if (iflags.hilite_delta && coloridx != NO_COLOR)
                            term_end_color();
                        End_Attr(attrmask);
                    }
                    if (twoparts) {
                        if ((attrmask & HL_BLINK) != 0)
                            term_start_attr(ATR_BLINK);
                        *bar2 = savedch;
                        tty_putstatusfield(bar2, x, y);
                        x += (int) strlen(bar2);
                        if ((attrmask & HL_BLINK) != 0)
                            term_end_attr(ATR_BLINK);
                    }
                    tty_putstatusfield("]", x++, y);
                } else {
                    /*
                     * +-----------------------------+
                     * | Everything else that is not |
                     * |   in a special case above   |
                     * +-----------------------------+
                     */
                    if (idx == BL_VERS
                        /* if 'version' is the last field in its row, right
                           justify it (otherwise just treat it as ordinary) */
                        && fieldorder[row][i + 1] == BL_FLUSH) {
                        int vstart;
                        char *dat = &cw->data[y][0];
                        /* FIXME:  there's something fishy going on here;
                           'x' ends up out of synch when conditions have
                           3rd row indentation and the indenting of version
                           overwrites them with spaces; this hides that */
                        int vx = tty_status[BEFORE][BL_CONDITION].x
                                 + tty_status[BEFORE][BL_CONDITION].lth;

                        if (i > 0 && fieldorder[row][i - 1] == BL_CONDITION
                            && x != vx) {
                            x = vx;
                            tty_curs(WIN_STATUS, x, y);
                        }
                        /* indent version to right justify it */
                        vstart = cw->cols - (int) tty_status[NOW][idx].lth;
                        if (x < vstart) {
                            do {
                                if (dat[x - 1] != ' ')
                                    tty_putstatusfield(" ", x, y);
                            } while (++x < vstart);
                            tty_status[NOW][BL_VERS].x = x;
                        }
                    }
                    if (iflags.hilite_delta) {
                        while (*text == ' ') {
                            tty_putstatusfield(" ", x++, y);
                            text++;
                        }
                        if (*text == '/' && idx == BL_EXP) {
                            tty_putstatusfield("/", x++, y);
                            text++;
                        }
                        attrmask = tty_status[NOW][idx].attr;
                        Begin_Attr(attrmask);
                        coloridx = tty_status[NOW][idx].color;
                        if (coloridx != NO_COLOR)
                            term_start_color(coloridx);
                    }
                    tty_putstatusfield(text, x, y);
                    x += (int) strlen(text);
                    if (iflags.hilite_delta) {
                        if (coloridx != NO_COLOR)
                            term_end_color();
                        End_Attr(attrmask);
                    }
                }
            } else {
                /* not rendered => same text as before */
                x += tlth;
            }
            finalx[row][NOW] = x - 1;
            /* reset .redraw and .dirty now that field has been rendered */
            tty_status[NOW][idx].dirty  = FALSE;
            tty_status[NOW][idx].redraw = FALSE;
            tty_status[NOW][idx].sanitycheck = FALSE;
            /*
             * For comparison of current and previous:
             * - Copy the entire tty_status struct.
             */
            tty_status[BEFORE][idx] = tty_status[NOW][idx];
        } /* for i=..., idx=fieldorder[][i] */
        x = finalx[row][NOW];
        if ((x < finalx[row][BEFORE] || !finalx[row][BEFORE])
            && x + 1 < cw->cols) {
            tty_curs(WIN_STATUS, x + 1, y);
            cl_end();
        }
        /*
         * For comparison of current and previous:
         * - Copy the last written column number on the row.
         */
        finalx[row][BEFORE] = finalx[row][NOW];
    } /* for row=... */
    return;
}

#undef Begin_Attr
#undef End_Attr
#ifdef condcolor
#undef condcolor
#endif
#ifdef term_start_color
#undef term_start_color
#undef term_end_color
#endif
#undef FORCE_RESET
#undef NO_RESET
#undef MAX_STATUS_ROWS
#undef StatusRows

#endif /* STATUS_HILITES */

#if defined(USER_SOUNDS) && defined(TTY_SOUND_ESCCODES)
void
play_usersound_via_idx(int idx, int volume)
{
    print_vt_soundcode_idx(idx, volume);
}
#endif /* USER_SOUNDS && TTY_SOUND_ESCCODES */

#ifdef VT_ANSI_COMMAND
#undef VT_ANSI_COMMAND
#endif
#ifdef AVTC_GLYPH_START /* TTY_TILES_ESCCODES */
#undef AVTC_GLYPH_START
#undef AVTC_GLYPH_END
#undef AVTC_SELECT_WINDOW
#undef AVTC_INLINE_SYNC
#endif
#ifdef AVTC_SOUND_PLAY /* TTY_SOUND_ESCCODES */
#undef AVTC_SOUND_PLAY
#endif

#ifdef print_vt_code
#undef print_vt_code
#endif
#undef print_vt_code1
#undef print_vt_code2
#undef print_vt_code3
#ifdef print_vt_soundcode_idx
#undef print_vt_soundcode_idx
#endif

#undef RESIZABLE
#undef HUPSKIP
#undef HUPSKIP_RESULT
#undef ttypanic

#endif /* TTY_GRAPHICS */

#undef H2344_BROKEN

/*wintty.c*/
