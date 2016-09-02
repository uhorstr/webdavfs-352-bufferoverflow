/*
 * Copyright (c) 1999-2004 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved
 */
/*
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * The NEXTSTEP Software License Agreement specifies the terms
 * and conditions for redistribution.
 *
 *	@(#)webdav_vnops.c	8.8 (Berkeley) 1/21/94
 */

#define APPLE_PRIVATE   1 // ����� so we can use sock_nointerrupt()
#include <sys/types.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/socket.h>
#include <sys/ubc.h>
#include <sys/fcntl.h>
#include <sys/unistd.h>
#include <sys/ioccom.h>
#include <vfs/vfs_support.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/param.h>

#include "webdav.h"

/*****************************************************************************/

#ifdef DEBUG

static int webdav_print(vnode_t vp)
{
	struct webdavnode *pt = VTOWEBDAV(vp);
	
	/* print a few things from the webdavnode */ 
	printf("tag VT_WEBDAV, webdav, id=%ld, obj_ref=%ld, cache_vnode=%ld\n",
		pt->pt_fileid, pt->pt_obj_ref, pt->pt_cache_vnode);
	return (0);
	(void)webdav_print(vp); /* stop complaining if we don't call this function */
}

__private_extern__
void log_vnop_start(char *str)
{
	struct timespec ts;
	
	printf("vnop %s start\n", str);
    ts.tv_sec = 0;
    ts.tv_nsec = 1 * 1000 * 1000;	/* wait for 1 ms */
    (void) msleep((caddr_t)&ts, NULL, PCATCH, "log_vnop_start", &ts);
}

__private_extern__
void log_vnop_error(char *str, int error)
{
	struct timespec ts;
	
	printf("vnop %s error = %d\n", str, error);
    ts.tv_sec = 0;
    ts.tv_nsec = 1 * 1000 * 1000;	/* wait for 1 ms */
    (void) msleep((caddr_t)&ts, NULL, PCATCH, "log_vnop_error", &ts);
}

#endif

/*****************************************************************************/

/*
 * webdav_copy_creds copies the uid_t and array of gid_t from a ucred into a webdav_cred.
 */
__private_extern__
void webdav_copy_creds(vfs_context_t context, struct webdav_cred *dest)
{
	ucred_t source;
	
	source = vfs_context_ucred(context);
	
	dest->pcr_uid = source->cr_uid;
	dest->pcr_ngroups = source->cr_ngroups;
	bcopy(source->cr_groups, dest->pcr_groups, NGROUPS * sizeof(gid_t));
}

/*****************************************************************************/

/*
 * webdav_dead is called when the mount_webdav daemon cannot communicate with
 * the remote WebDAV server and there will be no reconnection attempts.
 * It uses vfs_event_signal() to tell interested parties the connection with
 * the server is dead.
 */
static
void webdav_dead(struct webdavmount *fmp)
{
	if ( fmp != NULL )
	{
		if ( !(fmp->pm_status & WEBDAV_MOUNT_DEAD) )
		{
			printf("webdav server: %s: connection is dead\n", vfs_statfs(fmp->pm_mountp)->f_mntfromname);
			vfs_event_signal(&vfs_statfs(fmp->pm_mountp)->f_fsid, VQ_DEAD, 0);
			fmp->pm_status |= WEBDAV_MOUNT_DEAD;
		}
	}
}

/*****************************************************************************/

/*
 * webdav_down is called when the mount_webdav daemon cannot communicate with
 * the remote WebDAV server. It uses vfs_event_signal() to tell interested
 * parties the connection with the server is down.
 */
static
void webdav_down(struct webdavmount *fmp)
{
	if ( fmp != NULL )
	{
		if ( !(fmp->pm_status & (WEBDAV_MOUNT_TIMEO | WEBDAV_MOUNT_DEAD)) )
		{
			printf("webdav server: %s: not responding\n", vfs_statfs(fmp->pm_mountp)->f_mntfromname);
			vfs_event_signal(&vfs_statfs(fmp->pm_mountp)->f_fsid, VQ_NOTRESP, 0);
			fmp->pm_status |= WEBDAV_MOUNT_TIMEO;
		}
	}
}

/*****************************************************************************/

/*
 * webdav_up is called when the mount_webdav daemon can communicate with
 * the remote WebDAV server. It uses vfs_event_signal() to tell interested
 * parties the connection is OK again if the connection was having problems.
 */
static
void webdav_up(struct webdavmount *fmp)
{
	if ( fmp != NULL )
	{
        if ( (fmp->pm_status & WEBDAV_MOUNT_TIMEO) )
		{
			printf("webdav server: %s: is alive again\n", vfs_statfs(fmp->pm_mountp)->f_mntfromname);
			fmp->pm_status &= ~WEBDAV_MOUNT_TIMEO;
			vfs_event_signal(&vfs_statfs(fmp->pm_mountp)->f_fsid, VQ_NOTRESP, 1);
        }
	}
}

/*****************************************************************************/

/*
 * webdav_sendmsg is used to communicate with the userland half of the file
 * system.
 *
 * Inputs:
 *      vnop        the operation to be peformed -- defined operations
 *					are in webdav.h
 *      fmp         pointer to struct webdavmount for the file system
 *      request     pointer to the webdav_request struct
 *      requestsize size of request
 *      vardata     pointer to optional variable length data (the last field of
 *					request), or NULL if none
 *      vardatasize size of vardata, or 0 if no vardata
 *
 * Outputs:
 *      result      the result of the operation
 *      reply       pointer to the webdav_reply struct
 *      replysize   size of reply
 */
__private_extern__
int webdav_sendmsg(int vnop, struct webdavmount *fmp,
	void *request, size_t requestsize,
	void *vardata, size_t vardatasize,
	int *result, void *reply, size_t replysize)
{
	int error;
	socket_t so;
	int so_open;
	struct msghdr msg;
	struct iovec aiov[3];
	struct timeval tv;
	struct timeval lasttrytime;
	struct timeval currenttime;
	size_t iolen;

	/* get current time */
	microtime(&currenttime);
	so_open = FALSE;
	
	while ( TRUE )
	{
		lasttrytime.tv_sec = currenttime.tv_sec;
		
		/* make we're not force unmounting */
		if ( (fmp == NULL) || ((vnop != WEBDAV_UNMOUNT) && vfs_isforce(fmp->pm_mountp)) )
		{
			error = ENXIO;
			break;
		}
		
		/* don't open more connections than the user-land server can handle */
		while (fmp->pm_open_connections >= WEBDAV_MAX_KEXT_CONNECTIONS)
		{
			fmp->pm_status |= WEBDAV_MOUNT_CONNECTION_WANTED;
			error = msleep((caddr_t)&fmp->pm_open_connections, NULL, PCATCH, "webdav_sendmsg - pm_open_connections", NULL);
			if ( error )
			{
				break;
			}
		}
		++fmp->pm_open_connections;
		
		/* create a new socket */
		error = sock_socket(PF_LOCAL, SOCK_STREAM, 0, NULL, NULL, &so);
		if ( error != 0 )
		{
			printf("webdav_sendmsg: sock_socket() = %d\n", error);
			so_open = FALSE;
			--fmp->pm_open_connections;
			break;
		}
		else
		{
			so_open = TRUE;
		}

		/* set the socket receive timeout */
		tv.tv_sec = WEBDAV_SO_RCVTIMEO_SECONDS;
		tv.tv_usec = 0;
		error = sock_setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(struct timeval));
		if (error)
		{
			printf("webdav_sendmsg: sock_setsockopt() = %d\n", error);
			break;
		}
	
		/*
		 * When sock_connect() is called on local domain sockets, the attach
		 * code calls soreserve() with hard coded values (currently PIPSIZ -- 8192).
		 */
		
		/* make we're not force unmounting */
		if ( (fmp == NULL) || ((vnop != WEBDAV_UNMOUNT) && vfs_isforce(fmp->pm_mountp)) )
		{
			error = ENXIO;
			break;
		}

		/* kick off connection */
		error = sock_connect(so, fmp->pm_socket_name, 0);
		if (error && error != EINPROGRESS)
		{
			/* is the other side gone? If so, we're dead. */
			if ( error == ECONNREFUSED )
			{
				webdav_dead(fmp);
			}
			/* ENOENT is expected after a normal unmount */
			if ( error != ENOENT )
			{
				printf("webdav_sendmsg: sock_connect() = %d\n", error);
			}
			break;
		}
		
		/* disable interrupts on socket buffers */
		error = sock_nointerrupt(so, TRUE);
		if (error)
		{
			printf("webdav_sendmsg: sock_nointerrupt() = %d\n", error);
			break;
		}
		
		memset(&msg, 0, sizeof(msg));
		
		aiov[0].iov_base = (caddr_t) & vnop;
		aiov[0].iov_len = sizeof(vnop);
		aiov[1].iov_base = (caddr_t)request;
		aiov[1].iov_len = requestsize;
		if ( vardatasize == 0 )
		{
			msg.msg_iovlen = 2;
		}
		else
		{
			aiov[2].iov_base = vardata;
			aiov[2].iov_len = vardatasize;
			msg.msg_iovlen = 3;
		}
		msg.msg_iov = aiov;

		/* make we're not force unmounting */
		if ( (fmp == NULL) || ((vnop != WEBDAV_UNMOUNT) && vfs_isforce(fmp->pm_mountp)) )
		{
			error = ENXIO;
			break;
		}

		error = sock_send(so, &msg, 0, &iolen);
		if (error)
		{
			printf("webdav_sendmsg: sock_send() = %d\n", error);
			break;
		}
		
		memset(&msg, 0, sizeof(msg));
		
		aiov[0].iov_base = (caddr_t)result;
		aiov[0].iov_len = sizeof(*result);
		aiov[1].iov_base = (caddr_t)reply;
		aiov[1].iov_len = replysize;
		msg.msg_iov = aiov;
		msg.msg_iovlen = (replysize == 0 ? 1 : 2);
		
		while ( TRUE )
		{
			/* make we're not force unmounting */
			if ( (fmp == NULL) || ((vnop != WEBDAV_UNMOUNT) && vfs_isforce(fmp->pm_mountp)) )
			{
				error = ENXIO;
				break;
			}
			
			error = sock_receive(so, &msg, MSG_WAITALL, &iolen);
			
			/* did sock_receive timeout? */
			if (error != EWOULDBLOCK)
			{
				/* sock_receive did not time out */
				if ( error != 0 )
				{
					printf("webdav_sendmsg: sock_receive() = %d\n", error);
				}
				break;
			}
		}
		if ( error != 0 )
		{
			break;
		}
		
		if ( *result & WEBDAV_CONNECTION_DOWN_MASK )
		{
			/* communications with mount_webdav were OK, but the remote server is unreachable */
			if ( fmp->pm_status & WEBDAV_MOUNT_SUPPRESS_ALL_UI )
			{
				webdav_dead(fmp);
			}
			else
			{
				webdav_down(fmp);
			}
			*result &= ~WEBDAV_CONNECTION_DOWN_MASK;
			
			/* If this request failed because of the connection problem, retry */
			if ( *result == ENXIO && !(fmp->pm_status & WEBDAV_MOUNT_SUPPRESS_ALL_UI))
			{
				/* get current time */
				microtime(&currenttime);
				if ( currenttime.tv_sec < (lasttrytime.tv_sec + 2) )
				{
					struct timespec ts;
					
					/* sleep for 2 secs before retrying again */
					ts.tv_sec = 2;
					ts.tv_nsec = 0;
					error = msleep((caddr_t)&ts, NULL, PCATCH, "webdav_sendmsg", &ts);
					if ( (error != 0) && (error != EWOULDBLOCK) )
					{
						printf("webdav_sendmsg: msleep: %d\n", error);
						break;
					}
					microtime(&currenttime);
				}
				/* no break so we'll retry */
			}
			else
			{
				break;
			}
		}
		else
		{
			webdav_up(fmp);
			break;
		}
		
		(void) sock_shutdown(so, SHUT_RDWR); /* ignore failures - nothing can be done */
		sock_close(so);
		so_open = FALSE;
		--fmp->pm_open_connections;
		
		/* if anyone else is waiting for a connection, wake them up */
		if ( fmp->pm_status & WEBDAV_MOUNT_CONNECTION_WANTED )
		{
			fmp->pm_status &= ~WEBDAV_MOUNT_CONNECTION_WANTED;
			wakeup((caddr_t)&fmp->pm_open_connections);
		}
		
		/* ... and retry */
	}
	
	if ( so_open )
	{
		(void) sock_shutdown(so, SHUT_RDWR); /* ignore failures - nothing can be done */
		sock_close(so);
		so_open = FALSE;
		--fmp->pm_open_connections;
	}
	
	/* if anyone else is waiting for a connection, wake them up */
	if ( fmp->pm_status & WEBDAV_MOUNT_CONNECTION_WANTED )
	{
		fmp->pm_status &= ~WEBDAV_MOUNT_CONNECTION_WANTED;
		wakeup((caddr_t)&fmp->pm_open_connections);
	}
	
	/* translate all unexpected errors to EIO. Leave ENXIO (unmounting) alone. */
	if ( (error != 0) && (error != ENXIO) )
	{
		error = EIO;
	}

	return (error);
}

