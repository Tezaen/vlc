/*****************************************************************************
 * tcp.c:
 *****************************************************************************
 * Copyright (C) 2004-2005 the VideoLAN team
 * Copyright (C) 2005-2006 Rémi Denis-Courmont
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@videolan.org>
 *          Rémi Denis-Courmont <rem # videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>

#include <errno.h>

#ifdef HAVE_FCNTL_H
#   include <fcntl.h>
#endif
#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#endif
#ifdef HAVE_POLL
# include <poll.h>
#endif

#include <vlc_network.h>
#if defined (WIN32) || defined (UNDER_CE)
#   undef EINPROGRESS
#   define EINPROGRESS WSAEWOULDBLOCK
#   undef EINTR
#   define EINTR WSAEINTR
#   undef ETIMEDOUT
#   define ETIMEDOUT WSAETIMEDOUT
#endif

static int SocksNegociate( vlc_object_t *, int fd, int i_socks_version,
                           const char *psz_user, const char *psz_passwd );
static int SocksHandshakeTCP( vlc_object_t *,
                              int fd, int i_socks_version,
                              const char *psz_user, const char *psz_passwd,
                              const char *psz_host, int i_port );
extern int net_Socket( vlc_object_t *p_this, int i_family, int i_socktype,
                       int i_protocol );

/*****************************************************************************
 * __net_Connect:
 *****************************************************************************
 * Open a network connection.
 * @return socket handler or -1 on error.
 *****************************************************************************/
