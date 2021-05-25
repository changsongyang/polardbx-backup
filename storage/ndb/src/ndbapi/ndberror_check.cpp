/*
   Copyright (c) 2007, 2021, Oracle and/or its affiliates.
    All rights reserved. Use is subject to license terms.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <stdio.h>
#include <NdbDictionary.hpp>
#include "ndberror.c"

// Mock implementation of 'my_snprintf'
size_t my_snprintf(char* to, size_t n, const char* fmt, ...)
{
  abort();
  /*NOTREACHED*/
  return 0;  /* the function just satisfies the linker, never to be executed */
}


#include <NdbTap.hpp>

TAPTEST(ndberror_check)
{
  int ok= 1;
  /* check for duplicate error codes */
  for(int i = 0; i < NbErrorCodes; i++)
  {
    for(int j = i + 1; j < NbErrorCodes; j++)
    {
      if (ErrorCodes[i].code == ErrorCodes[j].code)
      {
        fprintf(stderr, "Duplicate error code %u\n", ErrorCodes[i].code);
        ok = 0;
      }
    }
  }
  return ok;
}

