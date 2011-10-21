/*
 * systime -- routines to fiddle a UNIX clock.
 *
 * ATTENTION: Get approval from Dave Mills on all changes to this file!
 *
 */
#include <config.h>

#include "ntp.h"
#include "ntp_syslog.h"
#include "ntp_unixtime.h"
#include "ntp_stdlib.h"
#include "ntp_random.h"
#include "ntpd.h"		/* for sys_precision */
#include "timevalops.h"
#include "timespecops.h"
#include "ntp_calendar.h"

#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_UTMP_H
# include <utmp.h>
#endif /* HAVE_UTMP_H */
#ifdef HAVE_UTMPX_H
# include <utmpx.h>
#endif /* HAVE_UTMPX_H */


#define	FUZZ	500e-6		/* fuzz pivot */

#ifndef USE_COMPILETIME_PIVOT
# define USE_COMPILETIME_PIVOT 1
#endif

#if defined(HAVE_CLOCK_GETTIME)
# define GET_SYSTIME_AS_TIMESPEC(tsp) clock_gettime(CLOCK_REALTIME, tsp)
#elif defined(HAVE_GETCLOCK)
# define GET_SYSTIME_AS_TIMESPEC(tsp) getclock(TIMEOFDAY, tsp)
#elif !defined(GETTIMEOFDAY)
# include "bletch: cannot get system time?"
#endif


/*
 * These routines (get_systime, step_systime, adj_systime) implement an
 * interface between the system independent NTP clock and the Unix
 * system clock in various architectures and operating systems. Time is
 * a precious quantity in these routines and every effort is made to
 * minimize errors by unbiased rounding and amortizing adjustment
 * residues.
 *
 * In order to improve the apparent resolution, provide unbiased
 * rounding and insure that the readings cannot be predicted, the low-
 * order unused portion of the time below the resolution limit is filled
 * with an unbiased random fuzz.
 *
 * The sys_tick variable secifies the system clock tick interval in
 * seconds. For systems that can interpolate between timer interrupts,
 * the resolution is presumed much less than the time to read the system
 * clock, which is the value of sys_tick after the precision has been
 * determined. For those systems that cannot interpolate between timer
 * interrupts, sys_tick will be much larger in the order of 10 ms, so the
 * fuzz should be that value. For Sunses the tick is not interpolated, but
 * the system clock is derived from a 2-MHz oscillator, so the resolution
 * is 500 ns and sys_tick is 500 ns.
 */
double	sys_tick = 0;		/* precision (time to read the clock) */
double	sys_residual = 0;	/* adjustment residue (s) */
time_stepped_callback	step_callback;

#ifndef SIM	/* ntpsim.c has get_systime() and friends for sim */

/*
 * get_systime - return system time in NTP timestamp format.
 */
void
get_systime(
	l_fp *now		/* system time */
	)
{
	double	dtemp;

#if defined(GET_SYSTIME_AS_TIMESPEC)

	struct timespec ts;	/* seconds and nanoseconds */

	/*
	 * Convert Unix timespec from seconds and nanoseconds to NTP
	 * seconds and fraction.
	 */
	GET_SYSTIME_AS_TIMESPEC(&ts);
	now->l_i = (int32)ts.tv_sec + JAN_1970;
	dtemp = 0;
	if (sys_tick > FUZZ)
		dtemp = ntp_random() * 2. / FRAC * sys_tick * 1e9;
	else if (sys_tick > 0)
		dtemp = ntp_random() * 2. / FRAC;
	dtemp = (ts.tv_nsec + dtemp) * 1e-9 + sys_residual;
	if (dtemp >= 1.) {
		dtemp -= 1.;
		now->l_i++;
	} else if (dtemp < 0) {
		dtemp += 1.;
		now->l_i--;
	}
	now->l_uf = (u_int32)(dtemp * FRAC);

#else /* have GETTIMEOFDAY */

	struct timeval tv;	/* seconds and microseconds */

	/*
	 * Convert Unix timeval from seconds and microseconds to NTP
	 * seconds and fraction.
	 */
	GETTIMEOFDAY(&tv, NULL);
	now->l_i = tv.tv_sec + JAN_1970;
	dtemp = 0;
	if (sys_tick > FUZZ)
		dtemp = ntp_random() * 2. / FRAC * sys_tick * 1e6;
	else if (sys_tick > 0)
		dtemp = ntp_random() * 2. / FRAC;
	dtemp = (tv.tv_usec + dtemp) * 1e-6 + sys_residual;
	if (dtemp >= 1.) {
		dtemp -= 1.;
		now->l_i++;
	} else if (dtemp < 0) {
		dtemp += 1.;
		now->l_i--;
	}
	now->l_uf = (u_int32)(dtemp * FRAC);

#endif /* have GETTIMEOFDAY */
}