int __net_Connect( vlc_object_t *p_this, const char *psz_host, int i_port,
                   int type, int proto )
{
    struct addrinfo hints, *res, *ptr;
    const char      *psz_realhost;
    char            *psz_socks;
    int             i_realport, i_val, i_handle = -1;

    if( i_port == 0 )
        i_port = 80; /* historical VLC thing */

    memset( &hints, 0, sizeof( hints ) );
    hints.ai_socktype = SOCK_STREAM;

    psz_socks = var_CreateGetNonEmptyString( p_this, "socks" );
    if( psz_socks != NULL )
    {
        char *psz = strchr( psz_socks, ':' );

        if( psz )
            *psz++ = '\0';

        psz_realhost = psz_socks;
        i_realport = ( psz != NULL ) ? atoi( psz ) : 1080;
        hints.ai_flags &= ~AI_NUMERICHOST;

        msg_Dbg( p_this, "net: connecting to %s port %d (SOCKS) "
                 "for %s port %d", psz_realhost, i_realport,
                 psz_host, i_port );

        /* We only implement TCP with SOCKS */
        switch( type )
        {
            case 0:
                type = SOCK_STREAM;
            case SOCK_STREAM:
                break;
            default:
                msg_Err( p_this, "Socket type not supported through SOCKS" );
                free( psz_socks );
                return -1;
        }
        switch( proto )
        {
            case 0:
                proto = IPPROTO_TCP;
            case IPPROTO_TCP:
                break;
            default:
                msg_Err( p_this, "Transport not supported through SOCKS" );
                free( psz_socks );
                return -1;
        }
    }
    else
    {
        psz_realhost = psz_host;
        i_realport = i_port;

        msg_Dbg( p_this, "net: connecting to %s port %d", psz_realhost,
                 i_realport );
    }

    i_val = vlc_getaddrinfo( p_this, psz_realhost, i_realport, &hints, &res );
    free( psz_socks );

    if( i_val )
    {
        msg_Err( p_this, "cannot resolve %s port %d : %s", psz_realhost,
                 i_realport, vlc_gai_strerror( i_val ) );
        return -1;
    }

    for( ptr = res; ptr != NULL; ptr = ptr->ai_next )
    {
        int fd = net_Socket( p_this, ptr->ai_family, type ?: ptr->ai_socktype,
                             proto ?: ptr->ai_protocol );
        if( fd == -1 )
        {
            msg_Dbg( p_this, "socket error: %m" );
            continue;
        }

        if( connect( fd, ptr->ai_addr, ptr->ai_addrlen ) )
        {
            socklen_t i_val_size = sizeof( i_val );
            div_t d;
            vlc_value_t timeout;

            if( net_errno != EINPROGRESS )
            {
                msg_Err( p_this, "connection failed: %m" );
                goto next_ai;
            }
            msg_Dbg( p_this, "connection: %m" );

            var_Create( p_this, "ipv4-timeout",
                        VLC_VAR_INTEGER | VLC_VAR_DOINHERIT );
            var_Get( p_this, "ipv4-timeout", &timeout );
            if( timeout.i_int < 0 )
            {
                msg_Err( p_this, "invalid negative value for ipv4-timeout" );
                timeout.i_int = 0;
            }
            d = div( timeout.i_int, 100 );

            for (;;)
            {
                struct pollfd ufd = { .fd = fd, .events = POLLOUT };
                int i_ret;

                if( p_this->b_die )
                {
                    msg_Dbg( p_this, "connection aborted" );
                    net_Close( fd );
                    vlc_freeaddrinfo( res );
                    return -1;
                }

                /*
                 * We'll wait 0.1 second if nothing happens
                 * NOTE:
                 * time out will be shortened if we catch a signal (EINTR)
                 */
                i_ret = poll (&ufd, 1, (d.quot > 0) ? 100 : d.rem);
                if( i_ret == 1 )
                    break;

                if( ( i_ret == -1 ) && ( net_errno != EINTR ) )
                {
                    msg_Err( p_this, "connection polling error: %m" );
                    goto next_ai;
                }

                if( d.quot <= 0 )
                {
                    msg_Warn( p_this, "connection timed out" );
                    goto next_ai;
                }

                d.quot--;
            }

#if !defined( SYS_BEOS ) && !defined( UNDER_CE )
            if( getsockopt( fd, SOL_SOCKET, SO_ERROR, (void*)&i_val,
                            &i_val_size ) == -1 || i_val != 0 )
            {
                errno = i_val;
                msg_Err( p_this, "connection failed: %m" );
                goto next_ai;
            }
#endif
        }

        msg_Dbg( p_this, "connection succeeded (socket = %d)", fd );
        i_handle = fd; /* success! */
        break;

next_ai: /* failure */
        net_Close( fd );
        continue;
    }

    vlc_freeaddrinfo( res );

    if( i_handle == -1 )
        return -1;

    if( psz_socks != NULL )
    {
        /* NOTE: psz_socks already free'd! */
        char *psz_user = var_CreateGetNonEmptyString( p_this, "socks-user" );
        char *psz_pwd  = var_CreateGetNonEmptyString( p_this, "socks-pwd" );

        if( SocksHandshakeTCP( p_this, i_handle, 5, psz_user, psz_pwd,
                               psz_host, i_port ) )
        {
            msg_Err( p_this, "SOCKS handshake failed" );
            net_Close( i_handle );
            i_handle = -1;
        }

        free( psz_user );
        free( psz_pwd );
    }

    return i_handle;
}


/*****************************************************************************
 * __net_Accept:
 *****************************************************************************
 * Accept a connection on a set of listening sockets and return it
 *****************************************************************************/
