/* top.c - Source file:         show Linux processes */
/*
 * Copyright (c) 2002-2013, by: James C. Warner
 *    All rights reserved.      8921 Hilloway Road
 *                              Eden Prairie, Minnesota 55347 USA
 *
 * This file may be used subject to the terms and conditions of the
 * GNU Library General Public License Version 2, or any later version
 * at your option, as published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 */
/* For contributions to this program, the author wishes to thank:
 *    Craig Small, <csmall@small.dropbear.id.au>
 *    Albert D. Cahalan, <albert@users.sf.net>
 *    Sami Kerola, <kerolasa@iki.fi>
 */

#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>

#include <ctype.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>       // foul sob, defines all sorts of stuff...
#undef    tab
#undef    TTY
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <values.h>

#include "../include/fileutils.h"
#include "../include/nls.h"

#include "../proc/devname.h"
#include "../proc/procps.h"
#include "../proc/readproc.h"
#include "../proc/sig.h"
#include "../proc/sysinfo.h"
#include "../proc/version.h"
#include "../proc/wchan.h"
#include "../proc/whattime.h"

#include "top.h"
#include "top_nls.h"

/*######  Miscellaneous global stuff  ####################################*/

        /* The original and new terminal definitions
           (only set when not in 'Batch' mode) */
static struct termios Tty_original,    // our inherited terminal definition
#ifndef TERMIO_PROXY
                      Tty_tweaked,     // for interactive 'line' input
#endif
                      Tty_raw;         // for unsolicited input
static int Ttychanged = 0;

        /* Program name used in error messages and local 'rc' file name */
static char *Myname;

        /* The 'local' config file support */
static char  Rc_name [OURPATHSZ];
static RCF_t Rc = DEF_RCFILE;
static int   Rc_questions;

        /* The run-time acquired page stuff */
static unsigned Page_size;
static unsigned Pg2K_shft = 0;

        /* SMP, Irix/Solaris mode, Linux 2.5.xx support */
static int         Cpu_faux_tot;
static float       Cpu_pmax;
static const char *Cpu_States_fmts;

        /* Specific process id monitoring support */
static pid_t Monpids [MONPIDMAX] = { 0 };
static int   Monpidsidx = 0;

        /* Current screen dimensions.
           note: the number of processes displayed is tracked on a per window
                 basis (see the WIN_t).  Max_lines is the total number of
                 screen rows after deducting summary information overhead. */
        /* Current terminal screen size. */
static int Screen_cols, Screen_rows, Max_lines;

        /* This is really the number of lines needed to display the summary
           information (0 - nn), but is used as the relative row where we
           stick the cursor between frames. */
static int Msg_row;

        /* The nearly complete scroll coordinates message for the current
           window, built at the time column headers are constructed */
static char Scroll_fmts [SMLBUFSIZ];

        /* Global/Non-windows mode stuff that is NOT persistent */
static int No_ksyms = -1,       // set to '0' if ksym avail, '1' otherwise
           PSDBopen = 0,        // set to '1' if psdb opened (now postponed)
           Batch = 0,           // batch mode, collect no input, dumb output
           Loops = -1,          // number of iterations, -1 loops forever
           Secure_mode = 0,     // set if some functionality restricted
           Thread_mode = 0,     // set w/ 'H' - show threads via readeither()
           Width_mode = 0;      // set w/ 'w' - potential output override

        /* Unchangeable cap's stuff built just once (if at all) and
           thus NOT saved in a WIN_t's RCW_t.  To accommodate 'Batch'
           mode, they begin life as empty strings so the overlying
           logic need not change ! */
static char  Cap_clr_eol    [CAPBUFSIZ] = "",    // global and/or static vars
             Cap_nl_clreos  [CAPBUFSIZ] = "",    // are initialized to zeros!
             Cap_clr_scr    [CAPBUFSIZ] = "",    // the assignments used here
             Cap_curs_norm  [CAPBUFSIZ] = "",    // cost nothing but DO serve
             Cap_curs_huge  [CAPBUFSIZ] = "",    // to remind people of those
             Cap_curs_hide  [CAPBUFSIZ] = "",    // batch requirements!
             Cap_home       [CAPBUFSIZ] = "",
             Cap_norm       [CAPBUFSIZ] = "",
             Cap_reverse    [CAPBUFSIZ] = "",
             Caps_off       [CAPBUFSIZ] = "",
             Caps_endline   [CAPBUFSIZ] = "";
#ifndef RMAN_IGNORED
static char  Cap_rmam       [CAPBUFSIZ] = "",
             Cap_smam       [CAPBUFSIZ] = "";
        /* set to 1 if writing to the last column would be troublesome
           (we don't distinguish the lowermost row from the other rows) */
static int   Cap_avoid_eol = 0;
#endif
static int   Cap_can_goto = 0;

        /* Some optimization stuff, to reduce output demands...
           The Pseudo_ guys are managed by adj_geometry and frame_make.  They
           are exploited in a macro and represent 90% of our optimization.
           The Stdout_buf is transparent to our code and regardless of whose
           buffer is used, stdout is flushed at frame end or if interactive. */
static char  *Pseudo_screen;
static int    Pseudo_row = PROC_XTRA;
static size_t Pseudo_size;
#ifndef OFF_STDIOLBF
        // less than stdout's normal buffer but with luck mostly '\n' anyway
static char  Stdout_buf[2048];
#endif

        /* Our four WIN_t's, and which of those is considered the 'current'
           window (ie. which window is associated with any summ info displayed
           and to which window commands are directed) */
static WIN_t  Winstk [GROUPSMAX];
static WIN_t *Curwin;

        /* Frame oriented stuff that can't remain local to any 1 function
           and/or that would be too cumbersome managed as parms,
           and/or that are simply more efficiently handled as globals
           [ 'Frames_...' (plural) stuff persists beyond 1 frame ]
           [ or are used in response to async signals received ! ] */
   // Frames_paused set by sig_paused(), unset by pause_pgm()
static volatile int Frames_paused;     // become a paused background job
   // Frames_resize set by do_key() & sig_resize(), unset by calibrate_fields()
static volatile int Frames_resize;     // time to rebuild all column headers
static          int Frames_libflags;   // PROC_FILLxxx flags

static int          Frame_maxtask;     // last known number of active tasks
                                       // ie. current 'size' of proc table
static float        Frame_etscale;     // so we can '*' vs. '/' WHEN 'pcpu'
static unsigned     Frame_running,     // state categories for this frame
                    Frame_sleepin,
                    Frame_stopped,
                    Frame_zombied;
static int          Frame_srtflg,      // the subject window's sort direction
                    Frame_ctimes,      // the subject window's ctimes flag
                    Frame_cmdlin;      // the subject window's cmdlin flag

        /* Support for 'history' processing so we can calculate %cpu */
static int    HHist_siz;               // max number of HST_t structs
static HST_t *PHist_sav,               // alternating 'old/new' HST_t anchors
             *PHist_new;
#ifndef OFF_HST_HASH
#define       HHASH_SIZ  1024
static int    HHash_one [HHASH_SIZ],   // actual hash tables ( hereafter known
              HHash_two [HHASH_SIZ],   // as PHash_sav/PHash_new )
              HHash_nul [HHASH_SIZ];   // 'empty' hash table image
static int   *PHash_sav = HHash_one,   // alternating 'old/new' hash tables
             *PHash_new = HHash_two;
#endif

        /* Support for automatically sized fixed-width column expansions.
         * (hopefully, the macros help clarify/document our new 'feature') */
static int Autox_array [P_MAXPFLGS],
           Autox_found;
#define AUTOX_NO      P_MAXPFLGS
#define AUTOX_COL(f)  if (P_MAXPFLGS > f) Autox_array[f] = Autox_found = 1
#define AUTOX_MODE   (0 > Rc.fixed_widest)

        /* Support for scale_mem and scale_num (to avoid duplication. */
#ifdef CASEUP_SUFIX                                                // nls_maybe
   static char Scaled_sfxtab[] =  { 'K', 'M', 'G', 'T', 'P', 'E', 0 };
#else                                                              // nls_maybe
   static char Scaled_sfxtab[] =  { 'k', 'm', 'g', 't', 'p', 'e', 0 };
#endif

/*######  Sort callbacks  ################################################*/

        /*
         * These happen to be coded in the enum identifier alphabetic order,
         * not the order of the enum 'pflgs' value.  Also note that a callback
         * routine may serve more than one column.
         */

SCB_STRS(CGR, cgroup[0])
SCB_STRV(CMD, Frame_cmdlin, cmdline, cmd)
SCB_NUM1(COD, trs)
SCB_NUMx(CPN, processor)
SCB_NUM1(CPU, pcpu)
SCB_NUM1(DAT, drs)
SCB_NUM1(DRT, dt)
SCB_STRS(ENV, environ[0])
SCB_NUM1(FLG, flags)
SCB_NUM1(FL1, maj_flt)
SCB_NUM1(FL2, min_flt)
SCB_NUM1(FV1, maj_delta)
SCB_NUM1(FV2, min_delta)
SCB_NUMx(GID, egid)
SCB_STRS(GRP, egroup)
SCB_NUMx(NCE, nice)
#ifdef OOMEM_ENABLE
SCB_NUM1(OOA, oom_adj)
SCB_NUM1(OOM, oom_score)
#endif
SCB_NUMx(PGD, pgrp)
SCB_NUMx(PID, tid)
SCB_NUMx(PPD, ppid)
SCB_NUMx(PRI, priority)
SCB_NUM1(RES, resident)                // also serves MEM !
SCB_STRX(SGD, supgid)
SCB_STRS(SGN, supgrp)
SCB_NUM1(SHR, share)
SCB_NUM1(SID, session)
SCB_NUMx(STA, state)
SCB_NUM1(SWP, vm_swap)
SCB_NUMx(TGD, tgid)
SCB_NUMx(THD, nlwp)
                                       // also serves TM2 !
static int SCB_NAME(TME) (const proc_t **P, const proc_t **Q) {
   if (Frame_ctimes) {
      if (((*P)->cutime + (*P)->cstime + (*P)->utime + (*P)->stime)
        < ((*Q)->cutime + (*Q)->cstime + (*Q)->utime + (*Q)->stime))
           return SORT_lt;
      if (((*P)->cutime + (*P)->cstime + (*P)->utime + (*P)->stime)
        > ((*Q)->cutime + (*Q)->cstime + (*Q)->utime + (*Q)->stime))
           return SORT_gt;
   } else {
      if (((*P)->utime + (*P)->stime) < ((*Q)->utime + (*Q)->stime))
         return SORT_lt;
      if (((*P)->utime + (*P)->stime) > ((*Q)->utime + (*Q)->stime))
         return SORT_gt;
   }
   return SORT_eq;
}
SCB_NUM1(TPG, tpgid)
SCB_NUMx(TTY, tty)
SCB_NUMx(UED, euid)
SCB_STRS(UEN, euser)
SCB_NUMx(URD, ruid)
SCB_STRS(URN, ruser)
SCB_NUMx(USD, suid)
SCB_STRS(USN, suser)
SCB_NUM1(VRT, size)
SCB_NUM1(WCH, wchan)

#ifdef OFF_HST_HASH
        /* special sort for procs_hlp() ! ------------------------ */
static int sort_HST_t (const HST_t *P, const HST_t *Q) {
   return P->pid - Q->pid;
}
#endif

/*######  Tiny useful routine(s)  ########################################*/

        /*
         * This routine simply formats whatever the caller wants and
         * returns a pointer to the resulting 'const char' string... */
static const char *fmtmk (const char *fmts, ...) __attribute__((format(printf,1,2)));
static const char *fmtmk (const char *fmts, ...) {
   static char buf[BIGBUFSIZ];          // with help stuff, our buffer
   va_list va;                          // requirements now exceed 1k

   va_start(va, fmts);
   vsnprintf(buf, sizeof(buf), fmts, va);
   va_end(va);
   return (const char *)buf;
} // end: fmtmk


        /*
         * This guy is just our way of avoiding the overhead of the standard
         * strcat function (should the caller choose to participate) */
static inline char *scat (char *dst, const char *src) {
   while (*dst) dst++;
   while ((*(dst++) = *(src++)));
   return --dst;
} // end: scat


        /*
         * This guy just facilitates Batch and protects against dumb ttys
         * -- we'd 'inline' him but he's only called twice per frame,
         * yet used in many other locations. */
static const char *tg2 (int x, int y) {
   // it's entirely possible we're trying for an invalid row...
   return Cap_can_goto ? tgoto(cursor_address, x, y) : "";
} // end: tg2

/*######  Exit/Interrput routines  #######################################*/

        /*
         * Reset the tty, if necessary */
static void at_eoj (void) {
   if (Ttychanged) {
      if (keypad_local) putp(keypad_local);
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &Tty_original);
      putp(tg2(0, Screen_rows));
      putp(Cap_curs_norm);
#ifndef RMAN_IGNORED
      putp(Cap_smam);
#endif
      putp("\n");
      Ttychanged = 0;
   }
   fflush(stdout);
} // end:


        /*
         * The real program end */
static void bye_bye (const char *str) NORETURN;
static void bye_bye (const char *str) {
   at_eoj();                 // restore tty in preparation for exit
#ifdef ATEOJ_RPTSTD
{  proc_t *p;
   if (!str) { fprintf(stderr,
      "\n%s's Summary report:"
      "\n\tProgram"
      "\n\t   Linux version = %u.%u.%u, %s"
      "\n\t   Hertz = %u (%u bytes, %u-bit time)"
      "\n\t   Page_size = %d, Cpu_faux_tot = %d, smp_num_cpus = %d"
      "\n\t   sizeof(CPU_t) = %u, sizeof(HST_t) = %u (%u HST_t's/Page), HHist_siz = %u"
      "\n\t   sizeof(proc_t) = %u, sizeof(proc_t.cmd) = %u, sizeof(proc_t*) = %u"
      "\n\t   Frames_libflags = %08lX"
      "\n\t   SCREENMAX = %u, ROWMINSIZ = %u, ROWMAXSIZ = %u"
      "\n\t   PACKAGE = '%s', LOCALEDIR = '%s'"
      "\n\tTerminal: %s"
      "\n\t   device = %s, ncurses = v%s"
      "\n\t   max_colors = %d, max_pairs = %d"
      "\n\t   Cap_can_goto = %s"
      "\n\t   Screen_cols = %d, Screen_rows = %d"
      "\n\t   Max_lines = %d, most recent Pseudo_size = %u"
#ifndef OFF_STDIOLBF
      "\n\t   Stdout_buf = %u, BUFSIZ = %u"
#endif
      "\n\tWindows and Curwin->"
      "\n\t   sizeof(WIN_t) = %u, GROUPSMAX = %d"
      "\n\t   winname = %s, grpname = %s"
#ifdef CASEUP_HEXES
      "\n\t   winflags = %08X, maxpflgs = %d"
#else
      "\n\t   winflags = %08x, maxpflgs = %d"
#endif
      "\n\t   sortindx = %d, fieldscur = %s"
      "\n\t   maxtasks = %d, varcolsz = %d, winlines = %d"
      "\n\t   strlen(columnhdr) = %d"
      "\n"
      , __func__
      , LINUX_VERSION_MAJOR(linux_version_code)
      , LINUX_VERSION_MINOR(linux_version_code)
      , LINUX_VERSION_PATCH(linux_version_code)
      , procps_version
      , (unsigned)Hertz, (unsigned)sizeof(Hertz), (unsigned)sizeof(Hertz) * 8
      , Page_size, Cpu_faux_tot, (int)smp_num_cpus, (unsigned)sizeof(CPU_t)
      , (unsigned)sizeof(HST_t), Page_size / (unsigned)sizeof(HST_t), HHist_siz
      , (unsigned)sizeof(proc_t), (unsigned)sizeof(p->cmd), (unsigned)sizeof(proc_t*)
      , (long)Frames_libflags
      , (unsigned)SCREENMAX, (unsigned)ROWMINSIZ, (unsigned)ROWMAXSIZ
      , PACKAGE, LOCALEDIR
#ifdef PRETENDNOCAP
      , "dumb"
#else
      , termname()
#endif
      , ttyname(STDOUT_FILENO), NCURSES_VERSION
      , max_colors, max_pairs
      , Cap_can_goto ? "yes" : "No!"
      , Screen_cols, Screen_rows
      , Max_lines, (unsigned)Pseudo_size
#ifndef OFF_STDIOLBF
      , (unsigned)sizeof(Stdout_buf), (unsigned)BUFSIZ
#endif
      , (unsigned)sizeof(WIN_t), GROUPSMAX
      , Curwin->rc.winname, Curwin->grpname
      , Curwin->rc.winflags, Curwin->maxpflgs
      , Curwin->rc.sortindx, Curwin->rc.fieldscur
      , Curwin->rc.maxtasks, Curwin->varcolsz, Curwin->winlines
      , (int)strlen(Curwin->columnhdr)
      );
   }
}
#endif // end: ATEOJ_RPTSTD

#ifndef OFF_HST_HASH
#ifdef ATEOJ_RPTHSH
   if (!str) {
      int i, j, pop, total_occupied, maxdepth, maxdepth_sav, numdepth
         , cross_foot, sz = HHASH_SIZ * (unsigned)sizeof(int);
      int depths[HHASH_SIZ];

      for (i = 0, total_occupied = 0, maxdepth = 0; i < HHASH_SIZ; i++) {
         int V = PHash_new[i];
         j = 0;
         if (-1 < V) {
            ++total_occupied;
            while (-1 < V) {
               V = PHist_new[V].lnk;
               if (-1 < V) j++;
            }
         }
         depths[i] = j;
         if (maxdepth < j) maxdepth = j;
      }
      maxdepth_sav = maxdepth;

      fprintf(stderr,
         "\n%s's Supplementary HASH report:"
         "\n\tTwo Tables providing for %d entries each + 1 extra for 'empty' image"
         "\n\t%dk (%d bytes) per table, %d total bytes (including 'empty' image)"
         "\n\tResults from latest hash (PHash_new + PHist_new)..."
         "\n"
         "\n\tTotal hashed = %d"
         "\n\tLevel-0 hash entries = %d (%d%% occupied)"
         "\n\tMax Depth = %d"
         "\n\n"
         , __func__
         , HHASH_SIZ, sz / 1024, sz, sz * 3
         , Frame_maxtask
         , total_occupied, (total_occupied * 100) / HHASH_SIZ
         , maxdepth + 1);

      if (total_occupied) {
         for (pop = total_occupied, cross_foot = 0; maxdepth; maxdepth--) {
            for (i = 0, numdepth = 0; i < HHASH_SIZ; i++)
               if (depths[i] == maxdepth) ++numdepth;
            fprintf(stderr,
               "\t %5d (%3d%%) hash table entries at depth %d\n"
               , numdepth, (numdepth * 100) / total_occupied, maxdepth + 1);
            pop -= numdepth;
            cross_foot += numdepth;
            if (0 == pop && cross_foot == total_occupied) break;
         }
         if (pop) {
            fprintf(stderr, "\t %5d (%3d%%) unchained hash table entries\n"
               , pop, (pop * 100) / total_occupied);
            cross_foot += pop;
         }
         fprintf(stderr,
            "\t -----\n"
            "\t %5d total entries occupied\n", cross_foot);

         if (maxdepth_sav) {
            fprintf(stderr, "\nPIDs at max depth: ");
            for (i = 0; i < HHASH_SIZ; i++)
               if (depths[i] == maxdepth_sav) {
                  j = PHash_new[i];
                  fprintf(stderr, "\n\tpos %4d:  %05d", i, PHist_new[j].pid);
                  while (-1 < j) {
                     j = PHist_new[j].lnk;
                     if (-1 < j) fprintf(stderr, ", %05d", PHist_new[j].pid);
                  }
               }
            fprintf(stderr, "\n");
         }
      }
   }
#endif // end: ATEOJ_RPTHSH
#endif // end: OFF_HST_HASH

   if (str) {
      fputs(str, stderr);
      exit(EXIT_FAILURE);
   }
   exit(EXIT_SUCCESS);
} // end: bye_bye


        /*
         * Standard error handler to normalize the look of all err output */
static void error_exit (const char *str) NORETURN;
static void error_exit (const char *str) {
   static char buf[MEDBUFSIZ];

   /* we'll use our own buffer so callers can still use fmtmk() and, yes the
      leading tab is not the standard convention, but the standard is wrong
      -- OUR msg won't get lost in screen clutter, like so many others! */
   snprintf(buf, sizeof(buf), "\t%s: %s\n", Myname, str);
   bye_bye(buf);
} // end: error_exit


        /*
         * Handle library errors ourselves rather than accept a default
         * fprintf to stderr (since we've mucked with the termios struct) */
static void library_err (const char *fmts, ...) NORETURN;
static void library_err (const char *fmts, ...) {
   static char tmp[MEDBUFSIZ];
   va_list va;

   va_start(va, fmts);
   vsnprintf(tmp, sizeof(tmp), fmts, va);
   va_end(va);
   error_exit(tmp);
} // end: library_err


        /*
         * Called in response to Frames_paused (tku: sig_paused) */
static void pause_pgm (void) {
   Frames_paused = 0;
   // reset terminal (maybe)
   if (Ttychanged) tcsetattr(STDIN_FILENO, TCSAFLUSH, &Tty_original);
   putp(tg2(0, Screen_rows));
   putp(Cap_curs_norm);
#ifndef RMAN_IGNORED
   putp(Cap_smam);
#endif
   fflush(stdout);
   raise(SIGSTOP);
   // later, after SIGCONT...
   if (Ttychanged) tcsetattr(STDIN_FILENO, TCSAFLUSH, &Tty_raw);
#ifndef RMAN_IGNORED
   putp(Cap_rmam);
#endif
} // end: pause_pgm


        /*
         * Catches all remaining signals not otherwise handled */
static void sig_abexit (int sig) {
   sigset_t ss;

   sigfillset(&ss);
   sigprocmask(SIG_BLOCK, &ss, NULL);
   at_eoj();                 // restore tty in preparation for exit
   fprintf(stderr, N_fmt(EXIT_signals_fmt)
      , sig, signal_number_to_name(sig), Myname);
   signal(sig, SIG_DFL);     // allow core dumps, if applicable
   raise(sig);               // ( plus set proper return code )
} // end: sig_abexit


        /*
         * Catches:
         *    SIGALRM, SIGHUP, SIGINT, SIGPIPE, SIGQUIT, SIGTERM,
         *    SIGUSR1 and SIGUSR2 */
static void sig_endpgm (int dont_care_sig) NORETURN;
static void sig_endpgm (int dont_care_sig) {
   sigset_t ss;

   sigfillset(&ss);
   sigprocmask(SIG_BLOCK, &ss, NULL);
   bye_bye(NULL);
   (void)dont_care_sig;
} // end: sig_endpgm


        /*
         * Catches:
         *    SIGTSTP, SIGTTIN and SIGTTOU */
static void sig_paused (int dont_care_sig) {
   (void)dont_care_sig;
   Frames_paused = 1;
} // end: sig_paused


        /*
         * Catches:
         *    SIGCONT and SIGWINCH */
static void sig_resize (int dont_care_sig) {
   (void)dont_care_sig;
   Frames_resize = 2;
} // end: sig_resize

/*######  Misc Color/Display support  ####################################*/

        /*
         * Make the appropriate caps/color strings for a window/field group.
         * note: we avoid the use of background color so as to maximize
         *       compatibility with the user's xterm settings */
