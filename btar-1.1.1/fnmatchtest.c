/*
    btar - no-tape archiver.
    Copyright (C) 2011  Lluis Batlle i Rossell

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
#include <fnmatch.h>
#include <stdio.h>

void test(const char *pattern, const char *str, int flags)
{
    int res;

    res = fnmatch(pattern, str, flags);
    printf(" fnmatch(\"%s\", \"%s\") = %i\n", pattern, str, res);
}

int main()
{
    const char path1[] = "home/viric/hola";
    const char path2[] = "home/viric/.xa/2";

    printf("nothing:\n");
    test("home/*", path1, 0);
    test("home/*", path2, 0);
    test("home/viric*", path1, 0);
    test("home/viric*", path2, 0);
    test("home/viric/*", path1, 0);
    test("home/viric/*", path2, 0);
    test("home/viric/", path1, 0);
    test("home/viric/", path2, 0);
    test("home*", path2, 0);
    test("home/", path2, 0);
    test("ho", path1, 0);
    test("home/vi", path1, 0);

    printf("FNM_PATHNAME:\n");
    test("home/*", path1, FNM_PATHNAME);
    test("home/*", path2, FNM_PATHNAME);
    test("home/viric*", path1, FNM_PATHNAME);
    test("home/viric*", path2, FNM_PATHNAME);
    test("home/viric/*", path1, FNM_PATHNAME);
    test("home/viric/*", path2, FNM_PATHNAME);
    test("home/viric/", path1, FNM_PATHNAME);
    test("home/viric/", path2, FNM_PATHNAME);
    test("home*", path2, FNM_PATHNAME);
    test("home/", path2, FNM_PATHNAME);

    return 0;
}
