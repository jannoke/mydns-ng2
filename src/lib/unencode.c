/**************************************************************************************************
	$Id: unencode.c,v 1.8 2006/01/18 20:46:46 bboy Exp $

	unencode.c: Routine to unencode a DNS-encoded name.

	Copyright (C) 2002-2005  Don Moore <bboy@bboy.net>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at Your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
**************************************************************************************************/

#include "mydns.h"


/**************************************************************************************************
	NAME_DECODE
	Decode the (possibly compressed) name that starts at `current' inside the message
	[start, start+slen) into `dest', which must hold at least DNS_MAXNAMELEN + 2 bytes.

	Everything read comes from untrusted packets, so:
	  - every octet read (length octets, label bytes, pointer octets, terminator) is checked
	    against the end of the message;
	  - a compression pointer must point strictly backward, to data earlier in the message
	    (RFC 1035 4.1.4), so a chain of pointers alone always terminates;
	  - the decoded wire length is capped at DNS_MAXNAMELEN, which also bounds a label/pointer
	    loop such as "label at 50 runs up to a pointer at 100 that jumps back to 50".

	Returns the position just past the name in the original (uncompressed) stream, or NULL
	with *errcode set.
**************************************************************************************************/
static uchar *
name_decode(uchar *start, size_t slen, uchar *current, char *dest, task_error_t *errcode) {
  uchar		*end = start + slen;
  uchar		*s = current;				/* Current read position */
  uchar		*rp = NULL;				/* Resume point after the first pointer */
  char		*d = dest;				/* Current write position */
  size_t	wirelen = 1;				/* Encoded length so far (incl. terminator) */

  if (!start || current < start || current >= end) {
    *errcode = ERR_Q_BUFFER_OVERFLOW;
    return (NULL);
  }

  for (;;) {
    uchar len;

    if (s >= end) {						/* Name runs off the end of the message */
      *errcode = ERR_Q_BUFFER_OVERFLOW;
      return (NULL);
    }
    len = *s;

    if (len == 0) {						/* Terminating root label */
      s++;
      break;
    }

    switch (len & 0xC0) {
    case 0xC0: {						/* Compression pointer */
      size_t	current_offset = s - start;
      uint16_t	new_offset;

      if (s + SIZE16 > end) {
	*errcode = ERR_Q_BUFFER_OVERFLOW;
	return (NULL);
      }
      DNS_GET16(new_offset, s);
      new_offset &= ~0xC000;

      if (new_offset >= current_offset) {			/* Not pointing strictly backward */
	*errcode = ERR_Q_INVALID_COMPRESSION;
	return (NULL);
      }
      if (!rp)
	rp = s;
      s = start + new_offset;
      break;
    }

    case 0x00:							/* Ordinary label of 1..63 octets */
      s++;
      if (len > end - s) {
	*errcode = ERR_Q_BUFFER_OVERFLOW;
	return (NULL);
      }
      wirelen += len + 1;
      if (wirelen > DNS_MAXNAMELEN) {
	*errcode = ERR_Q_NAME_TOO_LONG;
	return (NULL);
      }
      while (len--)
	*d++ = tolower(*s++);
      *d++ = '.';
      break;

    default:							/* 0x40/0x80: reserved label types */
      *errcode = ERR_Q_INVALID_COMPRESSION;
      return (NULL);
    }
  }

  if (d == dest)						/* The name is just "." */
    *d++ = '.';
  *d = '\0';
  return (rp ? rp : s);
}
/*--- name_decode() -----------------------------------------------------------------------------*/


/**************************************************************************************************
	NAME_UNENCODE
	Get a name from a compressed question into the caller's buffer.
	Returns the new location of src, or NULL if an error occurred.
	If an error occurs, puts the error code in `dest[0]'.
**************************************************************************************************/
uchar *
name_unencode(uchar *start, size_t slen, uchar *current, uchar *dest, size_t destsize) {
  char		buf[DNS_MAXNAMELEN + 2];
  task_error_t	errcode = 0;
  uchar		*next = NULL;

  if (!(next = name_decode(start, slen, current, buf, &errcode))) {
    dest[0] = errcode;
    return (NULL);
  }
  if (strlen(buf) >= destsize) {
    dest[0] = ERR_Q_BUFFER_OVERFLOW;
    return (NULL);
  }
  strcpy((char*)dest, buf);
  return (next);
}
/*--- name_unencode() ---------------------------------------------------------------------------*/


/**************************************************************************************************
	NAME_UNENCODE2
	Get a name from a compressed question into a newly allocated string.
	Returns the string (caller releases it) and advances *current past the name,
	or returns NULL with *errcode set.
**************************************************************************************************/
uchar *
name_unencode2(uchar *start, size_t slen, uchar **current, task_error_t *errcode) {
  char		*dest = ALLOCATE(DNS_MAXNAMELEN + 2, char[]);
  uchar		*next = NULL;

  if (!(next = name_decode(start, slen, *current, dest, errcode))) {
    RELEASE(dest);
    return (NULL);
  }
  *current = next;
  return ((uchar*)dest);
}
/*--- name_unencode2() --------------------------------------------------------------------------*/

/* vi:set ts=3: */