static void capsmk (WIN_t *q) {
   /* macro to test if a basic (non-color) capability is valid
         thanks: Floyd Davidson <floyd@ptialaska.net> */
 #define tIF(s)  s ? s : ""
   static int capsdone = 0;

   // we must NOT disturb our 'empty' terminfo strings!
   if (Batch) return;

   // these are the unchangeable puppies, so we only do 'em once
   if (!capsdone) {
      STRLCPY(Cap_clr_eol, tIF(clr_eol))
      STRLCPY(Cap_clr_scr, tIF(clear_screen))
      // due to the leading newline, the following must be used with care
      snprintf(Cap_nl_clreos, sizeof(Cap_nl_clreos), "\n%s", tIF(clr_eos));
      STRLCPY(Cap_curs_huge, tIF(cursor_visible))
      STRLCPY(Cap_curs_norm, tIF(cursor_normal))
      STRLCPY(Cap_curs_hide, tIF(cursor_invisible))
      STRLCPY(Cap_home, tIF(cursor_home))
      STRLCPY(Cap_norm, tIF(exit_attribute_mode))
      STRLCPY(Cap_reverse, tIF(enter_reverse_mode))
#ifndef RMAN_IGNORED
      if (!eat_newline_glitch) {
         STRLCPY(Cap_rmam, tIF(exit_am_mode))
         STRLCPY(Cap_smam, tIF(enter_am_mode))
         if (!*Cap_rmam || !*Cap_smam) {
            *Cap_rmam = '\0';
            *Cap_smam = '\0';
            if (auto_right_margin)
               Cap_avoid_eol = 1;
         }
         putp(Cap_rmam);
      }
#endif
      snprintf(Caps_off, sizeof(Caps_off), "%s%s", Cap_norm, tIF(orig_pair));
      snprintf(Caps_endline, sizeof(Caps_endline), "%s%s", Caps_off, Cap_clr_eol);
      if (tgoto(cursor_address, 1, 1)) Cap_can_goto = 1;
      capsdone = 1;
   }

   /* the key to NO run-time costs for configurable colors -- we spend a
      little time with the user now setting up our terminfo strings, and
      the job's done until he/she/it has a change-of-heart */
   STRLCPY(q->cap_bold, CHKw(q, View_NOBOLD) ? Cap_norm : tIF(enter_bold_mode))
   if (CHKw(q, Show_COLORS) && max_colors > 0) {
      STRLCPY(q->capclr_sum, tparm(set_a_foreground, q->rc.summclr))
      snprintf(q->capclr_msg, sizeof(q->capclr_msg), "%s%s"
         , tparm(set_a_foreground, q->rc.msgsclr), Cap_reverse);
      snprintf(q->capclr_pmt, sizeof(q->capclr_pmt), "%s%s"
         , tparm(set_a_foreground, q->rc.msgsclr), q->cap_bold);
      snprintf(q->capclr_hdr, sizeof(q->capclr_hdr), "%s%s"
         , tparm(set_a_foreground, q->rc.headclr), Cap_reverse);
      snprintf(q->capclr_rownorm, sizeof(q->capclr_rownorm), "%s%s"
         , Caps_off, tparm(set_a_foreground, q->rc.taskclr));
   } else {
      q->capclr_sum[0] = '\0';
#ifdef USE_X_COLHDR
      snprintf(q->capclr_msg, sizeof(q->capclr_pmt), "%s%s"
         , Cap_reverse, q->cap_bold);
#else
      STRLCPY(q->capclr_msg, Cap_reverse)
#endif
      STRLCPY(q->capclr_pmt, q->cap_bold)
      STRLCPY(q->capclr_hdr, Cap_reverse)
      STRLCPY(q->capclr_rownorm, Cap_norm)
   }

   // composite(s), so we do 'em outside and after the if
   snprintf(q->capclr_rowhigh, sizeof(q->capclr_rowhigh), "%s%s"
      , q->capclr_rownorm, CHKw(q, Show_HIBOLD) ? q->cap_bold : Cap_reverse);
 #undef tIF
} // end: capsmk


        /*
         * Show an error message (caller may include '\a' for sound) */
static void show_msg (const char *str) {
   PUTT("%s%s %.*s %s%s"
      , tg2(0, Msg_row)
      , Curwin->capclr_msg
      , Screen_cols - 2
      , str
      , Caps_off
      , Cap_clr_eol);
   fflush(stdout);
   usleep(MSG_USLEEP);
} // end: show_msg


        /*
         * Show an input prompt + larger cursor (if possible) */
static int show_pmt (const char *str) {
   int rc;

   PUTT("%s%s%.*s %s%s%s"
      , tg2(0, Msg_row)
      , Curwin->capclr_pmt
      , Screen_cols - 3
      , str
      , Cap_curs_huge
      , Caps_off
      , Cap_clr_eol);
   fflush(stdout);
   // +2 for the ': ' chars we added or -1 for the cursor...
   return ((rc = (int)strlen(str)+2) < Screen_cols) ? rc : Screen_cols-1;
} // end: show_pmt


        /*
         * Show a special coordinate message, in support of scrolling */
static inline void show_scroll (void) {
   PUTT(Scroll_fmts, tg2(0, Msg_row), Frame_maxtask);
   putp(tg2(0, Msg_row));
} // end: show_scroll


        /*
         * Show lines with specially formatted elements, but only output
         * what will fit within the current screen width.
         *    Our special formatting consists of:
         *       "some text <_delimiter_> some more text <_delimiter_>...\n"
         *    Where <_delimiter_> is a two byte combination consisting of a
         *    tilde followed by an ascii digit in the the range of 1 - 8.
         *       examples: ~1,  ~5,  ~8, etc.
         *    The tilde is effectively stripped and the next digit
         *    converted to an index which is then used to select an
         *    'attribute' from a capabilities table.  That attribute
         *    is then applied to the *preceding* substring.
         * Once recognized, the delimiter is replaced with a null character
         * and viola, we've got a substring ready to output!  Strings or
         * substrings without delimiters will receive the Cap_norm attribute.
         *
         * Caution:
         *    This routine treats all non-delimiter bytes as displayable
         *    data subject to our screen width marching orders.  If callers
         *    embed non-display data like tabs or terminfo strings in our
         *    glob, a line will truncate incorrectly at best.  Worse case
         *    would be truncation of an embedded tty escape sequence.
         *
         *    Tabs must always be avoided or our efforts are wasted and
         *    lines will wrap.  To lessen but not eliminate the risk of
         *    terminfo string truncation, such non-display stuff should
         *    be placed at the beginning of a "short" line. */
static void show_special (int interact, const char *glob) {
  /* note: the following is for documentation only,
           the real captab is now found in a group's WIN_t !
     +------------------------------------------------------+
     | char *captab[] = {                 :   Cap's/Delim's |
     |   Cap_norm, Cap_norm,              =   \000, \001,   |
     |   cap_bold, capclr_sum,            =   \002, \003,   |
     |   capclr_msg, capclr_pmt,          =   \004, \005,   |
     |   capclr_hdr,                      =   \006,         |
     |   capclr_rowhigh,                  =   \007,         |
     |   capclr_rownorm  };               =   \010 [octal!] |
     +------------------------------------------------------+ */
  /* ( Pssst, after adding the termcap transitions, row may )
     ( exceed 300+ bytes, even in an 80x24 terminal window! )
     ( And if we're no longer guaranteed lines created only )
     ( by top, we'll need larger buffs plus some protection )
     ( against overrunning them with this 'lin_end - glob'. ) */
   char tmp[LRGBUFSIZ], lin[LRGBUFSIZ], row[ROWMAXSIZ];
   char *rp, *lin_end, *sub_beg, *sub_end;
   int room;

   // handle multiple lines passed in a bunch
   while ((lin_end = strchr(glob, '\n'))) {
     #define myMIN(a,b) (((a) < (b)) ? (a) : (b))
      size_t lessor = myMIN((size_t)(lin_end - glob), sizeof(lin) -1);

      // create a local copy we can extend and otherwise abuse
      memcpy(lin, glob, lessor);
      // zero terminate this part and prepare to parse substrings
      lin[lessor] = '\0';
      room = Screen_cols;
      sub_beg = sub_end = lin;
      *(rp = row) = '\0';

      while (*sub_beg) {
         int ch = *sub_end;
         if ('~' == ch) ch = *(sub_end + 1) - '0';
         switch (ch) {
            case 0:                    // no end delim, captab makes normal
               *(sub_end + 1) = '\0';  // extend str end, then fall through
               *(sub_end + 2) = '\0';  // ( +1 optimization for usual path )
            case 1: case 2: case 3: case 4:
            case 5: case 6: case 7: case 8:
               *sub_end = '\0';
               snprintf(tmp, sizeof(tmp), "%s%.*s%s"
                  , Curwin->captab[ch], room, sub_beg, Caps_off);
               rp = scat(rp, tmp);
               room -= (sub_end - sub_beg);
               sub_beg = (sub_end += 2);
               break;
            default:                   // nothin' special, just text
               ++sub_end;
         }
         if (0 >= room) break;         // skip substrings that won't fit
      }

      if (interact) PUTT("%s%s\n", row, Cap_clr_eol);
      else PUFF("%s%s\n", row, Caps_endline);
      glob = ++lin_end;                // point to next line (maybe)

     #undef myMIN
   } // end: while 'lines'

   /* If there's anything left in the glob (by virtue of no trailing '\n'),
      it probably means caller wants to retain cursor position on this final
      line.  That, in turn, means we're interactive and so we'll just do our
      'fit-to-screen' thingy while also leaving room for the cursor... */
   if (*glob) PUTT("%.*s", Screen_cols -1, glob);
} // end: show_special


        /*
         * Create a nearly complete scroll coordinates message, but still
         * a format string since we'll be missing a tgoto and total tasks. */
static void updt_scroll_msg (void) {
   char tmp1[SMLBUFSIZ], tmp2[SMLBUFSIZ];
   int totpflgs = Curwin->totpflgs;
   int begpflgs = Curwin->begpflg + 1;

#ifndef USE_X_COLHDR
   if (CHKw(Curwin, Show_HICOLS)) {
      totpflgs -= 2;
      if (ENUpos(Curwin, Curwin->rc.sortindx) < Curwin->begpflg) begpflgs -= 2;
   }
#endif
   if (1 > totpflgs) totpflgs = 1;
   if (1 > begpflgs) begpflgs = 1;
   snprintf(tmp1, sizeof(tmp1)
      , N_fmt(SCROLL_coord_fmt), Curwin->begtask + 1, begpflgs, totpflgs);
   strcpy(tmp2, tmp1);
#ifndef SCROLLVAR_NO
   if (Curwin->varcolbeg)
      snprintf(tmp2, sizeof(tmp2), "%s + %d", tmp1, Curwin->varcolbeg);
#endif
   snprintf(Scroll_fmts, sizeof(Scroll_fmts)
      , "%%s%s  %.*s%s", Caps_off, Screen_cols - 3, tmp2, Cap_clr_eol);
} // end: updt_scroll_msg

/*######  Low Level Memory/Keyboard/File I/O support  ####################*/

        /*
         * Handle our own memory stuff without the risk of leaving the
         * user's terminal in an ugly state should things go sour. */

static void *alloc_c (size_t num) MALLOC;
static void *alloc_c (size_t num) {
   void *pv;

   if (!num) ++num;
   if (!(pv = calloc(1, num)))
      error_exit(N_txt(FAIL_alloc_c_txt));
   return pv;
} // end: alloc_c


static void *alloc_r (void *ptr, size_t num) MALLOC;
static void *alloc_r (void *ptr, size_t num) {
   void *pv;

   if (!num) ++num;
   if (!(pv = realloc(ptr, num)))
      error_exit(N_txt(FAIL_alloc_r_txt));
   return pv;
} // end: alloc_r


        /*
         * This routine isolates ALL user INPUT and ensures that we
         * wont be mixing I/O from stdio and low-level read() requests */
static int chin (int ech, char *buf, unsigned cnt) {
   fd_set fs;
   int rc = -1;

   fflush(stdout);
#ifndef TERMIO_PROXY
   if (ech) {
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &Tty_tweaked);
      rc = read(STDIN_FILENO, buf, cnt);
      tcsetattr(STDIN_FILENO, TCSAFLUSH, &Tty_raw);
   } else {
      FD_ZERO(&fs);
      FD_SET(STDIN_FILENO, &fs);
      if (0 < select(STDIN_FILENO + 1, &fs, NULL, NULL, NULL))
         rc = read(STDIN_FILENO, buf, cnt);
   }
#else
   (void)ech;
   FD_ZERO(&fs);
   FD_SET(STDIN_FILENO, &fs);
   if (0 < select(STDIN_FILENO + 1, &fs, NULL, NULL, NULL))
      rc = read(STDIN_FILENO, buf, cnt);
#endif

   // zero means EOF, might happen if we erroneously get detached from terminal
   if (0 == rc) bye_bye(NULL);

   // it may have been the beginning of a lengthy escape sequence
   tcflush(STDIN_FILENO, TCIFLUSH);

   // note: we do NOT produce a vaid 'string'
   return rc;
} // end: chin


        /*
         * Support for single keystroke input AND escaped cursor motion keys
         * note: we support more keys than we currently need, in case
         *       we attract new consumers in the future */
static int keyin (int init) {
   static char buf12[CAPBUFSIZ], buf13[CAPBUFSIZ]
      , buf14[CAPBUFSIZ], buf15[CAPBUFSIZ];
   static struct {
      const char *str;
      int key;
   } tinfo_tab[] = {
      { "\n", kbd_ENTER    }, { NULL, kbd_UP       }, { NULL, kbd_DOWN     },
      { NULL, kbd_LEFT     }, { NULL, kbd_RIGHT    }, { NULL, kbd_PGUP     },
      { NULL, kbd_PGDN     }, { NULL, kbd_HOME     }, { NULL, kbd_END      },
      { NULL, kbd_BKSP     }, { NULL, kbd_INS      }, { NULL, kbd_DEL      },
         // next 4 destined to be meta + arrow keys...
      { buf12, kbd_PGUP    }, { buf13, kbd_PGDN    },
      { buf14, kbd_HOME    }, { buf15, kbd_END     },
         // remainder are alternatives for above, just in case...
         // ( the k,j,l,h entries are the vim cursor motion keys )
      { "\033\\",   kbd_UP    }, { "\033/",    kbd_DOWN  }, /* meta+      \,/ */
      { "\033<",    kbd_LEFT  }, { "\033>",    kbd_RIGHT }, /* meta+      <,> */
      { "\033k",    kbd_UP    }, { "\033j",    kbd_DOWN  }, /* meta+      k,j */
      { "\033h",    kbd_LEFT  }, { "\033l",    kbd_RIGHT }, /* meta+      h,l */
      { "\033\013", kbd_PGUP  }, { "\033\012", kbd_PGDN  }, /* ctrl+meta+ k,j */
      { "\033\010", kbd_HOME  }, { "\033\014", kbd_END   }  /* ctrl+meta+ h,l */
   };
   char buf[SMLBUFSIZ], *pb;
   int i;

   if (init) {
    #define tOk(s)  s ? s : ""
      tinfo_tab[1].str  = tOk(key_up);
      tinfo_tab[2].str  = tOk(key_down);
      tinfo_tab[3].str  = tOk(key_left);
      tinfo_tab[4].str  = tOk(key_right);
      tinfo_tab[5].str  = tOk(key_ppage);
      tinfo_tab[6].str  = tOk(key_npage);
      tinfo_tab[7].str  = tOk(key_home);
      tinfo_tab[8].str  = tOk(key_end);
      tinfo_tab[9].str  = tOk(key_backspace);
      tinfo_tab[10].str = tOk(key_ic);
      tinfo_tab[11].str = tOk(key_dc);
      STRLCPY(buf12, fmtmk("\033%s", tOk(key_up)));
      STRLCPY(buf13, fmtmk("\033%s", tOk(key_down)));
      STRLCPY(buf14, fmtmk("\033%s", tOk(key_left)));
      STRLCPY(buf15, fmtmk("\033%s", tOk(key_right)));
      // next is critical so returned results match bound terminfo keys
      putp(tOk(keypad_xmit));
      // ( see the converse keypad_local at program end, just in case )
      return 0;
    #undef tOk
   }

   memset(buf, '\0', sizeof(buf));
   if (1 > chin(0, buf, sizeof(buf)-1)) return 0;

   /* some emulators implement 'key repeat' too well and we get duplicate
      key sequences -- so we'll focus on the last escaped sequence, while
      also allowing use of the meta key... */
   if (!(pb = strrchr(buf, '\033'))) pb = buf;
   else if (pb > buf && '\033' == *(pb - 1)) --pb;

   for (i = 0; i < MAXTBL(tinfo_tab); i++)
      if (!strcmp(tinfo_tab[i].str, pb))
         return tinfo_tab[i].key;

   // no match, so we'll return single non-escaped keystrokes only
   if (buf[0] == '\033' && buf[1]) return 0;
   return buf[0];
} // end: keyin


#ifndef TERMIO_PROXY
        /*
         * Get line oriented interactive input from the user,
         * using native tty support */
static char *linein (const char *prompt) {
   static const char ws[] = "\b\f\n\r\t\v\x1b\x9b";  // 0x1b + 0x9b are escape
   static char buf[MEDBUFSIZ];
   char *p;

   show_pmt(prompt);
   memset(buf, '\0', sizeof(buf));
   chin(1, buf, sizeof(buf)-1);
   putp(Cap_curs_norm);

   if ((p = strpbrk(buf, ws))) *p = '\0';
   // note: we DO produce a vaid 'string'
   return buf;
} // end: linein

#else
        /*
         * Get line oriented interactive input from the user,
         * going way beyond native tty support !
         * Unlike native tty input support, this function provides:
         * . true line editing, not just destructive backspace
         * . an input limit that's sensitive to current screen dimensions
         * . immediate signal response without the need to wait for '\n'
         * However, the user will lose the ability to paste keystrokes
         * when this function is chosen over the smaller alternative above! */
static char *linein (const char *prompt) {
    // thank goodness memmove allows the two strings to overlap
 #define sqzSTR  { memmove(&buf[pos], &buf[pos+1], bufMAX-pos); \
       buf[sizeof(buf)-1] = '\0'; }
 #define expSTR  if (len+1 < bufMAX && len+beg+1 < Screen_cols) { \
       memmove(&buf[pos+1], &buf[pos], bufMAX-pos); buf[pos] = ' '; }
 #define logCOL  (pos+1)
 #define phyCOL  (beg+pos+1)
 #define bufMAX  ((int)sizeof(buf)-2)  // -1 for '\0' string delimeter
   static char buf[MEDBUFSIZ+1];       // +1 for '\0' string delimeter
   int beg, pos, len;
   int key;

   pos = 0;
   beg = show_pmt(prompt);
   memset(buf, '\0', sizeof(buf));
   do {
      len = strlen(buf);
      switch (key = keyin(0)) {
         case kbd_ESC:
            buf[0] = '\0';             // fall through !
         case kbd_ENTER:
            break;
         case kbd_DEL:
         case kbd_DOWN:
            sqzSTR
            break;
         case kbd_BKSP :
            if (0 < pos) { --pos; sqzSTR }
            break;
         case kbd_INS:
         case kbd_UP:
            expSTR
            break;
         case kbd_LEFT:
            if (0 < pos) --pos;
            break;
         case kbd_RIGHT:
            if (pos < len) ++pos;
            break;
         case kbd_HOME:
            pos = 0;
            break;
         case kbd_END:
            pos = len;
            break;
         default:                      // what we REALLY wanted (maybe)
            if (isprint(key) && logCOL < bufMAX && phyCOL < Screen_cols)
               buf[pos++] = key;
            break;
      }
      putp(fmtmk("%s%s%s", tg2(beg, Msg_row), Cap_clr_eol, buf));
      putp(tg2(beg+pos, Msg_row));
   } while (key && kbd_ENTER != key && kbd_ESC != key);

   return buf;
 #undef sqzSTR
 #undef expSTR
 #undef logCOL
 #undef phyCOL
 #undef bufMAX
} // end: linein
#endif


        /*
         * This routine provides the i/o in support of files whose size
         * cannot be determined in advance.  Given a stream pointer, he'll
         * try to slurp in the whole thing and return a dynamically acquired
         * buffer supporting that single string glob.
         *
         * He always creates a buffer at least READMINSZ big, possibly
         * all zeros (an empty string), even if the file wasn't read. */
static int readfile (FILE *fp, char **baddr, size_t *bsize, size_t *bread) {
   char chunk[4096*16];
   size_t num;

   *bread = 0;
   *bsize = READMINSZ;
   *baddr = alloc_c(READMINSZ);
   if (fp) {
      while (0 < (num = fread(chunk, 1, sizeof(chunk) -1, fp))) {
         if (feof(fp) && chunk[num -1]) chunk[num++] = '\0';
         *baddr = alloc_r(*baddr, num + *bsize);
         memcpy(*baddr + *bread, chunk, num);
         *bread += num;
         *bsize += num;
      };
      // adjust for the null terminator, which was counted above
      if (*bread) --(*bread);
      return ferror(fp);
   }
   return ENOENT;
} // end: readfile

/*######  Small Utility routines  ########################################*/

        /*
         * Get a float from the user */
static float get_float (const char *prompt) {
   char *line;
   float f;

   if (!(*(line = linein(prompt)))) return -1.0;
   // note: we're not allowing negative floats
   if (strcspn(line, "+,.0123456789")) {
      show_msg(N_txt(BAD_numfloat_txt));
      return -1.0;
   }
   sscanf(line, "%f", &f);
   return f;
} // end: get_float


#define GET_INT_BAD  INT_MIN
#define GET_INTNONE (INT_MIN + 1)

        /*
         * Get an integer from the user, returning INT_MIN for error */
static int get_int (const char *prompt) {
   char *line;
   int n;

   if (!(*(line = linein(prompt)))) return GET_INTNONE;
   // note: we've got to allow negative ints (renice)
   if (strcspn(line, "-+0123456789")) {
      show_msg(N_txt(BAD_integers_txt));
      return GET_INT_BAD;
   }
   sscanf(line, "%d", &n);
   return n;
} // end: get_int


        /*
         * Make a hex value, and maybe suppress zeroes. */
static inline const char *hex_make (KLONG num, int noz) {
   static char buf[SMLBUFSIZ];
   int i;

#ifdef CASEUP_HEXES
   snprintf(buf, sizeof(buf), "%08" KLF "X", num);
#else
   snprintf(buf, sizeof(buf), "%08" KLF "x", num);
#endif
   if (noz)
      for (i = 0; buf[i]; i++)
         if ('0' == buf[i])
            buf[i] = '.';
   return buf;
} // end: hex_make


        /*
         * Validate the passed string as a user name or number,
         * and/or update the window's 'u/U' selection stuff. */
static const char *user_certify (WIN_t *q, const char *str, char typ) {
   struct passwd *pwd;
   char *endp;
   uid_t num;

   q->usrseltyp = 0;
   Monpidsidx = 0;
   if (*str) {
      num = (uid_t)strtoul(str, &endp, 0);
      if ('\0' == *endp) {
         pwd = getpwuid(num);
         if (!pwd) {
         /* allow foreign users, from e.g within chroot
          ( thanks Dr. Werner Fink <werner@suse.de> ) */
            q->usrseluid = num;
            q->usrseltyp = typ;
            return NULL;
         }
      } else
         pwd = getpwnam(str);
      if (!pwd) return N_txt(BAD_username_txt);
      q->usrseluid = pwd->pw_uid;
      q->usrseltyp = typ;
   }
   return NULL;
} // end: user_certify


        /*
         * Determine if this proc_t matches the 'u/U' selection criteria
         * for a given window -- it's called from only one place, and
         * likely inlined even without the directive */
static inline int user_matched (WIN_t *q, const proc_t *p) {
   switch(q->usrseltyp) {
      case 0:                                    // uid selection inactive
         return 1;
      case 'U':                                  // match any uid
         if (p->ruid == q->usrseluid) return 1;
         if (p->suid == q->usrseluid) return 1;
         if (p->fuid == q->usrseluid) return 1;
      // fall through...
      case 'u':                                  // match effective uid
         if (p->euid == q->usrseluid) return 1;
      // fall through...
      default:                                   // no match, don't display
         ;
   }
   return 0;
} // end: user_matched

/*######  Basic Formatting support  ######################################*/

        /*
         * Just do some justify stuff, then add post column padding. */
static inline const char *justify_pad (const char *str, int width, int justr) {
   static char l_fmt[]  = "%-*.*s%s", r_fmt[] = "%*.*s%s";
   static char buf[SCREENMAX];

   snprintf(buf, sizeof(buf), justr ? r_fmt : l_fmt, width, width, str, COLPADSTR);
   return buf;
} // end: justify_pad


        /*
         * Make and then justify a single character. */
static inline const char *make_chr (const char ch, int width, int justr) {
   static char buf[SMLBUFSIZ];

   snprintf(buf, sizeof(buf), "%c", ch);
   return justify_pad(buf, width, justr);
} // end: make_chr


        /*
         * Make and then justify an integer NOT subject to scaling,
         * and include a visual clue should tuncation be necessary. */
static inline const char *make_num (long num, int width, int justr, int col) {
   static char buf[SMLBUFSIZ];

   if (width < snprintf(buf, sizeof(buf), "%ld", num)) {
      buf[width-1] = COLPLUSCH;
      AUTOX_COL(col);
   }
   return justify_pad(buf, width, justr);
} // end: make_num


        /*
         * Make and then justify a character string,
         * and include a visual clue should tuncation be necessary. */
