/*****************************************************************************
 *  $Id: server-unixsock.c 1037 2011-04-07 20:02:56Z chris.m.dunlap $
 *****************************************************************************
 *  Written by Chris Dunlap <cdunlap@llnl.gov>.
 *  Copyright (C) 2007-2011 Lawrence Livermore National Security, LLC.
 *  Copyright (C) 2001-2007 The Regents of the University of California.
 *  UCRL-CODE-2002-009.
 *
 *  This file is part of ConMan: The Console Manager.
 *  For details, see <http://conman.googlecode.com/>.
 *
 *  ConMan is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  ConMan is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with ConMan.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/


#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>                  /* include before socket.h for bsd */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include "inevent.h"
#include "list.h"
#include "server.h"
#include "tpoll.h"
#include "util-file.h"
#include "util-str.h"


static int max_unixsock_dev_strlen(void);
static int connect_unixsock_obj(obj_t *unixsock);
static int disconnect_unixsock_obj(obj_t *unixsock);

extern tpoll_t tp_global;               /* defined in server.c */


int is_unixsock_dev(const char *dev, const char *cwd, char **path_ref)
{
    const char *prefix = "unix:";
    int         n;
    char        buf[PATH_MAX];

    assert(dev != NULL);

    if (strncasecmp(dev, prefix, strlen(prefix)) != 0) {
        return(0);
    }
    dev += strlen(prefix);
    if (dev[0] == '\0') {
        return(0);
    }
    if ((dev[0] != '/') && (cwd != NULL)) {
        n = snprintf(buf, sizeof(buf), "%s/%s", cwd, dev);
        if ((n < 0) || (n >= sizeof(buf))) {
            return(0);
        }
        dev = buf;
    }
    if (path_ref) {
        *path_ref = create_string(dev);
    }
    return(1);
}


obj_t * create_unixsock_obj(server_conf_t *conf, char *name, char *dev,
    char *errbuf, int errlen)
{
/*  Creates a new unix domain object and adds it to the master objs list.
 *  Returns the new objects, or NULL on error.
 */
    int           n;
    ListIterator  i;
    obj_t        *unixsock;
    int           rv;

    assert(conf != NULL);
    assert((name != NULL) && (name[0] != '\0'));
    assert((dev != NULL) && (dev[0] != '\0'));
    assert(errbuf != NULL);
    assert(errlen > 0);

    /*  Check length of device string.
     */
    n = max_unixsock_dev_strlen();
    if (strlen(dev) > n) {
        snprintf(errbuf, errlen,
            "console [%s] exceeds maximum device length of %d bytes", name, n);
        return(NULL);
    }
    /*  Check for duplicate console and device names.
     */
    i = list_iterator_create(conf->objs);
    while ((unixsock = list_next(i))) {
        if (is_console_obj(unixsock) && !strcmp(unixsock->name, name)) {
            snprintf(errbuf, errlen,
                "console [%s] specifies duplicate console name", name);
            break;
        }
        if (is_unixsock_obj(unixsock)
                && !strcmp(unixsock->aux.unixsock.dev, dev)) {
            snprintf(errbuf, errlen,
                "console [%s] specifies duplicate device \"%s\"", name, dev);
            break;
        }
    }
    list_iterator_destroy(i);
    if (unixsock != NULL) {
        return(NULL);
    }
    unixsock = create_obj(conf, name, -1, CONMAN_OBJ_UNIXSOCK);
    unixsock->aux.unixsock.dev = create_string(dev);
    unixsock->aux.unixsock.logfile = NULL;
    unixsock->aux.unixsock.timer = -1;
    unixsock->aux.unixsock.state = CONMAN_UNIXSOCK_DOWN;
    /*
     *  Add obj to the master conf->objs list.
     */
    list_append(conf->objs, unixsock);

    rv = inevent_add(unixsock->aux.unixsock.dev,
        (inevent_cb_f) open_unixsock_obj, unixsock);
    if (rv < 0) {
        log_msg(LOG_INFO,
            "Console [%s] unable to register device \"%s\" for inotify events",
            unixsock->name, unixsock->aux.unixsock.dev);
    }
    return(unixsock);
}


int open_unixsock_obj(obj_t *unixsock)
{
/*  (Re)opens the specified 'unixsock' obj.
 *  Returns 0 if the console is successfully opened; o/w, returns -1.
 */
    int rc = 0;

    assert(unixsock != NULL);
    assert(is_unixsock_obj(unixsock));

    if (unixsock->aux.unixsock.state == CONMAN_UNIXSOCK_UP) {
        rc = disconnect_unixsock_obj(unixsock);
    }
    else {
        rc = connect_unixsock_obj(unixsock);
    }
    return(rc);
}


static int max_unixsock_dev_strlen(void)
{
/*  Returns the maximum string length allowed for a unix domain device.
 *
 *  Note: This routine exists because the UNIX_PATH_MAX #define is not part
 *    of <sys/un.h> under Linux (it's in <linux/un.h> instead), and as such,
 *    I'm not sure how portable it is to use.
 */
    struct sockaddr_un saddr;

    /*  Subtract 1 byte for null termination of string.
     */
    return(sizeof(saddr.sun_path) - 1);
}


