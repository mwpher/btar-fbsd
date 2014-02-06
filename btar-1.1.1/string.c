/*
    btar - no-tape archiver.
    Copyright (C) 2012  Lluis Batlle i Rossell

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "main.h"

/* Fastest than strncpy, because it does not write all 'n' characters. Additionally,
 * it ensures the null-ending. */
void
strcpyn(char *dest, const char *src, size_t n)
{
    size_t i;

    for(i=0; i<n; ++i)
    {
        dest[i] = src[i];
        if (src[i] == '\0')
            break;
    }
    dest[n-1] = '\0';
}