static inline const char *make_str (const char *str, int width, int justr, int col) {
   static char buf[SCREENMAX];

   if (width < snprintf(buf, sizeof(buf), "%s", str)) {
      buf[width-1] = COLPLUSCH;
      AUTOX_COL(col);
   }
   return justify_pad(buf, width, justr);
} // end: make_str


        /*
         * Do some scaling then justify stuff.
         * We'll interpret 'num' as a kibibytes quantity and try to
         * format it to reach 'target' while also fitting 'width'. */
static const char *scale_mem (int target, unsigned long num, int width, int justr) {
#ifndef NOBOOST_MEMS
   //                               SK_Kb   SK_Mb      SK_Gb      SK_Tb      SK_Pb      SK_Eb
   static const char *fmttab[] =  { "%.0f", "%#.1f%c", "%#.3f%c", "%#.3f%c", "%#.3f%c", NULL };
#else
   static const char *fmttab[] =  { "%.0f", "%.0f%c",  "%.0f%c",  "%.0f%c",  "%.0f%c",  NULL };
#endif
   static char buf[SMLBUFSIZ];
   float scaled_num;
   char *psfx;
   int i;

   buf[0] = '\0';
   if (Rc.zero_suppress && 0 >= num)
      goto end_justifies;

   scaled_num = num;
   for (i = SK_Kb, psfx = Scaled_sfxtab; i < SK_Eb; psfx++, i++) {
      if (i >= target
      && (width >= snprintf(buf, sizeof(buf), fmttab[i], scaled_num, *psfx)))
         goto end_justifies;
      scaled_num /= 1024.0;
   }

   // well shoot, this outta' fit...
   snprintf(buf, sizeof(buf), "?");
end_justifies:
   return justify_pad(buf, width, justr);
} // end: scale_mem


        /*
         * Do some scaling then justify stuff. */
static const char *scale_num (unsigned long num, int width, int justr) {
   static char buf[SMLBUFSIZ];
   float scaled_num;
   char *psfx;

   buf[0] = '\0';
   if (Rc.zero_suppress && 0 >= num)
      goto end_justifies;
   if (width >= snprintf(buf, sizeof(buf), "%lu", num))
      goto end_justifies;

   scaled_num = num;
   for (psfx = Scaled_sfxtab; 0 < *psfx; psfx++) {
      scaled_num /= 1024.0;
      if (width >= snprintf(buf, sizeof(buf), "%.1f%c", scaled_num, *psfx))
         goto end_justifies;
      if (width >= snprintf(buf, sizeof(buf), "%.0f%c", scaled_num, *psfx))
         goto end_justifies;
   }

   // well shoot, this outta' fit...
   snprintf(buf, sizeof(buf), "?");
end_justifies:
   return justify_pad(buf, width, justr);
} // end: scale_num


        /*
         * Make and then justify a percentage, with decreasing precision. */
static const char *scale_pcnt (float num, int width, int justr) {
   static char buf[SMLBUFSIZ];

   buf[0] = '\0';
   if (Rc.zero_suppress && 0 >= num)
      goto end_justifies;
#ifndef NOBOOST_PCNT
   if (width >= snprintf(buf, sizeof(buf), "%#.3f", num))
      goto end_justifies;
   if (width >= snprintf(buf, sizeof(buf), "%#.2f", num))
      goto end_justifies;
#endif
   if (width >= snprintf(buf, sizeof(buf), "%#.1f", num))
      goto end_justifies;
   if (width >= snprintf(buf, sizeof(buf), "%*.0f", width, num))
      goto end_justifies;

   // well shoot, this outta' fit...
   snprintf(buf, sizeof(buf), "?");
end_justifies:
   return justify_pad(buf, width, justr);
} // end: scale_pcnt


        /*
         * Do some scaling stuff.
         * Format 'tics' to fit 'width', then justify it. */
static const char *scale_tics (TIC_t tics, int width, int justr) {
#ifdef CASEUP_SUFIX
 #define HH "%uH"                                                  // nls_maybe
 #define DD "%uD"
 #define WW "%uW"
#else
 #define HH "%uh"                                                  // nls_maybe
 #define DD "%ud"
 #define WW "%uw"
#endif
   static char buf[SMLBUFSIZ];
   unsigned long nt;    // narrow time, for speed on 32-bit
   unsigned cc;         // centiseconds
   unsigned nn;         // multi-purpose whatever

   buf[0] = '\0';
   nt  = (tics * 100ull) / Hertz;               // up to 68 weeks of cpu time
   if (Rc.zero_suppress && 0 >= nt)
      goto end_justifies;
   cc  = nt % 100;                              // centiseconds past second
   nt /= 100;                                   // total seconds
   nn  = nt % 60;                               // seconds past the minute
   nt /= 60;                                    // total minutes
   if (width >= snprintf(buf, sizeof(buf), "%lu:%02u.%02u", nt, nn, cc))
      goto end_justifies;
   if (width >= snprintf(buf, sizeof(buf), "%lu:%02u", nt, nn))
      goto end_justifies;
   nn  = nt % 60;                               // minutes past the hour
   nt /= 60;                                    // total hours
   if (width >= snprintf(buf, sizeof(buf), "%lu,%02u", nt, nn))
      goto end_justifies;
   nn = nt;                                     // now also hours
   if (width >= snprintf(buf, sizeof(buf), HH, nn))
      goto end_justifies;
   nn /= 24;                                    // now days
   if (width >= snprintf(buf, sizeof(buf), DD, nn))
      goto end_justifies;
   nn /= 7;                                     // now weeks
   if (width >= snprintf(buf, sizeof(buf), WW, nn))
      goto end_justifies;

   // well shoot, this outta' fit...
   snprintf(buf, sizeof(buf), "?");
end_justifies:
   return justify_pad(buf, width, justr);
 #undef HH
 #undef DD
 #undef WW
} // end: scale_tics

/*######  Fields Management support  #####################################*/

   /* These are the Fieldstab.lflg values used here and in calibrate_fields.
      (own identifiers as documentation and protection against changes) */
#define L_stat     PROC_FILLSTAT
#define L_statm    PROC_FILLMEM
#define L_status   PROC_FILLSTATUS
#define L_CGROUP   PROC_EDITCGRPCVT | PROC_FILLCGROUP
#define L_CMDLINE  PROC_EDITCMDLCVT | PROC_FILLARG
#define L_ENVIRON  PROC_EDITENVRCVT | PROC_FILLENV
#define L_EUSER    PROC_FILLUSR
#define L_OUSER    PROC_FILLSTATUS | PROC_FILLUSR
#define L_EGROUP   PROC_FILLSTATUS | PROC_FILLGRP
#define L_SUPGRP   PROC_FILLSTATUS | PROC_FILLSUPGRP
   // make 'none' non-zero (used to be important to Frames_libflags)
#define L_NONE     PROC_SPARE_1
   // from either 'stat' or 'status' (preferred), via bits not otherwise used
#define L_EITHER   PROC_SPARE_2
   // for calibrate_fields and summary_show 1st pass
#define L_DEFAULT  PROC_FILLSTAT

        /* These are our gosh darn 'Fields' !
           They MUST be kept in sync with pflags !! */
static FLD_t Fieldstab[] = {
   // a temporary macro, soon to be undef'd...
 #define SF(f) (QFP_t)SCB_NAME(f)
   // these identifiers reflect the default column alignment but they really
   // contain the WIN_t flag used to check/change justification at run-time!
 #define A_right Show_JRNUMS       /* toggled with upper case 'J' */
 #define A_left  Show_JRSTRS       /* toggled with lower case 'j' */

/* .width anomalies:
        a -1 width represents variable width columns
        a  0 width represents columns set once at startup (see zap_fieldstab)
   .lflg anomalies:
        P_UED, L_NONE  - natural outgrowth of 'stat()' in readproc        (euid)
        P_CPU, L_stat  - never filled by libproc, but requires times      (pcpu)
        P_CMD, L_stat  - may yet require L_CMDLINE in calibrate_fields    (cmd/cmdline)
        L_EITHER       - must L_status, else L_stat == 64-bit math (__udivdi3) on 32-bit !

     .width  .scale  .align    .sort     .lflg
     ------  ------  --------  --------  --------  */
   {     0,     -1,  A_right,  SF(PID),  L_NONE    },
   {     0,     -1,  A_right,  SF(PPD),  L_EITHER  },
   {     5,     -1,  A_right,  SF(UED),  L_NONE    },
   {     8,     -1,  A_left,   SF(UEN),  L_EUSER   },
   {     5,     -1,  A_right,  SF(URD),  L_status  },
   {     8,     -1,  A_left,   SF(URN),  L_OUSER   },
   {     5,     -1,  A_right,  SF(USD),  L_status  },
   {     8,     -1,  A_left,   SF(USN),  L_OUSER   },
   {     5,     -1,  A_right,  SF(GID),  L_NONE    },
   {     8,     -1,  A_left,   SF(GRP),  L_EGROUP  },
   {     0,     -1,  A_right,  SF(PGD),  L_stat    },
   {     8,     -1,  A_left,   SF(TTY),  L_stat    },
   {     0,     -1,  A_right,  SF(TPG),  L_stat    },
   {     0,     -1,  A_right,  SF(SID),  L_stat    },
   {     3,     -1,  A_right,  SF(PRI),  L_stat    },
   {     3,     -1,  A_right,  SF(NCE),  L_stat    },
   {     3,     -1,  A_right,  SF(THD),  L_EITHER  },
   {     0,     -1,  A_right,  SF(CPN),  L_stat    },
   {     0,     -1,  A_right,  SF(CPU),  L_stat    },
   {     6,     -1,  A_right,  SF(TME),  L_stat    },
   {     9,     -1,  A_right,  SF(TME),  L_stat    }, // P_TM2 slot
#ifndef NOBOOST_PCNT
   {     5,     -1,  A_right,  SF(RES),  L_statm   }, // P_MEM slot
#else
   {     4,     -1,  A_right,  SF(RES),  L_statm   }, // P_MEM slot
#endif
#ifndef NOBOOST_MEMS
   {     7,  SK_Kb,  A_right,  SF(VRT),  L_statm   },
   {     6,  SK_Kb,  A_right,  SF(SWP),  L_status  },
   {     6,  SK_Kb,  A_right,  SF(RES),  L_statm   },
   {     6,  SK_Kb,  A_right,  SF(COD),  L_statm   },
   {     7,  SK_Kb,  A_right,  SF(DAT),  L_statm   },
   {     6,  SK_Kb,  A_right,  SF(SHR),  L_statm   },
#else
   {     5,  SK_Kb,  A_right,  SF(VRT),  L_statm   },
   {     4,  SK_Kb,  A_right,  SF(SWP),  L_status  },
   {     4,  SK_Kb,  A_right,  SF(RES),  L_statm   },
   {     4,  SK_Kb,  A_right,  SF(COD),  L_statm   },
   {     5,  SK_Kb,  A_right,  SF(DAT),  L_statm   },
   {     4,  SK_Kb,  A_right,  SF(SHR),  L_statm   },
#endif
   {     4,     -1,  A_right,  SF(FL1),  L_stat    },
   {     4,     -1,  A_right,  SF(FL2),  L_stat    },
   {     4,     -1,  A_right,  SF(DRT),  L_statm   },
   {     1,     -1,  A_right,  SF(STA),  L_EITHER  },
   {    -1,     -1,  A_left,   SF(CMD),  L_EITHER  },
   {    10,     -1,  A_left,   SF(WCH),  L_stat    },
   {     8,     -1,  A_left,   SF(FLG),  L_stat    },
   {    -1,     -1,  A_left,   SF(CGR),  L_CGROUP  },
   {    -1,     -1,  A_left,   SF(SGD),  L_status  },
   {    -1,     -1,  A_left,   SF(SGN),  L_SUPGRP  },
   {     0,     -1,  A_right,  SF(TGD),  L_status  },
#ifdef OOMEM_ENABLE
#define L_oom      PROC_FILLOOM
   {     3,     -1,  A_right,  SF(OOA),  L_oom     },
   {     8,     -1,  A_right,  SF(OOM),  L_oom     },
#undef L_oom
#endif
   {    -1,     -1,  A_left,   SF(ENV),  L_ENVIRON },
   {     3,     -1,  A_right,  SF(FV1),  L_stat    },
   {     3,     -1,  A_right,  SF(FV2),  L_stat    }
 #undef SF
 #undef A_left
 #undef A_right
};


        /*
         * A calibrate_fields() *Helper* function to refresh the
         * cached screen geometry and related variables */
static void adj_geometry (void) {
   static size_t pseudo_max = 0;
   static int w_set = 0, w_cols = 0, w_rows = 0;
   struct winsize wz;

   Screen_cols = columns;    // <term.h>
   Screen_rows = lines;      // <term.h>

   if (-1 != ioctl(STDOUT_FILENO, TIOCGWINSZ, &wz)
   && 0 < wz.ws_col && 0 < wz.ws_row) {
      Screen_cols = wz.ws_col;
      Screen_rows = wz.ws_row;
   }

#ifndef RMAN_IGNORED
   // be crudely tolerant of crude tty emulators
   if (Cap_avoid_eol) Screen_cols--;
#endif

   // we might disappoint some folks (but they'll deserve it)
   if (SCREENMAX < Screen_cols) Screen_cols = SCREENMAX;

   if (!w_set) {
      if (Width_mode > 0)              // -w with arg, we'll try to honor
         w_cols = Width_mode;
      else
      if (Width_mode < 0) {            // -w without arg, try environment
         char *env_columns = getenv("COLUMNS"),
              *env_lines = getenv("LINES"),
              *ep;
         if (env_columns && *env_columns) {
            long t, tc = 0;
            t = strtol(env_columns, &ep, 0);
            if (!*ep && (t > 0) && (t <= 0x7fffffffL)) tc = t;
            if (0 < tc) w_cols = (int)tc;
         }
         if (env_lines && *env_lines) {
            long t, tr = 0;
            t = strtol(env_lines, &ep, 0);
            if (!*ep && (t > 0) && (t <= 0x7fffffffL)) tr = t;
            if (0 < tr) w_rows = (int)tr;
         }
         if (!w_cols) w_cols = SCREENMAX;
         if (w_cols && w_cols < W_MIN_COL) w_cols = W_MIN_COL;
         if (w_rows && w_rows < W_MIN_ROW) w_rows = W_MIN_ROW;
      }
      w_set = 1;
   }

   /* keep our support for output optimization in sync with current reality
      note: when we're in Batch mode, we don't really need a Pseudo_screen
            and when not Batch, our buffer will contain 1 extra 'line' since
            Msg_row is never represented -- but it's nice to have some space
            between us and the great-beyond... */
   if (Batch) {
      if (w_cols) Screen_cols = w_cols;
      Screen_rows = w_rows ? w_rows : MAXINT;
      Pseudo_size = (sizeof(*Pseudo_screen) * ROWMAXSIZ);
   } else {
      if (w_cols && w_cols < Screen_cols) Screen_cols = w_cols;
      if (w_rows && w_rows < Screen_rows) Screen_rows = w_rows;
      Pseudo_size = (sizeof(*Pseudo_screen) * ROWMAXSIZ) * Screen_rows;
   }
   // we'll only grow our Pseudo_screen, never shrink it
   if (pseudo_max < Pseudo_size) {
      pseudo_max = Pseudo_size;
      Pseudo_screen = alloc_r(Pseudo_screen, pseudo_max);
   }
   // ensure each row is repainted and, if SIGWINCH, clear the screen
   PSU_CLREOS(0);
   if (Frames_resize > 1) putp(Cap_clr_scr);
} // end: adj_geometry


        /*
         * A calibrate_fields() *Helper* function to build the
         * actual column headers and required library flags */
static void build_headers (void) {
   FLG_t f;
   char *s;
   WIN_t *w = Curwin;
#ifdef EQUCOLHDRYES
   int x, hdrmax = 0;
#endif
   int i, needpsdb = 0;

   Frames_libflags = 0;

   do {
      if (VIZISw(w)) {
         memset((s = w->columnhdr), 0, sizeof(w->columnhdr));
         if (Rc.mode_altscr) s = scat(s, fmtmk("%d", w->winnum));
         for (i = 0; i < w->maxpflgs; i++) {
            f = w->procflgs[i];
#ifdef USE_X_COLHDR
            if (CHKw(w, Show_HICOLS) && f == w->rc.sortindx) {
               s = scat(s, fmtmk("%s%s", Caps_off, w->capclr_msg));
               w->hdrcaplen += strlen(Caps_off) + strlen(w->capclr_msg);
            }
#else
            if (P_MAXPFLGS <= f) continue;
#endif
            if (P_WCH == f) needpsdb = 1;
            if (P_CMD == f && CHKw(w, Show_CMDLIN)) Frames_libflags |= L_CMDLINE;
            Frames_libflags |= Fieldstab[w->procflgs[i]].lflg;
            s = scat(s, justify_pad(N_col(f)
               , VARcol(f) ? w->varcolsz : Fieldstab[f].width
               , CHKw(w, Fieldstab[f].align)));
#ifdef USE_X_COLHDR
            if (CHKw(w, Show_HICOLS) && f == w->rc.sortindx) {
               s = scat(s, fmtmk("%s%s", Caps_off, w->capclr_hdr));
               w->hdrcaplen += strlen(Caps_off) + strlen(w->capclr_hdr);
            }
#endif
         }
#ifdef EQUCOLHDRYES
         // prepare to even out column header lengths...
         if (hdrmax + w->hdrcaplen < (x = strlen(w->columnhdr))) hdrmax = x - w->hdrcaplen;
#endif
         // with forest view mode, we'll need tgid, ppid & start_time...
         if (CHKw(w, Show_FOREST)) Frames_libflags |= (L_status | L_stat);
         // for 'busy' only processes, we'll need pcpu (utime & stime)...
         if (!CHKw(w, Show_IDLEPS)) Frames_libflags |= L_stat;
         // we must also accommodate an out of view sort field...
         f = w->rc.sortindx;
         Frames_libflags |= Fieldstab[f].lflg;
         if (P_CMD == f && CHKw(w, Show_CMDLIN)) Frames_libflags |= L_CMDLINE;
      } // end: VIZISw(w)

      if (Rc.mode_altscr) w = w->next;
   } while (w != Curwin);

#ifdef EQUCOLHDRYES
   /* now we can finally even out column header lengths
      (we're assuming entire columnhdr was memset to '\0') */
   if (Rc.mode_altscr && SCREENMAX > Screen_cols)
      for (i = 0; i < GROUPSMAX; i++) {
         w = &Winstk[i];
         if (CHKw(w, Show_TASKON))
            if (hdrmax + w->hdrcaplen > (x = strlen(w->columnhdr)))
               memset(&w->columnhdr[x], ' ', hdrmax + w->hdrcaplen - x);
      }
#endif

   // do we need the kernel symbol table (and is it already open?)
   if (needpsdb) {
      if (-1 == No_ksyms) {
         No_ksyms = 0;
         if (open_psdb_message(NULL, library_err))
            No_ksyms = 1;
         else
            PSDBopen = 1;
      }
   }
   // finalize/touchup the libproc PROC_FILLxxx flags for current config...
   if ((Frames_libflags & L_EITHER) && !(Frames_libflags & L_stat))
      Frames_libflags |= L_status;
   if (!Frames_libflags) Frames_libflags = L_DEFAULT;
   if (Monpidsidx) Frames_libflags |= PROC_PID;
} // end: build_headers


        /*
         * This guy coordinates the activities surrounding the maintenance
         * of each visible window's columns headers and the library flags
         * required for the openproc interface. */
static void calibrate_fields (void) {
   sigset_t newss, oldss;
   FLG_t f;
   char *s;
   const char *h;
   WIN_t *w = Curwin;
   int i, varcolcnt, len;

   // block SIGWINCH signals while we do our thing...
   sigemptyset(&newss);
   sigaddset(&newss, SIGWINCH);
   if (-1 == sigprocmask(SIG_BLOCK, &newss, &oldss))
      error_exit(fmtmk(N_fmt(FAIL_sigstop_fmt), strerror(errno)));

   adj_geometry();

   do {
      if (VIZISw(w)) {
         w->hdrcaplen = 0;   // really only used with USE_X_COLHDR
         // build window's pflgsall array, establish upper bounds for maxpflgs
         for (i = 0, w->totpflgs = 0; i < P_MAXPFLGS; i++) {
            if (FLDviz(w, i)) {
               f = FLDget(w, i);
#ifdef USE_X_COLHDR
               w->pflgsall[w->totpflgs++] = f;
#else
               if (CHKw(w, Show_HICOLS) && f == w->rc.sortindx) {
                  w->pflgsall[w->totpflgs++] = X_XON;
                  w->pflgsall[w->totpflgs++] = f;
                  w->pflgsall[w->totpflgs++] = X_XOF;
               } else
                  w->pflgsall[w->totpflgs++] = f;
#endif
            }
         }

         /* build a preliminary columns header not to exceed screen width
            while accounting for a possible leading window number */
         w->varcolsz = varcolcnt = 0;
         *(s = w->columnhdr) = '\0';
         if (Rc.mode_altscr) s = scat(s, " ");
         for (i = 0; i + w->begpflg < w->totpflgs; i++) {
            f = w->pflgsall[i + w->begpflg];
            w->procflgs[i] = f;
#ifndef USE_X_COLHDR
            if (P_MAXPFLGS <= f) continue;
#endif
            h = N_col(f);
            len = (VARcol(f) ? (int)strlen(h) : Fieldstab[f].width) + COLPADSIZ;
            // oops, won't fit -- we're outta here...
            if (Screen_cols < ((int)(s - w->columnhdr) + len)) break;
            if (VARcol(f)) { ++varcolcnt; w->varcolsz += strlen(h); }
            s = scat(s, fmtmk("%*.*s", len, len, h));
         }
#ifndef USE_X_COLHDR
         if (X_XON == w->procflgs[i - 1]) --i;
#endif

         /* establish the final maxpflgs and prepare to grow the variable column
            heading(s) via varcolsz - it may be a fib if their pflags weren't
            encountered, but that's ok because they won't be displayed anyway */
         w->maxpflgs = i;
         w->varcolsz += Screen_cols - strlen(w->columnhdr);
         if (varcolcnt) w->varcolsz /= varcolcnt;

         /* establish the field where all remaining fields would still
            fit within screen width, including a leading window number */
         *(s = w->columnhdr) = '\0';
         if (Rc.mode_altscr) s = scat(s, " ");
         for (i = w->totpflgs - 1; -1 < i; i--) {
            f = w->pflgsall[i];
#ifndef USE_X_COLHDR
            if (P_MAXPFLGS <= f) { w->endpflg = i; continue; }
#endif
            h = N_col(f);
            len = (VARcol(f) ? (int)strlen(h) : Fieldstab[f].width) + COLPADSIZ;
            if (Screen_cols < ((int)(s - w->columnhdr) + len)) break;
            s = scat(s, fmtmk("%*.*s", len, len, h));
            w->endpflg = i;
         }
#ifndef USE_X_COLHDR
         if (X_XOF == w->pflgsall[w->endpflg]) ++w->endpflg;
#endif
      } // end: if (VIZISw(w))

      if (Rc.mode_altscr) w = w->next;
   } while (w != Curwin);

   build_headers();
   if (CHKw(Curwin, View_SCROLL))
      updt_scroll_msg();

   Frames_resize = 0;
   if (-1 == sigprocmask(SIG_SETMASK, &oldss, NULL))
      error_exit(fmtmk(N_fmt(FAIL_sigmask_fmt), strerror(errno)));
} // end: calibrate_fields


        /*
         * Display each field represented in the current window's fieldscur
         * array along with its description.  Mark with bold and a leading
         * asterisk those fields associated with the "on" or "active" state.
         *
         * Special highlighting will be accorded the "focus" field with such
         * highlighting potentially extended to include the description.
         *
         * Below is the current Fieldstab space requirement and how
         * we apportion it.  The xSUFX is considered sacrificial,
         * something we can reduce or do without.
         *            0        1         2         3
         *            12345678901234567890123456789012
         *            * HEADING = Longest Description!
         *      xPRFX ----------______________________ xSUFX
         *    ( xPRFX has pos 2 & 10 for 'extending' when at minimums )
         *
         * The first 4 screen rows are reserved for explanatory text, the
         * maximum number of columns is currently 6 and a space is needed
         * between columns.  Thus, for example, with 40 fields a tty will
         * still remain useable under these extremes:
         *       rows   cols   displayed
         *       ----   ----   ------------------
         *       24     22     xPRFX only
         *       24     66     full xPRFX + xSUFX
         *       11     66     xPRFX only          ( w/ room for +2 )
         *       11    198     full xPRFX + xSUFX  ( w/ room for +2 )
         *    ( if not, the user deserves our most cryptic messages )
         */