/*****************************************************************************/

static
int webdav_lookup(struct vnop_lookup_args *ap, struct webdav_reply_lookup *reply_lookup)
{
	int error;
	int server_error;
	struct webdav_request_lookup request_lookup;
	
	/* set up the request */
	webdav_copy_creds(ap->a_context, &request_lookup.pcr);
	request_lookup.dir = VTOWEBDAV(ap->a_dvp)->pt_obj_ref;
	request_lookup.name_length = ap->a_cnp->cn_namelen;

	server_error = 0;
	bzero(reply_lookup, sizeof(struct webdav_reply_lookup));
	
	error = webdav_sendmsg(WEBDAV_LOOKUP, VFSTOWEBDAV(vnode_mount(ap->a_dvp)),
		&request_lookup, offsetof(struct webdav_request_lookup, name), 
		ap->a_cnp->cn_nameptr, ap->a_cnp->cn_namelen,
		&server_error, reply_lookup, sizeof(struct webdav_reply_lookup));
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}
	
	return ( error );
}

/*****************************************************************************/

__private_extern__
int webdav_get(
	struct mount *mp,			/* mount point */
	vnode_t dvp,				/* parent vnode */
	int markroot,				/* if 1, mark as root vnode */
	struct componentname *cnp,  /* componentname */
	object_ref obj_ref,			/* object's object_ref */
	ino_t obj_fileid,			/* object's file ID number */
	enum vtype obj_vtype,		/* VREG or VDIR */
	struct timespec obj_atime,  /* time of last access */
	struct timespec obj_mtime,  /* time of last data modification */
	struct timespec obj_ctime,  /* time of last file status change */
	off_t obj_filesize,			/* object's filesize */
	vnode_t *vpp)				/* vnode returned here */
{
	int error;
	struct vnode_fsparam vfsp;
	vnode_t vp;
	struct webdavnode *new_pt;

	/*
	 * Allocate a webdavnode before looking in the hash.  This is
	 * because the allocation could block, during which time
	 * a vnode could be added to or removed from the hash.
	 */
	MALLOC(new_pt, void *, sizeof(struct webdavnode), M_TEMP, M_WAITOK);

	/*
	 * See if the vnode is already in our hash.  If so,
	 * just return it (and free up the webdavnode we just allocated).
	 */
	vp = webdav_hashget(mp, obj_fileid);
	if (vp != NULLVP)
	{
		*vpp = vp;
		if ( cnp->cn_flags & MAKEENTRY )
		{
			cache_enter(dvp, vp, cnp);
		}
		FREE((caddr_t)new_pt, M_TEMP);
		error = 0;
	}
	else
	{
		/*
		 * There was nothing in the hash.  Before we block on I/O,
		 * we need to insert a matching webdavnode marked as being
		 * initialized.  Any other webdav_get() will block until we're
		 * finished here, and either find the fully initialized
		 * denode, or none at all.
		 */
		bzero(new_pt, sizeof(struct webdavnode));
		new_pt->pt_mountp = mp;
		new_pt->pt_parent = dvp;
		new_pt->pt_vnode = NULLVP;
		new_pt->pt_cache_vnode = NULLVP;
		new_pt->pt_obj_ref = obj_ref;
		new_pt->pt_fileid = obj_fileid;
		new_pt->pt_atime = obj_atime;
		new_pt->pt_mtime = obj_mtime;
		new_pt->pt_ctime = obj_ctime;
		new_pt->pt_filesize = obj_filesize;
		SET(new_pt->pt_status, WEBDAV_INIT);
		new_pt->pt_opencount = 0;
		webdav_hashins(new_pt);
		
		/* Create the vnode */
		vfsp.vnfs_mp = mp;
		vfsp.vnfs_vtype = obj_vtype;
		vfsp.vnfs_str = webdav_name;
		vfsp.vnfs_dvp = dvp;
		vfsp.vnfs_fsnode = new_pt;
		vfsp.vnfs_vops = webdav_vnodeop_p;
		vfsp.vnfs_markroot = markroot;
		vfsp.vnfs_marksystem = 0;	/* webdavfs has no "system" vnodes */
		vfsp.vnfs_rdev = 0;		/* webdavfs doesn't support block devices */
		vfsp.vnfs_filesize = obj_filesize;
		vfsp.vnfs_cnp = cnp;
		vfsp.vnfs_flags = (dvp && cnp && (cnp->cn_flags & MAKEENTRY)) ? 0 : VNFS_NOCACHE;
		
		error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &vp);
		if ( error == 0 )
		{
			/* Make the webdavnode reference the new vnode */
			new_pt->pt_vnode = vp;
			vnode_addfsref(vp);
			vnode_settag(vp, VT_WEBDAV);
			
			/* Return it.  We're done. */
			*vpp = vp;
			
			/* wake up anyone waiting */
			CLR(new_pt->pt_status, WEBDAV_INIT);
			if (ISSET(new_pt->pt_status, WEBDAV_WAITINIT))
			{
				wakeup(new_pt);
			}
		}
		else
		{
			/* remove the partially inited webdavnode from the hash */
			webdav_hashrem(new_pt);
			
			/* wake up anyone waiting */
			if (ISSET(new_pt->pt_status, WEBDAV_WAITINIT))
			{
				wakeup(new_pt);
			}
			
			/* and free up the memory */
			FREE((caddr_t)new_pt, M_TEMP);
		}
	}
	
	return ( error );
}

/*****************************************************************************/

/*
 *
 */
static int webdav_vnop_lookup(struct vnop_lookup_args *ap)
/*
	struct vnop_lookup_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_dvp;
		vnode_t *a_vpp;
		struct componentname *a_cnp;
		vfs_context_t a_context;
	};
*/
{
	vnode_t dvp;
	vnode_t vp;
	vnode_t *vpp;
	struct componentname *cnp;
	struct webdavmount *fmp;
	int islastcn;
	int isdotdot;
	int isdot;
	int nameiop;
	int error;
	struct webdav_reply_lookup reply_lookup;
	struct webdavnode *pt;

	START_MARKER("webdav_vnop_lookup");

	vpp = ap->a_vpp;
	dvp = ap->a_dvp;
	cnp = ap->a_cnp;
	nameiop = cnp->cn_nameiop;
	
	fmp = VFSTOWEBDAV(vnode_mount(dvp));
	
	*vpp = NULLVP;
	islastcn = cnp->cn_flags & ISLASTCN;
	
	/*
	 * To print out the name being looked up, use:
	 * printf("webdav_vnop_lookup: %*s\n", cnp->cn_namelen, cnp->cn_nameptr);
	 */
	
	/* See if we're looking up dot or dotdot */
	if ( cnp->cn_flags & ISDOTDOT )
	{
		isdotdot = TRUE;
		isdot = FALSE;
	}
	else if ( (cnp->cn_nameptr[0] == '.') && (cnp->cn_namelen == 1) )
	{
		isdotdot = FALSE;
		isdot = TRUE;
	}
	else
	{
		isdotdot = isdot = FALSE;
	}
	
	if ( cnp->cn_namelen > fmp->pm_name_max )
	{
		error = ENAMETOOLONG;
	}
	else if ( !vnode_isdir(dvp) )
	{
		error = ENOTDIR;
	}
	else if ( isdotdot && (vnode_isvroot(dvp)))
	{
		printf("webdav_vnop_lookup: invalid '..' from root\n");
		error = EIO;
	}
	else if ( islastcn && vnode_vfsisrdonly(dvp) &&
			  (nameiop == DELETE || nameiop == RENAME) )
	{
		error = EROFS;
	}
	else
	{
		error = cache_lookup(dvp, vpp, cnp);
		switch ( error )
		{
			case -1: /* positive match */
				/* just use the vnode found in the cache -- other calls may get ENOENT but that's OK */
				error = 0;
				break;  /* with vpp set */
			
			case 0: /* no match in cache (or aged out) */
				if ( isdot || isdotdot )
				{
					/* synthesize the lookup reply for dot and dotdot */
					pt = isdot ? VTOWEBDAV(dvp) : VTOWEBDAV(VTOWEBDAV(dvp)->pt_parent);
					reply_lookup.obj_ref = pt->pt_obj_ref;
					reply_lookup.obj_fileid = pt->pt_fileid;
					reply_lookup.obj_type = WEBDAV_DIR_TYPE;
					reply_lookup.obj_atime = pt->pt_atime;
					reply_lookup.obj_mtime = pt->pt_mtime;
					reply_lookup.obj_ctime = pt-> pt_ctime;
					reply_lookup.obj_filesize = pt->pt_filesize;
				}
				else
				{
					error = webdav_lookup(ap, &reply_lookup);
					if ( error != 0 )
					{
						/*
						 * If we get here we didn't find the entry we were looking for. But
						 * that's ok if we are creating or renaming and are at the end of
						 * the pathname.
						 */
						if ( (nameiop == CREATE || nameiop == RENAME) && islastcn )
						{
							error = EJUSTRETURN;
						}
						else
						{
							/* the lookup failed, return ENOENT */
							error = ENOENT;
						}
						
						break;	/* break with error != 0 */
					}
				}
				
				/* the lookup was OK or wasn't needed (dot or dotdot) */

				if ( (nameiop == DELETE) && islastcn )
				{
					if ( isdot )
					{
						error = vnode_get(dvp);
						if ( error == 0 )
						{
							*vpp = dvp;
						}
						break;
					}
					else if ( isdotdot )
					{
						vp = VTOWEBDAV(dvp)->pt_parent;
						error = vnode_get(vp);
						if ( error == 0 )
						{
							*vpp = vp;
						}
						break;
					}
				}
				
				if ( (nameiop == RENAME) && islastcn )
				{
					if ( isdot )
					{
						error = EISDIR;
						break;
					}
					else if ( isdotdot )
					{
						vp = VTOWEBDAV(dvp)->pt_parent;
						error = vnode_get(vp);
						if ( error == 0 )
						{
							*vpp = vp;
						}
						break;
					}
				}

				if ( isdot )
				{
					error = vnode_get(dvp);
					if ( error == 0 )
					{
						*vpp = dvp;
					}
				}
				else if ( isdotdot )
				{
					vp = VTOWEBDAV(dvp)->pt_parent;
					error = vnode_get(vp);
					if ( error == 0 )
					{
						*vpp = vp;
					}
				}
				else
				{
					error = webdav_get(vnode_mount(dvp), dvp, 0, cnp, reply_lookup.obj_ref, reply_lookup.obj_fileid,
						(reply_lookup.obj_type == WEBDAV_FILE_TYPE) ? VREG : VDIR,
						reply_lookup.obj_atime, reply_lookup.obj_mtime, reply_lookup.obj_ctime,
						reply_lookup.obj_filesize, vpp);
				}
				break;
				
			case ENOENT: /* negative match */
				/* we don't use negative caching so break with error */
				printf("webdav_vnop_lookup: ENOENT response from cache_lookup\n");
				break;
				
			default: /* unexpected error */
				printf("webdav_vnop_lookup: unexpected response from cache_lookup: %d\n", error);
				break;
		}
	}
		
	RET_ERR("webdav_vnop_lookup", error);
}

/*****************************************************************************/