/*
 * adj_systime - adjust system time by the argument.
 */
#if !defined SYS_WINNT
int				/* 0 okay, 1 error */
adj_systime(
	double now		/* adjustment (s) */
	)
{
	struct timeval adjtv;	/* new adjustment */
	struct timeval oadjtv;	/* residual adjustment */
	double	dtemp;
	long	ticks;
	int	isneg = 0;

	/*
	 * The Windows port adj_systime() depends on being called each
	 * second even when there's no additional correction, to allow
	 * emulation of adjtime() behavior on top of an API that simply
	 * sets the current rate.  This POSIX implementation needs to
	 * ignore invocations with zero correction, otherwise ongoing
	 * EVNT_NSET adjtime() can be aborted by a tiny adjtime()
	 * triggered by sys_residual.
	 */
	if (0. == now)
		return TRUE;

	/*
	 * Most Unix adjtime() implementations adjust the system clock
	 * in microsecond quanta, but some adjust in 10-ms quanta. We
	 * carefully round the adjustment to the nearest quantum, then
	 * adjust in quanta and keep the residue for later.
	 */
	dtemp = now + sys_residual;
	if (dtemp < 0) {
		isneg = 1;
		dtemp = -dtemp;
	}
	adjtv.tv_sec = (long)dtemp;
	dtemp -= adjtv.tv_sec;
	ticks = (long)(dtemp / sys_tick + .5);
	adjtv.tv_usec = (long)(ticks * sys_tick * 1e6);
	dtemp -= adjtv.tv_usec / 1e6;
	sys_residual = dtemp;

	/*
	 * Convert to signed seconds and microseconds for the Unix
	 * adjtime() system call. Note we purposely lose the adjtime()
	 * leftover.
	 */
	if (isneg) {
		adjtv.tv_sec = -adjtv.tv_sec;
		adjtv.tv_usec = -adjtv.tv_usec;
		sys_residual = -sys_residual;
	}
	if (adjtv.tv_sec != 0 || adjtv.tv_usec != 0) {
		if (adjtime(&adjtv, &oadjtv) < 0) {
			msyslog(LOG_ERR, "adj_systime: %m");
			return FALSE;
		}
	}
	return TRUE;
}
#endif


/*
 * step_systime - step the system clock.
 */