static void display_fields (int focus, int extend) {
 #define mxCOL  6
 #define yRSVD  4
 #define xSUFX  22
 #define xPRFX (10 + xadd)
 #define xTOTL (xPRFX + xSUFX)
   WIN_t *w = Curwin;                  // avoid gcc bloat with a local copy
   int i;                              // utility int (a row, tot cols, ix)
   int smax;                           // printable width of xSUFX
   int xadd = 0;                       // spacing between data columns
   int cmax = Screen_cols;             // total data column width
   int rmax = Screen_rows - yRSVD;     // total useable rows

   fflush(stdout);
   i = (P_MAXPFLGS % mxCOL) ? 1 : 0;
   if (rmax < i + (P_MAXPFLGS / mxCOL)) error_exit("++rows");      // nls_maybe
   i = P_MAXPFLGS / rmax;
   if (P_MAXPFLGS % rmax) ++i;
   if (i > 1) { cmax /= i; xadd = 1; }
   if (cmax > xTOTL) cmax = xTOTL;
   smax = cmax - xPRFX;
   if (smax < 0) error_exit("++cols");                             // nls_maybe

   for (i = 0; i < P_MAXPFLGS; ++i) {
      char sbuf[xSUFX+1];
      int b = FLDviz(w, i);
      FLG_t f = FLDget(w, i);
      const char *e = (i == focus && extend) ? w->capclr_hdr : "";

      // prep sacrificial suffix
      snprintf(sbuf, sizeof(sbuf), "= %s", N_fld(f));

      PUTT("%s%c%s%s %s%-7.7s%s%s%s %-*.*s%s"
         , tg2((i / rmax) * cmax, (i % rmax) + yRSVD)
         , b ? '*' : ' '
         , b ? w->cap_bold : Cap_norm
         , e
         , i == focus ? w->capclr_hdr : ""
         , N_col(f)
         , Cap_norm
         , b ? w->cap_bold : ""
         , e
         , smax, smax
         , sbuf
         , Cap_norm);
   }

   putp(Caps_off);
 #undef mxCOL
 #undef yRSVD
 #undef xSUFX
 #undef xPRFX
 #undef xTOTL
} // end: display_fields


        /*
         * Manage all fields aspects (order/toggle/sort), for all windows. */
static void fields_utility (void) {
#ifndef SCROLLVAR_NO
 #define unSCRL  { w->begpflg = w->varcolbeg = 0; OFFw(w, Show_HICOLS); }
#else
 #define unSCRL  { w->begpflg = 0; OFFw(w, Show_HICOLS); }
#endif
 #define swapEM  { char c; unSCRL; c = w->rc.fieldscur[i]; \
       w->rc.fieldscur[i] = *p; *p = c; p = &w->rc.fieldscur[i]; }
 #define spewFI  { char *t; f = w->rc.sortindx; t = strchr(w->rc.fieldscur, f + FLD_OFFSET); \
       if (!t) t = strchr(w->rc.fieldscur, (f + FLD_OFFSET) | 0x80); \
       i = (t) ? (int)(t - w->rc.fieldscur) : 0; }
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy
   const char *h = NULL;
   char *p = NULL;
   int i, key;
   FLG_t f;

   putp(Cap_clr_scr);
   spewFI

   do {
      if (!h) h = N_col(f);
      display_fields(i, (p != NULL));
      putp(Cap_home);
      show_special(1, fmtmk(N_unq(FIELD_header_fmt)
         , w->grpname, CHKw(w, Show_FOREST) ? N_txt(FOREST_views_txt) : h));

      switch (key = keyin(0)) {
         case kbd_UP:
            if (i > 0) { --i; if (p) swapEM }
            break;
         case kbd_DOWN:
            if (i + 1 < P_MAXPFLGS) { ++i; if (p) swapEM }
            break;
         case kbd_LEFT:
         case kbd_ENTER:
            p = NULL;
            break;
         case kbd_RIGHT:
            p = &w->rc.fieldscur[i];
            break;
         case kbd_HOME:
         case kbd_PGUP:
            if (!p) i = 0;
            break;
         case kbd_END:
         case kbd_PGDN:
            if (!p) i = P_MAXPFLGS - 1;
            break;
         case kbd_SPACE:
         case 'd':
            if (!p) { FLDtog(w, i); unSCRL }
            break;
         case 's':
#ifdef TREE_NORESET
            if (!p && !CHKw(w, Show_FOREST)) { w->rc.sortindx = f = FLDget(w, i); h = NULL; unSCRL }
#else
            if (!p) { w->rc.sortindx = f = FLDget(w, i); h = NULL; unSCRL; OFFw(w, Show_FOREST); }
#endif
            break;
         case 'a':
         case 'w':
            Curwin = w = ('a' == key) ? w->next : w->prev;
            spewFI
            h = p = NULL;
            break;
         default:                 // keep gcc happy
            break;
      }
   } while (key && 'q' != key && kbd_ESC != key);
 #undef unSCRL
 #undef swapEM
 #undef spewFI
} // end: fields_utility


        /*
         * This routine takes care of auto sizing field widths
         * if/when the user sets Rc.fixed_widest to -1.  Along the
         * way he reinitializes some things for the next frame. */
static inline void widths_resize (void) {
   int i;

   // next var may also be set by the guys that actually truncate stuff
   Autox_found = 0;
   for (i = 0; i < P_MAXPFLGS; i++) {
      if (Autox_array[i]) {
         Fieldstab[i].width++;
         Autox_array[i] = 0;
         Autox_found = 1;
      }
   }
   if (Autox_found) calibrate_fields();
} // end: widths_resize


        /*
         * This routine exists just to consolidate most of the messin'
         * around with the Fieldstab array and some related stuff. */
static void zap_fieldstab (void) {
   static int once;
   unsigned digits;
   char buf[8];

   if (!once) {
      Fieldstab[P_PID].width = Fieldstab[P_PPD].width
         = Fieldstab[P_PGD].width = Fieldstab[P_SID].width
         = Fieldstab[P_TGD].width = Fieldstab[P_TPG].width = 5;
      if (5 < (digits = get_pid_digits())) {
         if (10 < digits) error_exit(N_txt(FAIL_widepid_txt));
         Fieldstab[P_PID].width = Fieldstab[P_PPD].width
            = Fieldstab[P_PGD].width = Fieldstab[P_SID].width
            = Fieldstab[P_TGD].width = Fieldstab[P_TPG].width = digits;
      }
      once = 1;
   }

   /*** hotplug_acclimated ***/

   Fieldstab[P_CPN].width = 1;
   if (1 < (digits = (unsigned)snprintf(buf, sizeof(buf), "%u", (unsigned)smp_num_cpus))) {
      if (5 < digits) error_exit(N_txt(FAIL_widecpu_txt));
      Fieldstab[P_CPN].width = digits;
   }

#ifndef NOBOOST_PCNT
   Cpu_pmax = 99.9;
   Fieldstab[P_CPU].width = 5;
   if (Rc.mode_irixps && smp_num_cpus > 1 && !Thread_mode) {
      Cpu_pmax = 100.0 * smp_num_cpus;
      if (smp_num_cpus > 10) {
         if (Cpu_pmax > 99999.0) Cpu_pmax = 99999.0;
      } else {
         if (Cpu_pmax > 999.9) Cpu_pmax = 999.9;
      }
   }
#else
   Cpu_pmax = 99.9;
   Fieldstab[P_CPU].width = 4;
   if (Rc.mode_irixps && smp_num_cpus > 1 && !Thread_mode) {
      Cpu_pmax = 100.0 * smp_num_cpus;
      if (smp_num_cpus > 10) {
         if (Cpu_pmax > 99999.0) Cpu_pmax = 99999.0;
      } else {
         if (Cpu_pmax > 999.9) Cpu_pmax = 999.9;
      }
      Fieldstab[P_CPU].width = 5;
   }
#endif

   /* and accommodate optional wider non-scalable columns (maybe) */
   if (!AUTOX_MODE) {
      Fieldstab[P_UED].width = Fieldstab[P_URD].width
         = Fieldstab[P_USD].width = Fieldstab[P_GID].width
         = Rc.fixed_widest ? 5 + Rc.fixed_widest : 5;
      Fieldstab[P_UEN].width = Fieldstab[P_URN].width
         = Fieldstab[P_USN].width = Fieldstab[P_GRP].width
         = Rc.fixed_widest ? 8 + Rc.fixed_widest : 8;
      Fieldstab[P_TTY].width
         = Rc.fixed_widest ? 8 + Rc.fixed_widest : 8;
      Fieldstab[P_WCH].width
         = Rc.fixed_widest ? 10 + Rc.fixed_widest : 10;
   }

   /* plus user selectable scaling */
   Fieldstab[P_VRT].scale = Fieldstab[P_SWP].scale
      = Fieldstab[P_RES].scale = Fieldstab[P_COD].scale
      = Fieldstab[P_DAT].scale = Fieldstab[P_SHR].scale
      = Rc.task_mscale;

   // lastly, ensure we've got proper column headers...
   calibrate_fields();
} // end: zap_fieldstab

/*######  Library Interface  #############################################*/

        /*
         * This guy's modeled on libproc's 'eight_cpu_numbers' function except
         * we preserve all cpu data in our CPU_t array which is organized
         * as follows:
         *    cpus[0] thru cpus[n] == tics for each separate cpu
         *    cpus[Cpu_faux_tot]   == tics from the 1st /proc/stat line */
static CPU_t *cpus_refresh (CPU_t *cpus) {
   static FILE *fp = NULL;
   static int sav_cpus = -1;
   char buf[MEDBUFSIZ]; // enough for /proc/stat CPU line (not the intr line)
   int i;

   /*** hotplug_acclimated ***/
   if (sav_cpus != Cpu_faux_tot) {
      sav_cpus = Cpu_faux_tot;
      zap_fieldstab();
      if (fp) { fclose(fp); fp = NULL; }
      if (cpus) { free(cpus); cpus = NULL; }
   }

   /* by opening this file once, we'll avoid the hit on minor page faults
      (sorry Linux, but you'll have to close it for us) */
   if (!fp) {
      if (!(fp = fopen("/proc/stat", "r")))
         error_exit(fmtmk(N_fmt(FAIL_statopn_fmt), strerror(errno)));
      /* note: we allocate one more CPU_t than Cpu_faux_tot so the last
               slot can hold tics representing the /proc/stat cpu summary
               (the 1st line) -- that slot supports our View_CPUSUM toggle */
      cpus = alloc_c((1 + Cpu_faux_tot) * sizeof(CPU_t));
   }
   rewind(fp);
   fflush(fp);

   // remember from last time around
   memcpy(&cpus[Cpu_faux_tot].sav, &cpus[Cpu_faux_tot].cur, sizeof(CT_t));
   // then value the last slot with the cpu summary line
   if (!fgets(buf, sizeof(buf), fp)) error_exit(N_txt(FAIL_statget_txt));
   memset(&cpus[Cpu_faux_tot].cur, 0, sizeof(CT_t));
   if (4 > sscanf(buf, "cpu %Lu %Lu %Lu %Lu %Lu %Lu %Lu %Lu"
      , &cpus[Cpu_faux_tot].cur.u, &cpus[Cpu_faux_tot].cur.n, &cpus[Cpu_faux_tot].cur.s
      , &cpus[Cpu_faux_tot].cur.i, &cpus[Cpu_faux_tot].cur.w, &cpus[Cpu_faux_tot].cur.x
      , &cpus[Cpu_faux_tot].cur.y, &cpus[Cpu_faux_tot].cur.z))
         error_exit(N_txt(FAIL_statget_txt));
#ifndef CPU_ZEROTICS
   cpus[Cpu_faux_tot].cur.tot = cpus[Cpu_faux_tot].cur.u + cpus[Cpu_faux_tot].cur.s
      + cpus[Cpu_faux_tot].cur.n + cpus[Cpu_faux_tot].cur.i + cpus[Cpu_faux_tot].cur.w
      + cpus[Cpu_faux_tot].cur.x + cpus[Cpu_faux_tot].cur.y + cpus[Cpu_faux_tot].cur.z;
   /* if a cpu has registered substantially fewer tics than those expected,
      we'll force it to be treated as 'idle' so as not to present misleading
      percentages. */
   cpus[Cpu_faux_tot].edge =
      ((cpus[Cpu_faux_tot].cur.tot - cpus[Cpu_faux_tot].sav.tot) / smp_num_cpus) / (100 / TICS_EDGE);
#endif
   // now value each separate cpu's tics, maybe
   for (i = 0; i < Cpu_faux_tot && i < Screen_rows; i++) {
#ifdef PRETEND4CPUS
      rewind(fp);
      fgets(buf, sizeof(buf), fp);
#endif
      // remember from last time around
      memcpy(&cpus[i].sav, &cpus[i].cur, sizeof(CT_t));
      if (!fgets(buf, sizeof(buf), fp)) error_exit(N_txt(FAIL_statget_txt));
      memset(&cpus[i].cur, 0, sizeof(CT_t));
      if (4 > sscanf(buf, "cpu%d %Lu %Lu %Lu %Lu %Lu %Lu %Lu %Lu", &cpus[i].id
         , &cpus[i].cur.u, &cpus[i].cur.n, &cpus[i].cur.s
         , &cpus[i].cur.i, &cpus[i].cur.w, &cpus[i].cur.x
         , &cpus[i].cur.y, &cpus[i].cur.z)) {
            memmove(&cpus[i], &cpus[Cpu_faux_tot], sizeof(CPU_t));
            break;        // tolerate cpus taken offline
      }
#ifndef CPU_ZEROTICS
      cpus[i].edge = cpus[Cpu_faux_tot].edge;
      // this is for symmetry only, it's not currently required
      cpus[i].cur.tot = cpus[Cpu_faux_tot].cur.tot;
#endif
#ifdef PRETEND4CPUS
      cpus[i].id = i;
#endif
   }
   Cpu_faux_tot = i;      // tolerate cpus taken offline

   return cpus;
} // end: cpus_refresh


#ifdef OFF_HST_HASH
        /*
         * Binary Search for HST_t's put/get support */

static inline HST_t *hstbsrch (HST_t *hst, int max, int pid) {
   int mid, min = 0;

   while (min <= max) {
      mid = (min + max) / 2;
      if (pid < hst[mid].pid) max = mid - 1;
      else if (pid > hst[mid].pid) min = mid + 1;
      else return &hst[mid];
   }
   return NULL;
} // end: hstbsrch

#else
        /*
         * Hashing functions for HST_t's put/get support
         * (not your normal 'chaining', those damn HST_t's might move!) */

#define _HASH_(K) (K & (HHASH_SIZ - 1))

static inline HST_t *hstget (int pid) {
   int V = PHash_sav[_HASH_(pid)];

   while (-1 < V) {
      if (PHist_sav[V].pid == pid) return &PHist_sav[V];
      V = PHist_sav[V].lnk; }
   return NULL;
} // end: hstget


static inline void hstput (unsigned idx) {
   int V = _HASH_(PHist_new[idx].pid);

   PHist_new[idx].lnk = PHash_new[V];
   PHash_new[V] = idx;
} // end: hstput

#undef _HASH_
#endif

        /*
         * Refresh procs *Helper* function to eliminate yet one more need
         * to loop through our darn proc_t table.  He's responsible for:
         *    1) calculating the elapsed time since the previous frame
         *    2) counting the number of tasks in each state (run, sleep, etc)
         *    3) maintaining the HST_t's and priming the proc_t pcpu field
         *    4) establishing the total number tasks for this frame */
static void procs_hlp (proc_t *this) {
#ifdef OFF_HST_HASH
   static unsigned maxt_sav = 0;        // prior frame's max tasks
#endif
   TIC_t tics;
   HST_t *h;

   if (!this) {
      static struct timeval oldtimev;
      struct timeval timev;
      struct timezone timez;
      float et;
      void *v;

      gettimeofday(&timev, &timez);
      et = (timev.tv_sec - oldtimev.tv_sec)
         + (float)(timev.tv_usec - oldtimev.tv_usec) / 1000000.0;
      oldtimev.tv_sec = timev.tv_sec;
      oldtimev.tv_usec = timev.tv_usec;

      // if in Solaris mode, adjust our scaling for all cpus
      Frame_etscale = 100.0f / ((float)Hertz * (float)et * (Rc.mode_irixps ? 1 : smp_num_cpus));
#ifdef OFF_HST_HASH
      maxt_sav = Frame_maxtask;
#endif
      Frame_maxtask = Frame_running = Frame_sleepin = Frame_stopped = Frame_zombied = 0;

      // prep for saving this frame's HST_t's (and reuse mem each time around)
      v = PHist_sav;
      PHist_sav = PHist_new;
      PHist_new = v;
#ifdef OFF_HST_HASH
      // prep for binary search by sorting the last frame's HST_t's
      qsort(PHist_sav, maxt_sav, sizeof(HST_t), (QFP_t)sort_HST_t);
#else
      v = PHash_sav;
      PHash_sav = PHash_new;
      PHash_new = v;
      memcpy(PHash_new, HHash_nul, sizeof(HHash_nul));
#endif
      return;
   }

   switch (this->state) {
      case 'R':
         Frame_running++;
         break;
      case 'S':
      case 'D':
         Frame_sleepin++;
         break;
      case 'T':
         Frame_stopped++;
         break;
      case 'Z':
         Frame_zombied++;
         break;
      default:                    // keep gcc happy
         break;
   }

   if (Frame_maxtask+1 >= HHist_siz) {
      HHist_siz = HHist_siz * 5 / 4 + 100;
      PHist_sav = alloc_r(PHist_sav, sizeof(HST_t) * HHist_siz);
      PHist_new = alloc_r(PHist_new, sizeof(HST_t) * HHist_siz);
   }

   /* calculate time in this process; the sum of user time (utime) and
      system time (stime) -- but PLEASE dont waste time and effort on
      calcs and saves that go unused, like the old top! */
   PHist_new[Frame_maxtask].pid  = this->tid;
   PHist_new[Frame_maxtask].tics = tics = (this->utime + this->stime);
   // finally, save major/minor fault counts in case the deltas are displayable
   PHist_new[Frame_maxtask].maj = this->maj_flt;
   PHist_new[Frame_maxtask].min = this->min_flt;

#ifdef OFF_HST_HASH
   // find matching entry from previous frame and make stuff elapsed
   if ((h = hstbsrch(PHist_sav, maxt_sav - 1, this->tid))) {
      tics -= h->tics;
      this->maj_delta = this->maj_flt - h->maj;
      this->min_delta = this->min_flt - h->min;
   }
#else
   // hash & save for the next frame
   hstput(Frame_maxtask);
   // find matching entry from previous frame and make stuff elapsed
   if ((h = hstget(this->tid))) {
      tics -= h->tics;
      this->maj_delta = this->maj_flt - h->maj;
      this->min_delta = this->min_flt - h->min;
   }
#endif

   /* we're just saving elapsed tics, to be converted into %cpu if
      this task wins it's displayable screen row lottery... */
   this->pcpu = tics;

   // shout this to the world with the final call (or us the next time in)
   Frame_maxtask++;
} // end: procs_hlp


        /*
         * This guy's modeled on libproc's 'readproctab' function except
         * we reuse and extend any prior proc_t's.  He's been customized
         * for our specific needs and to avoid the use of <stdarg.h> */
static void procs_refresh (void) {
 #define n_used  Frame_maxtask                   // maintained by procs_hlp()
   static proc_t **private_ppt;                  // our base proc_t ptr table
   static int n_alloc = 0;                       // size of our private_ppt
   static int n_saved = 0;                       // last window ppt size
   proc_t *ptask;
   PROCTAB* PT;
   int i;
   proc_t*(*read_something)(PROCTAB*, proc_t*);

   procs_hlp(NULL);                              // prep for a new frame
   if (NULL == (PT = openproc(Frames_libflags, Monpids)))
      error_exit(fmtmk(N_fmt(FAIL_openlib_fmt), strerror(errno)));
   read_something = Thread_mode ? readeither : readproc;

   for (;;) {
      if (n_used == n_alloc) {
         n_alloc = 10 + ((n_alloc * 5) / 4);     // grow by over 25%
         private_ppt = alloc_r(private_ppt, sizeof(proc_t*) * n_alloc);
         // ensure NULL pointers for the additional memory just acquired
         memset(private_ppt + n_used, 0, sizeof(proc_t*) * (n_alloc - n_used));
      }
      // on the way to n_alloc, the library will allocate the underlying
      // proc_t storage whenever our private_ppt[] pointer is NULL...
      if (!(ptask = read_something(PT, private_ppt[n_used]))) break;
      procs_hlp((private_ppt[n_used] = ptask));  // tally this proc_t
   }

   closeproc(PT);

   // lastly, refresh each window's proc pointers table...
   if (n_saved == n_alloc)
      for (i = 0; i < GROUPSMAX; i++)
         memcpy(Winstk[i].ppt, private_ppt, sizeof(proc_t*) * n_used);
   else {
      n_saved = n_alloc;
      for (i = 0; i < GROUPSMAX; i++) {
         Winstk[i].ppt = alloc_r(Winstk[i].ppt, sizeof(proc_t*) * n_alloc);
         memcpy(Winstk[i].ppt, private_ppt, sizeof(proc_t*) * n_used);
      }
   }
 #undef n_used
} // end: procs_refresh


        /*
         * This serves as our interface to the memory & cpu count (sysinfo)
         * portion of libproc.  In support of those hotpluggable resources,
         * the sampling frequencies are reduced so as to minimize overhead.
         * We'll strive to verify the number of cpus every 5 minutes and the
         * memory availability/usage every 3 seconds. */
static void sysinfo_refresh (int forced) {
   static time_t mem_secs, cpu_secs;
   time_t cur_secs;

   if (forced)
      mem_secs = cpu_secs = 0;
   time(&cur_secs);

   /*** hotplug_acclimated ***/
   if (3 <= cur_secs - mem_secs) {
      meminfo();
      mem_secs = cur_secs;
   }
#ifndef PRETEND4CPUS
   /*** hotplug_acclimated ***/
   if (300 <= cur_secs - cpu_secs) {
      cpuinfo();
      Cpu_faux_tot = smp_num_cpus;
      cpu_secs = cur_secs;
   }
#endif
} // end: sysinfo_refresh

/*######  Inspect Other Output  ##########################################*/

        /*
         * HOWTO Extend the top 'inspect' functionality:
         *
         * To exploit the 'Y' interactive command, one must add entries to
         * the top personal configuration file.  Such entries simply reflect
         * a file to be read or command/pipeline to be executed whose results
         * will then be displayed in a separate scrollable window.
         *
         * Entries beginning with a '#' character are ignored, regardless of
         * content.  Otherwise they consist of the following 3 elements, each
         * of which must be separated by a tab character (thus 2 '\t' total):
         *     type:  literal 'file' or 'pipe'
         *     name:  selection shown on the Inspect screen
         *     fmts:  string representing a path or command
         *
         * The two types of Inspect entries are not interchangeable.
         * Those designated 'file' will be accessed using fopen/fread and must
         * reference a single file in the 'fmts' element.  Entries specifying
         * 'pipe' will employ popen/fread, their 'fmts' element could contain
         * many pipelined commands and, none can be interactive.
         *
         * Here are some examples of both types of inspection entries.
         * The first entry will be ignored due to the initial '#' character.
         * For clarity, the pseudo tab depictions (^I) are surrounded by an
         * extra space but the actual tabs would not be.
         *
         *     # pipe ^I Sockets ^I lsof -n -P -i 2>&1
         *     pipe ^I Open Files ^I lsof -P -p %d 2>&1
         *     file ^I NUMA Info ^I /proc/%d/numa_maps
         *     pipe ^I Log ^I tail -n100 /var/log/syslog | sort -Mr
         *
         * Caution:  If the output contains unprintable characters they will
         * be displayed in either the ^I notation or hexidecimal <FF> form.
         * This applies to tab characters as well.  So if one wants a more
         * accurate display, any tabs should be expanded within the 'fmts'.
         *
         * The following example takes what could have been a 'file' entry
         * but employs a 'pipe' instead so as to expand the tabs.
         *
         *     # next would have contained '\t' ...
         *     # file ^I <your_name> ^I /proc/%d/status
         *     # but this will eliminate embedded '\t' ...
         *     pipe ^I <your_name> ^I cat /proc/%d/status | expand -
         */

        /*
         * Our driving table support, the basis for generalized inspection,
         * built at startup (if at all) from rcfile or demo entries. */