static int webdav_vnop_open(struct vnop_open_args *ap)
/*
	struct vnop_open_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		int a_mode;
		vfs_context_t a_context;
	};
*/
{
	struct webdavnode *pt;
	vnode_t vp;
	int error, server_error;
	struct webdavmount *fmp;
	struct open_associatecachefile associatecachefile;
	struct webdav_request_open request_open;
	struct webdav_reply_open reply_open;

	START_MARKER("webdav_vnop_open");

	vp = ap->a_vp;
	pt = VTOWEBDAV(vp);
	fmp = VFSTOWEBDAV(vnode_mount(vp));

	error = server_error = 0;

	/* If it is already open then just ref the node
	 * and go on about our business. Make sure to set
	 * the write status if this is read/write open
	 */
	if (pt->pt_cache_vnode)
	{
		/* increment the open count */
		++pt->pt_opencount;
		if ( pt->pt_opencount == 0 )
		{
			/* don't wrap -- return an error */
			--pt->pt_opencount;
			return ( ENFILE );
		}
		/* Set the "dir not loaded" bit if this is a directory, that way
		 * readdir will know that it needs to force a directory download
		 * even if the first call turns out not to be in the middle of the
		 * directory
		 */
		if (vnode_vtype(vp) == VDIR)
		{
			pt->pt_status |= WEBDAV_DIR_NOT_LOADED;
		}
		return (0);
	}
	
	request_open.ref = -1;  /* set to -1 so that webdav_release_ref() won't do anything */

	webdav_copy_creds(ap->a_context, &request_open.pcr);
	request_open.flags = OFLAGS(ap->a_mode);
	request_open.obj_ref = pt->pt_obj_ref;
	
	if ( !vnode_isreg(vp) && !vnode_isdir(vp) )
	{
		/* This should never happen, but just in case */
		error = EFTYPE;
		goto dealloc_done;
	}

	associatecachefile.pid = 0;
	associatecachefile.cachevp = NULLVP;
	error = webdav_assign_ref(&associatecachefile, &request_open.ref);
	if ( error )
	{
		printf("webdav_vnop_open: webdav_assign_ref didn't work\n");
		goto dealloc_done;
	}
	
	bzero(&reply_open, sizeof(struct webdav_reply_open));
	
	error = webdav_sendmsg(WEBDAV_OPEN, fmp,
		&request_open, sizeof(struct webdav_request_open), 
		NULL, 0, 
		&server_error, &reply_open, sizeof(struct webdav_reply_open));
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}
	
	if (error == 0)
	{
		if (reply_open.pid != associatecachefile.pid)
		{
			printf("webdav_vnop_open: openreply.pid (%d) != associatecachefile.pid (%d)\n",
				reply_open.pid, associatecachefile.pid);
			error = EPERM;
			goto dealloc_done;
		}
		pt->pt_cache_vnode = associatecachefile.cachevp;

		/* set the open count */
		pt->pt_opencount = 1;
		
		/* Set the "dir not loaded" bit if this is a directory, that way
		 * readdir will know that it needs to force a directory download
		 * even if the first call turns out not to be in the middle of the
		 * directory
		 */
		if (vnode_isdir(vp))
		{
			pt->pt_status |= WEBDAV_DIR_NOT_LOADED;
			/* default to not ask for and to not cache additional directory information */
			vnode_setnocache(vp);
		}
	}

dealloc_done:

	webdav_release_ref(request_open.ref);
		
	RET_ERR("webdav_vnop_open", error);
}

/*****************************************************************************/

/*
 * webdav_fsync
 *
 * webdav_fsync flushes dirty pages (if any) to the cache file and then if
 * the file is dirty, pushes it up to the server.
 *
 * Callers of this routine must ensure the file is a regular file and that
 * there's a cache vnode.
 *
 * results:
 *	0		Success.
 *	EIO		A physical I/O error has occurred, or this error was generated for
 *			implementation-defined reasons.
 *	ENOSPC	The server returned 507 Insufficient Storage (WebDAV)
 */
static int webdav_fsync(struct vnop_fsync_args *ap)
/*
	struct vnop_fsync_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		int a_waitfor;
		vfs_context_t a_context;
	};
*/
{
	struct webdavnode *pt;
	vnode_t vp;
	vnode_t cachevp;
	int error, server_error;
	struct webdavmount *fmp;
	struct vnode_attr attrbuf;
	struct webdav_request_fsync request_fsync;
	
	vp = ap->a_vp;
	pt = VTOWEBDAV(vp);
	cachevp = pt->pt_cache_vnode;
	fmp = VFSTOWEBDAV(vnode_mount(vp));
	error = server_error = 0;

	if ( !(pt->pt_status & WEBDAV_DIRTY) ||
		 (pt->pt_status & WEBDAV_DELETED) )
	{
		/* If it's not a file, or there is no pt_cache_vnode, or it's not dirty,
		 * or it's been deleted, we have nothing to tell the server to sync.
		 */
		error = 0;
		goto done;
	}

	/* make sure the file is completely downloaded from the server */
	do
	{
		VATTR_INIT(&attrbuf);
		VATTR_WANTED(&attrbuf, va_flags);
		VATTR_WANTED(&attrbuf, va_data_size);
		error = vnode_getattr(cachevp, &attrbuf, ap->a_context);
		if (error)
		{
			goto done;
		}

		if (attrbuf.va_flags & UF_NODUMP)
		{
			struct timespec ts;
			
			/* We are downloading the file and we haven't finished
			 * since the user process is going push the entire file
			 * back to the server, we'll have to wait until we have
			 * gotten all of it. Otherwise we will have inadvertantly
			 * pushed back an incomplete file and wiped out the original
			 */
			ts.tv_sec = 0;
			ts.tv_nsec = WEBDAV_WAIT_FOR_PAGE_TIME;
			error = msleep((caddr_t)&ts, NULL, PCATCH, "webdav_fsync", &ts);
			if ( error)
			{
				if ( error == EWOULDBLOCK )
				{
					error = 0;
				}
				else
				{
					printf("webdav_fsync: msleep(): %d\n", error);
					/* convert pseudo-errors to EIO */
					if ( error < 0 )
					{
						error = EIO;
					}
					goto done;
				}
			}
		}
		else
		{
			/* the file has been downloaded */
			if ( pt->pt_filesize != (off_t)attrbuf.va_data_size )
			{
				/* keep the ubc size up to date */
				(void) ubc_setsize(vp, attrbuf.va_data_size); /* ignore failures - nothing can be done */
				pt->pt_filesize = (off_t)attrbuf.va_data_size;
			}
			break;
		}
	} while ( TRUE );

	/*
	 * The ubc_sync_range() call can be expensive.
	 * There is a fixed cpu cost involved that is directly proportional
	 * to the size of file.
	 * For a webdav vnode, we do not cache any file data in the VM unless
	 * the file is mmap()ed. So if the file was never mapped, there is
	 * no need to call ubc_sync_range(). 
	 */
	if ( pt->pt_status & WEBDAV_WASMAPPED )
	{
		/* This is where we need to tell UBC to flush out all of
		 * the dirty pages for this vnode. If we do that then our write
		 * and pageout routines will get called if anything needs to
		 * be written.  That will cause the status to be dirty if
		 * it needs to be marked as such.
		 * Note: ubc_sync_range() returns 0 on error.
		 */
		off_t current_size;
		
		current_size = ubc_getsize(vp);
		if ( current_size != 0 )
		{
			if ( ubc_sync_range(vp, (off_t)0, current_size, UBC_PUSHDIRTY | UBC_SYNC) == 0 )
			{
#ifdef DEBUG
				printf("webdav_fsync: ubc_sync_range failed\n");
#endif
				error = EIO;
				goto done;
			}
		}
	}

	if ( attrbuf.va_flags & UF_APPEND )
	{
		/* If the UF_APPEND flag is set, there was an error downloading the file from the
		 * server, so exit with an EIO result.
		 */
		error = EIO;
		goto done;
	}

	/* At this point, the file is completely downloaded into cachevp.
	 * Locking cachevp isn't needed because webdavfs vnops are only writer
	 * to cachevp after it is downloaded.
	 */
	
	/* clear the dirty flag before pushing this to the server */
	pt->pt_status &= ~WEBDAV_DIRTY;
	
	webdav_copy_creds(ap->a_context, &request_fsync.pcr);
	request_fsync.obj_ref = pt->pt_obj_ref;

	error = webdav_sendmsg(WEBDAV_FSYNC, fmp,
		&request_fsync, sizeof(struct webdav_request_fsync), 
		NULL, 0, 
		&server_error, NULL, 0);
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}

done:

	return ( error );
}

/*****************************************************************************/

/*
 * webdav_vnop_fsync
 *
 * webdav_vnop_fsync flushes dirty pages (if any) to the cache file and then if
 * the file is dirty, pushes it up to the server.
 *
 * results:
 *	0		Success.
 *	EIO		A physical I/O error has occurred, or this error was generated for
 *			implementation-defined reasons.
 *	ENOSPC	The server returned 507 Insufficient Storage (WebDAV)
 */
static int webdav_vnop_fsync(struct vnop_fsync_args *ap)
/*
	struct vnop_fsync_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		int a_waitfor;
		vfs_context_t a_context;
	};
*/
{
	int error;
	
	START_MARKER("webdav_vnop_fsync");
	
	if ( (VTOWEBDAV(ap->a_vp)->pt_cache_vnode != NULLVP) && vnode_isreg(ap->a_vp) )
	{
		error = webdav_fsync(ap);
	}
	else
	{
		error = 0;
	}

	
	RET_ERR("webdav_vnop_fsync", error);
}

/*****************************************************************************/

/*
	webdav_close_mnomap is the common subroutine used by webdav_vnop_close and
	webdav_vnop_mnomap to synchronize the file with the server if needed, and
	release the cache vnode if needed.
	results
		0		no error
		EIO		an I/O error occurred
		ENOSPC	No space left on device/server (from fsync)
*/
static
int webdav_close_mnomap(vnode_t vp, vfs_context_t context, int force_fsync)
{
	int error;
	int server_error;
	int fsync_error;
	struct webdavnode *pt;

	pt = VTOWEBDAV(vp);
	
	/* no errors yet */
	error = server_error = fsync_error = 0;
	
	/* If there is no cache file, then we have nothing to tell the server to close. */
	if ( pt->pt_cache_vnode != NULLVP )
	{
		/*
		 * If this is a file, synchronize the file with the server (if needed).
		 * Always call webdav_fsync on close requests with write access (force_fsync)
		 * and when the file is not open (at last close, and from mnomap after
		 * last close).
		 */
		if ( vnode_isreg(vp) && (force_fsync || (pt->pt_opencount == 0)) )
		{
			struct vnop_fsync_args fsync_args;
			
			fsync_args.a_vp = vp;
			fsync_args.a_waitfor = TRUE;
			fsync_args.a_context = context;
			fsync_error = webdav_fsync(&fsync_args);
		}
		
		/*
		 * We return errors from fsync no matter what since errors from fsync
		 * mean the data was not correctly written in userland.
		 */
		 
		/* If this the last close and we're not mapped, tell mount_webdav */
		if ( (pt->pt_opencount == 0) && !(pt->pt_status & WEBDAV_ISMAPPED) )
		{
			struct webdav_request_close request_close;
			vnode_t temp;
			
			webdav_copy_creds(context, &request_close.pcr);
			request_close.obj_ref = pt->pt_obj_ref;

			error = webdav_sendmsg(WEBDAV_CLOSE, VFSTOWEBDAV(vnode_mount(vp)),
				&request_close, sizeof(struct webdav_request_close), 
				NULL, 0, 
				&server_error, NULL, 0);
			if ( (error == 0) && (server_error != 0) )
			{
				error = server_error;
			}

			/* zero out pt_cache_vnode and then release the cache vnode */
			temp = pt->pt_cache_vnode;
			pt->pt_cache_vnode = NULLVP;
			vnode_rele(temp);   /* reference taken in webdav_sysctl() */
		}
	}
	else
	{
		/* no cache file. Why? */
		printf("webdav_close_mnomap: no cache file\n");
	}
	
	/* report any errors */
	if ( error == 0 )
	{
		/* fsync errors are more important to report than close errors */
		if ( fsync_error != 0 )
		{
			error = fsync_error;
		}
	}
	
	RET_ERR("webdav_close_mnomap", error);
}

/*****************************************************************************/

/*
 * webdav_vnop_close
 */
static int webdav_vnop_close(struct vnop_close_args *ap)
/*
	struct vnop_close_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		int a_fflag;
		vfs_context_t a_context;
	};
*/
{
	struct webdavnode *pt;
	int force_fsync;

	START_MARKER("webdav_vnop_close");
	
	pt = VTOWEBDAV(ap->a_vp);
	/*
	 * If this is a file and the corresponding open had write access,
	 * synchronize the file with the server (if needed). This must be done from
	 * close because this is the last chance we have to return an error to
	 * the file system client.
	 */
	force_fsync = (((ap->a_fflag & FWRITE) != 0) && vnode_isreg(ap->a_vp));
	
	/* decrement the open count */
	--pt->pt_opencount;
	
	RET_ERR("webdav_vnop_close", webdav_close_mnomap(ap->a_vp, ap->a_context, force_fsync));
}

/*****************************************************************************/

static int webdav_vnop_mmap(struct vnop_mmap_args *ap)
/*
	struct vnop_mmap_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		int a_fflags;
		vfs_context_t a_context;
	};
*/
{
	/* mark this file as mapped */
	START_MARKER("webdav_vnop_mmap");

	VTOWEBDAV(ap->a_vp)->pt_status |= (WEBDAV_ISMAPPED | WEBDAV_WASMAPPED);
	
	RET_ERR("webdav_vnop_mmap", 0);
}

/*****************************************************************************/

static int webdav_vnop_mnomap(struct vnop_mnomap_args *ap)
/*
	struct vnop_mnomap_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		vfs_context_t a_context;
	};
*/
{
	START_MARKER("webdav_vnop_mnomap");
	
	/* mark this file as unmapped */
	VTOWEBDAV(ap->a_vp)->pt_status &= ~WEBDAV_ISMAPPED;
	
	/* if the file is not open, this will fsync it and close it in user-land */
	RET_ERR("webdav_vnop_mnomap", webdav_close_mnomap(ap->a_vp, ap->a_context, FALSE));
}

/*****************************************************************************/