int __net_Accept( vlc_object_t *p_this, int *pi_fd, mtime_t i_wait )
{
    int timeout = (i_wait < 0) ? -1 : i_wait / 1000;
    int evfd;

    assert( pi_fd != NULL );

    vlc_object_lock (p_this);
    evfd = vlc_object_waitpipe (p_this);

    while (vlc_object_alive (p_this))
    {
        unsigned n = 0;
        while (pi_fd[n] != -1)
            n++;
        struct pollfd ufd[n + 1];

        /* Initialize file descriptor set */
        for (unsigned i = 0; i <= n; i++)
        {
            ufd[i].fd = (i < n) ? pi_fd[i] : evfd;
            ufd[i].events = POLLIN;
            ufd[i].revents = 0;
        }

        vlc_object_unlock (p_this);
        switch (poll (ufd, n, timeout))
        {
            case -1:
                if (net_errno != EINTR)
                    msg_Err (p_this, "poll error: %m");
            case 0:
                return -1; /* NOTE: p_this already unlocked */
        }
        vlc_object_lock (p_this);

        if (ufd[n].revents)
        {
            vlc_object_wait (p_this);
            errno = EINTR;
            break;
        }

        for (unsigned i = 0; i < n; i++)
        {
            if (ufd[i].revents == 0)
                continue;

            int sfd = ufd[i].fd;
            int fd = accept (sfd, NULL, NULL);
            if (fd == -1)
            {
                msg_Err (p_this, "accept failed (%m)");
                continue;
            }
            net_SetupSocket (fd);

            /*
             * Move listening socket to the end to let the others in the
             * set a chance next time.
             */
            memmove (pi_fd + i, pi_fd + i + 1, n - (i + 1));
            pi_fd[n - 1] = sfd;
            vlc_object_unlock (p_this);
            msg_Dbg (p_this, "accepted socket %d (from socket %d)", fd, sfd);
            return fd;
        }
    }
    vlc_object_unlock (p_this);
    return -1;
}


/*****************************************************************************
 * SocksNegociate:
 *****************************************************************************
 * Negociate authentication with a SOCKS server.
 *****************************************************************************/