struct I_ent {
   void (*func)(char *, int);     // a pointer to file/pipe/demo function
   char *type;                    // the type of entry ('file' or 'pipe')
   char *name;                    // the selection label for display
   char *fmts;                    // format string to build path or command
   int   farg;                    // 1 = '%d' in fmts, 0 = not (future use)
   const char *caps;              // not really caps, show_special() delim's
   char *fstr;                    // entry's current/active search string
   int   flen;                    // above's strlen, without call overhead
};
struct I_struc {
   int demo;                      // do NOT save table entries in rcfile
   int total;                     // total I_ent table entries
   char *raw;                     // all entries for 'W', incl '#' & blank
   struct I_ent *tab;
};
static struct I_struc Inspect;

static char   **Insp_p;           // pointers to each line start
static int      Insp_nl;          // total lines, total Insp_p entries
static char    *Insp_buf;         // the results from insp_do_file/pipe
static size_t   Insp_bufsz;       // allocated size of Insp_buf
static size_t   Insp_bufrd;       // bytes actually in Insp_buf
static struct I_ent *Insp_sel;    // currently selected Inspect entry

        // Our 'make status line' macro
#define INSP_MKSL(big,txt) { int _sz = big ? Screen_cols : 80; \
   putp(tg2(0, (Msg_row = 3))); \
   PUTT("%s%.*s", Curwin->capclr_hdr, Screen_cols -1 \
      , fmtmk("%-*.*s%s", _sz, _sz, txt, Cap_clr_eol)); \
   putp(Caps_off); }

        // Our 'row length' macro, equivalent to a strlen() call
#define INSP_RLEN(idx) (int)(Insp_p[idx +1] - Insp_p[idx] -1)

        // Our 'busy' (wait please) macro
#define INSP_BUSY  { INSP_MKSL(0, N_txt(YINSP_workin_txt)); \
   fflush(stdout); }


        /*
         * Establish the number of lines present in the Insp_buf glob plus
         * build the all important row start array.  It is that array that
         * others will rely on since we dare not try to use strlen() on what
         * is potentially raw binary data.  Who knows what some user might
         * name as a file or include in a pipeline (scary, ain't it?). */
static void insp_cnt_nl (void) {
   char *beg = Insp_buf;
   char *cur = Insp_buf;
   char *end = Insp_buf + Insp_bufrd + 1;

#ifdef INSP_SAVEBUF
{
   static int n = 1;
   char fn[SMLBUFSIZ];
   FILE *fd;
   snprintf(fn, sizeof(fn), "%s.Insp_buf.%02d.txt", Myname, n++);
   fd = fopen(fn, "w");
   if (fd) {
      fwrite(Insp_buf, 1, Insp_bufrd, fd);
      fclose(fd);
   }
}
#endif
   Insp_p = alloc_c(sizeof(char*) * 2);

   for (Insp_nl = 0; beg < end; beg++) {
      if (*beg == '\n') {
         Insp_p[Insp_nl++] = cur;
         // keep our array ahead of next potential need (plus the 2 above)
         Insp_p = alloc_r(Insp_p, (sizeof(char*) * (Insp_nl +3)));
         cur = beg +1;
      }
   }
   Insp_p[0] = Insp_buf;
   Insp_p[Insp_nl++] = cur;
   Insp_p[Insp_nl] = end;
   if ((end - cur) == 1)          // if there's a eof null delimiter,
      --Insp_nl;                  // don't count it as a new line
} // end: insp_cnt_nl


#ifndef INSP_OFFDEMO
        /*
         * The pseudo output DEMO utility. */
static void insp_do_demo (char *fmts, int pid) {
   (void)fmts; (void)pid;
   /* next will put us on a par with the real file/pipe read buffers
    ( and also avoid a harmless, but evil sounding, valgrind warning ) */
   Insp_bufsz = READMINSZ + strlen(N_txt(YINSP_dstory_txt));
   Insp_buf   = alloc_c(Insp_bufsz);
   Insp_bufrd = snprintf(Insp_buf, Insp_bufsz, "%s", N_txt(YINSP_dstory_txt));
   insp_cnt_nl();
} // end: insp_do_demo
#endif


        /*
         * The generalized FILE utility. */
static void insp_do_file (char *fmts, int pid) {
   char buf[LRGBUFSIZ];
   FILE *fp;
   int rc;

   snprintf(buf, sizeof(buf), fmts, pid);
   fp = fopen(buf, "r");
   rc = readfile(fp, &Insp_buf, &Insp_bufsz, &Insp_bufrd);
   if (fp) fclose(fp);
   if (rc) Insp_bufrd = snprintf(Insp_buf, Insp_bufsz, "%s"
      , fmtmk(N_fmt(YINSP_failed_fmt), strerror(errno)));
   insp_cnt_nl();
} // end: insp_do_file


        /*
         * The generalized PIPE utility. */
static void insp_do_pipe (char *fmts, int pid) {
   char buf[LRGBUFSIZ];
   FILE *fp;
   int rc;

   snprintf(buf, sizeof(buf), fmts, pid);
   fp = popen(buf, "r");
   rc = readfile(fp, &Insp_buf, &Insp_bufsz, &Insp_bufrd);
   if (fp) pclose(fp);
   if (rc) Insp_bufrd = snprintf(Insp_buf, Insp_bufsz, "%s"
      , fmtmk(N_fmt(YINSP_failed_fmt), strerror(errno)));
   insp_cnt_nl();
} // end: insp_do_pipe


        /*
         * This guy is a *Helper* function serving the following two masters:
         *   insp_find_str() - find the next Insp_sel->fstr match
         *   insp_make_row() - highlight any Insp_sel->fstr matches in-view
         * If Insp_sel->fstr is found in the designated row, he returns the
         * offset from the start of the row, otherwise he returns a huge
         * integer so traditional fencepost usage can be employed. */
static inline int insp_find_ofs (int col, int row) {
 #define begFS (int)(fnd - Insp_p[row])
   char *p, *fnd = NULL;

   if (Insp_sel->fstr[0]) {
      // skip this row, if there's no chance of a match
      if (memchr(Insp_p[row], Insp_sel->fstr[0], INSP_RLEN(row))) {
         for ( ; col < INSP_RLEN(row); col++) {
            if (!*(p = Insp_p[row] + col))       // skip any empty strings
               continue;
            fnd = STRSTR(p, Insp_sel->fstr);     // with binary data, each
            if (fnd)                             // row may have '\0'.  so
               break;                            // our scans must be done
            col += strlen(p);                    // as individual strings.
         }
         if (fnd && fnd < Insp_p[row + 1])       // and, we must watch out
            return begFS;                        // for potential overrun!
      }
   }
   return INT_MAX;
 #undef begFS
} // end: insp_find_ofs


        /*
         * This guy supports the inspect 'L' and '&' search provisions
         * and returns the row and *optimal* column for viewing any match
         * ( we'll always opt for left column justification since any )
         * ( preceding ctrl chars appropriate an unpredictable amount ) */
static void insp_find_str (int ch, int *col, int *row) {
 #define reDUX (found) ? N_txt(WORD_another_txt) : ""
   static int found;

   if ((ch == '&' || ch == 'n') && !Insp_sel->fstr[0]) {
      show_msg(N_txt(FIND_no_next_txt));
      return;
   }
   if (ch == 'L' || ch == '/') {
      snprintf(Insp_sel->fstr, FNDBUFSIZ, "%s", linein(N_txt(GET_find_str_txt)));
      Insp_sel->flen = strlen(Insp_sel->fstr);
      found = 0;
   }
   if (Insp_sel->fstr[0]) {
      int xx, yy;

      INSP_BUSY;
      for (xx = *col, yy = *row; yy < Insp_nl; ) {
         xx = insp_find_ofs(xx, yy);
         if (xx < INSP_RLEN(yy)) {
            found = 1;
            if (xx == *col &&  yy == *row) {     // matched where we were!
               ++xx;                             // ( was the user maybe )
               continue;                         // ( trying to fool us? )
            }
            *col = xx;
            *row = yy;
            return;
         }
         xx = 0;
         ++yy;
      }
      show_msg(fmtmk(N_fmt(FIND_no_find_fmt), reDUX, Insp_sel->fstr));
   }
 #undef reDUX
} // end: insp_find_str


        /*
         * This guy is a *Helper* function responsible for positioning a
         * single row in the current 'X axis', then displaying the results.
         * Along the way, he makes sure control characters and/or unprintable
         * characters display in a less-like fashion:
         *    '^A'    for control chars
         *    '<BC>'  for other unprintable stuff
         * Those will be highlighted with the current windows's capclr_msg,
         * while visible search matches display with capclr_hdr for emphasis.
         * ( we hide ugly plumbing in macros to concentrate on the algorithm ) */
static inline void insp_make_row (int col, int row) {
 #define maxSZ ( Screen_cols - (to + 1) )
 #define capNO { if (hicap) { putp(Caps_off); hicap = 0; } }
 #define mkFND { PUTT("%s%.*s%s", Curwin->capclr_hdr, maxSZ, Insp_sel->fstr, Caps_off); \
    fr += Insp_sel->flen -1; to += Insp_sel->flen; hicap = 0; }
#ifndef INSP_JUSTNOT
 #define mkCTL { int x = maxSZ; const char *p = fmtmk("^%c", uch + '@'); \
    PUTT("%s%.*s", (!hicap) ? Curwin->capclr_msg : "", x, p); to += 2; hicap = 1; }
 #define mkUNP { int x = maxSZ; const char *p = fmtmk("<%02X>", uch); \
    PUTT("%s%.*s", (!hicap) ? Curwin->capclr_msg : "", x, p); to += 4; hicap = 1; }
#else
 #define mkCTL { if ((to += 2) <= Screen_cols) \
    PUTT("%s^%c", (!hicap) ? Curwin->capclr_msg : "", uch + '@'); hicap = 1; }
 #define mkUNP { if ((to += 4) <= Screen_cols) \
    PUTT("%s<%02X>", (!hicap) ? Curwin->capclr_msg : "", uch); hicap = 1; }
#endif
 #define mkSTD { capNO; if (++to <= Screen_cols) { static char _str[2]; \
    _str[0] = uch; putp(_str); } }
   char tline[SCREENMAX];
   int fr, to, ofs;
   int hicap = 0;

   capNO;
   if (col < INSP_RLEN(row))
      memcpy(tline, Insp_p[row] + col, sizeof(tline));
   else tline[0] = '\n';

   for (fr = 0, to = 0, ofs = 0; to < Screen_cols -1; fr++) {
      if (!ofs)
         ofs = insp_find_ofs(col + fr, row);
      if (col + fr < ofs) {
         unsigned char uch = tline[fr];
         if (uch == '\n')   break;     // a no show  (he,he)
         if (uch > 126)     mkUNP      // show as: '<AB>'
         else if (uch < 32) mkCTL      // show as:  '^C'
         else               mkSTD      // a show off (he,he)
      } else {              mkFND      // a big show (he,he)
         ofs = 0;
      }
   }
   capNO;
   putp(Cap_clr_eol);

 #undef maxSZ
 #undef capNO
 #undef mkFND
 #undef mkCTL
 #undef mkUNP
 #undef mkSTD
} // end: insp_make_row


        /*
         * This guy is an insp_view_choice() *Helper* function who displays
         * a page worth of of the user's damages.  He also creates a status
         * line based on maximum digits for the current selection's lines and
         * hozizontal position (so it serves to inform, not distract, by
         * otherwise being jumpy). */
static inline void insp_show_pgs (int col, int row, int max) {
   char buf[SMLBUFSIZ];
   int r = snprintf(buf, sizeof(buf), "%d", Insp_nl);
   int c = snprintf(buf, sizeof(buf), "%d", col +Screen_cols);
   int l = row +1, ls = Insp_nl;;

   if (!Insp_bufrd)
      l = ls = 0;
   snprintf(buf, sizeof(buf), N_fmt(YINSP_status_fmt)
      , Insp_sel->name
      , r, l, r, ls
      , c, col + 1, c, col + Screen_cols
      , (unsigned long)Insp_bufrd);
   INSP_MKSL(0, buf);

   for ( ; max && row < Insp_nl; row++) {
      putp("\n");
      insp_make_row(col, row);
      --max;
   }
   if (max)
      putp(Cap_nl_clreos);
} // end: insp_show_pgs


        /*
         * This guy is responsible for displaying the Insp_buf contents and
         * managing all scrolling/locate requests until the user gives up. */
static int insp_view_choice (proc_t *obj) {
#ifdef INSP_SLIDE_1
 #define hzAMT  1
#else
 #define hzAMT  8
#endif
 #define maxLN (Screen_rows - (Msg_row +1))
 #define makHD(b1,b2,b3) { \
    snprintf(b1, sizeof(b1), "%s", make_num(obj->tid,   5, 1, AUTOX_NO)); \
    snprintf(b2, sizeof(b2), "%s", make_str(obj->cmd,   8, 1, AUTOX_NO)); \
    snprintf(b3, sizeof(b3), "%s", make_str(obj->euser, 8, 1, AUTOX_NO)); }
 #define makFS(dst) { if (Insp_sel->flen < 22) \
       snprintf(dst, sizeof(dst), "%s", Insp_sel->fstr); \
    else snprintf(dst, sizeof(dst), "%.19s...", Insp_sel->fstr); }
   char buf[SMLBUFSIZ];
   int key, curlin, curcol;

   for (curlin = curcol = 0;;) {
      char pid[6], cmd[9], usr[9];

      if (curcol < 0) curcol = 0;
      if (curlin >= Insp_nl) curlin = Insp_nl -1;
      if (curlin < 0) curlin = 0;

      putp(Cap_home);
      putp(Cap_curs_hide);
      makFS(buf)
      makHD(pid,cmd,usr)
      show_special(1, fmtmk(N_unq(INSP_hdrview_fmt)
         , pid, cmd, usr, (Insp_sel->fstr[0]) ? buf : " N/A "));   // nls_maybe
      insp_show_pgs(curcol, curlin, maxLN);
      /* fflush(stdin) didn't do the trick, so we'll just dip a little deeper
         lest repeated <Enter> keys produce immediate re-selection in caller */
      tcflush(STDIN_FILENO, TCIFLUSH);

      switch (key = keyin(0)) {
         case kbd_ENTER:          // must force new keyin()
            key = -1;             // fall through !
         case kbd_ESC: case 'q': case 0:
            putp(Cap_clr_scr);
            return key;
         case kbd_LEFT:
            curcol -= hzAMT;
            break;
         case kbd_RIGHT:
            curcol += hzAMT;
            break;
         case kbd_UP:
            --curlin;
            break;
         case kbd_DOWN:
            ++curlin;
            break;
         case kbd_PGUP: case 'b':
            curlin -= maxLN -1;   // keep 1 line for reference
            break;
         case kbd_PGDN: case kbd_SPACE:
            curlin += maxLN -1;   // ditto
            break;
         case kbd_HOME: case 'g':
            curcol = curlin = 0;
            break;
         case kbd_END: case 'G':
            curcol = 0;
            curlin = Insp_nl - maxLN;
            break;
         case 'L': case '&': case '/': case 'n':
            putp(Cap_curs_norm);
            insp_find_str(key, &curcol, &curlin);
            break;
         case '=':
            snprintf(buf, sizeof(buf), "%s: %s", Insp_sel->type, Insp_sel->fmts);
            INSP_MKSL(1, buf);    // show an extended SL
            key = keyin(0);
            if (!key) return key; // oops, we got signaled
            break;
         default:                 // keep gcc happy
            break;
      }
   }
 #undef hzAMT
 #undef maxLN
 #undef makHD
 #undef makFS
} // end: insp_view_choice


        /*
         * This is the main Inspect routine, responsible for:
         *   1) validating the passed pid (required, but not always used)
         *   2) presenting/establishing the target selection
         *   3) arranging to fill Insp_buf (via the Inspect.tab[?].func)
         *   4) invoking insp_view_choice for viewing/scrolling/searching
         *   5) cleaning up the dynamically acquired memory afterwards */
static void inspection_utility (int pid) {
 #define mkSEL(dst) { for (i = 0; i < Inspect.total; i++) Inspect.tab[i].caps = "~1"; \
      Inspect.tab[sel].caps = "~4"; dst[0] = '\0'; \
      for (i = 0; i < Inspect.total; i++) { char _s[SMLBUFSIZ]; \
         snprintf(_s, sizeof(_s), " %s %s", Inspect.tab[i].name, Inspect.tab[i].caps); \
         strcat(dst, _s); } }
   char sels[MEDBUFSIZ];
   static int sel;
   int i, key;
   proc_t *p;

   for (i = 0, p = NULL; i < Frame_maxtask; i++)
      if (pid == Curwin->ppt[i]->tid) {
         p = Curwin->ppt[i];
         break;
      }
   if (!p) {
      show_msg(fmtmk(N_fmt(YINSP_pidbad_fmt), pid));
      return;
   }
   putp(Cap_clr_scr);
   key = -1;

   do {
      mkSEL(sels);
      putp(Cap_home);
      putp(Cap_curs_hide);
      show_special(1, fmtmk(N_unq(INSP_hdrsels_fmt)
         , pid, p->cmd, p->euser, sels));
      INSP_MKSL(0, " ");

      if (-1 == key) key = keyin(0);
      switch (key) {
         case 0:
         case 'q':
         case kbd_ESC:
            break;
         case kbd_END:
            sel = 0;              // fall through !
         case kbd_LEFT:
            if (--sel < 0) sel = Inspect.total -1;
            key = -1;
            break;
         case kbd_HOME:
            sel = Inspect.total;  // fall through !
         case kbd_RIGHT:
            if (++sel >= Inspect.total) sel = 0;
            key = -1;
            break;
         case kbd_ENTER:
            INSP_BUSY;
            Insp_sel = &Inspect.tab[sel];
            Inspect.tab[sel].func(Inspect.tab[sel].fmts, pid);
            key = insp_view_choice(p);
            free(Insp_buf);
            free(Insp_p);
            break;
         default:                 // keep gcc happy
            key = -1;
            break;
      }
   } while (key && 'q' != key && kbd_ESC != key);

 #undef mkSEL
} // end: inspection_utility
#undef INSP_MKSL
#undef INSP_RLEN
#undef INSP_BUSY

/*######  Startup routines  ##############################################*/

        /*
         * No matter what *they* say, we handle the really really BIG and
         * IMPORTANT stuff upon which all those lessor functions depend! */
static void before (char *me) {
   struct sigaction sa;
   proc_t p;
   int i;

   atexit(close_stdout);

   // is /proc mounted?
   look_up_our_self(&p);

   // setup our program name
   Myname = strrchr(me, '/');
   if (Myname) ++Myname; else Myname = me;

   // accommodate nls/gettext potential translations
   initialize_nls();

   // establish cpu particulars
#ifdef PRETEND4CPUS
   smp_num_cpus = 4;
#endif
   Cpu_faux_tot = smp_num_cpus;
   Cpu_States_fmts = N_unq(STATE_lin2x4_fmt);
   if (linux_version_code > LINUX_VERSION(2, 5, 41))
      Cpu_States_fmts = N_unq(STATE_lin2x5_fmt);
   if (linux_version_code >= LINUX_VERSION(2, 6, 0))
      Cpu_States_fmts = N_unq(STATE_lin2x6_fmt);
   if (linux_version_code >= LINUX_VERSION(2, 6, 11))
      Cpu_States_fmts = N_unq(STATE_lin2x7_fmt);

   // get virtual page stuff
   Page_size = getpagesize();
   i = Page_size;
   while(i > 1024) { i >>= 1; Pg2K_shft++; }

#ifndef OFF_HST_HASH
   // prep for HST_t's put/get hashing optimizations
   for (i = 0; i < HHASH_SIZ; i++) HHash_nul[i] = -1;
   memcpy(HHash_one, HHash_nul, sizeof(HHash_nul));
   memcpy(HHash_two, HHash_nul, sizeof(HHash_nul));
#endif

#ifndef SIGRTMAX       // not available on hurd, maybe others too
#define SIGRTMAX 32
#endif
   // lastly, establish a robust signals environment
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_RESTART;
   for (i = SIGRTMAX; i; i--) {
      switch (i) {
         case SIGALRM: case SIGHUP:  case SIGINT:
         case SIGPIPE: case SIGQUIT: case SIGTERM:
         case SIGUSR1: case SIGUSR2:
            sa.sa_handler = sig_endpgm;
            break;
         case SIGTSTP: case SIGTTIN: case SIGTTOU:
            sa.sa_handler = sig_paused;
            break;
         case SIGCONT: case SIGWINCH:
            sa.sa_handler = sig_resize;
            break;
         default:
            sa.sa_handler = sig_abexit;
            break;
         case SIGCHLD: // we can't catch this
            continue;  // when opening a pipe
      }
      sigaction(i, &sa, NULL);
   }
} // end: before


        /*
         * A configs_read *Helper* function responsible for converting
         * a single window's old rc stuff into a new style rcfile entry */
static int config_cvt (WIN_t *q) {
   static struct {
      int old, new;
   } flags_tab[] = {
    #define old_View_NOBOLD  0x000001
    #define old_VISIBLE_tsk  0x000008
    #define old_Qsrt_NORMAL  0x000010
    #define old_Show_HICOLS  0x000200
    #define old_Show_THREAD  0x010000
      { old_View_NOBOLD, View_NOBOLD },
      { old_VISIBLE_tsk, Show_TASKON },
      { old_Qsrt_NORMAL, Qsrt_NORMAL },
      { old_Show_HICOLS, Show_HICOLS },
      { old_Show_THREAD, 0           }
    #undef old_View_NOBOLD
    #undef old_VISIBLE_tsk
    #undef old_Qsrt_NORMAL
    #undef old_Show_HICOLS
    #undef old_Show_THREAD
   };
   static const char fields_src[] = CVT_FIELDS;
#ifdef OOMEM_ENABLE
   char fields_dst[PFLAGSSIZ], *p1, *p2;
#else
   char fields_dst[PFLAGSSIZ];
#endif
   int i, j, x;

   // first we'll touch up this window's winflags...
   x = q->rc.winflags;
   q->rc.winflags = 0;
   for (i = 0; i < MAXTBL(flags_tab); i++) {
      if (x & flags_tab[i].old) {
         x &= ~flags_tab[i].old;
         q->rc.winflags |= flags_tab[i].new;
      }
   }
   q->rc.winflags |= x;
   SETw(q, Show_JRNUMS);

   // now let's convert old top's more limited fields...
   j = strlen(q->rc.fieldscur);
   if (j > CVT_FLDMAX)
      return 1;
   strcpy(fields_dst, fields_src);
#ifdef OOMEM_ENABLE
   /* all other fields represent the 'on' state with a capitalized version
      of a particular qwerty key.  for the 2 additional suse out-of-memory
      fields it makes perfect sense to do the exact opposite, doesn't it?
      in any case, we must turn them 'off' temporarily... */
   if ((p1 = strchr(q->rc.fieldscur, '[')))  *p1 = '{';
   if ((p2 = strchr(q->rc.fieldscur, '\\'))) *p2 = '|';
#endif
   for (i = 0; i < j; i++) {
      int c = q->rc.fieldscur[i];
      x = tolower(c) - 'a';
      if (x < 0 || x >= CVT_FLDMAX)
         return 1;
      fields_dst[i] = fields_src[x];
      if (isupper(c))
         FLDon(fields_dst[i]);
   }
#ifdef OOMEM_ENABLE
   // if we turned any suse only fields off, turn 'em back on OUR way...
   if (p1) FLDon(fields_dst[p1 - q->rc.fieldscur]);
   if (p2) FLDon(fields_dst[p2 - q->rc.fieldscur]);
#endif
   strcpy(q->rc.fieldscur, fields_dst);

   // lastly, we must adjust the old sort field enum...
   x = q->rc.sortindx;
   q->rc.sortindx = fields_src[x] - FLD_OFFSET;

   Rc_questions = 1;
   return 0;
} // end: config_cvt


        /*
         * Build the local RC file name then try to read both of 'em.
         * 'SYS_RCFILESPEC' contains two lines consisting of the secure
         *   mode switch and an update interval.  It's presence limits what
         *   ordinary users are allowed to do.
         * 'Rc_name' contains multiple lines - 3 global + 3 per window.
         *   line 1  : an eyecatcher and creating program/alias name
         *   line 2  : an id, Mode_altcsr, Mode_irixps, Delay_time, Curwin.
         *   For each of the 4 windows:
         *     line a: contains w->winname, fieldscur
         *     line b: contains w->winflags, sortindx, maxtasks
         *     line c: contains w->summclr, msgsclr, headclr, taskclr
         *   line 15 : Fixed_widest, Summ_mscale, Task_mscale, Zero_suppress */