/*
 * webdav_read_bytes
 *
 * webdav_read_bytes is called by webdav_vnop_read and webdav_vnop_pagein to
 * read bytes directly from the server when we haven't yet downloaded the
 * part of the file needed to retrieve the data. If this routine returns an
 * error, then the caller will just spin wait for the part of the file needed
 * to be downloaded.
 *
 * results:
 * 0	no error - bytes were read
 * !0	the bytes were not read and the caller must wait for the download
 *
 * To do:
 *		Pass in current file size so that this routine can compare against
 *		WEBDAV_WAIT_IF_WITHIN instead of the caller.
 */
static int webdav_read_bytes(vnode_t vp, uio_t a_uio, vfs_context_t context)
{
	int error;
	int server_error;
	struct webdavnode *pt;
	void *buffer;
	struct webdavmount *fmp;
	struct webdav_request_read request_read;

	pt = VTOWEBDAV(vp);
	error = server_error = 0;
	fmp = VFSTOWEBDAV(vnode_mount(vp));

	/* don't bother if the range starts at offset 0,
	 * or if the read is too big for an out-of-band read.
	 */
	if ( (uio_offset(a_uio) == 0) ||
		 (uio_resid(a_uio) > WEBDAV_MAX_IO_BUFFER_SIZE) )
	{
		/* return an error so the caller will wait */
		error = EINVAL;
		goto done;
	}

	/* Now allocate the buffer that we are going to use to hold the data that
	 * comes back
	 */
	request_read.count = uio_resid(a_uio); /* this won't overflow because we've already checked uio_resid's size */
	MALLOC(buffer, void *, request_read.count, M_TEMP, M_WAITOK);

	webdav_copy_creds(context, &request_read.pcr);
	request_read.obj_ref = pt->pt_obj_ref;
	request_read.offset = uio_offset(a_uio);

	error = webdav_sendmsg(WEBDAV_READ, fmp,
		&request_read, sizeof(struct webdav_request_read), 
		NULL, 0, 
		&server_error, buffer, request_read.count);
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}
	if (error)
	{
		/* return an error so the caller will wait */
		goto dealloc_done;
	}

	error = uiomove((caddr_t)buffer, request_read.count, a_uio);

dealloc_done:

	FREE((void *)buffer, M_TEMP);

done:

	return ( error );
}

/*****************************************************************************/

/*
 * webdav_rdwr
 *
 * webdav_rdwr is called by webdav_vnop_read and webdav_vnop_write. What webdav_vnop_read and
 * webdav_vnop_write need to do is so similar that a common subroutine can be used
 * for both. The "reading" flag is used in the few places where read and write
 * code is different.
 *
 * results:
 *	0		Success.
 *	EISDIR	Tried to read a directory.
 *	EIO		A physical I/O error has occurred, or this error was generated for
 *			implementation-defined reasons.
 */
static int webdav_rdwr(struct vnop_read_args *ap)
/*
	struct vnop_read_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		struct uio *a_uio;
		int a_ioflag;
		vfs_context_t a_context;
	};
*/
{
	struct webdavnode *pt;
	vnode_t cachevp;
	int error;
	upl_t upl;
	uio_t in_uio;
	struct vnode_attr attrbuf;
	off_t total_xfersize;
	kern_return_t kret;
	vnode_t vp;
	int mapped_upl;
	int file_changed;
	int reading;
	int tried_bytes;

	vp = ap->a_vp;
	pt = VTOWEBDAV(vp);
	cachevp = pt->pt_cache_vnode;
	in_uio = ap->a_uio;
	reading = (uio_rw(in_uio) == UIO_READ);
	file_changed = FALSE;
	total_xfersize = 0;
	tried_bytes = FALSE;

	/* make sure this is not a directory */
	if ( vnode_isdir(vp) )
	{
		error = EISDIR;
		goto exit;
	}
	
	/* make sure there's a cache file vnode associated with the webdav vnode */
	if ( cachevp == NULLVP )
	{
#ifdef DEBUG
		printf("webdav_rdwr: about to %s a uncached vnode\n", (reading ? "read from" : "write to"));
#endif
		error = EIO;
		goto exit;
	}
	
	if ( reading )
	{
		/* we've access the file */
		pt->pt_status |= WEBDAV_ACCESSED;
	}

	/* Start the sleep loop to wait on the background download. We will know that the webdav user
	 * process is finished when it either clears the nodump flag or sets the append only flag
	 * (indicating an error)
	 */
	do
	{
		off_t rounded_iolength;
		
		/* get the cache file's size and va_flags */
		VATTR_INIT(&attrbuf);
		VATTR_WANTED(&attrbuf, va_flags);
		VATTR_WANTED(&attrbuf, va_data_size);
		error = vnode_getattr(cachevp, &attrbuf, ap->a_context);
		if ( error )
		{
			goto unlock_exit;
		}

		/* Don't attempt I/O until either:
		 *    (a) the page containing the end of the I/O is in has been downloaded, or
		 *    (b) the entire file has been downloaded.
		 * This ensures we don't read partially downloaded data, or write into a
		 * a portion of the file that is still being downloaded.
		 */
		rounded_iolength = (off_t)round_page_64(uio_offset(in_uio) + uio_resid(in_uio));

		if ( (attrbuf.va_flags & UF_NODUMP) && (rounded_iolength > (off_t)attrbuf.va_data_size) )
		{
			struct timespec ts;
			
			/* We are downloading the file and we haven't gotten to
			 * to the bytes we need so sleep, and then check again.
			 */
			
			/* if reading, we may be able to read the part of the file we need out-of-band */
			if ( reading )
			{
				if ( !tried_bytes )
				{
					if ( rounded_iolength > ((off_t)attrbuf.va_data_size + WEBDAV_WAIT_IF_WITHIN) )
					{
						/* We aren't close to getting to the part of the file that contains
						 * the data we want so try to ask the server for the bytes
						 * directly. If that does not work, wait until the stuff gets down.
						 */
						error = webdav_read_bytes(vp, in_uio, ap->a_context);
						if ( !error )
						{
							/* we're done */
							goto exit;
						}
					}
					/* If we are here, we must have failed to get the bytes so return and
					* set tried_bytes so we won't attempt that again and sleep */
					tried_bytes = TRUE;
				}
			}
			
			/* sleep for a bit */
			ts.tv_sec = 0;
			ts.tv_nsec = WEBDAV_WAIT_FOR_PAGE_TIME;
			error = msleep((caddr_t)&ts, NULL, PCATCH, "webdav_rdwr", &ts);
			if ( error)
			{
				if ( error == EWOULDBLOCK )
				{
					error = 0;
				}
				else
				{
					printf("webdav_rdwr: msleep(): %d\n", error);
					/* convert pseudo-errors to EIO */
					if ( error < 0 )
					{
						error = EIO;
					}
					goto exit;
				}
			}
		}
		else
		{
			/* the part we need has been downloaded */
			if ( pt->pt_filesize < (off_t)attrbuf.va_data_size )
			{
				(void) ubc_setsize(vp, attrbuf.va_data_size); /* ignore failures - nothing can be done */
				pt->pt_filesize = (off_t)attrbuf.va_data_size;
			}
			break; /* out of while (TRUE) loop */
		}
	} while ( TRUE );

	if ( attrbuf.va_flags & UF_APPEND )
	{
		/* If the UF_APPEND flag is set, there was an error downloading the file from the
		 * server, so exit with an EIO result.
		 */
		error = EIO;
		goto unlock_exit;
	}

	/* At this point, cachevp is locked and either the file is completely downloaded into
	 * cachevp, or the page this I/O ends within has been completely downloaded into cachevp.
	 */
	
	/* Determine the total_xfersize. Reads must be within the current file;
	 * Writes can extend the file.
	 */
	if ( reading )
	{
		/* pin total_xfersize to EOF */
		if ( uio_offset(in_uio) > (off_t)attrbuf.va_data_size )
		{
			total_xfersize = 0;
		}
		else
		{
			total_xfersize = MIN(uio_resid(in_uio), ((off_t)attrbuf.va_data_size - uio_offset(in_uio)));
			/* make sure total_xfersize isn't negative */
			if ( total_xfersize < 0 )
			{
				total_xfersize = 0;
			}
		}
	}
	else
	{
		/* get total_xfersize and make sure it isn't negative */
		total_xfersize = (uio_resid(in_uio) < 0) ? (0) : (uio_resid(in_uio));
	}
	
	/*
	 * For a webdav vnode, we do not cache any file data in the VM unless
	 * the file is mmap()ed. So if the file was never mapped, there is
	 * no need to create a upl, scan for valid pages, etc, and the VOP_READ/WRITE
	 * to cachevp can handle the request completely.
	 */
	if ( pt->pt_status & WEBDAV_WASMAPPED )
	{
		/* If the ubc info exists we may need to get some or all of the data
		 * from mapped pages. */
				
		/* loop until total_xfersize has been transferred or error occurs */
		while ( total_xfersize > 0 )
		{
			int currentPage;
			int pagecount;
			int pageOffset;
			int xfersize;
			vm_offset_t addr;
			upl_page_info_t *pl;
			
			/* Determine the offset into the first page and how much to transfer this time.
			 * xfersize will be total_xfersize or as much as possible ending on a page boundary.
			 */
			pageOffset = uio_offset(in_uio) & PAGE_MASK;
			xfersize = MIN(total_xfersize, MAXBSIZE - pageOffset);
			
			/* create the upl so that we "own" the pages */
			kret = ubc_create_upl(vp,
				(vm_object_offset_t) trunc_page_64(uio_offset(in_uio)),
				(vm_size_t) round_page_32(xfersize + pageOffset),
				&upl,
				&pl,
				UPL_FLAGS_NONE);
            if ( kret != KERN_SUCCESS )
			{
#ifdef DEBUG
				printf("webdav_rdwr: ubc_create_upl failed %d\n", kret);
#endif
                error = EIO;
                goto unlock_exit;
            }
			
			/* Scan pages looking for valid/invalid ranges of pages.
			 * uiomove() the ranges of valid pages; VOP_READ/WRITE the ranges of invalid pages.
			 */
			mapped_upl = FALSE;
			currentPage = 0;
			pagecount = atop_32(pageOffset + xfersize - 1) + 1;
			while ( currentPage < pagecount )
			{
				int firstPageOfRange;
				int lastPageOfRange;
				int rangeIsValid;
				int requestSize;
				
				firstPageOfRange = lastPageOfRange = currentPage;
				rangeIsValid = upl_valid_page(pl, firstPageOfRange);
				++currentPage;
				
				/* find last page with same state */
				while ( (currentPage < pagecount) && (upl_valid_page(pl, currentPage) == rangeIsValid) )
				{
					lastPageOfRange = currentPage;
					++currentPage;
				}
				
				/* determine how much to uiomove() or VOP_READ() for this range of pages */
				requestSize = MIN(xfersize, (int)(ptoa_32(lastPageOfRange - firstPageOfRange + 1) - pageOffset));
				
				if ( rangeIsValid )
				{
					/* range is valid, uiomove it */
					
					/* map the upl the first time we need it mapped */
					if ( !mapped_upl )
					{
						kret = ubc_upl_map(upl, &addr);
						if ( kret != KERN_SUCCESS )
						{
#ifdef DEBUG
							printf("webdav_rdwr: ubc_upl_map failed %d\n", kret);
#endif
							error = EIO;
							goto unmap_unlock_exit;
						}
						mapped_upl = TRUE;
					}
					
					/* uiomove the the range firstPageOfRange through firstPageOfRange */
					error = uiomove((caddr_t)(addr + ptoa_32(firstPageOfRange) + pageOffset),
						requestSize,
						in_uio);
					if ( error )
					{
						goto unmap_unlock_exit;
					}
				}
				else
				{
					/* range is invalid, VOP_READ/WRITE it from the the cache file */
					user_ssize_t remainingRequestSize;
					
					/* subtract requestSize from uio_resid and save */
					remainingRequestSize = uio_resid(in_uio) - requestSize;
					
					/* adjust size of read */
					uio_setresid(in_uio, requestSize);
					
					if ( reading )
					{
						/* read it from the cache file */
						error = VNOP_READ(cachevp, in_uio, 0 /* no flags */, ap->a_context);
					}
					else
					{
						/* write it to the cache file */
						error = VNOP_WRITE(cachevp, in_uio, 0 /* no flags */, ap->a_context);
					}
					
					if ( error || (uio_resid(in_uio) != 0) )
					{
						uio_setresid(in_uio, remainingRequestSize + uio_resid(in_uio));
						goto unmap_unlock_exit;
					}
					
					/* set remaining uio_resid */
					uio_setresid(in_uio, remainingRequestSize);
				}
				
				if ( !reading )
				{
					/* after the write to the cache file has been completed, mark the file dirty */
					pt->pt_status |= WEBDAV_DIRTY;
					file_changed = TRUE;
				}
				
				/* set pageOffset to zero (which it will be if we need to loop again)
				 * and decrement xfersize and total_xfersize by requestSize.
				 */
				pageOffset = 0;
				xfersize -= requestSize;
				total_xfersize -= requestSize;
			}
			
			/* unmap the upl if needed */
			if ( mapped_upl )
			{
				kret = ubc_upl_unmap(upl);
				if (kret != KERN_SUCCESS)
				{
					panic("webdav_rdwr: ubc_upl_unmap() failed with (%d)", kret);
				}
			}
			
			/* get rid of the upl */
			kret = ubc_upl_abort(upl, 0);
			if ( kret != KERN_SUCCESS )
			{
#ifdef DEBUG
				printf("webdav_rdwr: ubc_upl_map failed %d\n", kret);
#endif
				error = EIO;
                goto unlock_exit;
			}
			
		}	/* end while loop */
	}
	else
	{
		/* No UBC, or was never mapped */
		if ( reading )
		{
			/* pass the read along to the underlying cache file */
			error = VNOP_READ(cachevp, in_uio, ap->a_ioflag, ap->a_context);
		}
		else
		{
			/* pass the write along to the underlying cache file */
			error = VNOP_WRITE(cachevp, in_uio, ap->a_ioflag, ap->a_context);
	
			/* after the write to the cache file has been completed... */
			pt->pt_status |= WEBDAV_DIRTY;
			file_changed = TRUE;
		}
	}

unlock_exit:

	if ( !reading )
	{
		/* Note: the sleep loop	at the top of this function ensures that the file can grow only
		 * if the file is completely downloaded.
		 */
		if ( file_changed &&							/* if the file changed */
			 (uio_offset(in_uio) > (off_t)attrbuf.va_data_size) )	/* and the file grew */
		{
			/* make sure the cache file's size is correct */
			struct vnode_attr vattr;
			
			/* set the size of the cache file */
			VATTR_INIT(&vattr);
			VATTR_SET(&vattr, va_data_size, uio_offset(in_uio));
			error = vnode_setattr(cachevp, &vattr, ap->a_context);
			
			/* let the UBC know the new size */
			if ( error == 0 )
			{
				(void) ubc_setsize(vp, uio_offset(in_uio)); /* ignore failures - nothing can be done */
			}
		}
	}
	
exit:

	return ( error );

unmap_unlock_exit:

	/* unmap the upl if it's mapped */
	if ( mapped_upl )
	{
		kret = ubc_upl_unmap(upl);
		if (kret != KERN_SUCCESS)
		{
			panic("webdav_rdwr: ubc_upl_unmap() failed with (%d)", kret);
		}
	}

	/* get rid of the upl */
	kret = ubc_upl_abort(upl, UPL_ABORT_ERROR);
	if ( kret != KERN_SUCCESS )
	{
#ifdef DEBUG
		printf("webdav_rdwr: ubc_upl_abort failed %d\n", kret);
#endif
		if (!error)
		{
			error = EIO;
		}
	}
	
	goto unlock_exit;
}

