/*
 * The contents of this file are subject to the AOLserver Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://aolserver.com/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is AOLserver Code and related documentation
 * distributed by AOL.
 * 
 * The Initial Developer of the Original Code is America Online,
 * Inc. Portions created by AOL are Copyright (C) 1999 America Online,
 * Inc. All Rights Reserved.
 *
 * Alternatively, the contents of this file may be used under the terms
 * of the GNU General Public License (the "GPL"), in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License, indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above, a recipient may use your
 * version of this file under either the License or the GPL.
 */


/* 
 * unix.c --
 *
 *	Unix specific routines.
 */

static const char *RCSID = "@(#) $Header: /Users/dossy/Desktop/cvs/aolserver/nsd/unix.c,v 1.14 2002/06/05 22:59:21 jgdavidson Exp $, compiled: " __DATE__ " " __TIME__;

#include "nsd.h"
#include <pwd.h>
#include <grp.h>

static Ns_Mutex lock;
static int debugMode;


/*
 *----------------------------------------------------------------------
 *
 * NsBlockSignals --
 *
 *	Block signals at startup.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Signals will be pending until NsHandleSignals.
 *
 *----------------------------------------------------------------------
 */

void
NsBlockSignals(int debug)
{
    sigset_t set;

    /*
     * Block SIGHUP, SIGPIPE, SIGTERM, and SIGINT. This mask is
     * inherited by all subsequent threads so that only this
     * thread will catch the signals in the sigwait() loop below.
     * Unfortunately this makes it impossible to kill the
     * server with a signal other than SIGKILL until startup
     * is complete.
     */

    debugMode = debug;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    if (!debugMode) {
        /* NB: Don't block SIGINT in debug mode for Solaris dbx. */
        sigaddset(&set, SIGINT);
    }
    ns_sigmask(SIG_BLOCK, &set, NULL);
}


/*
 *----------------------------------------------------------------------
 * NsRestoreSignals --
 *
 *	Restore all signals to their default value.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      A new thread will be created.
 *
 *----------------------------------------------------------------------
 */

void
NsRestoreSignals(void)
{
    sigset_t        set;
    int             sig;

    for (sig = 1; sig < NSIG; ++sig) {
        ns_signal(sig, (void (*)(int)) SIG_DFL);
    }
    sigfillset(&set);
    ns_sigmask(SIG_UNBLOCK, &set, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NsHandleSignals --
 *
 *	Loop forever processing signals until a term signal
 *  	is received.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	HUP callbacks may be called.
 *
 *----------------------------------------------------------------------
 */

void
NsHandleSignals(void)
{
    sigset_t set;
    int err, sig;
    
    /*
     * Wait endlessly for trigger wakeups.
     */

    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    if (!debugMode) {
        sigaddset(&set, SIGINT);
    }
    do {
	do {
	    err = ns_sigwait(&set, &sig);
	} while (err == EINTR);
	if (err != 0) {
	    Ns_Fatal("signal: ns_sigwait failed: %s", strerror(errno));
	}
	if (sig == SIGHUP) {
	    NsRunSignalProcs();
	}
    } while (sig == SIGHUP);

    /*
     * Unblock the signals and exit.
     */

    ns_sigmask(SIG_UNBLOCK, &set, NULL);
}


/*
 *----------------------------------------------------------------------
 *
 * NsSendSignal --
 *
 *	Send a signal to the main thread.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Main thread in NsHandleSignals will wakeup.
 *
 *----------------------------------------------------------------------
 */

void
NsSendSignal(int sig)
{
    if (kill(Ns_InfoPid(),  sig) != 0) {
    	Ns_Fatal("unix: kill() failed: '%s'", strerror(errno));
    }
}


/*
 *----------------------------------------------------------------------
 * ns_sockpair, ns_pipe --
 *
 *      Create a pipe/socketpair with fd's set close on exec.
 *
 * Results:
 *      0 if ok, -1 otherwise.
 *
 * Side effects:
 *      Updates given fd array.
 *
 *----------------------------------------------------------------------
 */

static int
Pipe(int *fds, int sockpair)
{
    int err;

    if (sockpair) {
    	err = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    } else {
    	err = pipe(fds);
    }
    if (!err) {
	fcntl(fds[0], F_SETFD, 1);
	fcntl(fds[1], F_SETFD, 1);
    }
    return err;
}

int
ns_sockpair(int *socks)
{
    return Pipe(socks, 1);
}

int
ns_pipe(int *fds)
{
    return Pipe(fds, 0);
}


/*
 *----------------------------------------------------------------------
 * Ns_GetUserHome --
 *
 *      Get the home directory name for a user name
 *
 * Results:
 *      Return NS_TRUE if user name is found in /etc/passwd file and 
 * 	NS_FALSE otherwise.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_GetUserHome(Ns_DString *pds, char *user)
{
    struct passwd  *pw;
    int             retcode;

    Ns_MutexLock(&lock);
    pw = getpwnam(user);
    if (pw == NULL) {
        retcode = NS_FALSE;
    } else {
        Ns_DStringAppend(pds, pw->pw_dir);
        retcode = NS_TRUE;
    }
    Ns_MutexUnlock(&lock);
    return retcode;
}


/*
 *----------------------------------------------------------------------
 * Ns_GetGid --
 *
 *      Get the group id from a group name.
 *
 * Results:
 * 	Group id or -1 if not found.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_GetGid(char *group)
{
    int             retcode;
    struct group   *grent;

    Ns_MutexLock(&lock);
    grent = getgrnam(group);
    if (grent == NULL) {
        retcode = -1;
    } else {
        retcode = grent->gr_gid;
    }
    Ns_MutexUnlock(&lock);
    return retcode;
}


/*
 *----------------------------------------------------------------------
 * Ns_GetUserGid --
 *
 *      Get the group id for a user name
 *
 * Results:
 *      Returns group id of the user name found in /etc/passwd or -1
 *	otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_GetUserGid(char *user)
{
    struct passwd  *pw;
    int             retcode;

    Ns_MutexLock(&lock);
    pw = getpwnam(user);
    if (pw == NULL) {
        retcode = -1;
    } else {
        retcode = pw->pw_gid;
    }
    Ns_MutexUnlock(&lock);
    return retcode;
}


/*
 *----------------------------------------------------------------------
 * Ns_GetUid --
 *
 *      Get user id for a user name.
 *
 * Results:
 *      Return NS_TRUE if user name is found in /etc/passwd file and 
 * 	NS_FALSE otherwise.
 *
 * Side effects:
 *      None.
 *
 *----------------------------------------------------------------------
 */

int
Ns_GetUid(char *user)
{
    struct passwd  *pw;
    int retcode;

    Ns_MutexLock(&lock);
    pw = getpwnam(user);
    if (pw == NULL) {
        retcode = -1;
    } else {
        retcode = pw->pw_uid;
    }
    Ns_MutexUnlock(&lock);
    return retcode;
}