static void configs_read (void) {
   float tmp_delay = DEF_DELAY;
   char fbuf[LRGBUFSIZ];
   const char *p;
   FILE *fp;
   int i;

   p = getenv("HOME");
   snprintf(Rc_name, sizeof(Rc_name), "%s/.%src", (p && *p) ? p : ".", Myname);

   fp = fopen(SYS_RCFILESPEC, "r");
   if (fp) {
      fbuf[0] = '\0';
      fgets(fbuf, sizeof(fbuf), fp);             // sys rc file, line 1
      if (strchr(fbuf, 's')) Secure_mode = 1;
      fbuf[0] = '\0';
      fgets(fbuf, sizeof(fbuf), fp);             // sys rc file, line 2
      sscanf(fbuf, "%f", &Rc.delay_time);
      fclose(fp);
   }

   fp = fopen(Rc_name, "r");
   if (fp) {
      int tmp_whole, tmp_fract;
      fbuf[0] = '\0';
      fgets(fbuf, sizeof(fbuf), fp);             // ignore eyecatcher
      if (6 != fscanf(fp
         , "Id:%c, Mode_altscr=%d, Mode_irixps=%d, Delay_time=%d.%d, Curwin=%d\n"
         , &Rc.id, &Rc.mode_altscr, &Rc.mode_irixps, &tmp_whole, &tmp_fract, &i)) {
            p = fmtmk(N_fmt(RC_bad_files_fmt), Rc_name);
            Rc_questions = -1;
            goto try_inspect_entries;            // maybe a faulty 'inspect' echo
      }
      // you saw that, right?  (fscanf stickin' it to 'i')
      Curwin = &Winstk[i];
      // this may be ugly, but it keeps us locale independent...
      tmp_delay = (float)tmp_whole + (float)tmp_fract / 1000;

      for (i = 0 ; i < GROUPSMAX; i++) {
         int x;
         WIN_t *w = &Winstk[i];
         p = fmtmk(N_fmt(RC_bad_entry_fmt), i+1, Rc_name);

         // note: "fieldscur=%__s" on next line should equal PFLAGSSIZ !
         if (2 != fscanf(fp, "%3s\tfieldscur=%64s\n"
            , w->rc.winname, w->rc.fieldscur))
               goto default_or_error;
#if PFLAGSSIZ > 64
 // too bad fscanf is not as flexible with his format string as snprintf
 # error Hey, fix the above fscanf 'PFLAGSSIZ' dependency !
#endif
         if (3 != fscanf(fp, "\twinflags=%d, sortindx=%d, maxtasks=%d\n"
            , &w->rc.winflags, &w->rc.sortindx, &w->rc.maxtasks))
               goto default_or_error;
         if (4 != fscanf(fp, "\tsummclr=%d, msgsclr=%d, headclr=%d, taskclr=%d\n"
            , &w->rc.summclr, &w->rc.msgsclr
            , &w->rc.headclr, &w->rc.taskclr))
               goto default_or_error;

         switch (Rc.id) {
            case 'f':                  // 3.3.0 thru 3.3.3 (procps-ng)
               SETw(w, Show_JRNUMS);   //    fall through !
            case 'g':                  // current RCF_VERSION_ID
            default:                   // and future versions?
               if (strlen(w->rc.fieldscur) != sizeof(DEF_FIELDS) - 1)
                  goto default_or_error;
               for (x = 0; x < P_MAXPFLGS; ++x)
                  if (P_MAXPFLGS <= FLDget(w, x))
                     goto default_or_error;
               break;
            case 'a':                  // 3.2.8 (former procps)
               if (config_cvt(w))
                  goto default_or_error;
               break;
         }
#ifndef USE_X_COLHDR
         OFFw(w, NOHICOL_xxx);
#endif
      } // end: for (GROUPSMAX)

      // any new addition(s) last, for older rcfiles compatibility...
      fscanf(fp, "Fixed_widest=%d, Summ_mscale=%d, Task_mscale=%d, Zero_suppress=%d\n"
         , &Rc.fixed_widest, &Rc.summ_mscale, &Rc.task_mscale, &Rc.zero_suppress);

try_inspect_entries:

      // we'll start off Inspect stuff with 1 'potential' blank line
      // ( only realized if we end up with Inspect.total > 0 )
      for (i = 0, Inspect.raw = strdup("\n");;) {
       #define iT(element) Inspect.tab[i].element
         size_t lraw = strlen(Inspect.raw) +1;
         char *s;

         if (!fgets(fbuf, sizeof(fbuf), fp)) break;
         lraw += strlen(fbuf) +1;
         Inspect.raw = alloc_r(Inspect.raw, lraw);
         strcat(Inspect.raw, fbuf);

         if (fbuf[0] == '#' || fbuf[0] == '\n') continue;
         Inspect.tab = alloc_r(Inspect.tab, sizeof(struct I_ent) * (i + 1));
         p = fmtmk(N_fmt(YINSP_rcfile_fmt), i +1);

         if (!(s = strtok(fbuf, "\t\n"))) { Rc_questions = 1; continue; }
         iT(type) = strdup(s);
         if (!(s = strtok(NULL, "\t\n"))) { Rc_questions = 1; continue; }
         iT(name) = strdup(s);
         if (!(s = strtok(NULL, "\t\n"))) { Rc_questions = 1; continue; }
         iT(fmts) = strdup(s);

         switch (toupper(fbuf[0])) {
            case 'F':
               iT(func) = insp_do_file;
               break;
            case 'P':
               iT(func) = insp_do_pipe;
               break;
            default:
               Rc_questions = 1;
               continue;
         }

         iT(farg) = (strstr(iT(fmts), "%d")) ? 1 : 0;
         iT(fstr) = alloc_c(FNDBUFSIZ);
         iT(flen) = 0;

         if (Rc_questions < 0) Rc_questions = 1;
         ++i;
       #undef iT
      } // end: for ('inspect' entries)

      Inspect.total = i;
#ifndef INSP_OFFDEMO
      if (!Inspect.total) {
       #define mkS(n) N_txt(YINSP_demo ## n ## _txt)
         const char *sels[] = { mkS(01), mkS(02), mkS(03) };
         Inspect.total = Inspect.demo = MAXTBL(sels);
         Inspect.tab = alloc_c(sizeof(struct I_ent) * Inspect.total);
         for (i = 0; i < Inspect.total; i++) {
            Inspect.tab[i].type = strdup(N_txt(YINSP_deqtyp_txt));
            Inspect.tab[i].name = strdup(sels[i]);
            Inspect.tab[i].func = insp_do_demo;
            Inspect.tab[i].fmts = strdup(N_txt(YINSP_deqfmt_txt));
            Inspect.tab[i].fstr = alloc_c(FNDBUFSIZ);
         }
       #undef mkS
      }
#endif
      if (Rc_questions < 0) {
         p = fmtmk(N_fmt(RC_bad_files_fmt), Rc_name);
         goto default_or_error;
      }
      fclose(fp);
   } // end: if (fp)

   // lastly, establish the true runtime secure mode and delay time
   if (!getuid()) Secure_mode = 0;
   if (!Secure_mode) Rc.delay_time = tmp_delay;
   return;

default_or_error:
#ifdef RCFILE_NOERR
{  RCF_t rcdef = DEF_RCFILE;
   fclose(fp);
   Rc = rcdef;
   for (i = 0 ; i < GROUPSMAX; i++)
      Winstk[i].rc  = Rc.win[i];
   Rc_questions = 1;
}
#else
   error_exit(p);
#endif
} // end: configs_read


        /*
         * Parse command line arguments.
         * Note: it's assumed that the rc file(s) have already been read
         *       and our job is to see if any of those options are to be
         *       overridden -- we'll force some on and negate others in our
         *       best effort to honor the loser's (oops, user's) wishes... */
static void parse_args (char **args) {
   /* differences between us and the former top:
      -C (separate CPU states for SMP) is left to an rcfile
      -u (user monitoring) added to compliment interactive 'u'
      -p (pid monitoring) allows a comma delimited list
      -q (zero delay) eliminated as redundant, incomplete and inappropriate
            use: "nice -n-10 top -d0" to achieve what was only claimed
      .  most switches act as toggles (not 'on' sw) for more user flexibility
      .  no deprecated/illegal use of 'breakargv:' with goto
      .  bunched args are actually handled properly and none are ignored
      .  we tolerate NO whitespace and NO switches -- maybe too tolerant? */
   static const char numbs_str[] = "+,-.0123456789";
   float tmp_delay = MAXFLOAT;
   char *p;
   int i;

   while (*args) {
      const char *cp = *(args++);

      while (*cp) {
         char ch;
         switch ((ch = *cp)) {
            case '\0':
               break;
            case '-':
               if (cp[1]) ++cp;
               else if (*args) cp = *args++;
               if (strspn(cp, numbs_str))
                  error_exit(fmtmk(N_fmt(WRONG_switch_fmt)
                     , cp, Myname, N_txt(USAGE_abbrev_txt)));
               continue;
            case 'b':
               Batch = 1;
               break;
            case 'c':
               TOGw(Curwin, Show_CMDLIN);
               break;
            case 'd':
               if (cp[1]) ++cp;
               else if (*args) cp = *args++;
               else error_exit(fmtmk(N_fmt(MISSING_args_fmt), ch));
                  /* a negative delay will be dealt with shortly... */
               if (1 != sscanf(cp, "%f", &tmp_delay))
                  error_exit(fmtmk(N_fmt(BAD_delayint_fmt), cp));
               break;
            case 'H':
               Thread_mode = 1;
               break;
            case 'h':
            case 'v':
               puts(fmtmk(N_fmt(HELP_cmdline_fmt)
                  , procps_version, Myname, N_txt(USAGE_abbrev_txt)));
               bye_bye(NULL);
            case 'i':
               TOGw(Curwin, Show_IDLEPS);
               Curwin->rc.maxtasks = 0;
               break;
            case 'n':
               if (cp[1]) cp++;
               else if (*args) cp = *args++;
               else error_exit(fmtmk(N_fmt(MISSING_args_fmt), ch));
               if (1 != sscanf(cp, "%d", &Loops) || 1 > Loops)
                  error_exit(fmtmk(N_fmt(BAD_niterate_fmt), cp));
               break;
            case 'o':
               if (cp[1]) cp++;
               else if (*args) cp = *args++;
               else error_exit(fmtmk(N_fmt(MISSING_args_fmt), ch));
               if (*cp == '+') { SETw(Curwin, Qsrt_NORMAL); ++cp; }
               else if (*cp == '-') { OFFw(Curwin, Qsrt_NORMAL); ++cp; }
               for (i = 0; i < P_MAXPFLGS; i++)
                  if (!STRCMP(cp, N_col(i))) break;
               if (i == P_MAXPFLGS)
                  error_exit(fmtmk(N_fmt(XTRA_sortopt_fmt), cp));
               OFFw(Curwin, Show_FOREST);
               Curwin->rc.sortindx = i;
               cp += strlen(cp);
               break;
            case 'O':
               for (i = 0; i < P_MAXPFLGS; i++)
                  puts(N_col(i));
               bye_bye(NULL);
            case 'p':
               if (Curwin->usrseltyp) error_exit(N_txt(SELECT_clash_txt));
               do { int pid;
                  if (cp[1]) cp++;
                  else if (*args) cp = *args++;
                  else error_exit(fmtmk(N_fmt(MISSING_args_fmt), ch));
                  if (Monpidsidx >= MONPIDMAX)
                     error_exit(fmtmk(N_fmt(LIMIT_exceed_fmt), MONPIDMAX));
                  if (1 != sscanf(cp, "%d", &pid) || 0 > pid)
                     error_exit(fmtmk(N_fmt(BAD_mon_pids_fmt), cp));
                  if (!pid) pid = getpid();
                  for (i = 0; i < Monpidsidx; i++)
                     if (Monpids[i] == pid) goto next_pid;
                  Monpids[Monpidsidx++] = pid;
               next_pid:
                  if (!(p = strchr(cp, ','))) break;
                  cp = p;
               } while (*cp);
               break;
            case 's':
               Secure_mode = 1;
               break;
            case 'S':
               TOGw(Curwin, Show_CTIMES);
               break;
            case 'u':
            case 'U':
            {  const char *errmsg;
               if (Monpidsidx || Curwin->usrseltyp) error_exit(N_txt(SELECT_clash_txt));
               if (cp[1]) cp++;
               else if (*args) cp = *args++;
               else error_exit(fmtmk(N_fmt(MISSING_args_fmt), ch));
               if ((errmsg = user_certify(Curwin, cp, ch))) error_exit(errmsg);
               cp += strlen(cp);
               break;
            }
            case 'w':
            {  const char *pn = NULL;
               int ai = 0, ci = 0;
               Width_mode = -1;
               if (cp[1]) pn = &cp[1];
               else if (*args) { pn = *args; ai = 1; }
               if (pn && !(ci = strspn(pn, "0123456789"))) { ai = 0; pn = NULL; }
               if (pn && (1 != sscanf(pn, "%d", &Width_mode)
               || Width_mode < W_MIN_COL))
                  error_exit(fmtmk(N_fmt(BAD_widtharg_fmt), pn, W_MIN_COL-1));
               cp++;
               args += ai;
               if (pn) cp = pn + ci;
               continue;
            }
            default :
               error_exit(fmtmk(N_fmt(UNKNOWN_opts_fmt)
                  , *cp, Myname, N_txt(USAGE_abbrev_txt)));

         } // end: switch (*cp)

         // advance cp and jump over any numerical args used above
         if (*cp) cp += strspn(&cp[1], numbs_str) + 1;

      } // end: while (*cp)
   } // end: while (*args)

   // fixup delay time, maybe...
   if (MAXFLOAT > tmp_delay) {
      if (Secure_mode)
         error_exit(N_txt(DELAY_secure_txt));
      if (0 > tmp_delay)
         error_exit(N_txt(DELAY_badarg_txt));
      Rc.delay_time = tmp_delay;
   }
} // end: parse_args


        /*
         * Set up the terminal attributes */
static void whack_terminal (void) {
   static char dummy[] = "dumb";
   struct termios tmptty;

   // the curses part...
   if (Batch) {
      setupterm(dummy, STDOUT_FILENO, NULL);
      return;
   }
#ifdef PRETENDNOCAP
   setupterm(dummy, STDOUT_FILENO, NULL);
#else
   setupterm(NULL, STDOUT_FILENO, NULL);
#endif
   // our part...
   if (-1 == tcgetattr(STDIN_FILENO, &Tty_original))
      error_exit(N_txt(FAIL_tty_get_txt));
   // ok, haven't really changed anything but we do have our snapshot
   Ttychanged = 1;

   // first, a consistent canonical mode for interactive line input
   tmptty = Tty_original;
   tmptty.c_lflag |= (ECHO | ECHOCTL | ECHOE | ICANON | ISIG);
   tmptty.c_lflag &= ~NOFLSH;
   tmptty.c_oflag &= ~TAB3;
   tmptty.c_iflag |= BRKINT;
   tmptty.c_iflag &= ~IGNBRK;
   if (key_backspace && 1 == strlen(key_backspace))
      tmptty.c_cc[VERASE] = *key_backspace;
#ifndef TERMIO_PROXY
   if (-1 == tcsetattr(STDIN_FILENO, TCSAFLUSH, &tmptty))
      error_exit(fmtmk(N_fmt(FAIL_tty_mod_fmt), strerror(errno)));
   tcgetattr(STDIN_FILENO, &Tty_tweaked);
#endif
   // lastly, a nearly raw mode for unsolicited single keystrokes
   tmptty.c_lflag &= ~(ECHO | ECHOCTL | ECHOE | ICANON);
   tmptty.c_cc[VMIN] = 1;
   tmptty.c_cc[VTIME] = 0;
   if (-1 == tcsetattr(STDIN_FILENO, TCSAFLUSH, &tmptty))
      error_exit(fmtmk(N_fmt(FAIL_tty_raw_fmt), strerror(errno)));
   tcgetattr(STDIN_FILENO, &Tty_raw);

#ifndef OFF_STDIOLBF
   // thanks anyway stdio, but we'll manage buffering at the frame level...
   setbuffer(stdout, Stdout_buf, sizeof(Stdout_buf));
#endif

   // and don't forget to ask keyin to initialize his tinfo_tab
   keyin(1);
} // end: whack_terminal

/*######  Windows/Field Groups support  #################################*/

        /*
         * Value a window's name and make the associated group name. */
static void win_names (WIN_t *q, const char *name) {
   /* note: sprintf/snprintf results are "undefined" when src==dst,
            according to C99 & POSIX.1-2001 (thanks adc) */
   if (q->rc.winname != name)
      snprintf(q->rc.winname, sizeof(q->rc.winname), "%s", name);
   snprintf(q->grpname, sizeof(q->grpname), "%d:%s", q->winnum, name);
} // end: win_names


        /*
         * Display a window/field group (ie. make it "current"). */
static WIN_t *win_select (char ch) {
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy

   /* if there's no ch, it means we're supporting the external interface,
      so we must try to get our own darn ch by begging the user... */
   if (!ch) {
      show_pmt(N_txt(CHOOSE_group_txt));
      if (1 > chin(0, (char *)&ch, 1)) return w;
   }
   switch (ch) {
      case 'a':                         // we don't carry 'a' / 'w' in our
         w = w->next;                   // pmt - they're here for a good
         break;                         // friend of ours -- wins_colors.
      case 'w':                         // (however those letters work via
         w = w->prev;                   // the pmt too but gee, end-loser
         break;                         // should just press the darn key)
      case '1': case '2' : case '3': case '4':
         w = &Winstk[ch - '1'];
         break;
      default:                    // keep gcc happy
         break;
   }
   return Curwin = w;
} // end: win_select


        /*
         * Just warn the user when a command can't be honored. */
static int win_warn (int what) {
   switch (what) {
      case Warn_ALT:
         show_msg(N_txt(DISABLED_cmd_txt));
         break;
      case Warn_VIZ:
         show_msg(fmtmk(N_fmt(DISABLED_win_fmt), Curwin->grpname));
         break;
      default:                    // keep gcc happy
         break;
   }
   /* we gotta' return false 'cause we're somewhat well known within
      macro society, by way of that sassy little tertiary operator... */
   return 0;
} // end: win_warn


        /*
         * Change colors *Helper* function to save/restore settings;
         * ensure colors will show; and rebuild the terminfo strings. */
static void wins_clrhlp (WIN_t *q, int save) {
   static int flgssav, summsav, msgssav, headsav, tasksav;

   if (save) {
      flgssav = q->rc.winflags; summsav = q->rc.summclr;
      msgssav = q->rc.msgsclr;  headsav = q->rc.headclr; tasksav = q->rc.taskclr;
      SETw(q, Show_COLORS);
   } else {
      q->rc.winflags = flgssav; q->rc.summclr = summsav;
      q->rc.msgsclr = msgssav;  q->rc.headclr = headsav; q->rc.taskclr = tasksav;
   }
   capsmk(q);
} // end: wins_clrhlp


        /*
         * Change colors used in display */
static void wins_colors (void) {
 #define kbdABORT  'q'
 #define kbdAPPLY  kbd_ENTER
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy
   int clr = w->rc.taskclr, *pclr = &w->rc.taskclr;
   char ch, tgt = 'T';

   if (0 >= max_colors) {
      show_msg(N_txt(COLORS_nomap_txt));
      return;
   }
   wins_clrhlp(w, 1);
   putp(Cap_clr_scr);
   putp(Cap_curs_huge);

   do {
      putp(Cap_home);
      // this string is well above ISO C89's minimum requirements!
      show_special(1, fmtmk(N_unq(COLOR_custom_fmt)
         , procps_version, w->grpname
         , CHKw(w, View_NOBOLD) ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)
         , CHKw(w, Show_COLORS) ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)
         , CHKw(w, Show_HIBOLD) ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)
         , tgt, clr, w->grpname));
      if (1 > chin(0, &ch, 1)) break;
      switch (ch) {
         case 'S':
            pclr = &w->rc.summclr;
            clr = *pclr;
            tgt = ch;
            break;
         case 'M':
            pclr = &w->rc.msgsclr;
            clr = *pclr;
            tgt = ch;
            break;
         case 'H':
            pclr = &w->rc.headclr;
            clr = *pclr;
            tgt = ch;
            break;
         case 'T':
            pclr = &w->rc.taskclr;
            clr = *pclr;
            tgt = ch;
            break;
         case '0': case '1': case '2': case '3':
         case '4': case '5': case '6': case '7':
            clr = ch - '0';
            *pclr = clr;
            break;
         case 'B':
            TOGw(w, View_NOBOLD);
            break;
         case 'b':
            TOGw(w, Show_HIBOLD);
            break;
         case 'z':
            TOGw(w, Show_COLORS);
            break;
         case 'a':
         case 'w':
            wins_clrhlp((w = win_select(ch)), 1);
            clr = w->rc.taskclr, pclr = &w->rc.taskclr;
            tgt = 'T';
            break;
         default:                 // keep gcc happy
            break;
      }
      capsmk(w);
   } while (kbdAPPLY != ch && kbdABORT != ch);

   if (kbdABORT == ch) wins_clrhlp(w, 0);
   putp(Cap_curs_norm);
 #undef kbdABORT
 #undef kbdAPPLY
} // end: wins_colors


        /*
         * Manipulate flag(s) for all our windows. */
static void wins_reflag (int what, int flg) {
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy

   do {
      switch (what) {
         case Flags_TOG:
            TOGw(w, flg);
            break;
         case Flags_SET:          // Ummmm, i can't find anybody
            SETw(w, flg);         // who uses Flags_set ...
            break;
         case Flags_OFF:
            OFFw(w, flg);
            break;
         default:                 // keep gcc happy
            break;
      }
         /* a flag with special significance -- user wants to rebalance
            display so we gotta' off some stuff then force on two flags... */
      if (EQUWINS_xxx == flg) {
#ifndef SCROLLVAR_NO
         w->rc.maxtasks = w->usrseltyp = w->begpflg = w->begtask = w->varcolbeg = 0;
#else
         w->rc.maxtasks = w->usrseltyp = w->begpflg = w->begtask = 0;
#endif
         Monpidsidx = 0;
         SETw(w, Show_IDLEPS | Show_TASKON);
      }
      w = w->next;
   } while (w != Curwin);
} // end: wins_reflag


        /*
         * Set up the raw/incomplete field group windows --
         * they'll be finished off after startup completes.
         * [ and very likely that will override most/all of our efforts ]
         * [               --- life-is-NOT-fair ---                     ] */
static void wins_stage_1 (void) {
   WIN_t *w;
   int i;

   for (i = 0; i < GROUPSMAX; i++) {
      w = &Winstk[i];
      w->winnum = i + 1;
      w->rc = Rc.win[i];
      w->captab[0] = Cap_norm;
      w->captab[1] = Cap_norm;
      w->captab[2] = w->cap_bold;
      w->captab[3] = w->capclr_sum;
      w->captab[4] = w->capclr_msg;
      w->captab[5] = w->capclr_pmt;
      w->captab[6] = w->capclr_hdr;
      w->captab[7] = w->capclr_rowhigh;
      w->captab[8] = w->capclr_rownorm;
      w->next = w + 1;
      w->prev = w - 1;
   }

   // fixup the circular chains...
   Winstk[GROUPSMAX - 1].next = &Winstk[0];
   Winstk[0].prev = &Winstk[GROUPSMAX - 1];
   Curwin = Winstk;
} // end: wins_stage_1


        /*
         * This guy just completes the field group windows after the
         * rcfiles have been read and command line arguments parsed */
static void wins_stage_2 (void) {
   int i;

   for (i = 0; i < GROUPSMAX; i++) {
      win_names(&Winstk[i], Winstk[i].rc.winname);
      capsmk(&Winstk[i]);
      Winstk[i].findstr = alloc_c(FNDBUFSIZ);
      Winstk[i].findlen = 0;
   }
   if (Batch) {
      OFFw(Curwin, View_SCROLL);
      signal(SIGHUP, SIG_IGN);    // allow running under nohup
   }
   // fill in missing Fieldstab members and build each window's columnhdr
   zap_fieldstab();
} // end: wins_stage_2

/*######  Interactive Input support (do_key helpers)  ####################*/

        /*
         * These routines exist just to keep the do_key() function
         * a reasonably modest size.  */