static int connect_unixsock_obj(obj_t *unixsock)
{
/*  Opens a connection to the specified (unixsock) obj.
 *  Returns 0 if the connection is successfully completed; o/w, returns -1.
 */
    unixsock_obj_t     *auxp;
    struct stat         st;
    struct sockaddr_un  saddr;
    int                 rc;

    assert(unixsock != NULL);
    assert(is_unixsock_obj(unixsock));
    assert(unixsock->aux.unixsock.state != CONMAN_UNIXSOCK_UP);
    assert(strlen(unixsock->aux.unixsock.dev) <= max_unixsock_dev_strlen());

    auxp = &(unixsock->aux.unixsock);

    if (auxp->timer >= 0) {
        (void) tpoll_timeout_cancel(tp_global, auxp->timer);
        auxp->timer = -1;
    }

    if (stat(auxp->dev, &st) < 0) {
        log_msg(LOG_DEBUG, "Console [%s] cannot stat device \"%s\": %s",
            unixsock->name, auxp->dev, strerror(errno));
        return(disconnect_unixsock_obj(unixsock));
    }
#ifdef S_ISSOCK
    /*  Danger, Will Robinson!  S_ISSOCK not in POSIX.1-1996.
     *
     *  If this is not defined, connect() will detect the error of the device
     *    not being a socket.
     */
    if (!S_ISSOCK(st.st_mode)) {
        log_msg(LOG_INFO, "Console [%s] device \"%s\" is not a socket",
            unixsock->name, auxp->dev, strerror(errno));
        return(disconnect_unixsock_obj(unixsock));
    }
#endif /* S_ISSOCK */

    memset(&saddr, 0, sizeof(saddr));
    saddr.sun_family = AF_UNIX;
    rc = strlcpy(saddr.sun_path, auxp->dev, sizeof(saddr.sun_path));
    assert(rc < sizeof(saddr.sun_path));

    if ((unixsock->fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        /*
         *  This error should probably not be considered fatal,
         *    but since it is elsewhere in the code, it is here as well.
         */
        log_err(errno, "Unable to create console [%s] socket", unixsock->name);
    }
    set_fd_nonblocking(unixsock->fd);
    set_fd_closed_on_exec(unixsock->fd);

    /*  FIXME: Check to see if connect() on a nonblocking unix domain socket
     *    can return EINPROGRESS.  I don't think it can.
     */
    if (connect(unixsock->fd,
            (struct sockaddr *) &saddr, sizeof(saddr)) < 0) {
        log_msg(LOG_INFO,
            "Console [%s] cannot connect to device \"%s\": %s",
            unixsock->name, auxp->dev, strerror(errno));
        return(disconnect_unixsock_obj(unixsock));
    }
    /*  Write-locking the unix domain socket appears ineffective.  But since
     *    create_unixsock_obj() already checks for duplicate devices, this is
     *    only an issue if two daemons are trying to simultaneously use the
     *    same local socket.
     */
    unixsock->gotEOF = 0;
    auxp->state = CONMAN_UNIXSOCK_UP;

    /*  Notify linked objs when transitioning into an UP state.
     */
    write_notify_msg(unixsock, LOG_NOTICE, "Console [%s] connected to \"%s\"",
        unixsock->name, auxp->dev);
    DPRINTF((9, "Opened [%s] unixsock: fd=%d dev=%s.\n",
            unixsock->name, unixsock->fd, auxp->dev));

    return(0);
}


static int disconnect_unixsock_obj(obj_t *unixsock)
{
/*  Closes the existing connection with the specified (unixsock) obj
 *    and sets a timer for establishing a new connection.
 *  Always returns -1.
 */
    unixsock_obj_t *auxp;

    assert(unixsock != NULL);
    assert(is_unixsock_obj(unixsock));

    auxp = &(unixsock->aux.unixsock);

    if (auxp->timer >= 0) {
        (void) tpoll_timeout_cancel(tp_global, auxp->timer);
        auxp->timer = -1;
    }
    if (unixsock->fd >= 0) {
        if (close(unixsock->fd) < 0) {
            log_msg(LOG_ERR,
                "Unable to close console [%s] socket \"%s\": %s",
                unixsock->name, auxp->dev, strerror(errno));
        }
        unixsock->fd = -1;
    }
    /*  Notify linked objs when transitioning from an UP state.
     */
    if (auxp->state == CONMAN_UNIXSOCK_UP) {
        auxp->state = CONMAN_UNIXSOCK_DOWN;
        write_notify_msg(unixsock, LOG_NOTICE,
            "Console [%s] disconnected from \"%s\"",
            unixsock->name, auxp->dev);
    }
    /*  Set timer for establishing new connection.
     */
    auxp->timer = tpoll_timeout_relative(tp_global,
        (callback_f) connect_unixsock_obj, unixsock,
        UNIXSOCK_RETRY_TIMEOUT * 1000);

    return(-1);
}