/*****************************************************************************/

/*
 * webdav_vnop_read
 *
 * webdav_vnop_read calls webdav_rdwr to do the work.
 */
static int webdav_vnop_read(struct vnop_read_args *ap)
/*
	struct vnop_read_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		struct uio *a_uio;
		int a_ioflag;
		vfs_context_t a_context;
	};
*/
{
	START_MARKER("webdav_vnop_read");
	
	RET_ERR("webdav_vnop_read", webdav_rdwr(ap));
}

/*****************************************************************************/

/*
 * webdav_vnop_write
 *
 * webdav_vnop_write calls webdav_rdwr to do the work.
 */
static int webdav_vnop_write(struct vnop_write_args *ap)
/*
	struct vnop_write_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		struct uio *a_uio;
		int a_ioflag;
		vfs_context_t a_context;
	};
*/
{
	START_MARKER("webdav_vnop_write");
	
	RET_ERR("webdav_vnop_write", webdav_rdwr((struct vnop_read_args *)ap));
}

/*****************************************************************************/

/*
 * webdav_vnop_getattr
 *
 * webdav_vnop_getattr returns the most up-to-date vattr information.
 *
 * results:
 *	0		Success.
 *	EIO		A physical I/O error has occurred, or this error was generated for
 *			implementation-defined reasons.
 */
static int webdav_vnop_getattr(struct vnop_getattr_args *ap)
/*
	struct vnop_getattr_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	};
*/
{
	vnode_t vp;
	vnode_t cachevp;
	struct vnode_attr *vap;
	struct vnode_attr cache_vap;
	struct webdavnode *pt;
	struct webdavmount *fmp;
	int error;
	int server_error;
	int cache_vap_valid;
	int callServer;
	struct webdav_request_getattr request_getattr;
	struct webdav_reply_getattr reply_getattr;
		
	START_MARKER("webdav_vnop_getattr");

	vp = ap->a_vp;
	vap = ap->a_vap;
	fmp = VFSTOWEBDAV(vnode_mount(vp));
	pt = VTOWEBDAV(vp);
	cachevp = pt->pt_cache_vnode;
	error = server_error = 0;
	
	/* get everything we need out of vp and related structures before
	 * making any blocking calls where vp could go away.
	 */
	/* full access for everyone - let the server decide what can really be done */
	VATTR_RETURN(vap, va_mode, S_IRUSR | S_IWUSR | S_IXUSR |	/* owner */
				   S_IRGRP | S_IWGRP | S_IXGRP |	/* group */
				   S_IROTH | S_IWOTH | S_IXOTH |	/* other */
				   (vnode_isdir(vp) ? S_IFDIR : S_IFREG));
	/* Why 1 for va_nlink?
	 * Getting the real link count for directories is expensive.
	 * Setting it to 1 lets FTS(3) (and other utilities that assume
	 * 1 means a file system doesn't support link counts) work.
	 */
	VATTR_RETURN(vap, va_nlink, 1);
	VATTR_RETURN(vap, va_uid, UNKNOWNUID);
	VATTR_RETURN(vap, va_gid, UNKNOWNUID);
	VATTR_RETURN(vap, va_fsid, vfs_statfs(vnode_mount(vp))->f_fsid.val[0]);
	VATTR_RETURN(vap, va_fileid, pt->pt_fileid);
	VATTR_RETURN(vap, va_access_time, pt->pt_atime);
	VATTR_RETURN(vap, va_modify_time, pt->pt_mtime);
	VATTR_RETURN(vap, va_change_time, pt->pt_ctime);
	VATTR_RETURN(vap, va_gen, 0);
	VATTR_RETURN(vap, va_flags, 0);
	VATTR_RETURN(vap, va_rdev, 0);
	VATTR_RETURN(vap, va_filerev, 0);
	
	bzero(&reply_getattr, sizeof(struct webdav_reply_getattr));
	
	if ( (vnode_isreg(vp)) && (cachevp != NULLVP) )
	{
		/* vp is a file and there's a cache file.
		 * Get the cache file's information since it is the latest.
		 */
		VATTR_INIT(&cache_vap);
		VATTR_WANTED(&cache_vap, va_flags);
		VATTR_WANTED(&cache_vap, va_data_size);
		VATTR_WANTED(&cache_vap, va_total_alloc);
		VATTR_WANTED(&cache_vap, va_iosize);
		VATTR_WANTED(&cache_vap, va_access_time);
		VATTR_WANTED(&cache_vap, va_modify_time);
		VATTR_WANTED(&cache_vap, va_change_time);
		error = vnode_getattr(cachevp, &cache_vap, ap->a_context);
		if (error)
		{
			printf("webdav_vnop_getattr: cachevp: %d\n", error);
			goto bad;
		}
				
		cache_vap_valid = TRUE;
		
		/* if the cache file is not complete or the download failed, we still need to call the server */
		if ( (cache_vap.va_flags & UF_NODUMP) || (cache_vap.va_flags & UF_APPEND) )
		{
			callServer = !(pt->pt_status & WEBDAV_DELETED);
		}
		else
		{
			callServer = FALSE;
		}
	}
	else
	{
		cache_vap_valid = FALSE;
		callServer = !(pt->pt_status & WEBDAV_DELETED);
	}
	
	if ( callServer )
	{
		/* get the server file's information */
		webdav_copy_creds(ap->a_context, &request_getattr.pcr);
		request_getattr.obj_ref = pt->pt_obj_ref;
		
		error = webdav_sendmsg(WEBDAV_GETATTR, fmp,
			&request_getattr, sizeof(struct webdav_request_getattr), 
			NULL, 0, 
			&server_error, &reply_getattr, sizeof(struct webdav_reply_getattr));
		if ( (error == 0) && (server_error != 0) )
		{
			error = server_error;
		}
		if (error)
		{
			goto bad;
		}
		
		/* use the server file's size info */
		VATTR_RETURN(vap, va_data_size, reply_getattr.obj_attr.st_size);
		VATTR_RETURN(vap, va_total_alloc, reply_getattr.obj_attr.st_blocks * S_BLKSIZE);
		VATTR_RETURN(vap, va_iosize, reply_getattr.obj_attr.st_blksize);
	}
	else
	{
		/* use the cache file's size info */
		VATTR_RETURN(vap, va_data_size, cache_vap.va_data_size);
		VATTR_RETURN(vap, va_total_alloc, cache_vap.va_total_alloc);
		VATTR_RETURN(vap, va_iosize, cache_vap.va_iosize);
	}
	
	if ( cache_vap_valid )
	{
		/* use the time stamps from the cache file if needed */
		if ( pt->pt_status & WEBDAV_DIRTY )
		{
			VATTR_RETURN(vap, va_access_time, cache_vap.va_access_time);
			VATTR_RETURN(vap, va_modify_time, cache_vap.va_modify_time);
			VATTR_RETURN(vap, va_change_time, cache_vap.va_change_time);
		}
		else if ( (pt->pt_status & WEBDAV_ACCESSED) )
		{
			/* Though we have not dirtied the file, we have accessed it so
			 * grab the cache file's access time.
			 */
			VATTR_RETURN(vap, va_access_time, cache_vap.va_access_time);
		}
	}
	else
	{
		/* no cache file, so use reply_getattr.obj_attr for times if possible */
		if ( reply_getattr.obj_attr.st_atimespec.tv_sec != 0 )
		{
			/* use the server times if they were returned (if the getlastmodified
			 * property isn't returned by the server, reply_getattr.obj_attr.va_atime will be 0)
			 */
			VATTR_RETURN(vap, va_access_time, reply_getattr.obj_attr.st_atimespec);
			VATTR_RETURN(vap, va_modify_time, reply_getattr.obj_attr.st_mtimespec);
			VATTR_RETURN(vap, va_change_time, reply_getattr.obj_attr.st_ctimespec);
		}
		else
		{
			/* otherwise, use the current time */
			nanotime(&vap->va_access_time);
			VATTR_SET_SUPPORTED(vap, va_access_time);
			VATTR_RETURN(vap, va_modify_time, vap->va_access_time);
			VATTR_RETURN(vap, va_change_time, vap->va_access_time);
		}
	}

bad:

	RET_ERR("webdav_vnop_getattr", error);
}

/*****************************************************************************/

/*
 * webdav_vnop_remove
 *
 * webdav_vnop_remove removes a file.
 *
 * results:
 *	0		Success.
 *	EBUSY	Caller requested Carbon delete semantics and file was open.
 *	EIO		A physical I/O error has occurred, or this error was generated for
 *			implementation-defined reasons.
 */
static int webdav_vnop_remove(struct vnop_remove_args *ap)
/*
	struct vnop_remove_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_dvp;
		vnode_t a_vp;
		struct componentname *a_cnp;
		int a_flags;
		vfs_context_t a_context;
	};
*/
{
	vnode_t vp;
	vnode_t dvp;
	struct webdavnode *pt;
	struct webdavmount *fmp;
	int error;
	int server_error;
	struct webdav_request_remove request_remove;
	
	START_MARKER("webdav_vnop_remove");

	vp = ap->a_vp;
	dvp = ap->a_dvp;
	fmp = VFSTOWEBDAV(vnode_mount(vp));
	pt = VTOWEBDAV(vp);
	error = server_error = 0;

    if (vnode_isdir(vp))
    {
        error = EPERM;
        goto bad;
    }
	
	/* If caller requested Carbon delete semantics and the file is in use, return EBUSY */
	if ( (ap->a_flags & VNODE_REMOVE_NODELETEBUSY) && vnode_isinuse(vp, 0) )
	{
		error = EBUSY;
		goto bad;
	}
	
	cache_purge(vp);

	webdav_copy_creds(ap->a_context, &request_remove.pcr);
	request_remove.obj_ref = pt->pt_obj_ref;

	error = webdav_sendmsg(WEBDAV_REMOVE, fmp,
		&request_remove, sizeof(struct webdav_request_remove), 
		NULL, 0, 
		&server_error, NULL, 0);
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}
	if (error)
	{
		goto bad;
	}

	/* Get the node off of the cache so that other lookups
	 * won't find it and think the file still exists
	 */
	pt->pt_status |= WEBDAV_DELETED;
	(void) vnode_recycle(vp); /* we don't care if the recycle was done or not */

bad:

	RET_ERR("webdav_vnop_remove", error);
}

/*****************************************************************************/