static void file_writerc (void) {
   FILE *fp;
   int i;

   if (Rc_questions) {
      show_pmt(N_txt(XTRA_warncfg_txt));
      if ('y' != tolower(keyin(0)))
         return;
      Rc_questions = 0;
   }
   if (!(fp = fopen(Rc_name, "w"))) {
      show_msg(fmtmk(N_fmt(FAIL_rc_open_fmt), Rc_name, strerror(errno)));
      return;
   }
   fprintf(fp, "%s's " RCF_EYECATCHER, Myname);
   fprintf(fp, "Id:%c, Mode_altscr=%d, Mode_irixps=%d, Delay_time=%d.%d, Curwin=%d\n"
      , RCF_VERSION_ID
      , Rc.mode_altscr, Rc.mode_irixps
        // this may be ugly, but it keeps us locale independent...
      , (int)Rc.delay_time, (int)((Rc.delay_time - (int)Rc.delay_time) * 1000)
      , (int)(Curwin - Winstk));

   for (i = 0 ; i < GROUPSMAX; i++) {
      fprintf(fp, "%s\tfieldscur=%s\n"
         , Winstk[i].rc.winname, Winstk[i].rc.fieldscur);
      fprintf(fp, "\twinflags=%d, sortindx=%d, maxtasks=%d\n"
         , Winstk[i].rc.winflags, Winstk[i].rc.sortindx
         , Winstk[i].rc.maxtasks);
      fprintf(fp, "\tsummclr=%d, msgsclr=%d, headclr=%d, taskclr=%d\n"
         , Winstk[i].rc.summclr, Winstk[i].rc.msgsclr
         , Winstk[i].rc.headclr, Winstk[i].rc.taskclr);
   }

   // any new addition(s) last, for older rcfiles compatibility...
   fprintf(fp, "Fixed_widest=%d, Summ_mscale=%d, Task_mscale=%d, Zero_suppress=%d\n"
      , Rc.fixed_widest, Rc.summ_mscale, Rc.task_mscale, Rc.zero_suppress);

   if (Inspect.raw)
      fputs(Inspect.raw, fp);

   fclose(fp);
   show_msg(fmtmk(N_fmt(WRITE_rcfile_fmt), Rc_name));
} // end: file_writerc


        /*
         * This guy is a *Helper* function serving the following two masters:
         *   find_string() - find the next match in a given window
         *   task_show()   - highlight all matches currently in-view
         * If q->findstr is found in the designated buffer, he returns the
         * offset from the start of the buffer, otherwise he returns -1. */
static inline int find_ofs (const WIN_t *q, const char *buf) {
   char *fnd;

   if (q->findstr[0] && (fnd = STRSTR(buf, q->findstr)))
      return (int)(fnd - buf);
   return -1;
} // end: find_ofs



   /* This is currently the one true prototype require by top.
      It is placed here, instead of top.h, so as to avoid a compiler
      warning when top_nls.c is compiled. */
static const char *task_show (const WIN_t *q, const proc_t *p);

static void find_string (int ch) {
 #define reDUX (found) ? N_txt(WORD_another_txt) : ""
   static int found;
   int i;

   if ('&' == ch && !Curwin->findstr[0]) {
      show_msg(N_txt(FIND_no_next_txt));
      return;
   }
   if ('L' == ch) {
      snprintf(Curwin->findstr, FNDBUFSIZ, "%s", linein(N_txt(GET_find_str_txt)));
      Curwin->findlen = strlen(Curwin->findstr);
      found = 0;
#ifndef USE_X_COLHDR
      if (Curwin->findstr[0]) SETw(Curwin, NOHICOL_xxx);
      else OFFw(Curwin, NOHICOL_xxx);
#endif
   }
   if (Curwin->findstr[0]) {
      SETw(Curwin, INFINDS_xxx);
      for (i = Curwin->begtask; i < Frame_maxtask; i++) {
         if (-1 < find_ofs(Curwin, task_show(Curwin, Curwin->ppt[i]))) {
            found = 1;
            if (i == Curwin->begtask) continue;
            Curwin->begtask = i;
            return;
         }
      }
      show_msg(fmtmk(N_fmt(FIND_no_find_fmt), reDUX, Curwin->findstr));
   }
 #undef reDUX
} // end: find_string


static void help_view (void) {
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy
   char ch;

   putp(Cap_clr_scr);
   putp(Cap_curs_huge);

   show_special(1, fmtmk(N_unq(KEYS_helpbas_fmt)
      , procps_version
      , w->grpname
      , CHKw(w, Show_CTIMES) ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)
      , Rc.delay_time
      , Secure_mode ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)
      , Secure_mode ? "" : N_unq(KEYS_helpext_fmt)));

   if (0 < chin(0, &ch, 1)
   && ('?' == ch || 'h' == ch || 'H' == ch)) {
      do {
         putp(Cap_clr_scr);
         show_special(1, fmtmk(N_unq(WINDOWS_help_fmt)
            , w->grpname
            , Winstk[0].rc.winname, Winstk[1].rc.winname
            , Winstk[2].rc.winname, Winstk[3].rc.winname));
         if (1 > chin(0, &ch, 1)) break;
         w = win_select(ch);
      } while (kbd_ENTER != ch && kbd_ESC != ch);
   }

   putp(Cap_curs_norm);
} // end: help_view


static void keys_global (int ch) {
   // standardized error message(s)
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy

   switch (ch) {
      case '?':
      case 'h':
         help_view();
         break;
      case 'B':
         TOGw(w, View_NOBOLD);
         capsmk(w);
         break;
      case 'd':
      case 's':
         if (Secure_mode)
            show_msg(N_txt(NOT_onsecure_txt));
         else {
            float tmp =
               get_float(fmtmk(N_fmt(DELAY_change_fmt), Rc.delay_time));
            if (-1 < tmp) Rc.delay_time = tmp;
         }
         break;
      case 'E':
         if (++Rc.summ_mscale > SK_Eb) Rc.summ_mscale = SK_Kb;
         break;
      case 'e':
         if (++Rc.task_mscale > SK_Pb) Rc.task_mscale = SK_Kb;
         break;
      case 'F':
      case 'f':
         fields_utility();
         break;
      case 'g':
         win_select(0);
         break;
      case 'H':
         Thread_mode = !Thread_mode;
         if (!CHKw(w, View_STATES))
            show_msg(fmtmk(N_fmt(THREADS_show_fmt)
               , Thread_mode ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)));
         // force an extra procs refresh to avoid %cpu distortions...
         Pseudo_row = PROC_XTRA;
         break;
      case 'I':
         if (Cpu_faux_tot > 1) {
            Rc.mode_irixps = !Rc.mode_irixps;
            show_msg(fmtmk(N_fmt(IRIX_curmode_fmt)
               , Rc.mode_irixps ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)));
         } else
            show_msg(N_txt(NOT_smp_cpus_txt));
         break;
      case 'k':
         if (Secure_mode) {
            show_msg(N_txt(NOT_onsecure_txt));
         } else {
            int pid, sig = SIGTERM, def = w->ppt[w->begtask]->tid;
            if (GET_INT_BAD < (pid = get_int(fmtmk(N_txt(GET_pid2kill_fmt), def)))) {
               char *str;
               if (0 > pid) pid = def;
               str = linein(fmtmk(N_fmt(GET_sigs_num_fmt), pid, SIGTERM));
               if (*str) sig = signal_name_to_number(str);
               if (0 < sig && kill(pid, sig))
                  show_msg(fmtmk(N_fmt(FAIL_signals_fmt)
                     , pid, sig, strerror(errno)));
               else if (0 > sig) show_msg(N_txt(BAD_signalid_txt));
            }
         }
         break;
      case 'r':
         if (Secure_mode)
            show_msg(N_txt(NOT_onsecure_txt));
         else {
            int val, pid, def = w->ppt[w->begtask]->tid;
            if (GET_INT_BAD < (pid = get_int(fmtmk(N_txt(GET_pid2nice_fmt), def)))) {
               if (0 > pid) pid = def;
               if (GET_INTNONE < (val = get_int(fmtmk(N_fmt(GET_nice_num_fmt), pid))))
                  if (setpriority(PRIO_PROCESS, (unsigned)pid, val))
                     show_msg(fmtmk(N_fmt(FAIL_re_nice_fmt)
                        , pid, val, strerror(errno)));
            }
         }
         break;
      case 'X':
      {  int wide = get_int(fmtmk(N_fmt(XTRA_fixwide_fmt), Rc.fixed_widest));
         if (GET_INTNONE < wide) {
            if (-1 < wide) Rc.fixed_widest = wide;
            else if (INT_MIN < wide) Rc.fixed_widest = -1;
         }
      }
         break;
      case 'Y':
         if (!Inspect.total)
            linein(N_txt(YINSP_noents_txt));
         else {
            int pid, def = w->ppt[w->begtask]->tid;
            if (GET_INT_BAD < (pid = get_int(fmtmk(N_fmt(YINSP_pidsee_fmt), def)))) {
               if (0 > pid) pid = def;
               if (pid) inspection_utility(pid);
            }
         }
         break;
      case 'Z':
         wins_colors();
         break;
      case '0':
         Rc.zero_suppress = !Rc.zero_suppress;
         break;
      case kbd_ENTER:        // these two have the effect of waking us
      case kbd_SPACE:        // from 'select()', updating hotplugged
         sysinfo_refresh(1); // resources and refreshing the display
         break;
      default:                    // keep gcc happy
         break;
   }
} // end: keys_global


static void keys_summary (int ch) {
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy

   switch (ch) {
      case '1':
         TOGw(w, View_CPUSUM);
         break;
      case 'C':
         VIZTOGw(w, View_SCROLL);
         break;
      case 'l':
         TOGw(w, View_LOADAV);
         break;
      case 'm':
         TOGw(w, View_MEMORY);
         break;
      case 't':
         TOGw(w, View_STATES);
         break;
      default:                    // keep gcc happy
         break;
   }
} // end: keys_summary


static void keys_task (int ch) {
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy

   switch (ch) {
      case '#':
      case 'n':
         if (VIZCHKw(w)) {
            int num = get_int(fmtmk(N_fmt(GET_max_task_fmt), w->rc.maxtasks));
            if (INT_MIN < num) {
               if (-1 < num ) w->rc.maxtasks = num;
               else show_msg(N_txt(BAD_max_task_txt));
            }
         }
         break;
      case '<':
#ifdef TREE_NORESET
         if (CHKw(w, Show_FOREST)) break;
#endif
         if (VIZCHKw(w)) {
            FLG_t *p = w->procflgs + w->maxpflgs - 1;
            while (p > w->procflgs && *p != w->rc.sortindx) --p;
            if (*p == w->rc.sortindx) {
               --p;
#ifndef USE_X_COLHDR
               if (P_MAXPFLGS < *p) --p;
#endif
               if (p >= w->procflgs) {
                  w->rc.sortindx = *p;
#ifndef TREE_NORESET
                  OFFw(w, Show_FOREST);
#endif
               }
            }
         }
         break;
      case '>':
#ifdef TREE_NORESET
         if (CHKw(w, Show_FOREST)) break;
#endif
         if (VIZCHKw(w)) {
            FLG_t *p = w->procflgs + w->maxpflgs - 1;
            while (p > w->procflgs && *p != w->rc.sortindx) --p;
            if (*p == w->rc.sortindx) {
               ++p;
#ifndef USE_X_COLHDR
               if (P_MAXPFLGS < *p) ++p;
#endif
               if (p < w->procflgs + w->maxpflgs) {
                  w->rc.sortindx = *p;
#ifndef TREE_NORESET
                  OFFw(w, Show_FOREST);
#endif
               }
            }
         }
         break;
      case 'b':
         if (VIZCHKw(w)) {
#ifdef USE_X_COLHDR
            if (!CHKw(w, Show_HIROWS))
#else
            if (!CHKw(w, Show_HICOLS | Show_HIROWS))
#endif
               show_msg(N_txt(HILIGHT_cant_txt));
            else {
               TOGw(w, Show_HIBOLD);
               capsmk(w);
            }
         }
         break;
      case 'c':
         VIZTOGw(w, Show_CMDLIN);
         break;
      case 'i':
         VIZTOGw(w, Show_IDLEPS);
         break;
      case 'J':
         VIZTOGw(w, Show_JRNUMS);
         break;
      case 'j':
         VIZTOGw(w, Show_JRSTRS);
         break;
      case 'R':
#ifdef TREE_NORESET
         if (!CHKw(w, Show_FOREST)) VIZTOGw(w, Qsrt_NORMAL);
#else
         if (VIZCHKw(w)) {
            TOGw(w, Qsrt_NORMAL);
            OFFw(w, Show_FOREST);
         }
#endif
         break;
      case 'S':
         if (VIZCHKw(w)) {
            TOGw(w, Show_CTIMES);
            show_msg(fmtmk(N_fmt(TIME_accumed_fmt) , CHKw(w, Show_CTIMES)
               ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)));
         }
         break;
      case 'U':
      case 'u':
         if (VIZCHKw(w)) {
            const char *errmsg;
            if ((errmsg = user_certify(w, linein(N_txt(GET_user_ids_txt)), ch)))
               show_msg(errmsg);
         }
         break;
      case 'V':
         if (VIZCHKw(w)) {
            TOGw(w, Show_FOREST);
            if (!ENUviz(w, P_CMD))
               show_msg(fmtmk(N_fmt(FOREST_modes_fmt) , CHKw(w, Show_FOREST)
                  ? N_txt(ON_word_only_txt) : N_txt(OFF_one_word_txt)));
         }
         break;
      case 'x':
         if (VIZCHKw(w)) {
#ifdef USE_X_COLHDR
            TOGw(w, Show_HICOLS);
            capsmk(w);
#else
            if (ENUviz(w, w->rc.sortindx)) {
               TOGw(w, Show_HICOLS);
               if (ENUpos(w, w->rc.sortindx) < w->begpflg) {
                  if (CHKw(w, Show_HICOLS)) w->begpflg += 2;
                  else w->begpflg -= 2;
                  if (0 > w->begpflg) w->begpflg = 0;
               }
               capsmk(w);
            }
#endif
         }
         break;
      case 'y':
         if (VIZCHKw(w)) {
            TOGw(w, Show_HIROWS);
            capsmk(w);
         }
         break;
      case 'z':
         if (VIZCHKw(w)) {
            TOGw(w, Show_COLORS);
            capsmk(w);
         }
         break;
      default:                    // keep gcc happy
         break;
   }
} // end: keys_task


static void keys_window (int ch) {
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy

   switch (ch) {
      case '+':
         if (ALTCHKw) wins_reflag(Flags_OFF, EQUWINS_xxx);
         break;
      case '-':
         if (ALTCHKw) TOGw(w, Show_TASKON);
         break;
      case '=':
         SETw(w, Show_IDLEPS | Show_TASKON);
#ifndef SCROLLVAR_NO
         w->rc.maxtasks = w->usrseltyp = w->begpflg = w->begtask = w->varcolbeg = 0;
#else
         w->rc.maxtasks = w->usrseltyp = w->begpflg = w->begtask = 0;
#endif
         Monpidsidx = 0;
         break;
      case '_':
         if (ALTCHKw) wins_reflag(Flags_TOG, Show_TASKON);
         break;
      case '&':
      case 'L':
         if (VIZCHKw(w)) {             // ( next 2 are strictly for the UI )
            SETw(w, Show_IDLEPS);      // make sure we're showing idle tasks
            w->usrseltyp = 0;          // make sure we're not user filtering
            find_string(ch);           // ( we'll search entire ppt anyway )
         }
         break;
      case 'A':
         Rc.mode_altscr = !Rc.mode_altscr;
         break;
      case 'a':
      case 'w':
         if (ALTCHKw) win_select(ch);
         break;
      case 'G':
         if (ALTCHKw) {
            char tmp[SMLBUFSIZ];
            STRLCPY(tmp, linein(fmtmk(N_fmt(NAME_windows_fmt), w->rc.winname)));
            if (tmp[0]) win_names(w, tmp);
         }
         break;
      case kbd_UP:
         if (VIZCHKw(w)) if (0 < w->begtask) w->begtask -= 1;
         break;
      case kbd_DOWN:
         if (VIZCHKw(w)) if (w->begtask < Frame_maxtask - 1) w->begtask += 1;
         break;
#ifdef USE_X_COLHDR // ------------------------------------
      case kbd_LEFT:
#ifndef SCROLLVAR_NO
         if (VIZCHKw(w)) {
            if (VARleft(w))
               w->varcolbeg -= SCROLLAMT;
            else if (0 < w->begpflg)
               w->begpflg -= 1;
         }
#else
         if (VIZCHKw(w)) if (0 < w->begpflg) w->begpflg -= 1;
#endif
         break;
      case kbd_RIGHT:
#ifndef SCROLLVAR_NO
         if (VIZCHKw(w)) {
            if (VARright(w)) {
               w->varcolbeg += SCROLLAMT;
               if (0 > w->varcolbeg) w->varcolbeg = 0;
            } else if (w->begpflg + 1 < w->totpflgs)
               w->begpflg += 1;
         }
#else
         if (VIZCHKw(w)) if (w->begpflg + 1 < w->totpflgs) w->begpflg += 1;
#endif
         break;
#else  // USE_X_COLHDR ------------------------------------
      case kbd_LEFT:
#ifndef SCROLLVAR_NO
         if (VIZCHKw(w)) {
            if (VARleft(w))
               w->varcolbeg -= SCROLLAMT;
            else if (0 < w->begpflg) {
               w->begpflg -= 1;
               if (P_MAXPFLGS < w->pflgsall[w->begpflg]) w->begpflg -= 2;
            }
         }
#else
         if (VIZCHKw(w)) if (0 < w->begpflg) {
            w->begpflg -= 1;
            if (P_MAXPFLGS < w->pflgsall[w->begpflg]) w->begpflg -= 2;
         }
#endif
         break;
      case kbd_RIGHT:
#ifndef SCROLLVAR_NO
         if (VIZCHKw(w)) {
            if (VARright(w)) {
               w->varcolbeg += SCROLLAMT;
               if (0 > w->varcolbeg) w->varcolbeg = 0;
            } else if (w->begpflg + 1 < w->totpflgs) {
               if (P_MAXPFLGS < w->pflgsall[w->begpflg])
                  w->begpflg += (w->begpflg + 3 < w->totpflgs) ? 3 : 0;
               else w->begpflg += 1;
            }
         }
#else
         if (VIZCHKw(w)) if (w->begpflg + 1 < w->totpflgs) {
            if (P_MAXPFLGS < w->pflgsall[w->begpflg])
               w->begpflg += (w->begpflg + 3 < w->totpflgs) ? 3 : 0;
            else w->begpflg += 1;
         }
#endif
         break;
#endif // USE_X_COLHDR ------------------------------------
      case kbd_PGUP:
         if (VIZCHKw(w)) if (0 < w->begtask) {
               w->begtask -= (w->winlines - 1);
               if (0 > w->begtask) w->begtask = 0;
            }
         break;
      case kbd_PGDN:
         if (VIZCHKw(w)) if (w->begtask < Frame_maxtask - 1) {
               w->begtask += (w->winlines - 1);
               if (w->begtask > Frame_maxtask - 1) w->begtask = Frame_maxtask - 1;
               if (0 > w->begtask) w->begtask = 0;
             }
         break;
      case kbd_HOME:
#ifndef SCROLLVAR_NO
         if (VIZCHKw(w)) w->begtask = w->begpflg = w->varcolbeg = 0;
#else
         if (VIZCHKw(w)) w->begtask = w->begpflg = 0;
#endif
         break;
      case kbd_END:
         if (VIZCHKw(w)) {
            w->begtask = (Frame_maxtask - w->winlines) + 1;
            if (0 > w->begtask) w->begtask = 0;
            w->begpflg = w->endpflg;
#ifndef SCROLLVAR_NO
            w->varcolbeg = 0;
#endif
         }
         break;
      default:                    // keep gcc happy
         break;
   }
} // end: keys_window


static void keys_xtra (int ch) {
// const char *xmsg;
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy

#ifdef TREE_NORESET
   if (CHKw(w, Show_FOREST)) return;
#else
   OFFw(w, Show_FOREST);
#endif
   /* these keys represent old-top compatibility --
      they're grouped here so that if users could ever be weaned,
      we would just whack do_key's key_tab entry and this function... */
   switch (ch) {
      case 'M':
         w->rc.sortindx = P_MEM;
//       xmsg = "Memory";
         break;
      case 'N':
         w->rc.sortindx = P_PID;
//       xmsg = "Numerical";
         break;
      case 'P':
         w->rc.sortindx = P_CPU;
//       xmsg = "CPU";
         break;
      case 'T':
         w->rc.sortindx = P_TM2;
//       xmsg = "Time";
         break;
      default:                    // keep gcc happy
         break;
   }
// some have objected to this message, so we'll just keep silent...
// show_msg(fmtmk("%s sort compatibility key honored", xmsg));
} // end: keys_xtra

/*######  Forest View support  ###########################################*/

        /*
         * We try to keep most existing code unaware of our activities
         * ( plus, maintain alphabetical order with carefully chosen )
         * ( function names: forest_a, forest_b, forest_c & forest_d )
         * ( each with exactly one letter more than its predecessor! ) */
static proc_t **Seed_ppt;                   // temporary window ppt ptr
static proc_t **Tree_ppt;                   // resized by forest_create
static int      Tree_idx;                   // frame_make initializes

        /*
         * This little recursive guy is the real forest view workhorse.
         * He fills in the Tree_ppt array and also sets the child indent
         * level which is stored in an unused proc_t padding byte. */
static void forest_adds (const int self, const int level) {
   int i;

   Tree_ppt[Tree_idx] = Seed_ppt[self];     // add this as root or child
   Tree_ppt[Tree_idx++]->pad_3 = level;     // borrow 1 byte, 127 levels
   for (i = self + 1; i < Frame_maxtask; i++) {
      if (Seed_ppt[self]->tid == Seed_ppt[i]->tgid
      || (Seed_ppt[self]->tid == Seed_ppt[i]->ppid && Seed_ppt[i]->tid == Seed_ppt[i]->tgid))
         forest_adds(i, level + 1);         // got one child any others?
   }
} // end: forest_adds


        /*
         * Our qsort callback to order a ppt by the non-display start_time
         * which will make us immune from any pid, ppid or tgid anomalies
         * if/when pid values are wrapped by the kernel! */
static int forest_based (const proc_t **x, const proc_t **y) {
   if ( (*x)->start_time > (*y)->start_time ) return  1;
   if ( (*x)->start_time < (*y)->start_time ) return -1;
   return 0;
} // end: forest_based


        /*
         * This routine is responsible for preparing the proc_t's for
         * a forest display in the designated window.  Upon completion,
         * he'll replace the original window ppt with our specially
         * ordered forest version. */
static void forest_create (WIN_t *q) {
   static int hwmsav;
   int i;

   Seed_ppt = q->ppt;                       // avoid passing WIN_t ptrs
   if (!Tree_idx) {                         // do just once per frame
      if (hwmsav < Frame_maxtask) {         // grow, but never shrink
         hwmsav = Frame_maxtask;
         Tree_ppt = alloc_r(Tree_ppt, sizeof(proc_t*) * hwmsav);
      }
      qsort(Seed_ppt, Frame_maxtask, sizeof(proc_t*), (QFP_t)forest_based);
      for (i = 0; i < Frame_maxtask; i++)   // avoid any hidepid distortions
         if (!Seed_ppt[i]->pad_3)           // identify real or pretend trees
            forest_adds(i, 1);              // add as parent plus its children
   }
   memcpy(Seed_ppt, Tree_ppt, sizeof(proc_t*) * Frame_maxtask);
} // end: forest_create


        /*
         * This guy adds the artwork to either p->cmd or p->cmdline
         * when in forest view mode, otherwise he just returns 'em. */
static inline const char *forest_display (const WIN_t *q, const proc_t *p) {
#ifndef SCROLLVAR_NO
   static char buf[1024*64*2]; // the same as readproc's MAX_BUFSZ
#else
   static char buf[ROWMINSIZ];
#endif
   const char *which = (CHKw(q, Show_CMDLIN)) ? *p->cmdline : p->cmd;

   if (!CHKw(q, Show_FOREST) || 1 == p->pad_3) return which;
   snprintf(buf, sizeof(buf), "%*s%s", 4 * (p->pad_3 - 1), " `- ", which);
   return buf;
} // end: forest_display

/*######  Main Screen routines  ##########################################*/

        /*
         * Process keyboard input during the main loop */