int
step_systime(
	double step
	)
{
	time_t pivot; /* for ntp era unfolding */
	struct timeval timetv, tvlast, tvdiff;
	struct timespec timets;
	struct calendar jd;
	l_fp fp_ofs, fp_sys; /* offset and target system time in FP */

	/*
	 * Get pivot time for NTP era unfolding. Since we don't step
	 * very often, we can afford to do the whole calculation from
	 * scratch. And we're not in the time-critical path yet.
	 */
#if SIZEOF_TIME_T > 4
	/*
	 * This code makes sure the resulting time stamp for the new
	 * system time is in the 2^32 seconds starting at 1970-01-01,
	 * 00:00:00 UTC.
	 */
	pivot = 0x80000000;
#if USE_COMPILETIME_PIVOT
	/*
	 * Add the compile time minus 10 years to get a possible target
	 * area of (compile time - 10 years) to (compile time + 126
	 * years).  This should be sufficient for a given binary of
	 * NTPD.
	 */
	if (ntpcal_get_build_date(&jd)) {
		jd.year -= 10;
		pivot += ntpcal_date_to_time(&jd);
	} else {
		msyslog(LOG_ERR,
			"step-systime: assume 1970-01-01 as build date");
	}
#else
	UNUSED_LOCAL(jd);
#endif /* USE_COMPILETIME_PIVOT */
#else
	UNUSED_LOCAL(jd);
	/* This makes sure the resulting time stamp is on or after
	 * 1969-12-31/23:59:59 UTC and gives us additional two years,
	 * from the change of NTP era in 2036 to the UNIX rollover in
	 * 2038. (Minus one second, but that won't hurt.) We *really*
	 * need a longer 'time_t' after that!  Or a different baseline,
	 * but that would cause other serious trouble, too.
	 */
	pivot = 0x7FFFFFFF;
#endif

	/* get the complete jump distance as l_fp */
	DTOLFP(sys_residual, &fp_sys);
	DTOLFP(step,         &fp_ofs);
	L_ADD(&fp_ofs, &fp_sys);

	/* ---> time-critical path starts ---> */

	/* get the current time as l_fp (without fuzz) and as struct timeval */
#if defined(GET_SYSTIME_AS_TIMESPEC)
	GET_SYSTIME_AS_TIMESPEC(&timets);
	timespec_abstolfp(&fp_sys, &timets);
	tvlast.tv_sec = timets.tv_sec;
	tvlast.tv_usec = (timets.tv_nsec + 500) / 1000;
#else /* have GETTIMEOFDAY */
	UNUSED_LOCAL(timets);
	GETTIMEOFDAY(&tvlast, NULL);
	timeval_abstolfp(&fp_sys, &tvlast);
#endif

	/* get the target time as l_fp */
	L_ADD(&fp_sys, &fp_ofs);

	/* unfold the new system time */
	timeval_absfromlfp(&timetv, &fp_sys, &pivot);

	/* now set new system time */
	if (ntp_set_tod(&timetv, NULL) != 0) {
		msyslog(LOG_ERR, "step-systime: %m");
		return FALSE;
	}

	/* <--- time-critical path ended with 'ntp_set_tod()' <--- */

	sys_residual = 0;
	if (step_callback)
		(*step_callback)();

#ifdef NEED_HPUX_ADJTIME
	/*
	 * CHECKME: is this correct when called by ntpdate?????
	 */
	_clear_adjtime();
#endif

	/*
	 * FreeBSD, for example, has:
	 * struct utmp {
	 *	   char    ut_line[UT_LINESIZE];
	 *	   char    ut_name[UT_NAMESIZE];
	 *	   char    ut_host[UT_HOSTSIZE];
	 *	   long    ut_time;
	 * };
	 * and appends line="|", name="date", host="", time for the OLD
	 * and appends line="{", name="date", host="", time for the NEW
	 * to _PATH_WTMP .
	 *
	 * Some OSes have utmp, some have utmpx.
	 */

	/*
	 * Write old and new time entries in utmp and wtmp if step
	 * adjustment is greater than one second.
	 *
	 * This might become even Uglier...
	 */
	timeval_sub(&tvdiff, &timetv, &tvlast);
	timeval_abs(&tvdiff, &tvdiff);
	if (tvdiff.tv_sec > 0) {
#ifdef HAVE_UTMP_H
		struct utmp ut;
#endif
#ifdef HAVE_UTMPX_H
		struct utmpx utx;
#endif

#ifdef HAVE_UTMP_H
		ZERO(ut);
#endif
#ifdef HAVE_UTMPX_H
		ZERO(utx);
#endif

		/* UTMP */

#ifdef UPDATE_UTMP
# ifdef HAVE_PUTUTLINE
#  ifndef _PATH_UTMP
#   define _PATH_UTMP UTMP_FILE
#  endif
		utmpname(_PATH_UTMP);
		ut.ut_type = OLD_TIME;
		strlcpy(ut.ut_line, OTIME_MSG, sizeof(ut.ut_line));
		ut.ut_time = tvlast.tv_sec;
		setutent();
		pututline(&ut);
		ut.ut_type = NEW_TIME;
		strlcpy(ut.ut_line, NTIME_MSG, sizeof(ut.ut_line));
		ut.ut_time = timetv.tv_sec;
		setutent();
		pututline(&ut);
		endutent();
# else /* not HAVE_PUTUTLINE */
# endif /* not HAVE_PUTUTLINE */
#endif /* UPDATE_UTMP */

		/* UTMPX */

#ifdef UPDATE_UTMPX
# ifdef HAVE_PUTUTXLINE
		utx.ut_type = OLD_TIME;
		strlcpy(utx.ut_line, OTIME_MSG, sizeof(utx.ut_line));
		utx.ut_tv = tvlast;
		setutxent();
		pututxline(&utx);
		utx.ut_type = NEW_TIME;
		strlcpy(utx.ut_line, NTIME_MSG, sizeof(utx.ut_line));
		utx.ut_tv = timetv;
		setutxent();
		pututxline(&utx);
		endutxent();
# else /* not HAVE_PUTUTXLINE */
# endif /* not HAVE_PUTUTXLINE */
#endif /* UPDATE_UTMPX */

		/* WTMP */

#ifdef UPDATE_WTMP
# ifdef HAVE_PUTUTLINE
#  ifndef _PATH_WTMP
#   define _PATH_WTMP WTMP_FILE
#  endif
		utmpname(_PATH_WTMP);
		ut.ut_type = OLD_TIME;
		strlcpy(ut.ut_line, OTIME_MSG, sizeof(ut.ut_line));
		ut.ut_time = tvlast.tv_sec;
		setutent();
		pututline(&ut);
		ut.ut_type = NEW_TIME;
		strlcpy(ut.ut_line, NTIME_MSG, sizeof(ut.ut_line));
		ut.ut_time = timetv.tv_sec;
		setutent();
		pututline(&ut);
		endutent();
# else /* not HAVE_PUTUTLINE */
# endif /* not HAVE_PUTUTLINE */
#endif /* UPDATE_WTMP */

		/* WTMPX */

#ifdef UPDATE_WTMPX
# ifdef HAVE_PUTUTXLINE
		utx.ut_type = OLD_TIME;
		utx.ut_tv = tvlast;
		strlcpy(utx.ut_line, OTIME_MSG, sizeof(utx.ut_line));
#  ifdef HAVE_UPDWTMPX
		updwtmpx(WTMPX_FILE, &utx);
#  else /* not HAVE_UPDWTMPX */
#  endif /* not HAVE_UPDWTMPX */
# else /* not HAVE_PUTUTXLINE */
# endif /* not HAVE_PUTUTXLINE */
# ifdef HAVE_PUTUTXLINE
		utx.ut_type = NEW_TIME;
		utx.ut_tv = timetv;
		strlcpy(utx.ut_line, NTIME_MSG, sizeof(utx.ut_line));
#  ifdef HAVE_UPDWTMPX
		updwtmpx(WTMPX_FILE, &utx);
#  else /* not HAVE_UPDWTMPX */
#  endif /* not HAVE_UPDWTMPX */
# else /* not HAVE_PUTUTXLINE */
# endif /* not HAVE_PUTUTXLINE */
#endif /* UPDATE_WTMPX */

	}
	return TRUE;
}

#endif	/* !SIM */