/*
 * webdav_vnop_rmdir
 *
 * webdav_vnop_rmdir removes a directory.
 *
 * results:
 *	0		Success.
 *	ENOTEMPTY Directory was not empty.
 *	EIO		A physical I/O error has occurred, or this error was generated for
 *			implementation-defined reasons.
 */
static int webdav_vnop_rmdir(struct vnop_rmdir_args *ap)
/*
	struct vnop_rmdir_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_dvp;
		vnode_t a_vp;
		struct componentname *a_cnp;
		vfs_context_t a_context;
	};
*/
{
	vnode_t vp;
	vnode_t dvp;
	struct webdavnode *pt;
	struct webdavmount *fmp;
	int error;
	int server_error;
	struct webdav_request_rmdir request_rmdir;
	
	START_MARKER("webdav_vnop_rmdir");

	vp = ap->a_vp;
	dvp = ap->a_dvp;
	pt = VTOWEBDAV(vp);
	fmp = VFSTOWEBDAV(vnode_mount(vp));
	error = server_error = 0;
	
	/* No rmdir "." please. */
	if ( pt == VTOWEBDAV(dvp) )
	{
		error = EINVAL;
		goto bad;
	}
	
	cache_purge(vp);

	webdav_copy_creds(ap->a_context, &request_rmdir.pcr);
	request_rmdir.obj_ref = pt->pt_obj_ref;

	error = webdav_sendmsg(WEBDAV_RMDIR, fmp,
		&request_rmdir, sizeof(struct webdav_request_rmdir), 
		NULL, 0, 
		&server_error, NULL, 0);
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}
	if (error)
	{
		goto bad;
	}

	/* Get the node off of the cache so that other lookups
	 * won't find it and think the file still exists
	 */
	pt->pt_status |= WEBDAV_DELETED;
	(void) vnode_recycle(vp); /* we don't care if the recycle was done or not */


bad:

	RET_ERR("webdav_vnop_rmdir", error);
}

/*****************************************************************************/

static int webdav_vnop_create(struct vnop_create_args *ap)
/*
	struct vnop_create_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_dvp;
		vnode_t *a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	};
*/
{
	struct componentname *cnp = ap->a_cnp;
	vnode_t *vpp = ap->a_vpp;
	vnode_t dvp = ap->a_dvp;
	struct webdavmount *fmp;
	int error = 0;
	int server_error = 0;
	struct timeval tv;
	struct timespec ts;
	struct webdav_request_create request_create;
	struct webdav_reply_create reply_create;
	
	START_MARKER("webdav_vnop_create");

	fmp = VFSTOWEBDAV(vnode_mount(dvp));

	/* Set *vpp to Null for error checking purposes */
	*vpp = NULL;

	/* We don't support special files so make sure this is a regular file */
	if (ap->a_vap->va_type != VREG)
	{
		error = ENOTSUP;
		goto bad;
	}

	webdav_copy_creds(ap->a_context, &request_create.pcr);
	request_create.dir = VTOWEBDAV(dvp)->pt_obj_ref;
	request_create.mode = ap->a_vap->va_mode;
	request_create.name_length = cnp->cn_namelen;

	bzero(&reply_create, sizeof(struct webdav_reply_create));
	
	error = webdav_sendmsg(WEBDAV_CREATE, fmp,
		&request_create, offsetof(struct webdav_request_create, name), 
		cnp->cn_nameptr, cnp->cn_namelen,
		&server_error, &reply_create, sizeof(struct webdav_reply_create));
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}
	if (error)
	{
		goto bad;
	}
	
	/* parent directory changed so force readdir to reload */
	VTOWEBDAV(dvp)->pt_status |= WEBDAV_DIR_NOT_LOADED;

	microtime(&tv);
	TIMEVAL_TO_TIMESPEC(&tv, &ts);
	
	error = webdav_get(vnode_mount(dvp), dvp, 0, cnp,
		reply_create.obj_ref, reply_create.obj_fileid, VREG, ts, ts, ts, 0, vpp);
	if (error)
	{
		/* nothing we can do except complain */
		printf("webdav_vnop_create: webdav_get failed\n");
	}
	
bad:

	RET_ERR("webdav_vnop_create", error);
}

/*****************************************************************************/

static int webdav_vnop_rename(struct vnop_rename_args *ap)
/*
	struct vnop_rename_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_fdvp;
		vnode_t a_fvp;
		struct componentname *a_fcnp;
		vnode_t a_tdvp;
		vnode_t a_tvp;
		struct componentname *a_tcnp;
		vfs_context_t a_context;
	};
*/
{
	vnode_t fvp = ap->a_fvp;
	vnode_t tvp = ap->a_tvp;
	vnode_t tdvp = ap->a_tdvp;
	vnode_t fdvp = ap->a_fdvp;
	struct componentname *tcnp;
	int error = 0, server_error = 0;
	struct webdav_request_rename request_rename;

	START_MARKER("webdav_vnop_rename");

	fvp = ap->a_fvp;
	tvp = ap->a_tvp;
	tdvp = ap->a_tdvp;
	fdvp = ap->a_fdvp;
	tcnp = ap->a_tcnp;
	
	cache_purge(fvp);
	
	webdav_copy_creds(ap->a_context, &request_rename.pcr);	
	request_rename.from_dir_ref = VTOWEBDAV(fdvp)->pt_obj_ref;
	request_rename.from_obj_ref = VTOWEBDAV(fvp)->pt_obj_ref;
	request_rename.to_dir_ref = VTOWEBDAV(tdvp)->pt_obj_ref;
	request_rename.to_obj_ref = (tvp != NULLVP) ? VTOWEBDAV(tvp)->pt_obj_ref : 0;
	request_rename.to_name_length = tcnp->cn_namelen;

	error = webdav_sendmsg(WEBDAV_RENAME, VFSTOWEBDAV(vnode_mount(fvp)),
		&request_rename, offsetof(struct webdav_request_rename, to_name), 
		tcnp->cn_nameptr, tcnp->cn_namelen, 
		&server_error, NULL, 0);
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}

	if ( tvp != NULLVP )
	{
		/* tvp may have been deleted even if the move failed so get it out of the cache */
		if (tvp != fvp)
		{
			cache_purge(tvp);
		}
		
		/* if no errors, we know tvp was deleted */
		if ( error == 0 )
		{
			VTOWEBDAV(tvp)->pt_status |= WEBDAV_DELETED;
			(void) vnode_recycle(tvp); /* we don't care if the recycle was done or not */
		}
	}
	
	/* parent directories may have changed (even if error) so force readdir to reload */
	VTOWEBDAV(fdvp)->pt_status |= WEBDAV_DIR_NOT_LOADED;
	VTOWEBDAV(tdvp)->pt_status |= WEBDAV_DIR_NOT_LOADED;

	RET_ERR("webdav_vnop_rename", error);
}

/*****************************************************************************/

static int webdav_vnop_mkdir(struct vnop_mkdir_args *ap)
/*
	struct vnop_mkdir_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_dvp;
		vnode_t *a_vpp;
		struct componentname *a_cnp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	};
*/
{
	struct componentname *cnp = ap->a_cnp;
	vnode_t *vpp = ap->a_vpp;
	vnode_t dvp = ap->a_dvp;
	struct webdavmount *fmp;
	int error = 0;
	int server_error = 0;
	struct timeval tv;
	struct timespec ts;
	struct webdav_request_mkdir request_mkdir;
	struct webdav_reply_mkdir reply_mkdir;

	START_MARKER("webdav_vnop_mkdir");

	fmp = VFSTOWEBDAV(vnode_mount(dvp));
	
	/* Set *vpp to Null for error checking purposes */
	*vpp = NULL;

	/* Make sure this is a directory */
	if (ap->a_vap->va_type != VDIR)
	{
		error = ENOTDIR;
		goto bad;
	}

	webdav_copy_creds(ap->a_context, &request_mkdir.pcr);
	request_mkdir.dir = VTOWEBDAV(dvp)->pt_obj_ref;
	request_mkdir.mode = ap->a_vap->va_mode;
	request_mkdir.name_length = cnp->cn_namelen;

	bzero(&reply_mkdir, sizeof(struct webdav_reply_mkdir));
	
	error = webdav_sendmsg(WEBDAV_MKDIR, fmp,
		&request_mkdir, offsetof(struct webdav_request_mkdir, name), 
		cnp->cn_nameptr, cnp->cn_namelen,
		&server_error, &reply_mkdir, sizeof(struct webdav_reply_mkdir));
	if ( (error == 0) && (server_error != 0) )
	{
		error = server_error;
	}
	if (error)
	{
		goto bad;
	}

	/* parent directory changed so force readdir to reload */
	VTOWEBDAV(dvp)->pt_status |= WEBDAV_DIR_NOT_LOADED;

	microtime(&tv);
	TIMEVAL_TO_TIMESPEC(&tv, &ts);
	microtime(&tv);
	
	error = webdav_get(vnode_mount(dvp), dvp, 0, cnp,
		reply_mkdir.obj_ref, reply_mkdir.obj_fileid, VDIR, ts, ts, ts, fmp->pm_dir_size, vpp);
	if (error)
	{
		/* nothing we can do except complain */
		printf("webdav_vnop_mkdir: webdav_get failed\n");
	}

bad:

	RET_ERR("webdav_vnop_mkdir", error);
}

/*****************************************************************************/

/*
 * webdav_vnop_setattr
 *
 * webdav_vnop_setattr set the attributes of a file.
 *
 * results:
 *	0		Success.
 *	EACCES	vp was VROOT.
 *	EINVAL	unsettable attribute
 *	EROFS	read-only file system
 *	EIO		A physical I/O error has occurred, or this error was generated for
 *			implementation-defined reasons.
 */
static int webdav_vnop_setattr(struct vnop_setattr_args *ap)
/*
	struct vnop_setattr_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		struct vnode_attr *a_vap;
		vfs_context_t a_context;
	};
*/
{
	int error;
	vnode_t vp;
	struct webdavnode *pt;
	vnode_t cachevp;
	struct vnode_attr attrbuf;

	START_MARKER("webdav_vnop_setattr");

	error = 0;
	vp = ap->a_vp;
	pt = VTOWEBDAV(vp);
	cachevp = pt->pt_cache_vnode;
	
	/* Can't mess with the root vnode */
	if (vnode_isvroot(vp))
	{
		error = EACCES;
		goto exit;
	}

	/* If there is a local cache file, we'll allow setting.	We won't talk to the
	 * server, but we will honor the local file set. This will at least make fsx work.
	 */
	if ( cachevp != NULLVP )
	{
		/* If we are changing the size, call ubc_setsize to fix things up
		 * with the UBC Also, make sure that we wait until the file is
		 * completely downloaded */
		if (VATTR_IS_ACTIVE(ap->a_vap, va_data_size) && vnode_isreg(vp))
		{
			do
			{
				VATTR_INIT(&attrbuf);
				VATTR_WANTED(&attrbuf, va_flags);
				VATTR_WANTED(&attrbuf, va_data_size);
				error = vnode_getattr(cachevp, &attrbuf, ap->a_context);
				if (error)
				{
					goto exit;
				}

				if (attrbuf.va_flags & UF_NODUMP)
				{
					struct timespec ts;
					
					/* We are downloading the file and we haven't finished
					* since the user process is going to extend the file with
					* writes until it is done, so sleep, and then check again.
					*/
					ts.tv_sec = 0;
					ts.tv_nsec = WEBDAV_WAIT_FOR_PAGE_TIME;
					error = msleep((caddr_t)&ts, NULL, PCATCH, "webdav_vnop_setattr", &ts);
					if ( error)
					{
						if ( error == EWOULDBLOCK )
						{
							error = 0;
						}
						else
						{
							printf("webdav_vnop_setattr: msleep(): %d\n", error);
							/* convert pseudo-errors to EIO */
							if ( error < 0 )
							{
								error = EIO;
							}
							goto exit;
						}
					}
				}
				else
				{
					/* the file has been downloaded */
					break; /* out of while (TRUE) loop */
				}
			} while ( TRUE );

			if (attrbuf.va_flags & UF_APPEND)
			{
				/* If the UF_APPEND flag is set, there was an error downloading the file from the
				 * server, so exit with an EIO result.
				 */
				error = EIO;
				goto exit;
			}

			/* At this point, cachevp is locked and the file is completely downloaded into cachevp */
	
			/* If the size of the file is changed, set WEBDAV_DIRTY.
			 * WEBDAV_DIRTY is not set for other cases of setattr
			 * because we don't actually save va_flags, va_mode,
			 * va_uid, va_gid, va_atime or va_mtime on the server.
			 *
			 * XXX -- Eventually, we need to add code to call
			 * webdav_lock() in webdav_file.c to make sure we have
			 * a LOCK on the WebDAV server, so we don't try to PUT
			 * with no lock. The way things work now, the truncate
			 * might not stick if the file on the server isn't open
			 * with write access and some other client of the server
			 * has a WebDAV LOCK on the file (in which case fsync will
			 * fail with EBUSY when it tries to PUT the file to the
			 * server).
			 */
			if ( ap->a_vap->va_data_size != attrbuf.va_data_size || (off_t)ap->a_vap->va_data_size != pt->pt_filesize )
			{
				pt->pt_status |= WEBDAV_DIRTY;
			}
			
			/* set the size and other attributes of the cache file */
			error = vnode_setattr(cachevp, ap->a_vap, ap->a_context);
			
			/* let the UBC know the new size */
			(void) ubc_setsize(vp, (off_t)ap->a_vap->va_data_size); /* ignore failures - nothing can be done */
			pt->pt_filesize = (off_t)ap->a_vap->va_data_size;
		}
		else
		{
			/* set the attributes of the cache file */
			error = vnode_setattr(cachevp, ap->a_vap, ap->a_context);
		}
	}

exit:

	RET_ERR("webdav_vnop_setattr", error);
}