static void do_key (int ch) {
   static struct {
      void (*func)(int ch);
      char keys[SMLBUFSIZ];
   } key_tab[] = {
      { keys_global,
         { '?', 'B', 'd', 'E', 'e', 'F', 'f', 'g', 'H', 'h'
         , 'I', 'k', 'r', 's', 'X', 'Y', 'Z', '0'
         , kbd_ENTER, kbd_SPACE, '\0' } },
      { keys_summary,
         { '1', 'C', 'l', 'm', 't', '\0' } },
      { keys_task,
         { '#', '<', '>', 'b', 'c', 'i', 'J', 'j', 'n', 'R', 'S'
         , 'U', 'u', 'V', 'x', 'y', 'z', '\0' } },
      { keys_window,
         { '+', '-', '=', '_', '&', 'A', 'a', 'G', 'L', 'w'
         , kbd_UP, kbd_DOWN, kbd_LEFT, kbd_RIGHT, kbd_PGUP, kbd_PGDN
         , kbd_HOME, kbd_END, '\0' } },
      { keys_xtra,
         { 'M', 'N', 'P', 'T', '\0'} }
   };
   int i;

   switch (ch) {
      case 0:                // ignored (always)
      case kbd_ESC:          // ignored (sometimes)
         return;
      case 'q':              // no return from this guy
         bye_bye(NULL);
      case 'W':              // no need for rebuilds
         file_writerc();
         return;
      default:               // and now, the real work...
         for (i = 0; i < MAXTBL(key_tab); ++i)
            if (strchr(key_tab[i].keys, ch)) {
               key_tab[i].func(ch);
               Frames_resize = 1;
               return;
            }
   };
   /* Frames_resize above will force a rebuild of all column headers and
      the PROC_FILLxxx flags.  It's NOT simply lazy programming.  Here are
      some keys that COULD require new column headers and/or libproc flags:
         'A' - likely
         'c' - likely when !Mode_altscr, maybe when Mode_altscr
         'F' - likely
         'f' - likely
         'g' - likely
         'H' - likely
         'I' - likely
         'J' - always
         'j' - always
         'Z' - likely, if 'Curwin' changed when !Mode_altscr
         '-' - likely (restricted to Mode_altscr)
         '_' - likely (restricted to Mode_altscr)
         '=' - maybe, but only when Mode_altscr
         '+' - likely (restricted to Mode_altscr)
         PLUS, likely for FOUR of the EIGHT cursor motion keys (scrolled)
      ( At this point we have a human being involved and so have all the time )
      ( in the world.  We can afford a few extra cpu cycles every now & then! )
    */

   show_msg(N_txt(UNKNOWN_cmds_txt));
} // end: do_key


        /*
         * State display *Helper* function to calc and display the state
         * percentages for a single cpu.  In this way, we can support
         * the following environments without the usual code bloat.
         *    1) single cpu machines
         *    2) modest smp boxes with room for each cpu's percentages
         *    3) massive smp guys leaving little or no room for process
         *       display and thus requiring the cpu summary toggle */
static void summary_hlp (CPU_t *cpu, const char *pfx) {
   /* we'll trim to zero if we get negative time ticks,
      which has happened with some SMP kernels (pre-2.4?)
      and when cpus are dynamically added or removed */
 #define TRIMz(x)  ((tz = (SIC_t)(x)) < 0 ? 0 : tz)
   SIC_t u_frme, s_frme, n_frme, i_frme, w_frme, x_frme, y_frme, z_frme, tot_frme, tz;
   float scale;

   u_frme = TRIMz(cpu->cur.u - cpu->sav.u);
   s_frme = TRIMz(cpu->cur.s - cpu->sav.s);
   n_frme = TRIMz(cpu->cur.n - cpu->sav.n);
   i_frme = TRIMz(cpu->cur.i - cpu->sav.i);
   w_frme = TRIMz(cpu->cur.w - cpu->sav.w);
   x_frme = TRIMz(cpu->cur.x - cpu->sav.x);
   y_frme = TRIMz(cpu->cur.y - cpu->sav.y);
   z_frme = TRIMz(cpu->cur.z - cpu->sav.z);
   tot_frme = u_frme + s_frme + n_frme + i_frme + w_frme + x_frme + y_frme + z_frme;
#ifdef CPU_ZEROTICS
   if (1 > tot_frme) tot_frme = 1;
#else
   if (tot_frme < cpu->edge)
      tot_frme = u_frme = s_frme = n_frme = i_frme = w_frme = x_frme = y_frme = z_frme = 0;
   if (1 > tot_frme) i_frme = tot_frme = 1;
#endif
   scale = 100.0 / (float)tot_frme;

   /* display some kinda' cpu state percentages
      (who or what is explained by the passed prefix) */
   show_special(0, fmtmk(Cpu_States_fmts, pfx
      , (float)u_frme * scale, (float)s_frme * scale
      , (float)n_frme * scale, (float)i_frme * scale
      , (float)w_frme * scale, (float)x_frme * scale
      , (float)y_frme * scale, (float)z_frme * scale));
 #undef TRIMz
} // end: summary_hlp


        /*
         * In support of a new frame:
         *    1) Display uptime and load average (maybe)
         *    2) Display task/cpu states (maybe)
         *    3) Display memory & swap usage (maybe) */
static void summary_show (void) {
 #define isROOM(f,n) (CHKw(w, f) && Msg_row + (n) < Screen_rows - 1)
 #define anyFLG 0xffffff
   static CPU_t *smpcpu = NULL;
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy

   // Display Uptime and Loadavg
   if (isROOM(View_LOADAV, 1)) {
      if (!Rc.mode_altscr)
         show_special(0, fmtmk(LOADAV_line, Myname, sprint_uptime()));
      else
         show_special(0, fmtmk(CHKw(w, Show_TASKON)? LOADAV_line_alt : LOADAV_line
            , w->grpname, sprint_uptime()));
      Msg_row += 1;
   }

   // Display Task and Cpu(s) States
   if (isROOM(View_STATES, 2)) {
      show_special(0, fmtmk(N_unq(STATE_line_1_fmt)
         , Thread_mode ? N_txt(WORD_threads_txt) : N_txt(WORD_process_txt)
         , Frame_maxtask, Frame_running, Frame_sleepin
         , Frame_stopped, Frame_zombied));
      Msg_row += 1;

      smpcpu = cpus_refresh(smpcpu);

      if (CHKw(w, View_CPUSUM)) {
         // display just the 1st /proc/stat line
         summary_hlp(&smpcpu[Cpu_faux_tot], N_txt(WORD_allcpus_txt));
         Msg_row += 1;
      } else {
         int i;
         char tmp[MEDBUFSIZ];
         // display each cpu's states separately, screen height permitting...
         for (i = 0; i < Cpu_faux_tot; i++) {
            snprintf(tmp, sizeof(tmp), N_fmt(WORD_eachcpu_fmt), smpcpu[i].id);
            summary_hlp(&smpcpu[i], tmp);
            Msg_row += 1;
            if (!isROOM(anyFLG, 1)) break;
         }
      }
   }

   // Display Memory and Swap stats
   if (isROOM(View_MEMORY, 2)) {
    #define bfT(n)  buftab[n].buf
    #define scT(e)  scaletab[Rc.summ_mscale]. e
    #define mkM(x) (float)kb_main_ ## x / scT(div)
    #define mkS(x) (float)kb_swap_ ## x / scT(div)
    #define prT(b,z) { if (9 < snprintf(b, 10, scT(fmts), z)) b[8] = '+'; }
      static struct {
         float div;
         const char *fmts;
         const char *label;
      } scaletab[] = {
         { 1, "%8.0f ", NULL },                            // kibibytes
         { 1024.0, "%#4.3f ", NULL },                      // mebibytes
         { 1024.0*1024, "%#4.3f ", NULL },                 // gibibytes
         { 1024.0*1024*1024, "%#4.3f ", NULL },            // tebibytes
         { 1024.0*1024*1024*1024, "%#4.3f ", NULL },       // pebibytes
         { 1024.0*1024*1024*1024*1024, "%#4.3f ", NULL }   // exbibytes
      };
      struct { //                                            0123456789
      // snprintf contents of each buf (after SK_Kb):       'nnnn.nnn 0'
      // and prT macro might replace space at buf[8] with:   ------> +
         char buf[10]; // MEMORY_lines_fmt provides for 8+1 bytes
      } buftab[8];

      if (!scaletab[0].label) {
         scaletab[0].label = N_txt(AMT_kilobyte_txt);
         scaletab[1].label = N_txt(AMT_megabyte_txt);
         scaletab[2].label = N_txt(AMT_gigabyte_txt);
         scaletab[3].label = N_txt(AMT_terabyte_txt);
         scaletab[4].label = N_txt(AMT_petabyte_txt);
         scaletab[5].label = N_txt(AMT_exxabyte_txt);
      }
      prT(bfT(0), mkM(total)); prT(bfT(1), mkM(used));
      prT(bfT(2), mkM(free));  prT(bfT(3), mkM(buffers));
      prT(bfT(4), mkS(total)); prT(bfT(5), mkS(used));
      prT(bfT(6), mkS(free));  prT(bfT(7), mkM(cached));

      show_special(0, fmtmk(N_unq(MEMORY_lines_fmt)
         , scT(label), bfT(0), bfT(1), bfT(2), bfT(3)
         , scT(label), bfT(4), bfT(5), bfT(6), bfT(7)));
      Msg_row += 2;
    #undef bfT
    #undef scT
    #undef mkM
    #undef mkS
    #undef prT
   }

 #undef isROOM
 #undef anyFLG
} // end: summary_show


        /*
         * Build the information for a single task row and
         * display the results or return them to the caller. */
static const char *task_show (const WIN_t *q, const proc_t *p) {
#ifndef SCROLLVAR_NO
 #define makeVAR(v)  { const char *pv = v; \
    if (!q->varcolbeg) cp = make_str(pv, q->varcolsz, Js, AUTOX_NO); \
    else cp = make_str(q->varcolbeg < (int)strlen(pv) ? pv + q->varcolbeg : "", q->varcolsz, Js, AUTOX_NO); }
#else
 #define makeVAR(v) cp = make_str(v, q->varcolsz, Js, AUTOX_NO)
#endif
 #define pages2K(n)  (unsigned long)( (n) << Pg2K_shft )
   static char rbuf[ROWMINSIZ];
   char *rp;
   int x;

   // we must begin a row with a possible window number in mind...
   *(rp = rbuf) = '\0';
   if (Rc.mode_altscr) rp = scat(rp, " ");

   for (x = 0; x < q->maxpflgs; x++) {
      const char *cp;
      FLG_t       i = q->procflgs[x];
      #define S   Fieldstab[i].scale        // these used to be variables
      #define W   Fieldstab[i].width        // but it's much better if we
      #define Js  CHKw(q, Show_JRSTRS)      // represent them as #defines
      #define Jn  CHKw(q, Show_JRNUMS)      // and only exec code if used

      switch (i) {
#ifndef USE_X_COLHDR
         // these 2 aren't real procflgs, they're used in column highlighting!
         case X_XON:
         case X_XOF:
            cp = NULL;
            if (!CHKw(q, INFINDS_xxx | NOHICOL_xxx)) {
               /* treat running tasks specially - entire row may get highlighted
                  so we needn't turn it on and we MUST NOT turn it off */
               if (!('R' == p->state && CHKw(q, Show_HIROWS)))
                  cp = (X_XON == i ? q->capclr_rowhigh : q->capclr_rownorm);
            }
            break;
#endif
         case P_CGR:
            makeVAR(p->cgroup[0]);
            break;
         case P_CMD:
            makeVAR(forest_display(q, p));
            break;
         case P_COD:
            cp = scale_mem(S, pages2K(p->trs), W, Jn);
            break;
         case P_CPN:
            cp = make_num(p->processor, W, Jn, AUTOX_NO);
            break;
         case P_CPU:
         {  float u = (float)p->pcpu * Frame_etscale;
            /* process can't use more %cpu than number of threads it has
             ( thanks Jaromir Capik <jcapik@redhat.com> ) */
            if (u > 100.0 * p->nlwp) u = 100.0 * p->nlwp;
            if (u > Cpu_pmax) u = Cpu_pmax;
            cp = scale_pcnt(u, W, Jn);
         }
            break;
         case P_DAT:
            cp = scale_mem(S, pages2K(p->drs), W, Jn);
            break;
         case P_DRT:
            cp = scale_num(p->dt, W, Jn);
            break;
         case P_ENV:
            makeVAR(p->environ[0]);
            break;
         case P_FLG:
            cp = make_str(hex_make(p->flags, 1), W, Js, AUTOX_NO);
            break;
         case P_FL1:
            cp = scale_num(p->maj_flt, W, Jn);
            break;
         case P_FL2:
            cp = scale_num(p->min_flt, W, Jn);
            break;
         case P_FV1:
            cp = scale_num(p->maj_delta, W, Jn);
            break;
         case P_FV2:
            cp = scale_num(p->min_delta, W, Jn);
            break;
         case P_GID:
            cp = make_num(p->egid, W, Jn, P_GID);
            break;
         case P_GRP:
            cp = make_str(p->egroup, W, Js, P_GRP);
            break;
         case P_MEM:
            cp = scale_pcnt((float)pages2K(p->resident) * 100 / kb_main_total, W, Jn);
            break;
         case P_NCE:
            cp = make_num(p->nice, W, Jn, AUTOX_NO);
            break;
#ifdef OOMEM_ENABLE
         case P_OOA:
            cp = make_num(p->oom_adj, W, Jn, AUTOX_NO);
            break;
         case P_OOM:
            cp = make_num(p->oom_score, W, Jn, AUTOX_NO);
            break;
#endif
         case P_PGD:
            cp = make_num(p->pgrp, W, Jn, AUTOX_NO);
            break;
         case P_PID:
            cp = make_num(p->tid, W, Jn, AUTOX_NO);
            break;
         case P_PPD:
            cp = make_num(p->ppid, W, Jn, AUTOX_NO);
            break;
         case P_PRI:
            if (-99 > p->priority || 999 < p->priority) {
               cp = make_str("rt", W, Jn, AUTOX_NO);
            } else
               cp = make_num(p->priority, W, Jn, AUTOX_NO);
            break;
         case P_RES:
            cp = scale_mem(S, pages2K(p->resident), W, Jn);
            break;
         case P_SGD:
            makeVAR(p->supgid);
            break;
         case P_SGN:
            makeVAR(p->supgrp);
            break;
         case P_SHR:
            cp = scale_mem(S, pages2K(p->share), W, Jn);
            break;
         case P_SID:
            cp = make_num(p->session, W, Jn, AUTOX_NO);
            break;
         case P_STA:
            cp = make_chr(p->state, W, Js);
            break;
         case P_SWP:
            cp = scale_mem(S, p->vm_swap, W, Jn);
            break;
         case P_TGD:
            cp = make_num(p->tgid, W, Jn, AUTOX_NO);
            break;
         case P_THD:
            cp = make_num(p->nlwp, W, Jn, AUTOX_NO);
            break;
         case P_TME:
         case P_TM2:
         {  TIC_t t = p->utime + p->stime;
            if (CHKw(q, Show_CTIMES)) t += (p->cutime + p->cstime);
            cp = scale_tics(t, W, Jn);
         }
            break;
         case P_TPG:
            cp = make_num(p->tpgid, W, Jn, AUTOX_NO);
            break;
         case P_TTY:
         {  char tmp[SMLBUFSIZ];
            dev_to_tty(tmp, W, p->tty, p->tid, ABBREV_DEV);
            cp = make_str(tmp, W, Js, P_TTY);
         }
            break;
         case P_UED:
            cp = make_num(p->euid, W, Jn, P_UED);
            break;
         case P_UEN:
            cp = make_str(p->euser, W, Js, P_UEN);
            break;
         case P_URD:
            cp = make_num(p->ruid, W, Jn, P_URD);
            break;
         case P_URN:
            cp = make_str(p->ruser, W, Js, P_URN);
            break;
         case P_USD:
            cp = make_num(p->suid, W, Jn, P_USD);
            break;
         case P_USN:
            cp = make_str(p->suser, W, Js, P_USN);
            break;
         case P_VRT:
            cp = scale_mem(S, pages2K(p->size), W, Jn);
            break;
         case P_WCH:
         {  const char *u;
            if (No_ksyms)
               u = hex_make(p->wchan, 0);
            else
               u = lookup_wchan(p->wchan, p->tid);
            cp = make_str(u, W, Js, P_WCH);
         }
            break;
         default:                 // keep gcc happy
            continue;

      } // end: switch 'procflag'

      if (cp)
         rp = scat(rp, cp);

      #undef S
      #undef W
      #undef Js
      #undef Jn
   } // end: for 'maxpflgs'

   if (!CHKw(q, INFINDS_xxx)) {
      const char *cap = ((CHKw(q, Show_HIROWS) && 'R' == p->state))
         ? q->capclr_rowhigh : q->capclr_rownorm;
      char *row = rbuf;
      int ofs;
      /* since we can't predict what the search string will be and,
         considering what a single space search request would do to
         potential buffer needs, when any matches are found we skip
         normal output routing and send all of the results directly
         to the terminal (and we sound asthmatic: poof, putt, puff) */
      if (-1 < (ofs = find_ofs(q, row))) {
         POOF("\n", cap);
         do {
            row[ofs] = '\0';
            PUTT("%s%s%s%s", row, q->capclr_hdr, q->findstr, cap);
            row += (ofs + q->findlen);
            ofs = find_ofs(q, row);
         } while (-1 < ofs);
         PUTT("%s%s", row, Caps_endline);
      } else
         PUFF("\n%s%s%s", cap, row, Caps_endline);
   }
   return rbuf;
 #undef makeVAR
 #undef pages2K
} // end: task_show


        /*
         * Squeeze as many tasks as we can into a single window,
         * after sorting the passed proc table. */
static int window_show (WIN_t *q, int wmax) {
 /* the isBUSY macro determines if a task is 'active' --
    it returns true if some cpu was used since the last sample.
    ( actual 'running' tasks will be a subset of those selected ) */
 #define isBUSY(x)   (0 < x->pcpu)
 #define winMIN(a,b) ((a < b) ? a : b)
   int i, lwin;

   // Display Column Headings -- and distract 'em while we sort (maybe)
   PUFF("\n%s%s%s", q->capclr_hdr, q->columnhdr, Caps_endline);

   if (CHKw(q, Show_FOREST))
      forest_create(q);
   else {
      if (CHKw(q, Qsrt_NORMAL)) Frame_srtflg = 1;   // this is always needed!
      else Frame_srtflg = -1;
      Frame_ctimes = CHKw(q, Show_CTIMES);          // this & next, only maybe
      Frame_cmdlin = CHKw(q, Show_CMDLIN);
      qsort(q->ppt, Frame_maxtask, sizeof(proc_t*), Fieldstab[q->rc.sortindx].sort);
   }

   i = q->begtask;
   lwin = 1;                                        // 1 for the column header
   wmax = winMIN(wmax, q->winlines + 1);            // ditto for winlines, too

   /* the least likely scenario is also the most costly, so we'll try to avoid
      checking some stuff with each iteration and check it just once... */
   if (CHKw(q, Show_IDLEPS) && !q->usrseltyp)
      while (i < Frame_maxtask && lwin < wmax) {
         task_show(q, q->ppt[i++]);
         ++lwin;
      }
   else
      while (i < Frame_maxtask && lwin < wmax) {
         if ((CHKw(q, Show_IDLEPS) || isBUSY(q->ppt[i]))
         && user_matched(q, q->ppt[i])) {
            task_show(q, q->ppt[i]);
            ++lwin;
         }
         ++i;
      }

   return lwin;
 #undef winMIN
 #undef isBUSY
} // end: window_show

/*######  Entry point plus two  ##########################################*/

        /*
         * This guy's just a *Helper* function who apportions the
         * remaining amount of screen real estate under multiple windows */
static void frame_hlp (int wix, int max) {
   int i, size, wins;

   // calc remaining number of visible windows
   for (i = wix, wins = 0; i < GROUPSMAX; i++)
      if (CHKw(&Winstk[i], Show_TASKON))
         ++wins;

   if (!wins) wins = 1;
   // deduct 1 line/window for the columns heading
   size = (max - wins) / wins;

   /* for subject window, set WIN_t winlines to either the user's
      maxtask (1st choice) or our 'foxized' size calculation
      (foxized  adj. -  'fair and balanced') */
   Winstk[wix].winlines =
      Winstk[wix].rc.maxtasks ? Winstk[wix].rc.maxtasks : size;
} // end: frame_hlp


        /*
         * Initiate the Frame Display Update cycle at someone's whim!
         * This routine doesn't do much, mostly he just calls others.
         *
         * (Whoa, wait a minute, we DO caretake those row guys, plus)
         * (we CALCULATE that IMPORTANT Max_lines thingy so that the)
         * (*subordinate* functions invoked know WHEN the user's had)
         * (ENOUGH already.  And at Frame End, it SHOULD be apparent)
         * (WE am d'MAN -- clearing UNUSED screen LINES and ensuring)
         * (the CURSOR is STUCK in just the RIGHT place, know what I)
         * (mean?  Huh, "doesn't DO MUCH"!  Never, EVER think or say)
         * (THAT about THIS function again, Ok?  Good that's better.)
         *
         * (ps. we ARE the UNEQUALED justification KING of COMMENTS!)
         * (No, I don't mean significance/relevance, only alignment.)
         */
static void frame_make (void) {
   WIN_t *w = Curwin;             // avoid gcc bloat with a local copy
   int i, scrlins;

   // deal with potential signals since the last time around...
   if (Frames_paused) pause_pgm();
   if (Frames_resize) zap_fieldstab();

   // whoa either first time or thread/task mode change, (re)prime the pump...
   if (Pseudo_row == PROC_XTRA) {
      procs_refresh();
      usleep(LIB_USLEEP);
      putp(Cap_clr_scr);
   } else
      putp(Batch ? "\n\n" : Cap_home);

   putp(Cap_curs_hide);
   procs_refresh();
   sysinfo_refresh(0);

   Tree_idx = Pseudo_row = Msg_row = scrlins = 0;
   summary_show();
   Max_lines = (Screen_rows - Msg_row) - 1;
   OFFw(Curwin, INFINDS_xxx);

   if (!Rc.mode_altscr) {
      // only 1 window to show so, piece o' cake
      w->winlines = w->rc.maxtasks ? w->rc.maxtasks : Max_lines;
      scrlins = window_show(w, Max_lines);
   } else {
      // maybe NO window is visible but assume, pieces o' cakes
      for (i = 0 ; i < GROUPSMAX; i++) {
         if (CHKw(&Winstk[i], Show_TASKON)) {
            frame_hlp(i, Max_lines - scrlins);
            scrlins += window_show(&Winstk[i], Max_lines - scrlins);
         }
         if (Max_lines <= scrlins) break;
      }
   }

   /* clear to end-of-screen (critical if last window is 'idleps off'),
      then put the cursor in-its-place, and rid us of any prior frame's msg
      (main loop must iterate such that we're always called before sleep) */
   if (scrlins < Max_lines) {
      putp(Cap_nl_clreos);
      PSU_CLREOS(Pseudo_row);
   }
   if (VIZISw(w) && CHKw(w, View_SCROLL)) show_scroll();
   else PUTT("%s%s", tg2(0, Msg_row), Cap_clr_eol);
   putp(Cap_curs_norm);
   fflush(stdout);

   /* we'll deem any terminal not supporting tgoto as dumb and disable
      the normal non-interactive output optimization... */
   if (!Cap_can_goto) PSU_CLREOS(0);

   /* lastly, check auto-sized width needs for the next iteration */
   if (AUTOX_MODE && Autox_found)
      widths_resize();
} // end: frame_make


        /*
         * duh... */
int main (int dont_care_argc, char **argv) {
   (void)dont_care_argc;
   before(*argv);
                                        //                 +-------------+
   wins_stage_1();                      //                 top (sic) slice
   configs_read();                      //                 > spread etc, <
   parse_args(&argv[1]);                //                 > lean stuff, <
   whack_terminal();                    //                 > onions etc. <
   wins_stage_2();                      //                 as bottom slice
                                        //                 +-------------+

   for (;;) {
      struct timeval tv;

      frame_make();

      if (0 < Loops) --Loops;
      if (!Loops) bye_bye(NULL);

      tv.tv_sec = Rc.delay_time;
      tv.tv_usec = (Rc.delay_time - (int)Rc.delay_time) * 1000000;

      if (Batch)
         select(0, NULL, NULL, NULL, &tv);
      else {
         fd_set fs;

         FD_ZERO(&fs);
         FD_SET(STDIN_FILENO, &fs);
         if (0 < select(STDIN_FILENO + 1, &fs, NULL, NULL, &tv))
            do_key(keyin(0));
         /* note:  above select might have been interrupted by some signal
                   in which case the return code would have been -1 and an
                   integer (volatile) switch set.  that in turn will cause
                   frame_make() to deal with it if we survived the handler
         */
      }
   }
   return 0;
} // end: main
