/*
   Copyright (c) 2000, 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* 
**  Virtual I/O library
**  Written by Andrei Errapart <andreie@no.spam.ee>
*/

#include	"all.h"

#include	<sys/types.h>
#include	<sys/stat.h>
#include	<stdio.h>

#include	<string.h>

VIO_NS_USING;

int
main(	int	argc,
	char**	argv)
{
	VioFd*	fs = 0;
	VioSocket*	ss = 0;
	int		fd = -1;
	char*		hh = "hshshsh\n";

	DBUG_ENTER("main");
	DBUG_PROCESS(argv[0]);
	DBUG_PUSH("d:t");

	fd = open("/dev/tty", O_WRONLY);
	if (fd<0)
	{
		perror("open");
		DBUG_RETURN(1);
	}
	fs = new VioFd(fd);
	ss = new VioSocket(fd);
	if (fs->write(hh,strlen(hh)) < 0)
		perror("write");
	ss->write(hh,strlen(hh));
	printf("peer_name:%s\n", ss->peer_name());
	printf("cipher_description:%s\n", ss->cipher_description());
	delete fs;
	delete ss;

	DBUG_RETURN(0);
}