/*****************************************************************************/

/*
 * webdav_vnop_readdir
 *
 * webdav_vnop_readdir reads directory entries. We'll use the cache file for
 * the needed I/O.
 *
 * results:
 *	0		Success.
 *	ENOTDIR	attempt to use non-VDIR.
 *	EINVAL	no cache file, or attempt to read from illegal offset in the directory.
 *	EIO		A physical I/O error has occurred, or this error was generated for
 *			implementation-defined reasons.
 */
static int webdav_vnop_readdir(struct vnop_readdir_args *ap)
/*
	struct vnop_readdir_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		struct uio *a_uio;
		int a_flags;
		int *a_eofflag;
		int *a_numdirent;
		vfs_context_t a_context;
	};
*/
{
	vnode_t vp;
	vnode_t cachevp;
	struct webdavnode *pt;
	struct webdavmount *fmp;
	int server_error;
	uio_t uio;
	int error;
	user_ssize_t count, lost;
	struct vnode_attr vattr;

	START_MARKER("webdav_vnop_readdir");

	vp = ap->a_vp;
	uio = ap->a_uio;
	pt = VTOWEBDAV(vp);
	fmp = VFSTOWEBDAV(vnode_mount(vp));
	error = 0;
	
	/* First make sure it is a directory we are dealing with */
	if ( !vnode_isdir(vp))
	{
		error = ENOTDIR;
		goto done;
	}

	/* Make sure we have a cache file. If not the call must be wrong some how */
	cachevp = pt->pt_cache_vnode;
	if ( cachevp == NULLVP )
	{
		error = EINVAL;
		goto done;
	}
	
	/* No support for requires seek offset (cookies) */
	if ( ap->a_flags & VNODE_READDIR_REQSEEKOFF )
	{
		error = EINVAL;
		goto done;
	}

	/*
	 * If we starting from the beginning or WEBDAV_DIR_NOT_LOADED is set,
	 * refresh the directory with the latest from the server.
	 */
	if ( (uio_offset(uio) == 0) || (pt->pt_status & WEBDAV_DIR_NOT_LOADED) )
	{
		struct webdav_request_readdir request_readdir;
		
		webdav_copy_creds(ap->a_context, &request_readdir.pcr);
		request_readdir.obj_ref = pt->pt_obj_ref;
		request_readdir.cache = !vnode_isnocache(vp);

		error = webdav_sendmsg(WEBDAV_READDIR, fmp,
			&request_readdir, sizeof(struct webdav_request_readdir), 
			NULL, 0, 
			&server_error, NULL, 0);
		if ( (error == 0) && (server_error != 0) )
		{
			error = server_error;
		}
		if (error)
		{
			/* set the WEBDAV_DIR_NOT_LOADED flag */
			pt->pt_status |= WEBDAV_DIR_NOT_LOADED;
			goto done;
		}

		/* We didn't get an error so clear the WEBDAV_DIR_NOT_LOADED flag */
		pt->pt_status &= ~WEBDAV_DIR_NOT_LOADED;
	}

	if ( ap->a_flags & VNODE_READDIR_EXTENDED )
	{
		/* XXX No support for VNODE_READDIR_EXTENDED (yet) */
		error = EINVAL;
	}
	else
	{
		/* Make sure we don't return partial entries. */
		if ( ((uio_offset(uio) % sizeof(struct dirent)) != 0) ||
			 (uio_resid(uio) < (user_ssize_t)sizeof(struct dirent)) )
		{
			error = EINVAL;
			goto done;
		}

		count = uio_resid(uio);
		count -= (uio_offset(uio) + count) % sizeof(struct dirent);
		if (count <= 0)
		{
			error = EINVAL;
			goto done;
		}

		lost = uio_resid(uio) - count;
		uio_setresid(uio, count);

		error = VNOP_READ(cachevp, uio, 0, ap->a_context);
		
		uio_setresid(uio, uio_resid(uio) + lost);
		
		if (ap->a_eofflag)
		{
			VATTR_INIT(&vattr);
			VATTR_WANTED(&vattr, va_data_size);
			error = vnode_getattr(cachevp, &vattr, ap->a_context);
			if (error)
			{
				goto done;
			}
			*ap->a_eofflag = (off_t)vattr.va_data_size <= uio_offset(uio);
		}
	}

done:

	RET_ERR("webdav_vnop_readdir", error);
}

/*****************************************************************************/

/*
 * webdav_vnop_inactive
 *
 * Note: A message cannot be sent to user-land from here because it might cause
 * a recursive problem (connecting needs a vnode, which could bring control back
 * here again). So, all communication with user-land is done at close or mnomap.
 */
static int webdav_vnop_inactive(struct vnop_inactive_args *ap)
/*
	struct vnop_inactive_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		vfs_context_t a_context;
	};
*/
{
	#pragma unused(ap)
	START_MARKER("webdav_vnop_inactive");

	RET_ERR("webdav_vnop_inactive", 0);
}

/*****************************************************************************/

static int webdav_vnop_reclaim(struct vnop_reclaim_args *ap)
/*
	struct vnop_reclaim_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		vfs_context_t a_context;
	};
*/
{
	vnode_t vp;
	struct webdavnode *pt;
	
	START_MARKER("webdav_vnop_reclaim");

	vp = ap->a_vp;
	pt = VTOWEBDAV(vp);
	
	/* remove the reference added for pt_vnode */
	vnode_removefsref(vp);
	
	/* remove from hash */
	webdav_hashrem(VTOWEBDAV(vp));
	
	/*
	 * In case we block during FREE_ZONEs below, get the entry out
	 * of tbe name cache now so subsequent lookups won't find it.
	 */
	cache_purge(vp);
	
	/* remove webdavnode pointer from vnode */
	vnode_clearfsnode(vp);

	/* free the webdavnode */
	FREE(pt, M_TEMP);
	
	RET_ERR("webdav_vnop_reclaim", 0);
}

/*****************************************************************************/

/*
 * webdav_vnop_pathconf
 *
 * Return POSIX pathconf information.
 */
static int webdav_vnop_pathconf(struct vnop_pathconf_args *ap)
/*
	struct vnop_pathconf_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		int a_name;
		register_t *a_retval;
		vfs_context_t a_context;
	};
*/
{
	int error;
	struct webdavmount *fmp;
	
	START_MARKER("webdav_vnop_pathconf");

	fmp = VFSTOWEBDAV(vnode_mount(ap->a_vp));

	/* default to return if a name argument is not supported or value is not provided */
	*ap->a_retval = -1;
	error = EINVAL;

	switch (ap->a_name)
	{
		case _PC_LINK_MAX:
			/* maximum value of a file's link count (1 for file systems that do not support link counts) */
			if ( fmp->pm_link_max >= 0 )
			{
				*ap->a_retval = fmp->pm_link_max;
				error = 0;
			}
			break;
		
		case _PC_NAME_MAX:
			/* The maximum number of bytes in a file name (does not include null at end) */
			if ( fmp->pm_name_max >= 0 )
			{
				*ap->a_retval = fmp->pm_name_max;
				error = 0;
			}
			break;
			
		case _PC_PATH_MAX:
			/* The maximum number of bytes in a relative pathname (does not include null at end) */
			if ( fmp->pm_path_max >= 0 )
			{
				*ap->a_retval = fmp->pm_path_max;
				error = 0;
			}
			break;
		
		case _PC_PIPE_BUF:
			/* The maximum number of bytes that can be written atomically to a pipe (usually PIPE_BUF if supported) */
			if ( fmp->pm_pipe_buf >= 0 )
			{
				*ap->a_retval = fmp->pm_pipe_buf;
				error = 0;
			}
			break;
		
		case _PC_CHOWN_RESTRICTED:
			/* Return _POSIX_CHOWN_RESTRICTED if appropriate privileges are required for the chown(2) */
			if ( fmp->pm_chown_restricted >= 0 )
			{
				*ap->a_retval = fmp->pm_chown_restricted;
				error = 0;
			}
			break;
		
		case _PC_NO_TRUNC:
			/* Return _POSIX_NO_TRUNC if file names longer than KERN_NAME_MAX are truncated */
			if ( fmp->pm_no_trunc >= 0 )
			{
				*ap->a_retval = fmp->pm_no_trunc;
				error = 0;
			}
			break;
		
		default:
			/* other name arguments are not supported */
			break;
	}
	
	RET_ERR("webdav_vnop_pathconf", error);
}

/*****************************************************************************/

/*
 * webdav_vnop_pagein
 *
 * Page in (read) a page of a file into memory.
 *
 * To do:
 *		If UBC ever supports an efficient way to move pages from the cache file's
 *		UPL to the webdav UPL (a_pl), use that method instead of calling VOP_READ.
 */