static int SocksNegociate( vlc_object_t *p_obj,
                           int fd, int i_socks_version,
                           const char *psz_socks_user,
                           const char *psz_socks_passwd )
{
    uint8_t buffer[128+2*256];
    int i_len;
    vlc_bool_t b_auth = VLC_FALSE;

    if( i_socks_version != 5 )
        return VLC_SUCCESS;

    /* We negociate authentication */

    if( ( psz_socks_user == NULL ) && ( psz_socks_passwd == NULL ) )
        b_auth = VLC_TRUE;

    buffer[0] = i_socks_version;    /* SOCKS version */
    if( b_auth )
    {
        buffer[1] = 2;                  /* Number of methods */
        buffer[2] = 0x00;               /* - No auth required */
        buffer[3] = 0x02;               /* - USer/Password */
        i_len = 4;
    }
    else
    {
        buffer[1] = 1;                  /* Number of methods */
        buffer[2] = 0x00;               /* - No auth required */
        i_len = 3;
    }

    if( net_Write( p_obj, fd, NULL, buffer, i_len ) != i_len )
        return VLC_EGENERIC;
    if( net_Read( p_obj, fd, NULL, buffer, 2, VLC_TRUE ) != 2 )
        return VLC_EGENERIC;

    msg_Dbg( p_obj, "socks: v=%d method=%x", buffer[0], buffer[1] );

    if( buffer[1] == 0x00 )
    {
        msg_Dbg( p_obj, "socks: no authentication required" );
    }
    else if( buffer[1] == 0x02 )
    {
        int i_len1 = __MIN( strlen(psz_socks_user), 255 );
        int i_len2 = __MIN( strlen(psz_socks_passwd), 255 );
        msg_Dbg( p_obj, "socks: username/password authentication" );

        /* XXX: we don't support user/pwd > 255 (truncated)*/
        buffer[0] = i_socks_version;        /* Version */
        buffer[1] = i_len1;                 /* User length */
        memcpy( &buffer[2], psz_socks_user, i_len1 );
        buffer[2+i_len1] = i_len2;          /* Password length */
        memcpy( &buffer[2+i_len1+1], psz_socks_passwd, i_len2 );

        i_len = 3 + i_len1 + i_len2;

        if( net_Write( p_obj, fd, NULL, buffer, i_len ) != i_len )
            return VLC_EGENERIC;

        if( net_Read( p_obj, fd, NULL, buffer, 2, VLC_TRUE ) != 2 )
            return VLC_EGENERIC;

        msg_Dbg( p_obj, "socks: v=%d status=%x", buffer[0], buffer[1] );
        if( buffer[1] != 0x00 )
        {
            msg_Err( p_obj, "socks: authentication rejected" );
            return VLC_EGENERIC;
        }
    }
    else
    {
        if( b_auth )
            msg_Err( p_obj, "socks: unsupported authentication method %x",
                     buffer[0] );
        else
            msg_Err( p_obj, "socks: authentification needed" );
        return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

/*****************************************************************************
 * SocksHandshakeTCP:
 *****************************************************************************
 * Open a TCP connection using a SOCKS server and return a handle (RFC 1928)
 *****************************************************************************/
static int SocksHandshakeTCP( vlc_object_t *p_obj,
                              int fd,
                              int i_socks_version,
                              const char *psz_user, const char *psz_passwd,
                              const char *psz_host, int i_port )
{
    uint8_t buffer[128+2*256];

    if( i_socks_version != 4 && i_socks_version != 5 )
    {
        msg_Warn( p_obj, "invalid socks protocol version %d", i_socks_version );
        i_socks_version = 5;
    }

    if( i_socks_version == 5 &&
        SocksNegociate( p_obj, fd, i_socks_version,
                        psz_user, psz_passwd ) )
        return VLC_EGENERIC;

    if( i_socks_version == 4 )
    {
        struct addrinfo hints, *p_res;

        /* v4 only support ipv4 */
        memset (&hints, 0, sizeof (hints));
        hints.ai_family = AF_INET;
        if( vlc_getaddrinfo( p_obj, psz_host, 0, &hints, &p_res ) )
            return VLC_EGENERIC;

        buffer[0] = i_socks_version;
        buffer[1] = 0x01;               /* CONNECT */
        SetWBE( &buffer[2], i_port );   /* Port */
        memcpy( &buffer[4],             /* Address */
                &((struct sockaddr_in *)(p_res->ai_addr))->sin_addr, 4 );
        vlc_freeaddrinfo( p_res );

        buffer[8] = 0;                  /* Empty user id */

        if( net_Write( p_obj, fd, NULL, buffer, 9 ) != 9 )
            return VLC_EGENERIC;
        if( net_Read( p_obj, fd, NULL, buffer, 8, VLC_TRUE ) != 8 )
            return VLC_EGENERIC;

        msg_Dbg( p_obj, "socks: v=%d cd=%d",
                 buffer[0], buffer[1] );

        if( buffer[1] != 90 )
            return VLC_EGENERIC;
    }
    else if( i_socks_version == 5 )
    {
        int i_hlen = __MIN(strlen( psz_host ), 255);
        int i_len;

        buffer[0] = i_socks_version;    /* Version */
        buffer[1] = 0x01;               /* Cmd: connect */
        buffer[2] = 0x00;               /* Reserved */
        buffer[3] = 3;                  /* ATYP: for now domainname */

        buffer[4] = i_hlen;
        memcpy( &buffer[5], psz_host, i_hlen );
        SetWBE( &buffer[5+i_hlen], i_port );

        i_len = 5 + i_hlen + 2;


        if( net_Write( p_obj, fd, NULL, buffer, i_len ) != i_len )
            return VLC_EGENERIC;

        /* Read the header */
        if( net_Read( p_obj, fd, NULL, buffer, 5, VLC_TRUE ) != 5 )
            return VLC_EGENERIC;

        msg_Dbg( p_obj, "socks: v=%d rep=%d atyp=%d",
                 buffer[0], buffer[1], buffer[3] );

        if( buffer[1] != 0x00 )
        {
            msg_Err( p_obj, "socks: CONNECT request failed\n" );
            return VLC_EGENERIC;
        }

        /* Read the remaining bytes */
        if( buffer[3] == 0x01 )
            i_len = 4-1 + 2;
        else if( buffer[3] == 0x03 )
            i_len = buffer[4] + 2;
        else if( buffer[3] == 0x04 )
            i_len = 16-1+2;
        else
            return VLC_EGENERIC;

        if( net_Read( p_obj, fd, NULL, buffer, i_len, VLC_TRUE ) != i_len )
            return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

void net_ListenClose( int *pi_fd )
{
    if( pi_fd != NULL )
    {
        int *pi;

        for( pi = pi_fd; *pi != -1; pi++ )
            net_Close( *pi );
        free( pi_fd );
    }
}