static int webdav_vnop_pagein(struct vnop_pagein_args *ap)
/*
	struct vnop_pagein_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		upl_t a_pl;
		vm_offset_t a_pl_offset;
		off_t a_f_offset;
		size_t a_size;
		int a_flags;
		vfs_context_t a_context;
	};
*/
{
	vm_offset_t ioaddr;
	uio_t auio;
	vnode_t cachevp;
	vnode_t vp;
	struct webdavnode *pt;
	int bytes_to_zero;
	int error;
	int tried_bytes;
	struct vnode_attr attrbuf;
	kern_return_t kret;

	START_MARKER("webdav_vnop_pagein");

	vp = ap->a_vp;
	pt = VTOWEBDAV(vp);
	cachevp = pt->pt_cache_vnode;
	error = 0;

	auio = uio_create(1, ap->a_f_offset, UIO_SYSSPACE, UIO_READ);
	if ( auio == NULL )
	{
		error = EIO;
		goto exit;
	}
	
	kret = ubc_upl_map(ap->a_pl, &ioaddr);
	if (kret != KERN_SUCCESS)
	{
		panic("webdav_vnop_pagein: ubc_upl_map() failed with (%d)", kret);
	}

	ioaddr += ap->a_pl_offset;	/* add a_pl_offset */
	
	error = uio_addiov(auio, (user_addr_t)ioaddr, ap->a_size);
	if ( error )
	{
		goto exit;
	}

	/* Ok, start the sleep loop to wait on the background download
	  We will know that the webdav user process is finished when it
	  either clears the nodump flag or sets the append only flag
	  (indicating an error) */
	tried_bytes = FALSE;
	do
	{
		VATTR_INIT(&attrbuf);
		VATTR_WANTED(&attrbuf, va_flags);
		VATTR_WANTED(&attrbuf, va_data_size);
		error = vnode_getattr(cachevp, &attrbuf, ap->a_context);
		if (error)
		{
			goto exit;
		}

		if ((attrbuf.va_flags & UF_NODUMP) && (uio_offset(auio) + uio_resid(auio)) > (off_t)attrbuf.va_data_size)
		{
			struct timespec ts;
			
			/* We are downloading the file and we haven't gotten to
			 * to the bytes we need so sleep, and then try the whole
			 * thing again.	We will take one shot at trying to get the
			 * bytes out of the file directly if that part hasn't yet
			 * been downloaded.	This is a little iffy since the VM system
			 * may now be chaching data that could theoritically be out of
			 * sync with what's on the server.  That is the following sequence
			 * of operations could lead to strange results:
			 * 1. I start a read and begin a down load
			 * 2. Another client changes the file
			 * 3. I do a byte read of the end of the file and get the new data
			 * 4. The download finishes and the underlying cache file has
			 *	 the old data, possibly depending on how the server works.
			 */
			if (!tried_bytes)
			{
				if ((uio_offset(auio) + uio_resid(auio)) > ((off_t)attrbuf.va_data_size + WEBDAV_WAIT_IF_WITHIN))
				{
					error = webdav_read_bytes(vp, auio, ap->a_context);
					if (!error)
					{
						if (uio_resid(auio) == 0)
						{
							goto exit;
						}
						else
						{
							/* we did not get all the data we wanted, we don't
							* know why so we'll just give up on the byte access
							* and wait for the data to download. We need to reset
							* the uio in that case since the VM system is not going
							* to be happy with partial reads
							*/
							uio_reset(auio, ap->a_f_offset, UIO_SYSSPACE, UIO_READ);
							
							error = uio_addiov(auio, (user_addr_t)ioaddr, ap->a_size);
							if ( error )
							{
								goto exit;
							}
						}
					}
				}
				
				/* If we are here, we must have failed to get the bytes so set
				 * tried_bytes so we won't make this mistake again and sleep */
				tried_bytes = TRUE;
			}

			ts.tv_sec = 0;
			ts.tv_nsec = WEBDAV_WAIT_FOR_PAGE_TIME;
			error = msleep((caddr_t)&ts, NULL, PCATCH, "webdav_vnop_pagein", &ts);
			if ( error)
			{
				if ( error == EWOULDBLOCK )
				{
					error = 0;
				}
				else
				{
					printf("webdav_vnop_pagein: msleep(): %d\n", error);
					/* convert pseudo-errors to EIO */
					if ( error < 0 )
					{
						error = EIO;
					}
					goto exit;
				}
			}
		}
		else
		{
			/* the part we need has been downloaded */
			if ( pt->pt_filesize < (off_t)attrbuf.va_data_size )
			{
				/* keep the ubc size up to date */
				(void) ubc_setsize(vp, attrbuf.va_data_size); /* ignore failures - nothing can be done */
				pt->pt_filesize = (off_t)attrbuf.va_data_size;
			}
			break;
		}
	} while ( TRUE );

	if (attrbuf.va_flags & UF_APPEND)
	{
		/* If the UF_APPEND flag is set, there was an error downloading the file from the
		 * server, so exit with an EIO result.
		 */
		error = EIO;
		goto exit;
	}

	/* At this point, cachevp is locked and either the file is completely downloaded into
	 * cachevp, or the page this I/O ends within has been completely downloaded into cachevp.
	 */
	
	if (ap->a_f_offset > (off_t)attrbuf.va_data_size)
	{
		/* Trying to pagein data beyond the eof is a no no */
		error = EFAULT;
		goto exit;
	}

	error = VNOP_READ(cachevp, auio, ((ap->a_flags & UPL_IOSYNC) ? IO_SYNC : 0), ap->a_context);

	if (uio_resid(auio) != 0)
	{
		/* If we were not able to read the entire page, check to
		 * see if we are at the end of the file, and if so, zero
		 * out the remaining part of the page
		 */
		if ((off_t)attrbuf.va_data_size < ap->a_f_offset + ap->a_size)
		{
			bytes_to_zero = ap->a_f_offset + ap->a_size - attrbuf.va_data_size;
			bzero((caddr_t)(ioaddr + ap->a_size - bytes_to_zero), bytes_to_zero);
		}
	}

exit:

	if ( auio != NULL )
	{
		uio_free(auio);
	}
	
	kret = ubc_upl_unmap(ap->a_pl);

	if (kret != KERN_SUCCESS)
	{
        panic("webdav_vnop_pagein: ubc_upl_unmap() failed with (%d)", kret);
	}

	if ( (ap->a_flags & UPL_NOCOMMIT) == 0 )
	{
		if (!error)
		{
			kret = ubc_upl_commit_range(ap->a_pl, ap->a_pl_offset, ap->a_size, UPL_COMMIT_FREE_ON_EMPTY);
		}
		else
		{
			kret = ubc_upl_abort_range(ap->a_pl, ap->a_pl_offset, ap->a_size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
	}

	RET_ERR("webdav_vnop_pagein", error);
}

/*****************************************************************************/

/*
 * webdav_vnop_pageout
 *
 * Page out (write) a page of a file from memory.
 *
 * To do:
 *		If UBC ever supports an efficient way to move pages from the webdav
 *		UPL (a_pl) to the cache file's UPL, use that method instead of calling VOP_WRITE.
 */
static int webdav_vnop_pageout(struct vnop_pageout_args *ap)
/*
	struct vnop_pageout_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		upl_t a_pl;
		vm_offset_t a_pl_offset;
		off_t a_f_offset;
		size_t a_size;
		int a_flags;
		vfs_context_t a_context;
	};
*/
{
	vm_offset_t ioaddr;
	uio_t auio;
	vnode_t cachevp;
	vnode_t vp;
	struct webdavnode *pt;
	int error;
	kern_return_t kret;
	struct vnode_attr attrbuf;
	
	START_MARKER("webdav_vnop_pageout");

	vp = ap->a_vp;
	pt = VTOWEBDAV(vp);
	cachevp = pt->pt_cache_vnode;
	error = 0;
	auio = NULL;

	if (vnode_vfsisrdonly(vp))
	{
		error = EROFS;
		goto exit;
	}

	auio = uio_create(1, ap->a_f_offset, UIO_SYSSPACE, UIO_WRITE);
	if ( auio == NULL )
	{
		error = EIO;
		goto exit;
	}
	
	kret = ubc_upl_map(ap->a_pl, &ioaddr);
	if (kret != KERN_SUCCESS)
	{
        panic("webdav_vnop_pageout: ubc_upl_map() failed with (%d)", kret);
	}

	ioaddr += ap->a_pl_offset;	/* add a_pl_offset */

	error = uio_addiov(auio, (user_addr_t)ioaddr, ap->a_size);
	if ( error )
	{
		goto exit;
	}
	
	do
	{
		VATTR_INIT(&attrbuf);
		VATTR_WANTED(&attrbuf, va_flags);
		VATTR_WANTED(&attrbuf, va_data_size);
		error = vnode_getattr(cachevp, &attrbuf, ap->a_context);
		if (error)
		{
			goto unlock_exit;
		}

		if ((attrbuf.va_flags & UF_NODUMP) && (uio_offset(auio) + uio_resid(auio)) > (off_t)attrbuf.va_data_size)
		{
			struct timespec ts;
			
			/* We are downloading the file and we haven't gotten to
			 * to the bytes we need so sleep, and then try the whole
			 * thing again.
			 */
			ts.tv_sec = 0;
			ts.tv_nsec = WEBDAV_WAIT_FOR_PAGE_TIME;
			error = msleep((caddr_t)&ts, NULL, PCATCH, "webdav_vnop_pageout", &ts);
			if ( error)
			{
				if ( error == EWOULDBLOCK )
				{
					error = 0;
				}
				else
				{
					printf("webdav_vnop_pageout: msleep(): %d\n", error);
					/* convert pseudo-errors to EIO */
					if ( error < 0 )
					{
						error = EIO;
					}
					goto exit;
				}
			}
		}
		else
		{
			/* the part we need has been downloaded */
			if ( pt->pt_filesize < (off_t)attrbuf.va_data_size )
			{
				/* keep the ubc size up to date */
				(void) ubc_setsize(vp, attrbuf.va_data_size); /* ignore failures - nothing can be done */
				pt->pt_filesize = (off_t)attrbuf.va_data_size;
			}
			break;
		}
	} while ( TRUE );

	if (attrbuf.va_flags & UF_APPEND)
	{
		/* If the UF_APPEND flag is set, there was an error downloading the file from the
		 * server, so exit with an EIO result.
		 */
		error = EIO;
		goto unlock_exit;
	}

	/* We don't want to write past the end of the file so 
	 * truncate the write to the size.
	 */
	if (uio_offset(auio) + uio_resid(auio) > (off_t)attrbuf.va_data_size)
	{
		if (uio_offset(auio) < (off_t)attrbuf.va_data_size)
		{
			uio_setresid(auio, attrbuf.va_data_size - uio_offset(auio));
		}
		else
		{
			/* If we are here, someone probably truncated a file that
			 * someone else had mapped. In any event we are not allowed
			 * to grow the file on a page out so return EFAULT as that is
			 * what VM is expecting.
			 */
			error = EFAULT;
			goto unlock_exit;
		}
	}

	error = VNOP_WRITE(cachevp, auio, ((ap->a_flags & UPL_IOSYNC) ? IO_SYNC : 0), ap->a_context);

unlock_exit:

	/* after the write to the cache file has been completed... */
	pt->pt_status |= WEBDAV_DIRTY;

exit:

	if ( auio != NULL )
	{
		uio_free(auio);
	}
	
	kret = ubc_upl_unmap(ap->a_pl);
	if (kret != KERN_SUCCESS)
	{
		panic("webdav_vnop_pageout: ubc_upl_unmap() failed with (%d)", kret);
	}

	if ( (ap->a_flags & UPL_NOCOMMIT) == 0 )
	{
		if (!error)
		{
			kret = ubc_upl_commit_range(ap->a_pl, ap->a_pl_offset, ap->a_size, UPL_COMMIT_FREE_ON_EMPTY);
		}
		else
		{
			kret = ubc_upl_abort_range(ap->a_pl, ap->a_pl_offset, ap->a_size, UPL_ABORT_ERROR | UPL_ABORT_FREE_ON_EMPTY);
		}
	}

	RET_ERR("webdav_vnop_pageout", error);
}

/*****************************************************************************/

static int webdav_vnop_ioctl(struct vnop_ioctl_args *ap)
/*
	struct vnop_ioctl_args {
		struct vnodeop_desc *a_desc;
		vnode_t a_vp;
		u_long a_command;
		caddr_t a_data;
		int a_fflag;
		vfs_context_t a_context;
	};
*/
{
	int error;
	vnode_t vp;
	
	START_MARKER("webdav_vnop_ioctl");

	error = EINVAL;
	vp = ap->a_vp;
	
	switch (ap->a_command)
	{
	case WEBDAV_INVALIDATECACHES:	/* invalidate all mount_webdav caches */
		{
			struct webdavmount *fmp;
			struct webdav_request_invalcaches request_invalcaches;
			int server_error;
			
			/* Note: Since this command is coming through fsctl(), vnode_get has been called on the vnode */
			
			/* set up the rest of the parameters needed to send a message */ 
			fmp = VFSTOWEBDAV(vnode_mount(vp));
			server_error = 0;
			
			webdav_copy_creds(ap->a_context, &request_invalcaches.pcr);

			error = webdav_sendmsg(WEBDAV_INVALCACHES, fmp,
				&request_invalcaches, sizeof(struct webdav_request_invalcaches), 
				NULL, 0, 
				&server_error, NULL, 0);
			if ( (error == 0) && (server_error != 0) )
			{
				error = server_error;
			}
		}
		break;
	
default:

		error = EINVAL;
		break;
	}

	RET_ERR("webdav_vnop_ioctl", error);
}

/*****************************************************************************/

#define VOPFUNC int (*)(void *)

int( **webdav_vnodeop_p)();

struct vnodeopv_entry_desc webdav_vnodeop_entries[] = {
	{&vnop_default_desc, (VOPFUNC)vn_default_error},				/* default */
	{&vnop_lookup_desc, (VOPFUNC)webdav_vnop_lookup},				/* lookup */
	{&vnop_create_desc, (VOPFUNC)webdav_vnop_create},				/* create */
	{&vnop_open_desc, (VOPFUNC)webdav_vnop_open},					/* open */
	{&vnop_close_desc, (VOPFUNC)webdav_vnop_close},					/* close */
	{&vnop_getattr_desc, (VOPFUNC)webdav_vnop_getattr},				/* getattr */
	{&vnop_setattr_desc, (VOPFUNC)webdav_vnop_setattr},			/* setattr */
	{&vnop_read_desc, (VOPFUNC)webdav_vnop_read},					/* read */
	{&vnop_write_desc, (VOPFUNC)webdav_vnop_write},					/* write */
	{&vnop_ioctl_desc, (VOPFUNC)webdav_vnop_ioctl},					/* ioctl */
	{&vnop_mmap_desc, (VOPFUNC)webdav_vnop_mmap},					/* mmap */
	{&vnop_mnomap_desc, (VOPFUNC)webdav_vnop_mnomap},				/* mnomap */
	{&vnop_fsync_desc, (VOPFUNC)webdav_vnop_fsync},					/* fsync */
	{&vnop_remove_desc, (VOPFUNC)webdav_vnop_remove},				/* remove */
	{&vnop_rename_desc, (VOPFUNC)webdav_vnop_rename},				/* rename */
	{&vnop_mkdir_desc, (VOPFUNC)webdav_vnop_mkdir},					/* mkdir */
	{&vnop_rmdir_desc, (VOPFUNC)webdav_vnop_rmdir},					/* rmdir */
	{&vnop_readdir_desc, (VOPFUNC)webdav_vnop_readdir},				/* readdir */
	{&vnop_inactive_desc, (VOPFUNC)webdav_vnop_inactive},			/* inactive */
	{&vnop_reclaim_desc, (VOPFUNC)webdav_vnop_reclaim},				/* reclaim */
	{&vnop_pathconf_desc, (VOPFUNC)webdav_vnop_pathconf},			/* pathconf */
	{&vnop_pagein_desc, (VOPFUNC)webdav_vnop_pagein},				/* pagein */
	{&vnop_pageout_desc, (VOPFUNC)webdav_vnop_pageout},				/* pageout */
	{(struct vnodeop_desc *)NULL, (VOPFUNC)NULL}					/* end of table */
};

struct vnodeopv_desc webdav_vnodeop_opv_desc = {
	&webdav_vnodeop_p, webdav_vnodeop_entries};

/*****************************************************************************/
